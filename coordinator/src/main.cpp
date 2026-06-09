/**
 * ESP32-C6 Zigbee Coordinator — Universal IAS Zone Bridge
 *
 * Flusso dati:
 *   Sensore IAS Zone →[Zigbee]→ ESP32-C6 →[UART JSON]→ MCU Host
 *
 * Zone types rilevati automaticamente (standard ZCL IAS Zone):
 *   motion, contact, fire, water, gas, personal_emergency, vibration,
 *   remote_control, key_fob, keypad, warning_device, standard_cie
 *
 * Protocollo UART (115200 8N1, delimitato da newline):
 *   TX → host (evento zona):
 *     {"id":"0xABCD","zone_type":"water","zone_type_id":42,
 *      "alarm1":0,"alarm2":0,"tamper":0,"battery_low":0,
 *      "supervision_fail":0,"restore_fail":0,"trouble":0,"ac_fault":0,"test":0,
 *      "joined":1,"sleep":0,"battery":85,"linkquality":155,"seq":N}
 *
 *   TX → host (eventi di sistema):
 *     {"event":"device_joined","short_addr":"0xABCD","seq":N}
 *     {"event":"device_left","seq":N}
 *     {"event":"network_open","param":60,"seq":N}
 *     {"event":"network_closed","seq":N}
 *     {"event":"c6_reset","reason":"POWERON","code":1,"seq":N}
 *     {"event":"c6_heartbeat","seq":N}
 *
 *   RX ← host:
 *     {"ack":1,"seq":N,"status":"ok"}
 *     {"cmd":"open_network","duration":60}
 *     {"cmd":"close_network"}
 *
 * Pulsante BOOT (GPIO9):
 *   - Pressione breve a runtime : apre la rete Zigbee per OPEN_NETWORK_SECS
 *   - Tenuto al power-on       : cancella la NVRAM Zigbee (re-pair completo)
 */

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HardwareSerial.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <nvs_flash.h>
#include <esp_system.h>

#include "esp_zigbee_core.h"
#include "nwk/esp_zigbee_nwk.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "zcl/esp_zigbee_zcl_ias_zone.h"
#include "zcl/esp_zigbee_zcl_common.h"
#include "zcl/esp_zigbee_zcl_power_config.h"
#include "zcl/esp_zigbee_zcl_command.h"

#include "config_c6.h"
#include "debug.h"

// ============================================================
// Registro dispositivi
// ============================================================

struct DeviceInfo {
    uint16_t short_addr;
    uint8_t  endpoint;
    uint16_t zone_type_id;    // 0xFFFF = non ancora noto
    uint16_t zone_status;     // bitmask IAS ZoneStatus corrente
    int      battery;         // -1 = sconosciuto
    uint8_t  lqi;
    bool     joined;
    bool     ever_seen;       // almeno una notifica zona ricevuta
    uint32_t last_activity_ms;
    bool     used;
};

static DeviceInfo g_devices[MAX_DEVICES];

static DeviceInfo* findDevice(uint16_t short_addr) {
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (g_devices[i].used && g_devices[i].short_addr == short_addr)
            return &g_devices[i];
    }
    return nullptr;
}

static DeviceInfo* findOrCreateDevice(uint16_t short_addr, uint8_t endpoint = 1) {
    DeviceInfo* d = findDevice(short_addr);
    if (d) return d;
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (!g_devices[i].used) {
            g_devices[i]              = {};
            g_devices[i].short_addr   = short_addr;
            g_devices[i].endpoint     = endpoint;
            g_devices[i].zone_type_id = 0xFFFF;
            g_devices[i].battery      = -1;
            g_devices[i].used         = true;
            return &g_devices[i];
        }
    }
    DBG_PRINTF("[C6] Tabella dispositivi piena! 0x%04X ignorato\n", short_addr);
    return nullptr;
}

// ============================================================
// Mapping zone type → stringa
// ============================================================

static const char* zoneTypeName(uint16_t id) {
    switch (id) {
        case 0x0000: return "standard_cie";
        case 0x000D: return "motion";
        case 0x0015: return "contact";
        case 0x0028: return "fire";
        case 0x0029: return "water";
        case 0x002A: return "water";       // Sonoff SNZB-05P e ecosistema HA usano 0x002A per flood
        case 0x002B: return "personal_emergency";
        case 0x002C: return "vibration";
        case 0x002D: return "remote_control";
        case 0x010F: return "key_fob";
        case 0x0115: return "keypad";
        case 0x021D: return "warning_device";
        default:     return "unknown";
    }
}

// ============================================================
// Tipi eventi inter-task
// ============================================================

struct ZbEvent {
    enum Type : uint8_t {
        ZONE_CHANGE, BATTERY_UPDATE, DEVICE_JOINED, ZONE_TYPE_READ, DEVICE_LEFT
    } type;
    uint16_t short_addr;
    uint8_t  endpoint;
    uint16_t zone_status;    // ZONE_CHANGE
    uint16_t zone_type_id;   // ZONE_TYPE_READ
    int      battery;        // BATTERY_UPDATE, -1 = non valido
    uint8_t  lqi;
};

// ============================================================
// Stato globale (aggiornato SOLO da loop())
// ============================================================

static uint32_t g_seq       = 0;
static uint32_t g_lastHB    = 0;

static bool     g_networkOpen        = false;
static uint32_t g_networkOpenUntilMs = 0;
static bool     g_forceNvramErase    = false;

static uint32_t g_bootMs              = 0;
static bool     g_anySensorEverSeen   = false;
static uint32_t g_lastReopenMs        = 0;

static uint16_t g_enrollPendingAddr = 0;
static uint8_t  g_enrollPendingEp   = 0;

static QueueHandle_t  g_evtQueue = nullptr;
static HardwareSerial HostSerial(S3_UART_NUM);

static char g_ackBuf[128];
static int  g_ackPos = 0;

// ============================================================
// Frame JSON → host
// ============================================================

static void sendJsonFrame(const DeviceInfo& dev, bool sleep) {
    JsonDocument doc;
    char id_buf[8];
    snprintf(id_buf, sizeof(id_buf), "0x%04X", dev.short_addr);

    doc["id"]        = id_buf;
    doc["zone_type"] = zoneTypeName(dev.zone_type_id);
    if (dev.zone_type_id != 0xFFFF)
        doc["zone_type_id"] = (int)dev.zone_type_id;

    uint16_t s = dev.zone_status;
    doc["alarm1"]          = (s & (uint16_t)ESP_ZB_ZCL_IAS_ZONE_ZONE_STATUS_ALARM1)      ? 1 : 0;
    doc["alarm2"]          = (s & (uint16_t)ESP_ZB_ZCL_IAS_ZONE_ZONE_STATUS_ALARM2)      ? 1 : 0;
    doc["tamper"]          = (s & (uint16_t)ESP_ZB_ZCL_IAS_ZONE_ZONE_STATUS_TAMPER)      ? 1 : 0;
    doc["battery_low"]     = (s & (uint16_t)ESP_ZB_ZCL_IAS_ZONE_ZONE_STATUS_BATTERY)     ? 1 : 0;
    doc["supervision_fail"]= (s & (uint16_t)ESP_ZB_ZCL_IAS_ZONE_ZONE_STATUS_SUPERVISION) ? 1 : 0;
    doc["restore_fail"]    = (s & (uint16_t)ESP_ZB_ZCL_IAS_ZONE_ZONE_STATUS_RESTORE)     ? 1 : 0;
    doc["trouble"]         = (s & (uint16_t)ESP_ZB_ZCL_IAS_ZONE_ZONE_STATUS_TROUBLE)     ? 1 : 0;
    doc["ac_fault"]        = (s & (uint16_t)ESP_ZB_ZCL_IAS_ZONE_ZONE_STATUS_AC_MAINS)    ? 1 : 0;
    doc["test"]            = (s & (uint16_t)ESP_ZB_ZCL_IAS_ZONE_ZONE_STATUS_TEST)        ? 1 : 0;

    doc["joined"] = dev.joined ? 1 : 0;
    doc["sleep"]  = sleep ? 1 : 0;
    doc["seq"]    = (uint32_t)(++g_seq);
    if (dev.battery >= 0) doc["battery"]     = dev.battery;
    if (dev.lqi > 0)      doc["linkquality"] = (int)dev.lqi;

    char buf[380];
    size_t n = serializeJson(doc, buf, sizeof(buf) - 1);
    buf[n++] = '\n';
    HostSerial.write(reinterpret_cast<const uint8_t*>(buf), n);
}

static const char* resetReasonToString(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_EXT:       return "EXT";
        case ESP_RST_SW:        return "SW";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "UNKNOWN";
    }
}

static void sendResetEventFrame(esp_reset_reason_t reason) {
    char buf[120];
    snprintf(buf, sizeof(buf),
             "{\"event\":\"c6_reset\",\"reason\":\"%s\",\"code\":%d,\"seq\":%lu}\n",
             resetReasonToString(reason), static_cast<int>(reason), (unsigned long)++g_seq);
    HostSerial.write(reinterpret_cast<const uint8_t*>(buf), strlen(buf));
}

static void sendBridgeHeartbeatEvent() {
    char buf[80];
    snprintf(buf, sizeof(buf),
             "{\"event\":\"c6_heartbeat\",\"seq\":%lu}\n",
             (unsigned long)(++g_seq));
    HostSerial.write(reinterpret_cast<const uint8_t*>(buf), strlen(buf));
}

static void sendEventFrame(const char* event, int param = -1) {
    char buf[100];
    if (param >= 0) {
        snprintf(buf, sizeof(buf),
                 "{\"event\":\"%s\",\"param\":%d,\"seq\":%lu}\n",
                 event, param, (unsigned long)g_seq);
    } else {
        snprintf(buf, sizeof(buf),
                 "{\"event\":\"%s\",\"seq\":%lu}\n",
                 event, (unsigned long)g_seq);
    }
    HostSerial.write(reinterpret_cast<const uint8_t*>(buf), strlen(buf));
}

// ============================================================
// Helper di network steering
// ============================================================

static void startNetworkSteeringCb(uint8_t /*param*/) {
    esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
}

static void requestNetworkSteering() {
    esp_zb_scheduler_alarm(startNetworkSteeringCb, 0, 0);
}

// ============================================================
// Parsing ACK / comandi dall'host
// ============================================================

static void processHostFrame(const char* json) {
    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok) return;

    if ((doc["ack"] | 0) == 1) {
        DBG_PRINTF("[ACK] seq=%lu status=%s\n",
            doc["seq"].as<unsigned long>(),
            doc["status"].as<const char*>() ?: "?");
        return;
    }

    const char* cmd = doc["cmd"] | "";
    if (strcmp(cmd, "open_network") == 0) {
        uint8_t dur = (uint8_t)(doc["duration"] | OPEN_NETWORK_SECS);
        g_networkOpen        = true;
        g_networkOpenUntilMs = millis() + (uint32_t)dur * 1000UL;
        DBG_PRINTF("[C6] CMD open_network dur=%us\n", dur);
        requestNetworkSteering();
        sendEventFrame("network_open", (int)dur);
    } else if (strcmp(cmd, "close_network") == 0) {
        g_networkOpen = false;
        DBG_PRINTLN("[C6] CMD close_network");
        sendEventFrame("network_closed");
    }
}

static void pollHostAck() {
    while (HostSerial.available()) {
        char c = (char)HostSerial.read();
        if (c == '\n' || c == '\r') {
            if (g_ackPos > 0) {
                g_ackBuf[g_ackPos] = '\0';
                processHostFrame(g_ackBuf);
                g_ackPos = 0;
            }
        } else if (g_ackPos < (int)sizeof(g_ackBuf) - 2) {
            g_ackBuf[g_ackPos++] = c;
        } else {
            g_ackPos = 0;
        }
    }
}

// ============================================================
// Callback Zigbee → push in coda
// ============================================================

static void pushZoneChange(uint16_t short_addr, uint8_t endpoint, uint16_t zone_status) {
    if (!g_evtQueue) return;
    ZbEvent evt = {};
    evt.type        = ZbEvent::ZONE_CHANGE;
    evt.short_addr  = short_addr;
    evt.endpoint    = endpoint;
    evt.zone_status = zone_status;
    evt.battery     = -1;
    if (xQueueSend(g_evtQueue, &evt, 0) != pdPASS) {
        DBG_PRINTF("[ZB] queue piena, ZoneChange perso (0x%04X status=0x%04X)\n",
                   short_addr, zone_status);
    }
}

static void pushZoneTypeRead(uint16_t short_addr, uint8_t endpoint, uint16_t zone_type_id) {
    if (!g_evtQueue) return;
    ZbEvent evt = {};
    evt.type         = ZbEvent::ZONE_TYPE_READ;
    evt.short_addr   = short_addr;
    evt.endpoint     = endpoint;
    evt.zone_type_id = zone_type_id;
    evt.battery      = -1;
    xQueueSend(g_evtQueue, &evt, 0);
}

static void pushBatteryUpdate(uint16_t short_addr, uint8_t endpoint, int pct) {
    if (!g_evtQueue) return;
    ZbEvent evt = {};
    evt.type       = ZbEvent::BATTERY_UPDATE;
    evt.short_addr = short_addr;
    evt.endpoint   = endpoint;
    evt.battery    = pct;
    xQueueSend(g_evtQueue, &evt, 0);
}

static int calcBatteryPctFromVoltage(uint8_t rawVoltage) {
    if (rawVoltage == 0x00 || rawVoltage == 0xFF) return -1;
    float volts = static_cast<float>(rawVoltage) / 10.0f;
    float pct = (volts - 2.2f) * 100.0f / (3.0f - 2.2f);
    if (pct < 0.0f)   pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    return static_cast<int>(pct + 0.5f);
}

// ============================================================
// Comandi ZCL verso i dispositivi
// ============================================================

static void sendIasCieAddressWrite(uint16_t shortAddr, uint8_t dstEndpoint) {
    static esp_zb_ieee_addr_t cieAddr;
    esp_zb_get_long_address(cieAddr);

    esp_zb_zcl_attribute_t attr = {};
    attr.id = ESP_ZB_ZCL_ATTR_IAS_ZONE_IAS_CIE_ADDRESS_ID;
    attr.data.type  = ESP_ZB_ZCL_ATTR_TYPE_IEEE_ADDR;
    attr.data.size  = sizeof(esp_zb_ieee_addr_t);
    attr.data.value = cieAddr;

    esp_zb_zcl_write_attr_cmd_t cmd = {};
    cmd.zcl_basic_cmd.dst_addr_u.addr_short = shortAddr;
    cmd.zcl_basic_cmd.dst_endpoint = dstEndpoint;
    cmd.zcl_basic_cmd.src_endpoint = EP_CIE;
    cmd.address_mode    = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.clusterID       = ESP_ZB_ZCL_CLUSTER_ID_IAS_ZONE;
    cmd.direction       = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
    cmd.dis_defalut_resp = 0;
    cmd.manuf_specific  = 0;
    cmd.manuf_code      = 0;
    cmd.attr_number     = 1;
    cmd.attr_field      = &attr;
    esp_zb_zcl_write_attr_cmd_req(&cmd);
}

static void readZoneTypeAttr(uint16_t shortAddr, uint8_t dstEndpoint) {
    static uint16_t attr_id = ESP_ZB_ZCL_ATTR_IAS_ZONE_ZONETYPE_ID;
    esp_zb_zcl_read_attr_cmd_t cmd = {};
    cmd.zcl_basic_cmd.dst_addr_u.addr_short = shortAddr;
    cmd.zcl_basic_cmd.dst_endpoint = dstEndpoint;
    cmd.zcl_basic_cmd.src_endpoint = EP_CIE;
    cmd.address_mode     = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.clusterID        = ESP_ZB_ZCL_CLUSTER_ID_IAS_ZONE;
    cmd.direction        = 0;  // to server
    cmd.dis_defalut_resp = 0;
    cmd.manuf_specific   = 0;
    cmd.manuf_code       = 0;
    cmd.attr_number      = 1;
    cmd.attr_field       = &attr_id;
    esp_zb_zcl_read_attr_cmd_req(&cmd);
}

static void sendIasZoneReportConfig(uint16_t shortAddr, uint8_t dstEndpoint) {
    static uint16_t reportChange = 0;
    esp_zb_zcl_config_report_record_t record = {};
    record.direction     = ESP_ZB_ZCL_REPORT_DIRECTION_SEND;
    record.attributeID   = ESP_ZB_ZCL_ATTR_IAS_ZONE_ZONESTATUS_ID;
    record.attrType      = ESP_ZB_ZCL_ATTR_TYPE_16BITMAP;
    record.min_interval  = 1;
    record.max_interval  = 600;
    record.reportable_change = &reportChange;

    esp_zb_zcl_config_report_cmd_t cmd = {};
    cmd.zcl_basic_cmd.dst_addr_u.addr_short = shortAddr;
    cmd.zcl_basic_cmd.dst_endpoint = dstEndpoint;
    cmd.zcl_basic_cmd.src_endpoint = EP_CIE;
    cmd.address_mode    = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.clusterID       = ESP_ZB_ZCL_CLUSTER_ID_IAS_ZONE;
    cmd.direction       = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
    cmd.dis_defalut_resp = 0;
    cmd.manuf_specific  = 0;
    cmd.manuf_code      = 0;
    cmd.record_number   = 1;
    cmd.record_field    = &record;
    esp_zb_zcl_config_report_cmd_req(&cmd);
}

static void sendPowerConfigReportConfig(uint16_t shortAddr, uint8_t dstEndpoint) {
    static uint8_t reportChange8 = 5;
    esp_zb_zcl_config_report_record_t records[2] = {};

    records[0].direction     = ESP_ZB_ZCL_REPORT_DIRECTION_SEND;
    records[0].attributeID   = ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID;
    records[0].attrType      = ESP_ZB_ZCL_ATTR_TYPE_U8;
    records[0].min_interval  = 3600;
    records[0].max_interval  = 43200;
    records[0].reportable_change = &reportChange8;

    records[1].direction     = ESP_ZB_ZCL_REPORT_DIRECTION_SEND;
    records[1].attributeID   = ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID;
    records[1].attrType      = ESP_ZB_ZCL_ATTR_TYPE_U8;
    records[1].min_interval  = 300;
    records[1].max_interval  = 3600;
    records[1].reportable_change = &reportChange8;

    esp_zb_zcl_config_report_cmd_t cmd = {};
    cmd.zcl_basic_cmd.dst_addr_u.addr_short = shortAddr;
    cmd.zcl_basic_cmd.dst_endpoint = dstEndpoint;
    cmd.zcl_basic_cmd.src_endpoint = EP_CIE;
    cmd.address_mode    = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.clusterID       = ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG;
    cmd.direction       = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
    cmd.dis_defalut_resp = 0;
    cmd.manuf_specific  = 0;
    cmd.manuf_code      = 0;
    cmd.record_number   = 2;
    cmd.record_field    = records;
    esp_zb_zcl_config_report_cmd_req(&cmd);
}

// Fallback enrollment: molti sensori IAS non inviano EnrollRequest dopo la
// scrittura CIE — forza dopo 2 s. Se arriva una EnrollRequest naturale,
// g_enrollPendingAddr viene azzerat e questo callback è un no-op.
static void forceEnrollResponse(uint8_t /*param*/) {
    if (g_enrollPendingAddr == 0) return;
    esp_zb_zcl_ias_zone_enroll_response_cmd_t resp = {};
    resp.zcl_basic_cmd.dst_addr_u.addr_short = g_enrollPendingAddr;
    resp.zcl_basic_cmd.dst_endpoint          = g_enrollPendingEp;
    resp.zcl_basic_cmd.src_endpoint          = EP_CIE;
    resp.address_mode    = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    resp.enroll_rsp_code = ESP_ZB_ZCL_IAS_ZONE_ENROLL_RESPONSE_CODE_SUCCESS;
    resp.zone_id = 0;
    DBG_PRINTF("[ZB] forceEnrollResponse → 0x%04X ep=%d\n",
               g_enrollPendingAddr, g_enrollPendingEp);
    esp_zb_zcl_ias_zone_enroll_cmd_resp(&resp);
    sendIasZoneReportConfig(g_enrollPendingAddr, g_enrollPendingEp);
    sendPowerConfigReportConfig(g_enrollPendingAddr, g_enrollPendingEp);
    // zone type letto già in DEVICE_ANNCE; se non ancora risposto, ritenta
    readZoneTypeAttr(g_enrollPendingAddr, g_enrollPendingEp);
    g_enrollPendingAddr = 0;
}

// ============================================================
// Gestore azioni ZCL (task Zigbee — NON usare Serial/HostSerial qui)
// ============================================================

static esp_err_t zbActionHandler(esp_zb_core_action_callback_id_t cb_id, const void* msg) {
    switch (cb_id) {

        // --- Notifica cambio stato zona ---
        case ESP_ZB_CORE_CMD_IAS_ZONE_ZONE_STATUS_CHANGE_NOT_ID: {
            const auto* n =
                static_cast<const esp_zb_zcl_ias_zone_status_change_notification_message_t*>(msg);
            pushZoneChange(n->info.src_address.u.short_addr,
                           n->info.src_endpoint,
                           n->zone_status);
            break;
        }

        // --- Richiesta enrollment naturale (include il zone_type) ---
        case ESP_ZB_CORE_CMD_IAS_ZONE_ZONE_ENROLL_REQUEST_ID: {
            const auto* req =
                static_cast<const esp_zb_zcl_ias_zone_enroll_request_message_t*>(msg);
            g_enrollPendingAddr = 0;  // annulla il fallback forzato

            esp_zb_zcl_ias_zone_enroll_response_cmd_t resp = {};
            resp.zcl_basic_cmd.dst_addr_u.addr_short = req->info.src_address.u.short_addr;
            resp.zcl_basic_cmd.dst_endpoint = req->info.src_endpoint;
            resp.zcl_basic_cmd.src_endpoint = req->info.dst_endpoint;
            resp.address_mode    = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
            resp.enroll_rsp_code = ESP_ZB_ZCL_IAS_ZONE_ENROLL_RESPONSE_CODE_SUCCESS;
            resp.zone_id = 0;
            esp_zb_zcl_ias_zone_enroll_cmd_resp(&resp);
            sendIasZoneReportConfig(req->info.src_address.u.short_addr,
                                    req->info.src_endpoint);
            sendPowerConfigReportConfig(req->info.src_address.u.short_addr,
                                        req->info.src_endpoint);
            // zone_type disponibile direttamente nella richiesta
            pushZoneTypeRead(req->info.src_address.u.short_addr,
                             req->info.src_endpoint,
                             req->zone_type);
            break;
        }

        // --- Risposta a read attribute (zone type letto con readZoneTypeAttr) ---
        case ESP_ZB_CORE_CMD_READ_ATTR_RESP_CB_ID: {
            const auto* resp =
                static_cast<const esp_zb_zcl_cmd_read_attr_resp_message_t*>(msg);
            if (resp->info.cluster != ESP_ZB_ZCL_CLUSTER_ID_IAS_ZONE) break;
            for (const auto* v = resp->variables; v != nullptr; v = v->next) {
                if (v->status == ESP_ZB_ZCL_STATUS_SUCCESS &&
                    v->attribute.id == ESP_ZB_ZCL_ATTR_IAS_ZONE_ZONETYPE_ID &&
                    v->attribute.data.value != nullptr) {
                    uint16_t zt = *static_cast<const uint16_t*>(v->attribute.data.value);
                    pushZoneTypeRead(resp->info.src_address.u.short_addr,
                                     resp->info.src_endpoint, zt);
                }
            }
            break;
        }

        // --- Report attributi (batteria, zone_status) ---
        case ESP_ZB_CORE_REPORT_ATTR_CB_ID: {
            const auto* r =
                static_cast<const esp_zb_zcl_report_attr_message_t*>(msg);

            if (r->cluster == ESP_ZB_ZCL_CLUSTER_ID_IAS_ZONE &&
                r->attribute.id == ESP_ZB_ZCL_ATTR_IAS_ZONE_ZONESTATUS_ID &&
                r->attribute.data.value) {
                uint16_t zs = *static_cast<const uint16_t*>(r->attribute.data.value);
                pushZoneChange(r->src_address.u.short_addr,
                               r->src_endpoint, zs);
                break;
            }
            if (r->cluster == ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG &&
                r->attribute.data.value) {
                if (r->attribute.id ==
                    ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID) {
                    uint8_t rawPct = *static_cast<const uint8_t*>(r->attribute.data.value);
                    if (rawPct != 0xFF && rawPct <= 200)
                        pushBatteryUpdate(r->src_address.u.short_addr,
                                         r->src_endpoint,
                                         static_cast<int>(rawPct / 2));
                } else if (r->attribute.id ==
                           ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID) {
                    uint8_t rawV = *static_cast<const uint8_t*>(r->attribute.data.value);
                    int pct = calcBatteryPctFromVoltage(rawV);
                    if (pct >= 0)
                        pushBatteryUpdate(r->src_address.u.short_addr,
                                         r->src_endpoint, pct);
                }
            }
            break;
        }

        default:
            break;
    }
    return ESP_OK;
}

// ============================================================
// Gestore segnali Zigbee
// ============================================================

void esp_zb_app_signal_handler(esp_zb_app_signal_t* sig_struct) {
    uint32_t*                p_sig    = sig_struct->p_app_signal;
    esp_err_t                err      = sig_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = static_cast<esp_zb_app_signal_type_t>(*p_sig);

    switch (sig_type) {

        case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
            break;

        case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
            if (err == ESP_OK) {
                DBG_PRINTLN("[ZB] Primo avvio: formazione rete in corso...");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_FORMATION);
            } else {
                DBG_PRINTF("[ZB] Errore di boot: %s — nuovo tentativo\n", esp_err_to_name(err));
                esp_zb_scheduler_alarm(
                    reinterpret_cast<esp_zb_callback_t>(
                        esp_zb_bdb_start_top_level_commissioning),
                    ESP_ZB_BDB_MODE_INITIALIZATION, 1000);
            }
            break;

        case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
            if (err == ESP_OK) {
                DBG_PRINTF("[ZB] Reboot: rete esistente, apertura steering (%ds)\n",
                           BOOT_NETWORK_SECS);
                if (g_evtQueue) {
                    ZbEvent evt = {};
                    evt.type = ZbEvent::DEVICE_JOINED;
                    evt.short_addr = 0xFFFF;  // sentinella: segnala reboot con rete esistente
                    xQueueSend(g_evtQueue, &evt, 0);
                }
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            } else {
                esp_zb_scheduler_alarm(
                    reinterpret_cast<esp_zb_callback_t>(
                        esp_zb_bdb_start_top_level_commissioning),
                    ESP_ZB_BDB_MODE_INITIALIZATION, 1000);
            }
            break;

        case ESP_ZB_BDB_SIGNAL_FORMATION:
            if (err == ESP_OK) {
                DBG_PRINTF("[ZB] Rete formata — PAN 0x%04X, ch %d (steering %ds)\n",
                              esp_zb_get_pan_id(), esp_zb_get_current_channel(),
                              BOOT_NETWORK_SECS);
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            } else {
                DBG_PRINTLN("[ZB] Errore di formazione — nuovo tentativo");
                esp_zb_scheduler_alarm(
                    reinterpret_cast<esp_zb_callback_t>(
                        esp_zb_bdb_start_top_level_commissioning),
                    ESP_ZB_BDB_MODE_NETWORK_FORMATION, 1000);
            }
            break;

        case ESP_ZB_BDB_SIGNAL_STEERING:
            DBG_PRINTF("[ZB] Steering %s\n",
                          err == ESP_OK ? "OK — rete aperta" : "chiusa/errore");
            break;

        case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE: {
            auto* p = static_cast<esp_zb_zdo_signal_device_annce_params_t*>(
                          esp_zb_app_signal_get_params(p_sig));
            DBG_PRINTF("[ZB] Dispositivo annunciato: 0x%04X\n", p->device_short_addr);
            sendIasCieAddressWrite(p->device_short_addr, 1);
            // Leggi subito il zone type (risposta via ESP_ZB_CORE_CMD_READ_ATTR_RESP_CB_ID)
            readZoneTypeAttr(p->device_short_addr, 1);
            g_enrollPendingAddr = p->device_short_addr;
            g_enrollPendingEp   = 1;
            esp_zb_scheduler_alarm(forceEnrollResponse, 0, 2000);
            if (g_evtQueue) {
                ZbEvent evt = {};
                evt.type       = ZbEvent::DEVICE_JOINED;
                evt.short_addr = p->device_short_addr;
                evt.endpoint   = 1;
                xQueueSend(g_evtQueue, &evt, 0);
            }
            break;
        }

        case ESP_ZB_ZDO_SIGNAL_LEAVE:
            DBG_PRINTLN("[ZB] Un dispositivo ha lasciato la rete");
            if (g_evtQueue) {
                ZbEvent evt = {};
                evt.type = ZbEvent::DEVICE_LEFT;
                xQueueSend(g_evtQueue, &evt, 0);
            }
            break;

        default:
            break;
    }
}

// ============================================================
// Task Zigbee (core 0, alta priorità)
// ============================================================

static void zigbeeTask(void* /*pv*/) {
    esp_zb_platform_config_t platform_cfg = {
        .radio_config = { .radio_mode = ZB_RADIO_MODE_NATIVE },
        .host_config  = { .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE },
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&platform_cfg));

    esp_zb_cfg_t zb_cfg = {
        .esp_zb_role         = ESP_ZB_DEVICE_TYPE_COORDINATOR,
        .install_code_policy = false,
    };
    zb_cfg.nwk_cfg.zczr_cfg.max_children = MAX_DEVICES;

    esp_zb_nvram_erase_at_start(g_forceNvramErase);
    esp_zb_init(&zb_cfg);

    // Endpoint CIE: cluster Basic + cluster IAS Zone (ruolo CIE)
    // IAS_ZONE_TYPE sul coordinatore = 0x0000 (Standard CIE): accetta qualsiasi sensore
    esp_zb_cluster_list_t* cl = esp_zb_zcl_cluster_list_create();

    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version  = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = 0x03,
    };
    esp_zb_cluster_list_add_basic_cluster(
        cl, esp_zb_basic_cluster_create(&basic_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_ias_zone_cluster_cfg_t ias_cfg = {
        .zone_state  = ESP_ZB_ZCL_IAS_ZONE_ZONESTATE_NOT_ENROLLED,
        .zone_type   = ESP_ZB_ZCL_IAS_ZONE_ZONETYPE_STANDARD_CIE,  // 0x0000: accetta tutti i tipi
        .zone_status = 0,
    };
    esp_zb_cluster_list_add_ias_zone_cluster(
        cl, esp_zb_ias_zone_cluster_create(&ias_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_endpoint_config_t ep_cfg = {
        .endpoint           = EP_CIE,
        .app_profile_id     = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id      = ESP_ZB_HA_IAS_ZONE_ID,
        .app_device_version = 0,
    };
    esp_zb_ep_list_t* ep_list = esp_zb_ep_list_create();
    esp_zb_ep_list_add_ep(ep_list, cl, ep_cfg);
    esp_zb_device_register(ep_list);

    esp_zb_core_action_handler_register(zbActionHandler);
    esp_zb_set_primary_network_channel_set(1UL << ZIGBEE_CHANNEL);

    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
    vTaskDelete(nullptr);
}

// ============================================================
// Helpers loop()
// ============================================================

static bool anyDeviceJoined() {
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (g_devices[i].used && g_devices[i].joined) return true;
    }
    return false;
}

static bool anyDeviceEverSeen() {
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (g_devices[i].used && g_devices[i].ever_seen) return true;
    }
    return false;
}

// ============================================================
// setup() / loop()
// ============================================================

void setup() {
    Serial.begin(115200);
    delay(300);
    DBG_PRINTLN("[C6] === Boot ESP32-C6 Zigbee Coordinator (Universal IAS) ===");

    esp_reset_reason_t resetReason = esp_reset_reason();
    DBG_PRINTF("[C6] Causa di reset: %s (%d)\n",
               resetReasonToString(resetReason), static_cast<int>(resetReason));

    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
    delay(20);
    bool bootPressed = true;
    for (uint8_t i = 0; i < 6; i++) {
        if (digitalRead(BOOT_BUTTON_PIN) != LOW) { bootPressed = false; break; }
        delay(15);
    }
    g_forceNvramErase = bootPressed;
    if (g_forceNvramErase) DBG_PRINTLN("[C6] BOOT tenuto: cancellazione NVRAM Zigbee abilitata");

    HostSerial.end();
    HostSerial.setRxBufferSize(S3_UART_BUF);
    delay(20);
    HostSerial.begin(S3_UART_BAUD, SERIAL_8N1, S3_UART_RX_PIN, S3_UART_TX_PIN);
    delay(20);

    delay(8000);
    sendResetEventFrame(resetReason);

    esp_err_t nvsErr = nvs_flash_init();
    if (nvsErr == ESP_ERR_NVS_NO_FREE_PAGES || nvsErr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvsErr = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvsErr);

    g_evtQueue = xQueueCreate(16, sizeof(ZbEvent));
    configASSERT(g_evtQueue);

    xTaskCreatePinnedToCore(zigbeeTask, "zb_task", 8192, nullptr, 5, nullptr, 0);

    g_bootMs = millis();
    g_lastHB = g_bootMs;
    DBG_PRINTLN("[C6] Boot completato — in attesa della rete Zigbee...");
}

void loop() {
    uint32_t now = millis();

    // --- Elabora eventi dal zigbeeTask ---
    ZbEvent evt;
    while (xQueueReceive(g_evtQueue, &evt, 0) == pdTRUE) {

        if (evt.type == ZbEvent::ZONE_CHANGE) {
            DeviceInfo* d = findOrCreateDevice(evt.short_addr, evt.endpoint);
            if (d) {
                d->zone_status      = evt.zone_status;
                d->joined           = true;
                d->ever_seen        = true;
                d->last_activity_ms = now;
                g_anySensorEverSeen = true;
                bool alm1 = (evt.zone_status & (uint16_t)ESP_ZB_ZCL_IAS_ZONE_ZONE_STATUS_ALARM1) != 0;
                bool alm2 = (evt.zone_status & (uint16_t)ESP_ZB_ZCL_IAS_ZONE_ZONE_STATUS_ALARM2) != 0;
                DBG_PRINTF("[C6] 0x%04X ZoneChange: status=0x%04X alarm1=%d alarm2=%d type=%s\n",
                           evt.short_addr, evt.zone_status, alm1, alm2,
                           zoneTypeName(d->zone_type_id));
                sendJsonFrame(*d, false);
                g_lastHB = now;
            }

        } else if (evt.type == ZbEvent::BATTERY_UPDATE && evt.battery >= 0) {
            DeviceInfo* d = findOrCreateDevice(evt.short_addr, evt.endpoint);
            if (d) {
                d->battery          = evt.battery;
                d->joined           = true;
                d->last_activity_ms = now;
                DBG_PRINTF("[C6] 0x%04X Batteria: %d%%\n", evt.short_addr, evt.battery);
                sendJsonFrame(*d, false);
                g_lastHB = now;
            }

        } else if (evt.type == ZbEvent::ZONE_TYPE_READ) {
            DeviceInfo* d = findOrCreateDevice(evt.short_addr, evt.endpoint);
            if (d) {
                d->zone_type_id = evt.zone_type_id;
                DBG_PRINTF("[C6] 0x%04X ZoneType: 0x%04X (%s)\n",
                           evt.short_addr, evt.zone_type_id,
                           zoneTypeName(evt.zone_type_id));
                sendJsonFrame(*d, false);
            }

        } else if (evt.type == ZbEvent::DEVICE_JOINED) {
            if (evt.short_addr == 0xFFFF) {
                // Reboot con rete esistente: assume tutti i device precedenti ancora connessi
                g_networkOpen        = true;
                g_networkOpenUntilMs = now + (uint32_t)BOOT_NETWORK_SECS * 1000UL;
                sendEventFrame("network_open", BOOT_NETWORK_SECS);
            } else {
                DeviceInfo* d = findOrCreateDevice(evt.short_addr, evt.endpoint);
                if (d) {
                    d->joined           = true;
                    d->last_activity_ms = now;
                }
                char buf[88];
                snprintf(buf, sizeof(buf),
                         "{\"event\":\"device_joined\",\"short_addr\":\"0x%04X\",\"seq\":%lu}\n",
                         evt.short_addr, (unsigned long)++g_seq);
                HostSerial.write(reinterpret_cast<const uint8_t*>(buf), strlen(buf));
                DBG_PRINTF("[C6] Dispositivo unito: 0x%04X\n", evt.short_addr);
                if (g_networkOpen) {
                    g_networkOpenUntilMs = now + 30000;
                }
            }

        } else if (evt.type == ZbEvent::DEVICE_LEFT) {
            // Senza indirizzo specifico, segna tutti come non connessi
            for (int i = 0; i < MAX_DEVICES; i++) {
                if (g_devices[i].used) {
                    g_devices[i].joined    = false;
                    g_devices[i].ever_seen = false;
                    g_devices[i].zone_status = 0;
                }
            }
            sendEventFrame("device_left");
            DBG_PRINTLN("[C6] Rete: dispositivo uscito");
        }
    }

    // --- Timeout rete aperta ---
    if (g_networkOpen && now >= g_networkOpenUntilMs) {
        g_networkOpen = false;
        sendEventFrame("network_closed");
        DBG_PRINTLN("[C6] Finestra di pairing chiusa (timeout)");
    }

    // --- Heartbeat periodico ---
    if (now - g_lastHB >= HEARTBEAT_MS) {
        g_lastHB = now;
        sendBridgeHeartbeatEvent();
        for (int i = 0; i < MAX_DEVICES; i++) {
            if (!g_devices[i].used) continue;
            const DeviceInfo& d = g_devices[i];
            if (!d.joined && !d.ever_seen) continue;
            bool sleeping = d.joined && !d.ever_seen && d.zone_status == 0;
            sendJsonFrame(d, sleeping);
        }
    }

    // --- ACK / comandi dall'host ---
    pollHostAck();

    // --- Timeout attività per-device: passa in ONLINE_SLEEP senza perdere il pairing ---
    static const uint32_t SENSOR_ACTIVITY_TIMEOUT_MS = 90000UL;
    for (int i = 0; i < MAX_DEVICES; i++) {
        DeviceInfo& d = g_devices[i];
        if (!d.used || !d.ever_seen) continue;
        if (d.last_activity_ms > 0 &&
            (now - d.last_activity_ms) >= SENSOR_ACTIVITY_TIMEOUT_MS) {
            d.ever_seen   = false;
            d.zone_status = 0;
            DBG_PRINTF("[C6] 0x%04X inattivo da %lus — ONLINE_SLEEP\n",
                       d.short_addr,
                       (unsigned long)(now - d.last_activity_ms) / 1000UL);
        }
    }

    // --- Riapertura automatica per end-device in backoff ---
    if (!anyDeviceJoined() && !anyDeviceEverSeen() && !g_networkOpen &&
        (now - g_lastReopenMs) >= REJOIN_REOPEN_PAUSE_MS) {
        g_lastReopenMs       = now;
        g_networkOpen        = true;
        g_networkOpenUntilMs = now + (uint32_t)BOOT_NETWORK_SECS * 1000UL;
        DBG_PRINTLN("[C6] Nessun dispositivo connesso — riapertura rete");
        requestNetworkSteering();
    }

    // --- Pulsante BOOT: apre finestra di pairing ---
    static uint32_t lastBtnMs = 0;
    if (now - lastBtnMs > 300 && digitalRead(BOOT_BUTTON_PIN) == LOW) {
        lastBtnMs = now;
        DBG_PRINTF("[C6] Pulsante boot: apertura finestra (%ds)\n", OPEN_NETWORK_SECS);
        g_networkOpen        = true;
        g_networkOpenUntilMs = now + (uint32_t)OPEN_NETWORK_SECS * 1000UL;
        requestNetworkSteering();
        sendEventFrame("network_open", OPEN_NETWORK_SECS);
    }

    delay(10);
}
