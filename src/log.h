#ifndef LOG_H
#define LOG_H

#include <Arduino.h>

#define LOG_ENABLED 1
#if LOG_ENABLED
  #define LOG_BEGIN(speed) Serial.begin(speed)
  #define LOG(text) Serial.print(text)
  #define LOGLN(text) Serial.println(text)
  #define LOGF(pattern, ...) Serial.printf(pattern, __VA_ARGS__)
#else
  #define LOG_BEGIN(speed)
  #define LOG(text)
  #define LOGLN(text)
  #define LOGF(pattern, ...)
#endif

#endif