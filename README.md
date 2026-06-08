# ESP32-C6 Zigbee Coordinator

Generic Zigbee IAS Zone coordinator for the ESP32-C6 with a UART JSON bridge to any host MCU.

Flash the `coordinator/` firmware on an ESP32-C6 board, connect it via UART to your host MCU (ESP32-S3, ESP32, Arduino, Raspberry Pi, …), add the two files from `host_bridge/` to your host project, and you have a ready-to-use Zigbee sensor bridge with less than 10 lines of integration code.

---

## What it does

```
IAS Zone Sensor ─[Zigbee]─► ESP32-C6 ─[UART JSON]─► Host MCU
                             coordinator              your application
```

- **Coordinator** (ESP32-C6): acts as a Zigbee coordinator, enrolls IAS Zone sensors, and forwards all sensor events and battery reports to the host as newline-delimited JSON frames over UART.
- **Host bridge** (any Arduino-compatible MCU): receives frames, parses them, and calls your application callbacks — no Zigbee stack knowledge required on the host side.

Compatible with any Zigbee IAS Zone sensor (flood, motion, door/window contact, …).
Tested with the Sonoff SNZB-05P flood sensor.

---

## Repository structure

```
esp32c6-zigbee-coordinator/
├── coordinator/              ESP32-C6 firmware (standalone PlatformIO project)
│   ├── platformio.ini
│   ├── link_zigbee.py        required build script for pioarduino 53.x
│   ├── src/
│   │   └── main.cpp
│   └── include/
│       ├── config_c6.h       ← edit this: pins, sensor ID, timing
│       └── debug.h           serial debug macros
├── host_bridge/              Two files to add to your host project
│   ├── uart_c6_config.h      ← edit this: host UART pins and parameters
│   ├── uart_c6_bridge.h      public API
│   └── uart_c6_bridge.cpp    implementation (no external dependencies)
└── docs/
    └── wiring.md             wiring diagram, JSON protocol, pairing procedure
```

---

## Hardware requirements

| Component          | Notes                                                  |
|--------------------|--------------------------------------------------------|
| ESP32-C6 board     | ESP32-C6-DevKitC-1 or Seeed XIAO ESP32-C6             |
| Host MCU           | Any Arduino-compatible board with a free hardware UART |
| Zigbee IAS sensor  | Sonoff SNZB-05P or any IAS Zone-compatible device      |
| 100 µF capacitor   | Recommended on C6 3V3/GND to absorb Zigbee TX spikes  |

---

## Quick start

### 1. Flash the coordinator

Edit [`coordinator/include/config_c6.h`](coordinator/include/config_c6.h) if you need to change:
- UART pins (`S3_UART_RX_PIN` / `S3_UART_TX_PIN`)
- Sensor identity (`SENSOR_ID` / `SENSOR_NAME`)
- Zigbee channel (`ZIGBEE_CHANNEL`)
- IAS zone type (`IAS_ZONE_TYPE`)

Then flash:
```bash
cd coordinator
pio run -e coordinator --target upload
```

### 2. Integrate the host bridge

Copy `host_bridge/uart_c6_config.h`, `uart_c6_bridge.h`, and `uart_c6_bridge.cpp` into your host project.

Edit `uart_c6_config.h` to set the correct UART pins for your host board:
```c
#define UART_C6_TX_PIN  18   // host TX → C6 RX
#define UART_C6_RX_PIN  17   // host RX ← C6 TX
```

Add the bridge to your sketch:
```cpp
#include "uart_c6_bridge.h"

void onSensor(const UartC6SensorFrame& f) {
    Serial.printf("[Sensor] id=%s alarm=%d battery=%d%% lq=%d joined=%d\n",
                  f.id, f.alarm, f.battery, f.linkquality, f.joined);

    if (f.alarm) {
        // your alarm handling here
    }
}

void onEvent(const UartC6EventFrame& f) {
    if (f.event == UartC6Event::NetworkOpen)
        Serial.printf("[Zigbee] Network open for %lus\n", (unsigned long)f.param);
    else if (f.event == UartC6Event::DeviceJoined)
        Serial.printf("[Zigbee] Device joined: %s\n", f.extra);
}

void setup() {
    Serial.begin(115200);
    uartC6BridgeInit(onSensor, onEvent);
}

void loop() {
    uartC6BridgePoll();   // non-blocking, call every loop tick
}
```

### 3. Pair a sensor

Press the **BOOT button** on the ESP32-C6 to open the Zigbee pairing window for 60 seconds, then put your sensor into pairing mode. See [docs/wiring.md](docs/wiring.md) for the full procedure.

---

## Host bridge API

```cpp
// Initialize UART and register callbacks (call once from setup)
void uartC6BridgeInit(UartC6SensorCallback onSensor, UartC6EventCallback onEvent);

// Non-blocking poll — call every loop tick
void uartC6BridgePoll();

// Send open/close network commands to the C6
void uartC6OpenNetwork(uint8_t durationSec = 60);
void uartC6CloseNetwork();

// Diagnostics
UartC6Stats      uartC6BridgeGetStats();
uint32_t         uartC6BridgeLastFrameAgeMs();
```

See [`host_bridge/uart_c6_bridge.h`](host_bridge/uart_c6_bridge.h) for the full struct definitions.

---

## JSON protocol

See [docs/wiring.md](docs/wiring.md) for the complete frame format reference and wiring diagram.

---

## Supported sensors

Any Zigbee IAS Zone sensor should work.  
Change `IAS_ZONE_TYPE` in `config_c6.h` to match your sensor:

| Zone type | Value  | Example                  |
|-----------|--------|--------------------------|
| Water     | 0x002D | Sonoff SNZB-05P          |
| Motion    | 0x0028 | Aqara, IKEA, Sonoff PIR  |
| Contact   | 0x0015 | Door/window contact      |

---

## Requirements

- **PlatformIO** with platform `pioarduino/platform-espressif32` ≥ 53.03.10  
  (standard `espressif32` does not expose the Zigbee API)
- **ArduinoJson** ≥ 7.0 (`bblanchon/ArduinoJson`)
- arduino-esp32 ≥ 3.x (included in the pioarduino platform)

---

## Origin

Extracted and generalized from [vergus-virgil](https://github.com/Drsplump/vergus-virgil),
a water-leak detection system for ESP32-S3 + ESP32-C6.
