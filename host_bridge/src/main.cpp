#include <Arduino.h>
#include "uart_c6_bridge.h"

void onSensor(const UartC6SensorFrame& f) {
    Serial.printf("[SENSOR] id=%s alarm=%d battery=%d%% lqi=%d\n",
                  f.id, f.alarm, f.battery, f.linkquality);
}

void onEvent(const UartC6EventFrame& f) {
    Serial.printf("[EVENT] type=%d param=%lu extra=%s\n",
                  (int)f.event, f.param, f.extra ? f.extra : "");
}

void setup() {
    Serial.begin(115200);
    uartC6BridgeInit(onSensor, onEvent);
}

void loop() {
    uartC6BridgePoll();
}
