#pragma once

// Simple debug macros — output on Serial (USB CDC).
// Define DEBUG_DISABLED before including this file to silence all output.

#ifndef DEBUG_DISABLED
  #define DBG_PRINTF(fmt, ...)  Serial.printf(fmt, ##__VA_ARGS__)
  #define DBG_PRINTLN(msg)      Serial.println(msg)
  #define DBG_PRINT(msg)        Serial.print(msg)
#else
  #define DBG_PRINTF(fmt, ...)  do {} while(0)
  #define DBG_PRINTLN(msg)      do {} while(0)
  #define DBG_PRINT(msg)        do {} while(0)
#endif
