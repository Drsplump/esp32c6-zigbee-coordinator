/**
 * ESP32-C6 Zigbee Coordinator — Bridge generica IAS Zone
 *
 * Flusso dati:
 *   Sensore IAS Zone →[Zigbee]→ ESP32-C6 →[UART JSON]→ MCU Host
 *
 * Protocollo UART (115200 8N1, delimitato da newline):
 *   TX → host: {"id":"...","name":"...","alarm":0/1,"control":0/1,
 *               "joined":1,"sleep":0,"battery":85,"linkquality":155,"seq":N}
 *   RX ← host: {"ack":1,"seq":N,"status":"ok","queued":1}
 *            or {"cmd":"open_network","duration":60}
 *            or {"cmd":"close_network"}
 *
 * Pulsante BOOT (GPIO9):
 *   - Pressione breve a runtime : apre la rete Zigbee per OPEN_NETWORK_SECS
 *   - Tenuto al power-on       : cancella la NVRAM Zigbee (re-pair completo)
 *
 * Modificare coordinator/include/config_c6.h per cambiare pin, identità sensore
 * e parametri di temporizzazione senza toccare questo file.
 */

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HardwareSerial.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <nvs_flash.h>
#include <esp_system.h>

// API Zigbee ESP-IDF (arduino-esp32 3.x con ZIGBEE_MODE_COORDINATOR)
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
// Tipi
// ============================================================

struct ZbEvent {
    enum Type : uint8_t { ZONE_CHANGE, BATTERY_UPDATE, DEVICE_JOINED } type;
    bool     alarm;
    bool     tamper;
    int      battery;     // -1 = invariato
    uint8_t  lqi;
    uint16_t short_addr;  // solo DEVICE_JOINED
};

// ============================================================
// Stato globale (aggiornato SOLO da loop())
// ============================================================

static bool     g_alarm    = false;
static bool     g_tamper   = false;
static int      g_battery  = -1;
static uint8_t  g_lqi      = 0;

static uint32_t g_seq      = 0;
static uint32_t g_lastHB   = 0;

static bool     g_networkOpen        = false;
static uint32_t g_networkOpenUntilMs = 0;
static bool     g_forceNvramErase    = false;

static uint32_t g_bootMs              = 0;
static bool     g_sensorEverSeen      = false;
static bool     g_sensorJoined        = false;
static uint32_t g_lastReopenMs        = 0;
static uint32_t g_lastSensorActivityMs = 0;

static uint16_t g_enrollPendingAddr = 0;
static uint8_t  g_enrollPendingEp   = 0;

static QueueHandle_t  g_evtQueue = nullptr;
static HardwareSerial HostSerial(S3_UART_NUM);

static char g_ackBuf[96];
static int  g_ackPos = 0;

// ============================================================
// Frame JSON → host
// ============================================================

static void sendJsonFrame(bool alarm, bool tamper, int battery, uint8_t lqi,
                          int control, bool joined, bool sleep) {
    JsonDocument doc;
    doc["id"]      = SENSOR_ID;
    doc["name"]    = SENSOR_NAME;
    doc["alarm"]   = alarm   ? 1 : 0;
    doc["control"] = control;
    doc["joined"]  = joined ? 1 : 0;
    doc["sleep"]   = sleep  ? 1 : 0;
    doc["seq"]     = (uint32_t)(++g_seq);
    if (battery >= 0) doc["battery"]     = battery;
    if (lqi > 0)      doc["linkquality"] = (int)lqi;
    if (tamper)       doc["tamper"]      = true;

    char buf[200];
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
            g_ackPos = 0;  // overflow: scarta
        }
    }
}

// ============================================================
// Callback Zigbee (chiamate dal task Zigbee → push in coda)
// ============================================================

static void pushZoneChange(uint16_t zoneStatus) {
    if (!g_evtQueue) return;
    ZbEvent evt;
    evt.type   = ZbEvent::ZONE_CHANGE;
    evt.alarm  = (zoneStatus & (ESP_ZB_ZCL_IAS_ZONE_ZONE_STATUS_ALARM1 |
                                ESP_ZB_ZCL_IAS_ZONE_ZONE_STATUS_ALARM2)) != 0;
    evt.tamper = (zoneStatus & 0x0004) != 0;
    evt.battery = -1;
    evt.lqi     = 0;
    if (xQueueSend(g_evtQueue, &evt, 0) != pdPASS) {
        DBG_PRINTF("[ZB] queue piena, ZoneChange perso (0x%04X)\n", zoneStatus);
    }
}

static void pushBatteryPctValue(int pct) {
    if (!g_evtQueue) return;
    ZbEvent evt;
    evt.type    = ZbEvent::BATTERY_UPDATE;
    evt.alarm   = false;
    evt.tamper  = false;
    evt.battery = pct;
    evt.lqi     = 0;
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

// Fallback di enrollment forzato: molti sensori IAS (es. SNZB-05P) non inviano
// una EnrollRequest dopo la scrittura dell'indirizzo CIE — forza dopo 2 s.
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
    g_enrollPendingAddr = 0;
}

// Gestore azioni ZCL (chiamato dal task Zigbee — NON usare Serial qui)
static esp_err_t zbActionHandler(esp_zb_core_action_callback_id_t cb_id, const void* msg) {
    switch (cb_id) {

        case ESP_ZB_CORE_CMD_IAS_ZONE_ZONE_STATUS_CHANGE_NOT_ID: {
            const auto* n =
                static_cast<const esp_zb_zcl_ias_zone_status_change_notification_message_t*>(msg);
            pushZoneChange(n->zone_status);
            break;
        }

        case ESP_ZB_CORE_CMD_IAS_ZONE_ZONE_ENROLL_REQUEST_ID: {
            const auto* req =
                static_cast<const esp_zb_zcl_ias_zone_enroll_request_message_t*>(msg);
            g_enrollPendingAddr = 0;  // EnrollRequest naturale — annulla il fallback forzato
            esp_zb_zcl_ias_zone_enroll_response_cmd_t resp = {};
            resp.zcl_basic_cmd.dst_addr_u.addr_short = req->info.src_address.u.short_addr;
            resp.zcl_basic_cmd.dst_endpoint = req->info.src_endpoint;
            resp.zcl_basic_cmd.src_endpoint = req->info.dst_endpoint;
            resp.address_mode    = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
            resp.enroll_rsp_code = ESP_ZB_ZCL_IAS_ZONE_ENROLL_RESPONSE_CODE_SUCCESS;
            resp.zone_id = 0;
            esp_zb_zcl_ias_zone_enroll_cmd_resp(&resp);
            sendIasZoneReportConfig(resp.zcl_basic_cmd.dst_addr_u.addr_short,
                                    resp.zcl_basic_cmd.dst_endpoint);
            sendPowerConfigReportConfig(resp.zcl_basic_cmd.dst_addr_u.addr_short,
                                        resp.zcl_basic_cmd.dst_endpoint);
            break;
        }

        case ESP_ZB_CORE_REPORT_ATTR_CB_ID: {
            const auto* r =
                static_cast<const esp_zb_zcl_report_attr_message_t*>(msg);
            if (r->cluster == ESP_ZB_ZCL_CLUSTER_ID_IAS_ZONE &&
                r->attribute.id == ESP_ZB_ZCL_ATTR_IAS_ZONE_ZONESTATUS_ID &&
                r->attribute.data.value) {
                uint16_t zone_status = *static_cast<const uint16_t*>(r->attribute.data.value);
                pushZoneChange(zone_status);
                break;
            }
            if (r->cluster == ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG && r->attribute.data.value) {
                if (r->attribute.id == ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID) {
                    uint8_t rawPct = *static_cast<const uint8_t*>(r->attribute.data.value);
                    if (rawPct != 0xFF && rawPct <= 200)
                        pushBatteryPctValue(static_cast<int>(rawPct / 2));
                } else if (r->attribute.id == ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID) {
                    uint8_t rawVolt = *static_cast<const uint8_t*>(r->attribute.data.value);
                    int pct = calcBatteryPctFromVoltage(rawVolt);
                    if (pct >= 0) pushBatteryPctValue(pct);
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
// Gestore segnali (richiesto dallo stack Zigbee)
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
                g_sensorJoined       = true;
                g_sensorEverSeen     = false;
                g_lastSensorActivityMs = 0;
                DBG_PRINTF("[ZB] Reboot: rete esistente, apertura steering (%ds)\n",
                           BOOT_NETWORK_SECS);
                g_networkOpen        = true;
                g_networkOpenUntilMs = millis() + (uint32_t)BOOT_NETWORK_SECS * 1000UL;
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
                sendEventFrame("network_open", BOOT_NETWORK_SECS);
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
                g_networkOpen        = true;
                g_networkOpenUntilMs = millis() + (uint32_t)BOOT_NETWORK_SECS * 1000UL;
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
                sendEventFrame("network_open", BOOT_NETWORK_SECS);
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
            DBG_PRINTF("[ZB] Dispositivo unito: 0x%04X\n", p->device_short_addr);
            sendIasCieAddressWrite(p->device_short_addr, 1);
            g_enrollPendingAddr = p->device_short_addr;
            g_enrollPendingEp   = 1;
            esp_zb_scheduler_alarm(forceEnrollResponse, 0, 2000);
            if (g_evtQueue) {
                ZbEvent evt = {};
                evt.type       = ZbEvent::DEVICE_JOINED;
                evt.short_addr = p->device_short_addr;
                if (xQueueSend(g_evtQueue, &evt, 0) != pdPASS) {
                    DBG_PRINTF("[ZB] DEVICE_JOINED queue piena! 0x%04X\n", p->device_short_addr);
                }
            }
            break;
        }

        case ESP_ZB_ZDO_SIGNAL_LEAVE:
            DBG_PRINTLN("[ZB] Dispositivo uscito dalla rete");
            g_sensorJoined        = false;
            g_sensorEverSeen      = false;
            g_lastSensorActivityMs = 0;
            g_alarm  = false;
            g_tamper = false;
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
    zb_cfg.nwk_cfg.zczr_cfg.max_children = 10;

    esp_zb_nvram_erase_at_start(g_forceNvramErase);
    esp_zb_init(&zb_cfg);

    // Costruisci endpoint: cluster Basic + cluster IAS Zone (ruolo CIE)
    esp_zb_cluster_list_t* cl = esp_zb_zcl_cluster_list_create();

    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version  = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = 0x03,  // CC (corrente continua)
    };
    esp_zb_cluster_list_add_basic_cluster(
        cl, esp_zb_basic_cluster_create(&basic_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_ias_zone_cluster_cfg_t ias_cfg = {
        .zone_state  = ESP_ZB_ZCL_IAS_ZONE_ZONESTATE_NOT_ENROLLED,
        .zone_type   = IAS_ZONE_TYPE,
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
    esp_zb_stack_main_loop();  // non ritorna mai
    vTaskDelete(nullptr);
}

// ============================================================
// setup() / loop()
// ============================================================

void setup() {
    Serial.begin(115200);
    delay(300);
    DBG_PRINTLN("[C6] === Boot ESP32-C6 Zigbee Coordinator ===");

    esp_reset_reason_t resetReason = esp_reset_reason();
    DBG_PRINTF("[C6] Causa di reset: %s (%d)\n",
               resetReasonToString(resetReason), static_cast<int>(resetReason));

    // Rileva pulsante BOOT tenuto al power-on → cancella NVRAM Zigbee
    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
    delay(20);
    bool bootPressed = true;
    for (uint8_t i = 0; i < 6; i++) {
        if (digitalRead(BOOT_BUTTON_PIN) != LOW) { bootPressed = false; break; }
        delay(15);
    }
    g_forceNvramErase = bootPressed;
    if (g_forceNvramErase) DBG_PRINTLN("[C6] BOOT tenuto: cancellazione NVRAM Zigbee abilitata");

    // UART verso MCU host
    HostSerial.end();
    HostSerial.setRxBufferSize(S3_UART_BUF);
    delay(20);
    HostSerial.begin(S3_UART_BAUD, SERIAL_8N1, S3_UART_RX_PIN, S3_UART_TX_PIN);
    delay(20);

    // Attendi che l'MCU host inizializzi il suo bridge UART prima di inviare frame
    delay(8000);
    sendResetEventFrame(resetReason);

    // NVS deve essere pronto prima dell'init dello stack Zigbee
    esp_err_t nvsErr = nvs_flash_init();
    if (nvsErr == ESP_ERR_NVS_NO_FREE_PAGES || nvsErr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvsErr = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvsErr);

    g_evtQueue = xQueueCreate(8, sizeof(ZbEvent));
    configASSERT(g_evtQueue);

    // Task Zigbee su core 0 (core radio)
    xTaskCreatePinnedToCore(zigbeeTask, "zb_task", 8192, nullptr, 5, nullptr, 0);

    g_bootMs = millis();
    g_lastHB = g_bootMs;
    DBG_PRINTLN("[C6] Boot completato — in attesa della rete Zigbee...");
}

void loop() {
    uint32_t now = millis();

    // Elabora eventi dal zigbeeTask
    ZbEvent evt;
    while (xQueueReceive(g_evtQueue, &evt, 0) == pdTRUE) {
        if (evt.type == ZbEvent::ZONE_CHANGE) {
            g_alarm  = evt.alarm;
            g_tamper = evt.tamper;
            g_sensorEverSeen      = true;
            g_sensorJoined        = true;
            g_lastSensorActivityMs = now;
            sendJsonFrame(g_alarm, g_tamper, g_battery, g_lqi, g_alarm ? 1 : 0, true, false);
            g_lastHB = now;
            DBG_PRINTF("[C6] ZoneChange: alarm=%d tamper=%d\n", g_alarm, g_tamper);
        } else if (evt.type == ZbEvent::BATTERY_UPDATE && evt.battery >= 0) {
            g_battery = evt.battery;
            g_sensorJoined        = true;
            g_lastSensorActivityMs = now;
            DBG_PRINTF("[C6] Batteria: %d%%\n", g_battery);
            sendJsonFrame(g_alarm, g_tamper, g_battery, g_lqi, g_alarm ? 1 : 0, true, false);
            g_lastHB = now;
        } else if (evt.type == ZbEvent::DEVICE_JOINED) {
            g_sensorJoined        = true;
            g_lastSensorActivityMs = now;
            DBG_PRINTF("[C6] Dispositivo unito (loop): 0x%04X\n", evt.short_addr);
            char buf[80];
            snprintf(buf, sizeof(buf),
                     "{\"event\":\"device_joined\",\"short_addr\":\"0x%04X\",\"seq\":%lu}\n",
                     evt.short_addr, (unsigned long)++g_seq);
            HostSerial.write(reinterpret_cast<const uint8_t*>(buf), strlen(buf));
            if (g_networkOpen) {
                g_networkOpenUntilMs = now + 30000;  // chiudi tra 30 s dopo il join
            }
        }
    }

    // Timeout rete aperta
    if (g_networkOpen && now >= g_networkOpenUntilMs) {
        g_networkOpen = false;
        sendEventFrame("network_closed");
        DBG_PRINTLN("[C6] Finestra di pairing chiusa (timeout)");
    }

    // Heartbeat periodico
    if (now - g_lastHB >= HEARTBEAT_MS) {
        g_lastHB = now;
        sendBridgeHeartbeatEvent();
        if (g_sensorJoined || g_sensorEverSeen) {
            bool sleeping = g_sensorJoined && !g_sensorEverSeen && !g_alarm && !g_tamper;
            sendJsonFrame(g_alarm, g_tamper, g_battery, g_lqi, 0, g_sensorJoined, sleeping);
        }
    }

    // Elabora ACK / comandi dall'host
    pollHostAck();

    // Timeout attività sensore: segna come inattivo senza perdere lo stato di pairing
    static const uint32_t SENSOR_ACTIVITY_TIMEOUT_MS = 90000UL;
    static bool sensorInactiveLogged = false;
    if (g_sensorEverSeen && g_lastSensorActivityMs > 0 &&
        (now - g_lastSensorActivityMs) >= SENSOR_ACTIVITY_TIMEOUT_MS) {
        g_sensorEverSeen = false;
        g_alarm  = false;
        g_tamper = false;
        if (!sensorInactiveLogged) {
            DBG_PRINTF("[C6] Sensore inattivo da %lus — stato: ONLINE_SLEEP (pairing mantenuto)\n",
                       (now - g_lastSensorActivityMs) / 1000UL);
            sensorInactiveLogged = true;
        }
    }
    if (g_lastSensorActivityMs > 0 &&
        (now - g_lastSensorActivityMs) < SENSOR_ACTIVITY_TIMEOUT_MS) {
        sensorInactiveLogged = false;
    }

    // Riapri automaticamente lo steering per end-device orfani in deep backoff
    // (gli end device ritentano il re-join ogni 15–30 min dopo disconnessione prolungata)
    if (!g_sensorJoined && !g_sensorEverSeen && !g_networkOpen &&
        (now - g_lastReopenMs) >= REJOIN_REOPEN_PAUSE_MS) {
        g_lastReopenMs       = now;
        g_networkOpen        = true;
        g_networkOpenUntilMs = now + (uint32_t)BOOT_NETWORK_SECS * 1000UL;
        DBG_PRINTLN("[C6] Sensore non connesso — riapertura rete (attesa indefinita)");
        requestNetworkSteering();
    }

    // Pulsante BOOT: apre finestra di pairing
    static uint32_t lastBtnMs = 0;
    if (now - lastBtnMs > 300 && digitalRead(BOOT_BUTTON_PIN) == LOW) {
        lastBtnMs = now;
        DBG_PRINTF("[C6] Pulsante boot: apertura finestra di pairing (%ds)\n", OPEN_NETWORK_SECS);
        g_networkOpen        = true;
        g_networkOpenUntilMs = now + (uint32_t)OPEN_NETWORK_SECS * 1000UL;
        requestNetworkSteering();
        sendEventFrame("network_open", OPEN_NETWORK_SECS);
    }

    delay(10);
}
