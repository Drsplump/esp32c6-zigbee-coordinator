# Collegamento fisico e protocollo JSON

## Schema di collegamento ESP32-S3 ↔ ESP32-C6

| ESP32-S3 (host)    | Direzione | ESP32-C6 (coordinatore) |
|--------------------|-----------|-------------------------|
| GPIO18 — UART1 TX  | →         | D2 / GPIO2 — UART1 RX   |
| GPIO17 — UART1 RX  | ←         | D3 / GPIO21 — UART1 TX  |
| GND                | ↔         | GND                     |

> **Alimentazione**: lo XIAO ESP32-C6 può essere alimentato dal rail 3,3V dell'host.
> Verifica che il regolatore 3,3V della tua scheda possa fornire il carico aggiuntivo
> (~80–120 mA tipici con Zigbee attivo, fino a ~200 mA durante i burst TX).
> Aggiungi un **condensatore da 100 µF** sui pin 3V3/GND del C6 per assorbire
> i picchi di corrente Zigbee TX e prevenire reset per brownout.
> Entrambe le schede lavorano a 3,3V — nessun level shifter necessario.

## Parametri UART

| Parametro   | Valore  |
|-------------|---------|
| Baud rate   | 115200  |
| Bit di dati | 8       |
| Parità      | Nessuna |
| Bit di stop | 1       |

---

## Procedura di pairing (sensore IAS Zone, es. Sonoff SNZB-05P)

1. Flasha il coordinatore: `pio run -e coordinator --target upload`
2. Apri la finestra di pairing: premi il **pulsante BOOT** (GPIO9) sul C6 → rete aperta per 60 s
3. Metti il sensore in modalità pairing: tieni premuto il suo pulsante per ~5 s fino a che il LED lampeggia rapidamente
4. Il sensore si associa automaticamente; il C6 esegue l'enrollment IAS Zone
5. Il C6 inizia a inviare frame JSON all'host ogni 10 s + ad ogni cambio di stato

**Re-pairing completo** (cancella NVRAM Zigbee): tieni premuto BOOT mentre applichi alimentazione al C6.

---

## Formato dei frame JSON

### C6 → Host (dati sensore)

```json
{
  "id": "0xACA5",
  "zone_type": "water",
  "zone_type_id": 45,
  "alarm1": 0,
  "alarm2": 0,
  "tamper": 0,
  "battery_low": 0,
  "supervision_fail": 0,
  "restore_fail": 0,
  "trouble": 0,
  "ac_fault": 0,
  "test": 0,
  "joined": 1,
  "sleep": 0,
  "battery": 85,
  "linkquality": 155,
  "seq": 42
}
```

> `battery` e `linkquality` sono omessi finché il sensore non invia il relativo report.  
> Il tipo zona (`zone_type` / `zone_type_id`) è rilevato automaticamente dal coordinatore.

### Host → C6 (ACK)

```json
{"ack": 1, "seq": 42, "status": "ok", "queued": 1}
```

> Inviato automaticamente dal bridge dopo ogni frame valido (disabilitabile con `UART_C6_ACK_ENABLED 0`).

### C6 → Host (eventi di sistema)

```json
{"event": "c6_reset",       "reason": "POWERON", "code": 1,  "seq": 1}
{"event": "network_open",   "param": 60,                      "seq": 5}
{"event": "network_closed",                                   "seq": 6}
{"event": "device_joined",  "short_addr": "0x1A2B",          "seq": 7}
{"event": "c6_heartbeat",                                     "seq": 10}
```

### Host → C6 (comandi)

```json
{"cmd": "open_network",  "duration": 60}
{"cmd": "close_network"}
```

---

## Mappatura campi UartC6SensorFrame

| Campo bridge   | Campo JSON C6    | Descrizione                               |
|----------------|------------------|-------------------------------------------|
| `id`           | `id`             | Indirizzo corto Zigbee (es. `"0xACA5"`)   |
| `name`         | `zone_type`      | Tipo zona (es. `"water"`, `"motion"`)     |
| `alarm`        | `alarm1`         | Allarme principale attivo                 |
| `control`      | alarm1\|\|alarm2\|\|tamper | `true` se richiede azione       |
| `tamper`       | `tamper`         | Manomissione rilevata                     |
| `joined`       | `joined`         | Sensore associato alla rete               |
| `sleep`        | `sleep`          | Sensore online ma in sleep                |
| `battery`      | `battery`        | Batteria % (`-1` = non ancora ricevuto)   |
| `linkquality`  | `linkquality`    | LQI Zigbee 0–255 (`-1` = non disponibile)|
| `seq`          | `seq`            | Numero sequenza frame                     |

---

## Tipi di zona IAS supportati

Il coordinatore rileva il tipo automaticamente al momento del pairing. Nessuna configurazione manuale necessaria.

| Tipo zona   | zone_type_id        | Esempio sensore                            |
|-------------|---------------------|--------------------------------------------|
| `water`     | 0x002A (42)         | Sonoff SNZB-05P — molti vendor usano 0x002A |
| `water`     | 0x0029 (41)         | Vendor che seguono spec ZCL stretta         |
| `motion`    | 0x000D (13)         | PIR standard (Aqara, IKEA, Sonoff)         |
| `fire`      | 0x0028 (40)         | Rilevatore fumo/calore                     |
| `contact`   | 0x0015 (21)         | Contatto porta/finestra                    |
| `co`        | 0x002B (43)         | Sensore monossido di carbonio              |
| `vibration` | 0x002C (44)         | Sensore vibrazione                         |
