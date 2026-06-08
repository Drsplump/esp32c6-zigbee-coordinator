/**
 * uart_c6_bridge.cpp  —  Generic host-side UART bridge for ESP32-C6 Zigbee Coordinator
 *
 * No application-specific dependencies.  All state is forwarded to the
 * UartC6SensorCallback / UartC6EventCallback registered in uartC6BridgeInit().
 *
 * Required libraries: ArduinoJson >= 7.0  (bblanchon/ArduinoJson)
 * Target framework:   Arduino (ESP32 arduino-esp32 >= 3.x recommended)
 */

#include "uart_c6_bridge.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HardwareSerial.h>

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

static HardwareSerial       sUart(UART_C6_INDEX);
static UartC6SensorCallback sSensorCb  = nullptr;
static UartC6EventCallback  sEventCb   = nullptr;
static UartC6Stats          sStats     = {};

static char sRingBuf[UART_C6_BUFFER_SIZE];
static int  sRingPos = 0;

// ---------------------------------------------------------------------------
// ACK helper
// ---------------------------------------------------------------------------

static void sendAck(uint32_t seq, bool ok, const char* reason, bool queued) {
#if UART_C6_ACK_ENABLED
    char ack[96];
    if (ok) {
        snprintf(ack, sizeof(ack),
                 "{\"ack\":1,\"seq\":%lu,\"status\":\"ok\",\"queued\":%d}\n",
                 (unsigned long)seq, queued ? 1 : 0);
    } else {
        snprintf(ack, sizeof(ack),
                 "{\"ack\":1,\"seq\":%lu,\"status\":\"error\",\"reason\":\"%s\"}\n",
                 (unsigned long)seq, reason ? reason : "unknown");
    }
    size_t written = sUart.write(reinterpret_cast<const uint8_t*>(ack), strlen(ack));
    if (written > 0) sStats.ackSent++;
    else             sStats.ackFailed++;
#else
    (void)seq; (void)ok; (void)reason; (void)queued;
#endif
}

// ---------------------------------------------------------------------------
// JSON bool helper (handles bool, int, and string representations)
// ---------------------------------------------------------------------------

static bool parseBool(JsonVariantConst v, bool fallback) {
    if (v.is<bool>())        return v.as<bool>();
    if (v.is<int>())         return v.as<int>() != 0;
    if (v.is<const char*>()) {
        const char* s = v.as<const char*>();
        if (strcmp(s, "true") == 0 || strcmp(s, "1") == 0 || strcmp(s, "on") == 0)  return true;
        if (strcmp(s, "false") == 0 || strcmp(s, "0") == 0 || strcmp(s, "off") == 0) return false;
    }
    return fallback;
}

// ---------------------------------------------------------------------------
// Frame processor
// ---------------------------------------------------------------------------

static void processFrame(const char* json) {
    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok) {
        sStats.parseFailures++;
        sendAck(0, false, "parse_fail", false);
        return;
    }

    sStats.framesReceived++;
    sStats.lastFrameMs = millis();

    // --- Event frames ---
    const char* eventKey = doc["event"] | "";
    if (eventKey[0] != '\0') {
        if (sEventCb) {
            UartC6EventFrame ef;
            ef.param = 0;
            ef.extra = "";

            if (strcmp(eventKey, "network_open") == 0) {
                ef.event = UartC6Event::NetworkOpen;
                ef.param = doc["param"] | 60;
            } else if (strcmp(eventKey, "network_closed") == 0) {
                ef.event = UartC6Event::NetworkClosed;
            } else if (strcmp(eventKey, "device_joined") == 0) {
                ef.event = UartC6Event::DeviceJoined;
                ef.extra = doc["short_addr"] | "";
            } else if (strcmp(eventKey, "c6_reset") == 0) {
                ef.event = UartC6Event::C6Reset;
                ef.param = (uint32_t)(doc["code"] | -1);
                ef.extra = doc["reason"] | "";
            } else if (strcmp(eventKey, "c6_heartbeat") == 0) {
                ef.event = UartC6Event::C6Heartbeat;
            } else {
                return;  // unknown event — ignore
            }
            sEventCb(ef);
        }
        return;
    }

    // --- Sensor data frames ---
    UartC6SensorFrame sf;
    sf.id          = doc["id"]   | UART_C6_DEFAULT_SENSOR_ID;
    sf.name        = doc["name"] | UART_C6_DEFAULT_SENSOR_NAME;
    sf.alarm       = parseBool(doc["alarm"],   false);
    sf.control     = parseBool(doc["control"], false);
    sf.joined      = parseBool(doc["joined"],  true);
    sf.sleep       = parseBool(doc["sleep"],   false);
    sf.battery     = doc["battery"]     | -1;
    sf.linkquality = doc["linkquality"] | -1;
    sf.tamper      = parseBool(doc["tamper"], false);
    sf.seq         = doc["seq"] | 0;

    if (sSensorCb) sSensorCb(sf);

    sendAck(sf.seq, true, nullptr, true);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void uartC6BridgeInit(UartC6SensorCallback onSensor, UartC6EventCallback onEvent) {
    sSensorCb = onSensor;
    sEventCb  = onEvent;
    memset(&sStats, 0, sizeof(sStats));
    sRingPos = 0;

    sUart.setRxBufferSize(UART_C6_BUFFER_SIZE);
    sUart.begin(UART_C6_BAUD, SERIAL_8N1, UART_C6_RX_PIN, UART_C6_TX_PIN);
    sStats.initialized = true;
}

void uartC6BridgePoll() {
    while (sUart.available() > 0) {
        int c = sUart.read();
        if (c < 0) break;

        if (c == '\n' || c == '\r') {
            if (sRingPos > 0) {
                sRingBuf[sRingPos] = '\0';
                if (sRingBuf[0] == '{') processFrame(sRingBuf);
                sRingPos = 0;
            }
        } else if (sRingPos < (UART_C6_BUFFER_SIZE - 1)) {
            sRingBuf[sRingPos++] = static_cast<char>(c);
        } else {
            sStats.overflowDrops++;
            sRingPos = 0;
            sendAck(0, false, "overflow", false);
        }
    }
}

void uartC6OpenNetwork(uint8_t durationSec) {
    char cmd[56];
    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"open_network\",\"duration\":%u}\n", durationSec);
    sUart.write(reinterpret_cast<const uint8_t*>(cmd), strlen(cmd));
}

void uartC6CloseNetwork() {
    static const char cmd[] = "{\"cmd\":\"close_network\"}\n";
    sUart.write(reinterpret_cast<const uint8_t*>(cmd), strlen(cmd));
}

UartC6Stats uartC6BridgeGetStats() {
    return sStats;
}

uint32_t uartC6BridgeLastFrameAgeMs() {
    if (sStats.lastFrameMs == 0) return UINT32_MAX;
    return millis() - sStats.lastFrameMs;
}
