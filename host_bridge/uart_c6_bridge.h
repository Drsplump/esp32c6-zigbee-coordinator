#pragma once

/**
 * uart_c6_bridge.h  —  Generic host-side UART bridge for ESP32-C6 Zigbee Coordinator
 *
 * Receives newline-delimited JSON frames from the C6 coordinator over UART and
 * forwards them to your application via two callbacks:
 *
 *   onSensorData  — called for every sensor data frame (alarm state, battery, …)
 *   onEvent       — called for coordinator events (network open/closed, device join, reset)
 *
 * Optionally sends an ACK back to the C6 after each valid data frame
 * (controlled by UART_C6_ACK_ENABLED in uart_c6_config.h).
 *
 * The bridge is intentionally thin: it does NO sensor registry, NO safety logic,
 * and NO application state.  All of that belongs in your application callbacks.
 *
 * --- Minimum integration ---
 *
 *   void mySensorCallback(const UartC6SensorFrame& f) {
 *       Serial.printf("alarm=%d battery=%d%%\n", f.alarm, f.battery);
 *   }
 *
 *   void setup() {
 *       uartC6BridgeInit(mySensorCallback, nullptr);
 *   }
 *
 *   void loop() {
 *       uartC6BridgePoll();   // call every loop tick (non-blocking)
 *   }
 */

#include <stdint.h>
#include "uart_c6_config.h"

// ---------------------------------------------------------------------------
// Sensor data frame (populated for every non-event JSON frame from the C6)
// ---------------------------------------------------------------------------
struct UartC6SensorFrame {
    const char* id;           // sensor id string (points into internal buffer — copy if needed)
    const char* name;         // sensor name string
    bool        alarm;        // true = alarm/flood active
    bool        control;      // true = host should act (alarm || explicit control)
    bool        joined;       // true = sensor associated with the coordinator
    bool        sleep;        // true = sensor online but in deep sleep (no recent activity)
    int         battery;      // % remaining, -1 if unknown
    int         linkquality;  // Zigbee LQI (0–255), -1 if unknown
    bool        tamper;       // true = tamper alarm
    uint32_t    seq;          // frame sequence number from the C6
};

// ---------------------------------------------------------------------------
// Event frame (populated for coordinator lifecycle events)
// ---------------------------------------------------------------------------
enum class UartC6Event : uint8_t {
    NetworkOpen,    // Zigbee pairing window opened  (param = duration in seconds)
    NetworkClosed,  // Zigbee pairing window closed  (param = 0)
    DeviceJoined,   // A device announced itself     (extra = short addr string e.g. "0x1A2B")
    C6Reset,        // C6 rebooted                   (param = reset reason code, extra = reason label)
    C6Heartbeat,    // Bridge heartbeat (no sensor data)
};

struct UartC6EventFrame {
    UartC6Event event;
    uint32_t    param;        // event-specific numeric parameter (see UartC6Event docs)
    const char* extra;        // event-specific string parameter  (points into internal buffer)
};

// ---------------------------------------------------------------------------
// Callback types
// ---------------------------------------------------------------------------

// Called for every valid sensor data frame.
// The frame fields are valid only for the duration of the callback — copy what you need.
typedef void (*UartC6SensorCallback)(const UartC6SensorFrame& frame);

// Called for every coordinator event frame.
// Pass nullptr if you don't need event notifications.
typedef void (*UartC6EventCallback)(const UartC6EventFrame& frame);

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Initialize the UART hardware and register callbacks.
// Call once from setup().
void uartC6BridgeInit(UartC6SensorCallback onSensor, UartC6EventCallback onEvent);

// Non-blocking poll: reads available bytes, decodes complete frames, fires callbacks.
// Call every loop tick (or every network/IO task tick).
void uartC6BridgePoll();

// Send {"cmd":"open_network","duration":N} to the C6.
void uartC6OpenNetwork(uint8_t durationSec = 60);

// Send {"cmd":"close_network"} to the C6.
void uartC6CloseNetwork();

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------
struct UartC6Stats {
    uint32_t framesReceived;  // complete JSON frames received
    uint32_t parseFailures;   // frames discarded due to JSON errors
    uint32_t overflowDrops;   // frames discarded due to ring-buffer overflow
    uint32_t ackSent;         // ACKs sent to the C6
    uint32_t ackFailed;       // ACKs that failed to write
    uint32_t lastFrameMs;     // millis() of last valid frame (0 = never)
    bool     initialized;     // true after uartC6BridgeInit()
};

UartC6Stats uartC6BridgeGetStats();

// millis() elapsed since last valid frame; returns UINT32_MAX if no frame ever received.
uint32_t uartC6BridgeLastFrameAgeMs();
