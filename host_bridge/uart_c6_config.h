#pragma once

// ============================================================
// Host-side UART bridge config — adapt to your MCU and wiring
// ============================================================

// UART hardware index (1 or 2 on most ESP32 variants; 0 is usually debug/USB).
#define UART_C6_INDEX       1

// Physical pins on the host MCU.
//   Host UART TX (GPIO18) → C6 D2/GPIO2  (RX)
//   Host UART RX (GPIO17) ← C6 D3/GPIO21 (TX)
#define UART_C6_TX_PIN      18
#define UART_C6_RX_PIN      17

// Baud rate — must match S3_UART_BAUD in coordinator/include/config_c6.h
#define UART_C6_BAUD        115200

// Ring-buffer size (bytes). Max expected frame < 300 bytes.
#define UART_C6_BUFFER_SIZE 512

// Set to 1 to send a JSON ACK back to the C6 after each valid data frame.
// Set to 0 for unidirectional (fire-and-forget) operation.
#define UART_C6_ACK_ENABLED 1

// Default sensor ID / name used when the incoming frame omits the "id" / "name" fields.
#define UART_C6_DEFAULT_SENSOR_ID   "zigbee_sensor_01"
#define UART_C6_DEFAULT_SENSOR_NAME "Zigbee Sensor"

// If no frame arrives within this period the host considers the C6 offline.
#define UART_C6_OFFLINE_TIMEOUT_MS  60000
