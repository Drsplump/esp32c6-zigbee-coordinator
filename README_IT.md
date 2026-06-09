# ESP32-C6 Coordinatore Zigbee

[🇬🇧 English](README.md) | 🇮🇹 Italiano

Coordinatore Zigbee IAS Zone generico per ESP32-C6 con bridge UART JSON verso qualsiasi MCU host.

Flasha il firmware `coordinator` sull'ESP32-C6, collega le due schede via UART, flasha il firmware `host_bridge` sull'ESP32-S3 (o qualsiasi MCU Arduino-compatibile), e hai un bridge Zigbee funzionante con meno di 10 righe di codice di integrazione.

---

## Come funziona

```
Sensore IAS Zone ─[Zigbee]─► ESP32-C6 ─[UART JSON]─► ESP32-S3 / MCU Host
                              coordinatore              la tua applicazione
```

- **Coordinatore** (ESP32-C6): agisce come coordinatore Zigbee, rileva automaticamente il tipo di zona IAS, ed invia frame JSON delimitati da newline all'host su UART ad ogni cambio di stato e ogni 10 secondi.
- **Bridge host** (ESP32-S3 o MCU Arduino-compatibile): riceve i frame, li analizza e chiama i callback della tua applicazione — nessuna conoscenza dello stack Zigbee richiesta.

Compatibile con qualsiasi sensore Zigbee IAS Zone (allagamento, movimento, contatto porta/finestra, …).
Testato con il sensore di allagamento Sonoff SNZB-05P.

---

## Struttura del repository

```
esp32c6-zigbee-coordinator/
├── platformio.ini            ← unico file PlatformIO con entrambi gli env
├── coordinator/
│   ├── src/main.cpp          firmware coordinatore ESP32-C6
│   ├── include/
│   │   ├── config_c6.h       ← modifica qui: pin UART, canale Zigbee, timing
│   │   └── debug.h
│   └── link_zigbee.py        script necessario per pioarduino 53.x
├── host_bridge/
│   ├── platformio.ini        progetto PlatformIO standalone per ESP32-S3
│   ├── src/main.cpp          esempio di integrazione (modifica qui la tua app)
│   ├── uart_c6_config.h      ← modifica qui: pin UART host e parametri
│   ├── uart_c6_bridge.h      API pubblica
│   └── uart_c6_bridge.cpp    implementazione bridge (senza dipendenze esterne)
└── docs/
    └── wiring.md             schema di collegamento, protocollo JSON, pairing
```

---

## Hardware necessario

| Componente          | Note                                                              |
|---------------------|-------------------------------------------------------------------|
| ESP32-C6-DevKitC-1  | Coordinatore Zigbee                                               |
| ESP32-S3-DevKitC-1  | MCU host (o qualsiasi scheda Arduino con UART hardware libera)    |
| Sensore Zigbee IAS  | Sonoff SNZB-05P o qualsiasi dispositivo IAS Zone                  |
| Condensatore 100 µF | Consigliato su 3V3/GND del C6 per assorbire i picchi Zigbee TX   |

---

## Collegamento fisico

| ESP32-S3 (host)    | Direzione | ESP32-C6 (coordinatore) |
|--------------------|-----------|-------------------------|
| GPIO18 — UART1 TX  | →         | D2 / GPIO2 — UART1 RX   |
| GPIO17 — UART1 RX  | ←         | D3 / GPIO21 — UART1 TX  |
| GND                | ↔         | GND                     |

> Entrambe le schede lavorano a 3,3V — nessun level shifter necessario.  
> I pin si modificano in `coordinator/include/config_c6.h` (lato C6) e `host_bridge/uart_c6_config.h` (lato S3).

---

## Avvio rapido

### 1. Apri il progetto

Apri `esp32c6-zigbee-coordinator.code-workspace` in VSCode. Nella barra in basso di PlatformIO trovi i due environment: **coordinator** e **host_bridge**.

### 2. Flasha il coordinatore (ESP32-C6)

Seleziona l'env `coordinator` e usa il pulsante Upload, oppure da terminale dalla root del progetto:

```bash
pio run -e coordinator --target upload
```

Per modificare la configurazione edita [`coordinator/include/config_c6.h`](coordinator/include/config_c6.h):

| Define              | Default | Descrizione                              |
|---------------------|---------|------------------------------------------|
| `S3_UART_TX_PIN`    | 21      | Pin TX del C6 verso l'host (D3/GPIO21)   |
| `S3_UART_RX_PIN`    | 2       | Pin RX del C6 dall'host (D2/GPIO2)       |
| `S3_UART_BAUD`      | 115200  | Baud rate UART                           |
| `ZIGBEE_CHANNEL`    | 13      | Canale Zigbee                            |
| `MAX_DEVICES`       | 10      | Numero massimo di sensori simultanei     |
| `HEARTBEAT_MS`      | 10000   | Intervallo heartbeat verso l'host (ms)   |
| `BOOT_BUTTON_PIN`   | 9       | GPIO del pulsante BOOT                   |
| `OPEN_NETWORK_SECS` | 60      | Durata finestra di pairing (pulsante)    |

### 3. Flasha il bridge host (ESP32-S3)

Seleziona l'env `host_bridge` e usa Upload, oppure:

```bash
pio run -e host_bridge --target upload
```

Modifica [`host_bridge/uart_c6_config.h`](host_bridge/uart_c6_config.h) per i pin della tua scheda:

| Define               | Default | Descrizione                              |
|----------------------|---------|------------------------------------------|
| `UART_C6_TX_PIN`     | 18      | GPIO18 S3 TX → D2/GPIO2 C6 RX           |
| `UART_C6_RX_PIN`     | 17      | GPIO17 S3 RX ← D3/GPIO21 C6 TX          |
| `UART_C6_BAUD`       | 115200  | Baud rate (deve corrispondere al C6)     |
| `UART_C6_INDEX`      | 1       | Numero UART hardware (1 o 2)             |
| `UART_C6_ACK_ENABLED`| 1       | Invia ACK al C6 dopo ogni frame valido   |

La logica applicativa va in `host_bridge/src/main.cpp`:

```cpp
#include "uart_c6_bridge.h"

void onSensor(const UartC6SensorFrame& f) {
    Serial.printf("[Sensore] id=%s tipo=%s alarm=%d batteria=%d%% lqi=%d\n",
                  f.id, f.name, f.alarm, f.battery, f.linkquality);
    if (f.alarm) {
        // gestisci l'allarme qui
    }
}

void onEvent(const UartC6EventFrame& f) {
    if (f.event == UartC6Event::NetworkOpen)
        Serial.printf("[Zigbee] Rete aperta per %lus\n", (unsigned long)f.param);
    else if (f.event == UartC6Event::DeviceJoined)
        Serial.printf("[Zigbee] Dispositivo unito: %s\n", f.extra);
}

void setup() {
    Serial.begin(115200);
    uartC6BridgeInit(onSensor, onEvent);
}

void loop() {
    uartC6BridgePoll();
}
```

### 4. Associa un sensore

1. Premi il **pulsante BOOT** sull'ESP32-C6 → finestra di pairing aperta per 60 secondi
2. Metti il sensore in modalità pairing (solitamente tasto tenuto per ~5 s)
3. Il sensore si associa automaticamente; il C6 esegue l'enrollment IAS Zone
4. I frame JSON iniziano ad arrivare sull'host entro pochi secondi

Per un **re-pairing completo** (cancella NVRAM Zigbee): tieni premuto BOOT mentre applichi alimentazione al C6.

---

## API bridge host

```cpp
// Inizializza UART e registra i callback (chiama una volta in setup)
void uartC6BridgeInit(UartC6SensorCallback onSensor, UartC6EventCallback onEvent);

// Poll non bloccante — chiama ad ogni ciclo di loop
void uartC6BridgePoll();

// Comandi verso il C6
void uartC6OpenNetwork(uint8_t durationSec = 60);
void uartC6CloseNetwork();

// Diagnostica
UartC6Stats  uartC6BridgeGetStats();
uint32_t     uartC6BridgeLastFrameAgeMs();
```

### Struttura UartC6SensorFrame

| Campo        | Tipo     | Descrizione                                              |
|--------------|----------|----------------------------------------------------------|
| `id`         | `char*`  | Indirizzo corto Zigbee (es. `"0xACA5"`)                  |
| `name`       | `char*`  | Tipo zona rilevato automaticamente (es. `"water"`)       |
| `alarm`      | `bool`   | `true` = alarm1 attivo                                   |
| `control`    | `bool`   | `true` se alarm1 \|\| alarm2 \|\| tamper                 |
| `tamper`     | `bool`   | `true` = tamper attivo                                   |
| `joined`     | `bool`   | `true` = sensore associato                               |
| `sleep`      | `bool`   | `true` = sensore in sleep (nessuna attività recente)     |
| `battery`    | `int`    | Percentuale batteria, `-1` se non ancora ricevuto        |
| `linkquality`| `int`    | LQI Zigbee (0–255), `-1` se non disponibile             |
| `seq`        | `uint32` | Numero sequenza frame                                    |

---

## Protocollo JSON

Vedi [docs/wiring.md](docs/wiring.md) per il formato completo dei frame.

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
