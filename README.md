# ESP32-C6 Zigbee Coordinator

🇬🇧 English | [🇮🇹 Italiano](README_IT.md)

Generic Zigbee IAS Zone coordinator for the ESP32-C6 with a UART JSON bridge to any host MCU.

Flash the `coordinator` firmware on an ESP32-C6, connect the two boards via UART, flash the `host_bridge` firmware on an ESP32-S3 (or any Arduino-compatible MCU), and you have a ready-to-use Zigbee sensor bridge with less than 10 lines of integration code.

---

## What it does

```
IAS Zone Sensor ─[Zigbee]─► ESP32-C6 ─[UART JSON]─► ESP32-S3 / Host MCU
                             coordinator              your application
```

- **Coordinator** (ESP32-C6): acts as a Zigbee coordinator, auto-detects the IAS Zone type, and sends newline-delimited JSON frames to the host on every state change and every 10 seconds.
- **Host bridge** (ESP32-S3 or any Arduino-compatible MCU): receives frames, parses them, and calls your application callbacks — no Zigbee stack knowledge required.

Compatible with any Zigbee IAS Zone sensor (flood, motion, door/window contact, …).
Tested with the Sonoff SNZB-05P flood sensor.

---

## Repository structure

```
esp32c6-zigbee-coordinator/
├── platformio.ini            ← single PlatformIO file with both envs
├── coordinator/
│   ├── src/main.cpp          ESP32-C6 coordinator firmware
│   ├── include/
│   │   ├── config_c6.h       ← edit here: UART pins, Zigbee channel, timing
│   │   └── debug.h
│   └── link_zigbee.py        required build script for pioarduino 53.x
├── host_bridge/
│   ├── platformio.ini        standalone PlatformIO project for ESP32-S3
│   ├── src/main.cpp          integration example (put your app logic here)
│   ├── uart_c6_config.h      ← edit here: host UART pins and parameters
│   ├── uart_c6_bridge.h      public API
│   └── uart_c6_bridge.cpp    bridge implementation (no external dependencies)
└── docs/
    └── wiring.md             wiring diagram, JSON protocol, pairing procedure
```

---

## Hardware requirements

| Component           | Notes                                                            |
|---------------------|------------------------------------------------------------------|
| ESP32-C6-DevKitC-1  | Zigbee coordinator                                               |
| ESP32-S3-DevKitC-1  | Host MCU (or any Arduino board with a free hardware UART)        |
| Zigbee IAS sensor   | Sonoff SNZB-05P or any IAS Zone-compatible device                |
| 100 µF capacitor    | Recommended on C6 3V3/GND to absorb Zigbee TX current spikes    |

---

## Wiring

| ESP32-S3 (host)    | Direction | ESP32-C6 (coordinator)   |
|--------------------|-----------|--------------------------|
| GPIO18 — UART1 TX  | →         | D2 / GPIO2 — UART1 RX    |
| GPIO17 — UART1 RX  | ←         | D3 / GPIO21 — UART1 TX   |
| GND                | ↔         | GND                      |

> Both boards operate at 3.3V — no level shifter required.  
> Pins can be changed in `coordinator/include/config_c6.h` (C6 side) and `host_bridge/uart_c6_config.h` (host side).

---

## Quick start

### 1. Open the project

Open `esp32c6-zigbee-coordinator.code-workspace` in VSCode. The PlatformIO status bar shows two environments: **coordinator** and **host_bridge**.

### 2. Flash the coordinator (ESP32-C6)

Select the `coordinator` env and click Upload, or from the project root:

```bash
pio run -e coordinator --target upload
```

Edit [`coordinator/include/config_c6.h`](coordinator/include/config_c6.h) to change pins or Zigbee settings.

### 3. Flash the host bridge (ESP32-S3)

Select the `host_bridge` env and click Upload, or:

```bash
pio run -e host_bridge --target upload
```

Edit [`host_bridge/uart_c6_config.h`](host_bridge/uart_c6_config.h) for your board's UART pins.  
Put your application logic in [`host_bridge/src/main.cpp`](host_bridge/src/main.cpp):

```cpp
#include "uart_c6_bridge.h"

void onSensor(const UartC6SensorFrame& f) {
    Serial.printf("[Sensor] id=%s type=%s alarm=%d battery=%d%% lqi=%d\n",
                  f.id, f.name, f.alarm, f.battery, f.linkquality);
    if (f.alarm) { /* handle alarm */ }
}

void onEvent(const UartC6EventFrame& f) {
    if (f.event == UartC6Event::NetworkOpen)
        Serial.printf("[Zigbee] Network open for %lus\n", (unsigned long)f.param);
}

void setup() { Serial.begin(115200); uartC6BridgeInit(onSensor, onEvent); }
void loop()  { uartC6BridgePoll(); }
```

### 4. Pair a sensor

1. Press **BOOT** on the ESP32-C6 → pairing window opens for 60 seconds
2. Put sensor into pairing mode (usually hold its button ~5 s)
3. Sensor joins and is enrolled automatically
4. JSON frames start arriving on the host within seconds

For a **full re-pair** (erase Zigbee NVRAM): hold BOOT while powering up the C6.

---

## Host bridge API

```cpp
void uartC6BridgeInit(UartC6SensorCallback onSensor, UartC6EventCallback onEvent);
void uartC6BridgePoll();
void uartC6OpenNetwork(uint8_t durationSec = 60);
void uartC6CloseNetwork();
UartC6Stats  uartC6BridgeGetStats();
uint32_t     uartC6BridgeLastFrameAgeMs();
```

See [`host_bridge/uart_c6_bridge.h`](host_bridge/uart_c6_bridge.h) for full struct definitions.

---

## JSON protocol

See [docs/wiring.md](docs/wiring.md) for the complete frame format reference.

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
