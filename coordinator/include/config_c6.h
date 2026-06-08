#pragma once

// ============================================================
// ESP32-C6 Zigbee Coordinator — Hardware and parameter config
// ============================================================
// Edit this file to adapt the coordinator to your setup.

// --- UART to host MCU (e.g. ESP32-S3) ---
// Physical wiring:
//   C6 GPIO2  (RX) ← Host UART TX
//   C6 GPIO21 (TX) → Host UART RX
//   Common GND required. Both sides are 3.3V — no level shifter needed.
#define S3_UART_NUM       1        // UART1 on C6
#define S3_UART_RX_PIN    2        // C6 GPIO2
#define S3_UART_TX_PIN    21       // C6 GPIO21
#define S3_UART_BAUD      115200
#define S3_UART_BUF       256

// --- Sensor identity (must match SENSOR_ID in your host config) ---
// These values appear in every JSON frame sent to the host.
#define SENSOR_ID         "zigbee_sensor_01"
#define SENSOR_NAME       "Zigbee Sensor"

// --- Heartbeat to host ---
// How often to send a status frame even if nothing changed.
#define HEARTBEAT_MS      10000UL  // every 10 s

// --- Zigbee ---
// Channel must match your ZHA/Z2M coordinator channel if migrating devices.
#define ZIGBEE_CHANNEL    13       // ZHA default channel
#define EP_CIE            1        // CIE endpoint on the coordinator

// IAS Zone type reported to the sensor.
// Common values:
//   0x002D = Water/Flood sensor (Sonoff SNZB-05P)
//   0x0028 = Motion sensor
//   0x0015 = Door/Window contact
#define IAS_ZONE_TYPE     0x002D

// --- Boot button (open pairing network) ---
// GPIO9 = BOOT button on ESP32-C6-DevKitC-1 and XIAO ESP32-C6
#define BOOT_BUTTON_PIN         9
#define OPEN_NETWORK_SECS       60   // Manual pairing window (boot button press)
#define BOOT_NETWORK_SECS      900   // Window at boot: 15 min for end-device re-join after deep backoff

// Minimum pause between automatic steering re-opens.
// The network stays open (windows of BOOT_NETWORK_SECS) indefinitely
// until the sensor announces itself or sends a zone notification.
#define REJOIN_REOPEN_PAUSE_MS  (10UL * 1000UL)  // 10 s pause between windows
