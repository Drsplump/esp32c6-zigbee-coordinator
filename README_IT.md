# ESP32-C6 Coordinatore Zigbee

[🇬🇧 English](README.md) | 🇮🇹 Italiano

Coordinatore Zigbee IAS Zone generico per ESP32-C6 con bridge UART JSON verso qualsiasi MCU host.

Flasha il firmware in `coordinator/` su una scheda ESP32-C6, collegala via UART al tuo MCU host (ESP32-S3, ESP32, Arduino, Raspberry Pi, …), aggiungi i due file di `host_bridge/` al tuo progetto host, e hai un bridge Zigbee funzionante con meno di 10 righe di codice di integrazione.

---

## Come funziona

```
Sensore IAS Zone ─[Zigbee]─► ESP32-C6 ─[UART JSON]─► MCU Host
                              coordinatore              la tua applicazione
```

- **Coordinatore** (ESP32-C6): agisce come coordinatore Zigbee, esegue l'enrollment dei sensori IAS Zone e inoltra tutti gli eventi e i report batteria all'host come frame JSON delimitati da newline su UART.
- **Bridge host** (qualsiasi MCU Arduino-compatibile): riceve i frame, li analizza e chiama i callback della tua applicazione — nessuna conoscenza dello stack Zigbee richiesta lato host.

Compatibile con qualsiasi sensore Zigbee IAS Zone (allagamento, movimento, contatto porta/finestra, …).
Testato con il sensore di allagamento Sonoff SNZB-05P.

---

## Struttura del repository

```
esp32c6-zigbee-coordinator/
├── coordinator/              Firmware ESP32-C6 (progetto PlatformIO autonomo)
│   ├── platformio.ini
│   ├── link_zigbee.py        script di build necessario per pioarduino 53.x
│   ├── src/
│   │   └── main.cpp
│   └── include/
│       ├── config_c6.h       ← modifica qui: pin, ID sensore, timing
│       └── debug.h           macro di debug seriale
├── host_bridge/              Due file da aggiungere al tuo progetto host
│   ├── uart_c6_config.h      ← modifica qui: pin UART host e parametri
│   ├── uart_c6_bridge.h      API pubblica
│   └── uart_c6_bridge.cpp    implementazione (senza dipendenze esterne)
└── docs/
    └── wiring.md             schema di collegamento, protocollo JSON, procedura di pairing
```

---

## Hardware necessario

| Componente         | Note                                                        |
|--------------------|-------------------------------------------------------------|
| Scheda ESP32-C6    | ESP32-C6-DevKitC-1 oppure Seeed XIAO ESP32-C6              |
| MCU Host           | Qualsiasi scheda Arduino-compatibile con una UART hardware  |
| Sensore Zigbee IAS | Sonoff SNZB-05P o qualsiasi dispositivo IAS Zone            |
| Condensatore 100 µF | Consigliato su 3V3/GND del C6 per assorbire i picchi Zigbee TX |

---

## Avvio rapido

### 1. Flasha il coordinatore

Modifica [`coordinator/include/config_c6.h`](coordinator/include/config_c6.h) se vuoi cambiare:
- Pin UART (`S3_UART_RX_PIN` / `S3_UART_TX_PIN`)
- Identità del sensore (`SENSOR_ID` / `SENSOR_NAME`)
- Canale Zigbee (`ZIGBEE_CHANNEL`)
- Tipo di zona IAS (`IAS_ZONE_TYPE`)

Poi flasha:
```bash
cd coordinator
pio run -e coordinator --target upload
```

### 2. Integra il bridge host

Copia `uart_c6_config.h`, `uart_c6_bridge.h` e `uart_c6_bridge.cpp` nel tuo progetto host.

Modifica `uart_c6_config.h` per impostare i pin UART corretti della tua scheda:
```c
#define UART_C6_TX_PIN  18   // TX host → RX C6
#define UART_C6_RX_PIN  17   // RX host ← TX C6
```

Aggiungi il bridge al tuo sketch:
```cpp
#include "uart_c6_bridge.h"

void suSensore(const UartC6SensorFrame& f) {
    Serial.printf("[Sensore] id=%s allarme=%d batteria=%d%% lq=%d joined=%d\n",
                  f.id, f.alarm, f.battery, f.linkquality, f.joined);

    if (f.alarm) {
        // gestisci l'allarme qui
    }
}

void suEvento(const UartC6EventFrame& f) {
    if (f.event == UartC6Event::NetworkOpen)
        Serial.printf("[Zigbee] Rete aperta per %lus\n", (unsigned long)f.param);
    else if (f.event == UartC6Event::DeviceJoined)
        Serial.printf("[Zigbee] Dispositivo unito: %s\n", f.extra);
}

void setup() {
    Serial.begin(115200);
    uartC6BridgeInit(suSensore, suEvento);
}

void loop() {
    uartC6BridgePoll();   // non bloccante, chiama ad ogni ciclo
}
```

### 3. Associa un sensore

Premi il **pulsante BOOT** sull'ESP32-C6 per aprire la finestra di pairing Zigbee per 60 secondi, poi metti il sensore in modalità pairing. Vedi [docs/wiring.md](docs/wiring.md) per la procedura completa.

---

## API bridge host

```cpp
// Inizializza UART e registra i callback (chiama una volta in setup)
void uartC6BridgeInit(UartC6SensorCallback onSensor, UartC6EventCallback onEvent);

// Poll non bloccante — chiama ad ogni ciclo
void uartC6BridgePoll();

// Invia comandi di apertura/chiusura rete al C6
void uartC6OpenNetwork(uint8_t durationSec = 60);
void uartC6CloseNetwork();

// Diagnostica
UartC6Stats  uartC6BridgeGetStats();
uint32_t     uartC6BridgeLastFrameAgeMs();
```

Vedi [`host_bridge/uart_c6_bridge.h`](host_bridge/uart_c6_bridge.h) per le definizioni complete delle struct.

---

## Protocollo JSON

Vedi [docs/wiring.md](docs/wiring.md) per il riferimento completo al formato dei frame e lo schema di collegamento.

---

## Sensori supportati

Qualsiasi sensore Zigbee IAS Zone dovrebbe funzionare.  
Cambia `IAS_ZONE_TYPE` in `config_c6.h` in base al tuo sensore:

| Tipo zona  | Valore | Esempio                       |
|------------|--------|-------------------------------|
| Acqua      | 0x002D | Sonoff SNZB-05P               |
| Movimento  | 0x0028 | Aqara, IKEA, Sonoff PIR       |
| Contatto   | 0x0015 | Contatto porta/finestra       |

---

## Requisiti

- **PlatformIO** con platform `pioarduino/platform-espressif32` ≥ 53.03.10  
  (il platform standard `espressif32` non espone le API Zigbee)
- **ArduinoJson** ≥ 7.0 (`bblanchon/ArduinoJson`)
- arduino-esp32 ≥ 3.x (incluso nel platform pioarduino)

---

## Origine

Estratto e generalizzato da [vergus-virgil](https://github.com/Drsplump/vergus-virgil),
un sistema di rilevamento perdite d'acqua per ESP32-S3 + ESP32-C6.
