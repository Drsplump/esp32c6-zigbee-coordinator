#include <Arduino.h>
#include "uart_c6_bridge.h"

static uint32_t sLastStats = 0;

void onSensor(const UartC6SensorFrame& f) {
    Serial.printf("[SENSOR] id=%s type=%s alarm=%d tamper=%d battery=%d%% lqi=%d\n",
                  f.id, f.name, f.alarm, f.tamper, f.battery, f.linkquality);
}

void onEvent(const UartC6EventFrame& f) {
    Serial.printf("[EVENT] type=%d param=%lu extra=%s\n",
                  (int)f.event, f.param, f.extra ? f.extra : "");
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("[S3] Host bridge avviato");
    Serial.printf("[S3] UART%d  RX=GPIO%d  TX=GPIO%d  baud=%d\n",
                  UART_C6_INDEX, UART_C6_RX_PIN, UART_C6_TX_PIN, UART_C6_BAUD);
    uartC6BridgeInit(onSensor, onEvent);
    Serial.println("[S3] Bridge inizializzato, in attesa del C6...");
}

void loop() {
    uartC6BridgePoll();

    // Stampa statistiche ogni 15 secondi
    uint32_t now = millis();
    if (now - sLastStats >= 15000) {
        sLastStats = now;
        UartC6Stats s = uartC6BridgeGetStats();
        uint32_t age = uartC6BridgeLastFrameAgeMs();
        Serial.printf("[S3] stats: rx=%lu fail=%lu  ultimo_frame=%s\n",
                      s.framesReceived, s.parseFailures,
                      age == UINT32_MAX ? "mai" : String(age / 1000).c_str());
    }
}
