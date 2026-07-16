#include "mqtt_ha.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "mqtt_client.h"

#include "appcfg.h"
#include "wifi.h"
#include "charger_state.h"
#include "charger_control.h"
#include "charger_proto.h"

static const char *TAG = "mqtt_ha";

static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;
static int  s_avail = -1;              // last availability: -1 unknown, 0/1

static char s_devid[24];               // "chargermon_A651"
static char s_suffix[8];               // "A651"
static char s_base[40];                // "chargermon/A651"
static char s_prefix[32];              // HA discovery prefix
static char s_avail_topic[56];
static char s_state_topic[56];
static char s_t_program[64], s_t_charge[64], s_t_refresh[64];

bool mqtt_ha_connected(void)
{
    return s_connected;
}

// ==========================================================================
// State + availability publishing
// ==========================================================================
static void publish_availability(bool online)
{
    if (!s_connected || s_avail == (int)online) {
        return;
    }
    s_avail = online;
    esp_mqtt_client_publish(s_client, s_avail_topic, online ? "online" : "offline",
                            0, 1, true);
}

static void publish_state(const charger_state_t *s)
{
    if (!s_connected) {
        return;
    }
    char j[384];
    int n = snprintf(j, sizeof(j),
        "{\"voltage\":%.1f,\"current\":%.2f,\"capacity\":%u,"
        "\"step\":%u,\"step_text\":\"%s\",\"state\":%u,"
        "\"program\":%u,\"program_text\":\"%s\",\"error\":%u,"
        "\"ah\":%.3f,\"link\":\"%s\",\"rssi\":%d,"
        "\"charging\":%s,\"battery\":%s}",
        s->voltage_v, s->current_a, s->capacity_pct,
        s->step, cp_step_name(s->step), s->charger_state,
        s->program, cp_program_name(s->program), s->error,
        s->charge_delivered_ah, cs_link_name(s->link), wifi_get_rssi(),
        s->charging ? "true" : "false", s->battery_present ? "true" : "false");
    esp_mqtt_client_publish(s_client, s_state_topic, j, n, 0, false);
}

// charger_state observer (state-task context): push state + availability.
static void on_state(const charger_state_t *snap, void *ctx)
{
    (void)ctx;
    publish_availability(snap->link == CB_LINK_READY);
    publish_state(snap);
}

// ==========================================================================
// Home Assistant discovery
// ==========================================================================
// Publish one retained discovery config. `fields` is the entity-specific JSON;
// the wrapper adds the shared unique_id / availability / device / state_topic.
static void disc(const char *comp, const char *obj, const char *fields)
{
    char topic[160], body[1024];
    snprintf(topic, sizeof(topic), "%s/%s/%s/%s/config", s_prefix, comp, s_devid, obj);
    int n = snprintf(body, sizeof(body),
        "{\"unique_id\":\"%s_%s\",\"object_id\":\"%s_%s\","
        "\"availability_topic\":\"%s\",\"state_topic\":\"%s\","
        "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"Charger Monitor %s\","
        "\"model\":\"Powertech MB3906\",\"manufacturer\":\"Fremnet\"},%s}",
        s_devid, obj, s_devid, obj, s_avail_topic, s_state_topic,
        s_devid, s_suffix, fields);
    esp_mqtt_client_publish(s_client, topic, body, n, 1, true);
}

static void sensor(const char *obj, const char *name, const char *unit,
                   const char *dev_class, const char *tmpl)
{
    char u[48] = "", d[48] = "";
    if (unit)      snprintf(u, sizeof(u), ",\"unit_of_measurement\":\"%s\"", unit);
    if (dev_class) snprintf(d, sizeof(d), ",\"device_class\":\"%s\"", dev_class);
    char f[320];
    snprintf(f, sizeof(f), "\"name\":\"%s\",\"value_template\":\"%s\"%s%s",
             name, tmpl, u, d);
    disc("sensor", obj, f);
}

static void publish_discovery(void)
{
    sensor("voltage",  "Voltage",  "V",   "voltage",         "{{ value_json.voltage }}");
    sensor("current",  "Current",  "A",   "current",         "{{ value_json.current }}");
    sensor("capacity", "Capacity", "%",   "battery",         "{{ value_json.capacity }}");
    sensor("rssi",     "WiFi RSSI", "dBm", "signal_strength", "{{ value_json.rssi }}");
    sensor("ah",       "Charge delivered", "Ah", NULL,        "{{ value_json.ah }}");
    sensor("step",     "Charge step",   NULL, NULL, "{{ value_json.step_text }}");
    sensor("program",  "Program",       NULL, NULL, "{{ value_json.program_text }}");
    sensor("cstate",   "Charger state", NULL, NULL, "{{ value_json.state }}");
    sensor("error",    "Last error",    NULL, NULL, "{{ value_json.error }}");
    sensor("link",     "BLE link",      NULL, NULL, "{{ value_json.link }}");

    // Program select: options mirror cp_program_name so the reported program_text
    // matches an option. Command routes to setBatteryMode.
    char opts[256] = "[";
    for (int i = 0; i <= 8; i++) {
        char one[32];
        snprintf(one, sizeof(one), "%s\"%s\"", i ? "," : "", cp_program_name(i));
        strlcat(opts, one, sizeof(opts));
    }
    strlcat(opts, "]", sizeof(opts));
    char f[512];
    snprintf(f, sizeof(f),
        "\"name\":\"Program\",\"command_topic\":\"%s\",\"options\":%s,"
        "\"value_template\":\"{{ value_json.program_text }}\"", s_t_program, opts);
    disc("select", "program_sel", f);

    // Charging switch: OFF -> standby (reliable), ON -> enableBattery.
    snprintf(f, sizeof(f),
        "\"name\":\"Charging\",\"command_topic\":\"%s\",\"payload_on\":\"ON\","
        "\"payload_off\":\"OFF\",\"value_template\":"
        "\"{{ 'ON' if value_json.charging else 'OFF' }}\"", s_t_charge);
    disc("switch", "charge_sw", f);

    // Refresh button: re-poll discrete state.
    snprintf(f, sizeof(f), "\"name\":\"Refresh\",\"command_topic\":\"%s\"", s_t_refresh);
    disc("button", "refresh_btn", f);
}

// ==========================================================================
// Command routing
// ==========================================================================
static bool topic_is(const char *topic, int tlen, const char *want)
{
    return tlen == (int)strlen(want) && strncmp(topic, want, tlen) == 0;
}

static void handle_command(const char *topic, int tlen, const char *data, int dlen)
{
    char val[32];
    int vl = dlen < (int)sizeof(val) - 1 ? dlen : (int)sizeof(val) - 1;
    memcpy(val, data, vl);
    val[vl] = '\0';

    if (topic_is(topic, tlen, s_t_program)) {
        for (int i = 0; i <= 8; i++) {
            if (!strcmp(val, cp_program_name(i))) {
                ESP_LOGI(TAG, "cmd program '%s' (%d) -> %d", val, i,
                         charger_control_set_program(i));
                return;
            }
        }
        ESP_LOGW(TAG, "unknown program '%s'", val);
    } else if (topic_is(topic, tlen, s_t_charge)) {
        // ON resumes the last user-selected program (setBatteryMode is the only
        // reliable start; enableBattery alone does nothing, PROTOCOL.md §8). OFF
        // is the verified stop. ON before any program was ever selected -> RANGE.
        cc_result_t r = strcmp(val, "ON") == 0 ? charger_control_start_last()
                                               : charger_control_stop();
        if (r == CC_ERR_RANGE) {
            ESP_LOGW(TAG, "charge ON with no stored program; use the Program select first");
        }
        ESP_LOGI(TAG, "cmd charge '%s' -> %d", val, r);
    } else if (topic_is(topic, tlen, s_t_refresh)) {
        ESP_LOGI(TAG, "cmd refresh -> %d", charger_control_refresh());
    }
}

// ==========================================================================
// MQTT events
// ==========================================================================
static void on_mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    esp_mqtt_event_handle_t e = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected to broker");
        s_connected = true;
        s_avail = -1;
        publish_discovery();
        esp_mqtt_client_subscribe(s_client, s_t_program, 1);
        esp_mqtt_client_subscribe(s_client, s_t_charge, 1);
        esp_mqtt_client_subscribe(s_client, s_t_refresh, 1);
        {
            charger_state_t snap;
            charger_state_get(&snap);
            publish_availability(snap.link == CB_LINK_READY);
            publish_state(&snap);
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        break;
    case MQTT_EVENT_DATA:
        handle_command(e->topic, e->topic_len, e->data, e->data_len);
        break;
    default:
        break;
    }
}

// ==========================================================================
// Public API
// ==========================================================================
void mqtt_ha_start(void)
{
    const app_config_t *c = appcfg_get();
    if (c->mqtt_host[0] == '\0') {
        ESP_LOGI(TAG, "no broker configured; MQTT disabled");
        return;
    }

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_suffix, sizeof(s_suffix), "%02X%02X", mac[4], mac[5]);
    snprintf(s_devid, sizeof(s_devid), "chargermon_%s", s_suffix);
    snprintf(s_base, sizeof(s_base), "chargermon/%s", s_suffix);
    strlcpy(s_prefix, c->mqtt_prefix[0] ? c->mqtt_prefix : "homeassistant", sizeof(s_prefix));
    snprintf(s_avail_topic, sizeof(s_avail_topic), "%s/availability", s_base);
    snprintf(s_state_topic, sizeof(s_state_topic), "%s/state", s_base);
    snprintf(s_t_program, sizeof(s_t_program), "%s/program/set", s_base);
    snprintf(s_t_charge,  sizeof(s_t_charge),  "%s/charge/set",  s_base);
    snprintf(s_t_refresh, sizeof(s_t_refresh), "%s/refresh/set", s_base);

    esp_mqtt_client_config_t cfg = {
        .broker.address.hostname  = c->mqtt_host,
        .broker.address.port      = c->mqtt_port ? c->mqtt_port : 1883,
        .broker.address.transport = MQTT_TRANSPORT_OVER_TCP,
        .credentials.client_id    = s_devid,
        .session.last_will = {
            .topic  = s_avail_topic,
            .msg    = "offline",
            .qos    = 1,
            .retain = 1,
        },
    };
    if (c->mqtt_user[0]) {
        cfg.credentials.username = c->mqtt_user;
        cfg.credentials.authentication.password = c->mqtt_pass;
    }

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "client init failed");
        return;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_mqtt_event, NULL);
    charger_state_add_observer(on_state, NULL);
    esp_mqtt_client_start(s_client);
    ESP_LOGI(TAG, "started (broker %s:%u, id %s)", c->mqtt_host,
             cfg.broker.address.port, s_devid);
}
