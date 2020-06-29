## WakeOnEsp
Little ESP8266 powerd device to start, stop and reset a PC via the button pins on the main board.

This project is inspired by an article from heise.de -> https://heise.de/-4417866
It uses the same hardware layout with optocouplers but I wrote my own firmware that is configurable and controllable by a web interface ([IotWebConf](https://github.com/prampec/IotWebConf)) and via MQTT ([PubSubClient](https://github.com/knolleary/pubsubclient)).