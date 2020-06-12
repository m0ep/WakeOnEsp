#include <Arduino.h>
#include <IotWebConf.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>

#include "log.h"

#define IWC_STATUS_PIN LED_BUILTIN
#define IWC_CONFIG_VERSION "woe1"

#define IWC_STRING_LEN 128
#define IWC_NUMBER_LEN 32

#define ACTION_FEQ_LIMIT 5000
#define ACTION_NONE 0
#define ACTION_POWER 1
#define ACTION_FORCE_OFF 2
#define ACTION_RESET 3

#define MQTT_FEQ_LIMIT 5000
#define MQTT_PREFIX_CMND "cmnd"
#define MQTT_PREFIX_TELE "tele"

#define CMND_POWER "power"
#define CMND_FORCE_OFF "force_off"
#define CMND_RESET "reset"
#define CMND_MAX_LEN 10

#define POWER_PIN D7
#define RESET_PIN D6

const char cDeviceName[] = "WakeOnEsp";
const char cInitialPwd[] = "wakeonesp";

DNSServer dnsServer;
WebServer webServer(80);
HTTPUpdateServer httpUpdater;
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

IotWebConf webConf(cDeviceName, &dnsServer, &webServer, cInitialPwd, IWC_CONFIG_VERSION);

char mqttServerValue[IWC_STRING_LEN];
char mqttPortValue[IWC_NUMBER_LEN];
char mqttUserNameValue[IWC_STRING_LEN];
char mqttUserPasswordValue[IWC_STRING_LEN];
char mqttTopicTemplateValue[IWC_STRING_LEN];

IotWebConfSeparator mqttSeparator = IotWebConfSeparator("MQTT");
IotWebConfParameter mqttServerParam = IotWebConfParameter(
  "Server", 
  "mqttServer", 
  mqttServerValue, 
  IWC_STRING_LEN);

IotWebConfParameter mqttPortParam = IotWebConfParameter(
  "Port", 
  "mqttPort", 
  mqttPortValue, 
  IWC_NUMBER_LEN,
  "number",
  "0..65535",
  "1883",
  "min='1' max='65535' step='1'");

IotWebConfParameter mqttUserNameParam = IotWebConfParameter(
  "User", 
  "mqttUser",
  mqttUserNameValue, 
  IWC_STRING_LEN);

IotWebConfParameter mqttUserPasswordParam = IotWebConfParameter(
  "Password", 
  "mqttPass", 
  mqttUserPasswordValue, 
  IWC_STRING_LEN, 
  "password");

IotWebConfParameter mqttTopicTemplateParam = IotWebConfParameter(
  "Topic Template", 
  "mqttTopicTpl", 
  mqttTopicTemplateValue, 
  IWC_STRING_LEN);

volatile bool needMqttConnect = false;
volatile bool needReset = false;
volatile uint8_t needAction = ACTION_NONE;

volatile bool mqttConnected = false;

unsigned long lastMqttConnectionAttempt = 0;
unsigned long lastActionAttempt = 0;

char mqttTopicCmnd[IWC_STRING_LEN * 2];
char mqttTopicTele[IWC_STRING_LEN * 2];
char mqttTopicLwt[IWC_STRING_LEN * 2];

void pinSetup();
void iwcSetup();
void webSetup();
void mqttSetup();

void handleRoot();
void handleWoE();

void onIwcConfigSaved();
void onIwcWifiConnected();
void onMqttConnected();

void mqttConnect(unsigned long now);
void mqttCallback(char* topic, byte* payload, unsigned int length);

void applyAction(uint8_t action, unsigned long now);
void togglePin(uint8_t pin, unsigned long ms);

void setup() {
  LOG_BEGIN(9600);
  while (!Serial);

  pinMode(LED_BUILTIN, OUTPUT);

  LOGLN("Setting up WoE...");

  pinSetup();
  iwcSetup();
  webSetup();
  mqttSetup();

  LOGLN("Setup done");
}

void pinSetup(){
    LOG("  Setup pins...");

    pinMode(LED_BUILTIN, OUTPUT);

    pinMode(POWER_PIN, OUTPUT);
    digitalWrite(POWER_PIN, LOW);

    pinMode(RESET_PIN, OUTPUT);
    digitalWrite(RESET_PIN, LOW);

    LOGLN("done");
}

void iwcSetup(){
  LOG("  Setup IotWebConfig...");

  webConf.setStatusPin(IWC_STATUS_PIN);
  webConf.addParameter(&mqttSeparator);
  webConf.addParameter(&mqttServerParam);
  webConf.addParameter(&mqttPortParam);
  webConf.addParameter(&mqttUserNameParam);
  webConf.addParameter(&mqttUserPasswordParam);
  webConf.addParameter(&mqttTopicTemplateParam);
  webConf.setConfigSavedCallback(&onIwcConfigSaved);
  webConf.setWifiConnectionCallback(&onIwcWifiConnected);
  webConf.setupUpdateServer(&httpUpdater);
  webConf.getApTimeoutParameter()->visible = true;

  boolean validConfig = webConf.init();
  if (!validConfig)
  {
    mqttServerValue[0] = '\0';
    mqttPortValue[0] = '\0';
    mqttUserNameValue[0] = '\0';
    mqttUserPasswordValue[0] = '\0';
    strcpy(mqttTopicTemplateValue, "woe/%name%/%prefix%");
  }

  LOGLN("done");
}

void webSetup() {
  LOG("  Setup webserver...");

  webServer.on("/", handleRoot);
  webServer.on("/woe", handleWoE);
  webServer.on("/config", []{ webConf.handleConfig(); });
  webServer.onNotFound([](){webConf.handleNotFound();});

  LOGLN("done");
}

void mqttSetup() {
  LOG("  Setup MQTT...");

  mqttClient.setCallback(&mqttCallback);

  String cmdTopicStr = String(mqttTopicTemplateValue);
  cmdTopicStr.replace("%name%", webConf.getThingName());
  cmdTopicStr.replace("%prefix%", MQTT_PREFIX_CMND);
  strcpy(mqttTopicCmnd, cmdTopicStr.c_str());
  
  String teleTopicStr = String(mqttTopicTemplateValue);
  teleTopicStr.replace("%name%", webConf.getThingName());
  teleTopicStr.replace("%prefix%", MQTT_PREFIX_TELE);
  strcpy(mqttTopicTele, teleTopicStr.c_str());

  String lwtTopicStr = String(mqttTopicTele);
  lwtTopicStr += "/state";
  strcpy(mqttTopicLwt, lwtTopicStr.c_str());

  LOGLN("done");
}

void loop() {
  webConf.doLoop();

  if(mqttClient.connected()){
    mqttClient.loop();
  }

  if (needReset){
    Serial.println("Rebooting after 1 second.");
    webConf.delay(1000);
    ESP.restart();
  }

  if(needMqttConnect){
    mqttConnect(millis());
  }

  if(ACTION_NONE != needAction){
    applyAction(needAction, millis());
  }
}

void handleRoot(){
  if (webConf.handleCaptivePortal()){
    // -- Captive portal request were already served.
    return;
  }

  if (webServer.hasArg("action")){
    String action = webServer.arg("action");
    if (action.equals(CMND_POWER)){
      needAction = ACTION_POWER;
    } else if (action.equals(CMND_FORCE_OFF)){
      needAction = ACTION_FORCE_OFF;
    } else if (action.equals(CMND_RESET)){
      needAction = ACTION_RESET;
    }

    // Redirect to root to remove action parameter from URL.
    webServer.sendHeader("Location", String("/"), true);
    webServer.send ( 302, "text/plain", "");
    return;
  }

  String s = "<!DOCTYPE html><html lang=\"en\"><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1, user-scalable=no\"/>";
  s += "<title>IotWebConf 02 Status and Reset</title></head><body>";
  s += "<div>";
  s += "<button type='button' onclick=\"location.href='woe?action=power';\" >Power</button>";
  s += "<button type='button' onclick=\"location.href='woe?action=force_off';\" >Force OFF</button>";
  s += "<button type='button' onclick=\"location.href='woe?action=reset';\" >Reset</button>";
  s += "</div>";
  s += "<div style=\"margin-top: 1em;\">Go to <a href='config'>configure page</a> to change settings.</div>";
  s += "</body></html>\n";

  webServer.send(200, "text/html", s);
}

void handleWoE(){
  if (webServer.hasArg("action")){
    String action = webServer.arg("action");
    if (action.equals("power")){
      needAction = ACTION_POWER;
    } else if (action.equals("force")){
      needAction = ACTION_FORCE_OFF;
    } else if (action.equals("reset")){
      needAction = ACTION_RESET;
    }
  }

  // Redirect to root to remove action parameter from URL.
  webServer.sendHeader("Location", String("/"), true);
  webServer.send ( 302, "text/plain", "");
}

void onIwcConfigSaved() {
  LOGLN("onIwcConfigSaved");
  needReset = true;
}

void onIwcWifiConnected(){
  LOGLN("onIwcWifiConnected");
  needMqttConnect = true;
}

void applyAction(uint8_t action, unsigned long now){
  if (ACTION_FEQ_LIMIT < (now - lastActionAttempt)){
    if(ACTION_POWER == action){
      LOGLN("Action - Power On/Off");
      togglePin(POWER_PIN, 200);
    } else if(ACTION_FORCE_OFF == action){
      LOGLN("Action - Force Off");
      togglePin(POWER_PIN, 5000);
    } else if(ACTION_RESET == action){
      LOGLN("Action - Reset");
      togglePin(RESET_PIN, 200);
    }
    lastActionAttempt = now;
  }

  needAction = ACTION_NONE;
}

void togglePin(uint8_t pin, unsigned long ms){
    LOGF("Toggle pin %d on for %lu ms\n", pin, ms);
    digitalWrite(pin, HIGH);
    digitalWrite(LED_BUILTIN, LOW);
    webConf.delay(ms);
    digitalWrite(pin, LOW);
    digitalWrite(LED_BUILTIN, HIGH);
    LOGF("Toggle pin %d off\n", pin);
}

void mqttConnect(unsigned long now){
  if(MQTT_FEQ_LIMIT < (now - lastMqttConnectionAttempt)){
    uint16_t mqttPort = atoi(mqttPortValue);
    mqttClient.setServer(mqttServerValue, mqttPort);

    LOGF("Try to connect to MQTT %s:%d\n", mqttServerValue, mqttPort);
    if(mqttClient.connect(
        webConf.getThingName(), 
        mqttUserNameValue, 
        mqttUserPasswordValue,
        mqttTopicLwt,
        0, 
        true,
        "Offline")){
          onMqttConnected();
          lastMqttConnectionAttempt = 0;
    } else {
        LOGF("Failed to connect to MQTT - state=%d\n", mqttClient.state());
        lastMqttConnectionAttempt = now;
    }
  }
}

void onMqttConnected(){
  LOGLN("Connected to MQTT server");
  needMqttConnect = false;
  mqttConnected = true;

  if(mqttClient.subscribe(mqttTopicCmnd)){
    LOGLN("Connected to topic");
  } else {
    LOGLN("Failed to connect to topic");
  }

  mqttClient.publish(mqttTopicLwt, "Online", true);
}

#define MIN(a,b) (((a)<(b))?(a):(b))

void mqttCallback(char* topic, byte* payload, unsigned int length){
  if(0 == strcmp(mqttTopicCmnd, topic)){
    char cmnd[CMND_MAX_LEN + 1];
    size_t len = MIN(CMND_MAX_LEN, length);
    memcpy(cmnd, payload, len );
    cmnd[len] = '\0';

    if(0 == strcmp(CMND_POWER, cmnd)){
      needAction = ACTION_POWER;
    } else if(0 == strcmp(CMND_FORCE_OFF,cmnd)){
      needAction = ACTION_FORCE_OFF;
    } else if(0 == strcmp(CMND_RESET, cmnd)){
      needAction = ACTION_RESET;
    } else {
      LOGF("Received unknown command %s\n", cmnd);
    }
  }
}