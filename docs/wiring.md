# Wiring — ESP32-C6 ↔ Host MCU (ESP32-S3 example)

## UART connections

| Host MCU (e.g. ESP32-S3) | Direction | ESP32-C6 (Coordinator) |
|--------------------------|-----------|------------------------|
| GPIO18 — UART1 TX        | →         | GPIO2 — UART1 RX       |
| GPIO17 — UART1 RX        | ←         | GPIO21 — UART1 TX      |
| 3.3V                     | →         | 3V3                    |
| GND                      | ↔         | GND                    |

> **Power**: the XIAO ESP32-C6 can be powered from the host 3.3V rail.
> Verify the 3.3V regulator on your board can supply the extra load
> (~80–120 mA typical with Zigbee active, up to ~200 mA during TX bursts).
> Add a **100 µF capacitor** on the 3V3/GND pins of the C6 to absorb
> Zigbee TX current spikes and prevent brownout resets.
> Both sides run at 3.3V — no level shifter required.

## UART parameters

| Parameter  | Value  |
|------------|--------|
| Baud rate  | 115200 |
| Data bits  | 8      |
| Parity     | None   |
| Stop bits  | 1      |

## Zigbee pairing procedure (IAS Zone sensor, e.g. Sonoff SNZB-05P)

1. Flash coordinator firmware: `pio run -e coordinator --target upload`
2. Open pairing window: press **BOOT button** (GPIO9) on the C6 → LED blinks, network open for 60 s
3. Put sensor into pairing mode: hold its button for ~5 s until LED blinks rapidly
4. Sensor joins automatically; C6 enrolls it (IAS Zone enrollment)
5. C6 starts sending JSON frames to host every 10 s + on every state change

## JSON frame formats

### C6 → Host (sensor data)
```json
{"id":"zigbee_sensor_01","name":"Zigbee Sensor","alarm":0,"control":0,
 "joined":1,"sleep":0,"battery":85,"linkquality":120,"seq":42}
```

### Host → C6 (ACK)
```json
{"ack":1,"seq":42,"status":"ok","queued":1}
```

### C6 → Host (events)
```json
{"event":"network_open","param":60,"seq":5}
{"event":"network_closed","seq":6}
{"event":"device_joined","short_addr":"0x1A2B","seq":7}
{"event":"c6_reset","reason":"POWERON","code":1,"seq":1}
{"event":"c6_heartbeat","seq":10}
```

### Host → C6 (commands)
```json
{"cmd":"open_network","duration":60}
{"cmd":"close_network"}
```

## NVRAM erase (full re-pair)

Hold the **BOOT button** while applying power to the C6.
The coordinator will erase its Zigbee NVRAM on startup, allowing the sensor to pair as if new.
