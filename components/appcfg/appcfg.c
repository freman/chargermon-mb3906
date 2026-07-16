#include "appcfg.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "appcfg";

#define NS            "cfg"       // NVS namespace
#define SCHEMA_VER    1           // bump when a stored field's meaning changes

// Stored per-field (not as one blob) so adding a field later just reads back the
// default when its key is absent - forward/backward compatible without a wipe.
#define K_WIFI_SSID   "wifi_ssid"
#define K_WIFI_PSK    "wifi_psk"
#define K_MQTT_HOST   "mqtt_host"
#define K_MQTT_PORT   "mqtt_port"
#define K_MQTT_USER   "mqtt_user"
#define K_MQTT_PASS   "mqtt_pass"
#define K_MQTT_PREFIX "mqtt_prefix"
#define K_ADMIN_PASS  "admin_pass"
#define K_AP_PASS     "ap_pass"
#define K_AP_TIMEOUT  "ap_to"
#define K_LAST_PROG   "last_prog"
#define K_SCHEMA_VER  "schema_ver"

static app_config_t s_cfg;

static void set_defaults(app_config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->mqtt_port            = 1883;
    strcpy(c->mqtt_prefix, "homeassistant");
    c->fallback_ap_timeout_s = 120;
}

// Copy an NVS string key into dst[cap] if present; leave dst untouched otherwise.
static void load_str(nvs_handle_t h, const char *key, char *dst, size_t cap)
{
    size_t len = cap;
    if (nvs_get_str(h, key, dst, &len) != ESP_OK) {
        return;   // absent -> keep the default already in dst
    }
}

void appcfg_init(void)
{
    set_defaults(&s_cfg);

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no stored config; using defaults (unprovisioned)");
        return;
    }

    uint16_t ver = 0;
    nvs_get_u16(h, K_SCHEMA_VER, &ver);

    load_str(h, K_WIFI_SSID,   s_cfg.wifi_ssid,   sizeof(s_cfg.wifi_ssid));
    load_str(h, K_WIFI_PSK,    s_cfg.wifi_psk,    sizeof(s_cfg.wifi_psk));
    load_str(h, K_MQTT_HOST,   s_cfg.mqtt_host,   sizeof(s_cfg.mqtt_host));
    nvs_get_u16(h, K_MQTT_PORT, &s_cfg.mqtt_port);
    load_str(h, K_MQTT_USER,   s_cfg.mqtt_user,   sizeof(s_cfg.mqtt_user));
    load_str(h, K_MQTT_PASS,   s_cfg.mqtt_pass,   sizeof(s_cfg.mqtt_pass));
    load_str(h, K_MQTT_PREFIX, s_cfg.mqtt_prefix, sizeof(s_cfg.mqtt_prefix));
    load_str(h, K_ADMIN_PASS,  s_cfg.admin_pass,  sizeof(s_cfg.admin_pass));
    load_str(h, K_AP_PASS,     s_cfg.fallback_ap_pass, sizeof(s_cfg.fallback_ap_pass));
    nvs_get_u16(h, K_AP_TIMEOUT, &s_cfg.fallback_ap_timeout_s);
    nvs_get_u8(h, K_LAST_PROG, &s_cfg.last_program);

    nvs_close(h);

    // Schema migrations would run here (ver < SCHEMA_VER). None yet.
    ESP_LOGI(TAG, "config loaded (schema v%u, provisioned=%d)", ver,
             appcfg_is_provisioned());
}

const app_config_t *appcfg_get(void)
{
    return &s_cfg;
}

esp_err_t appcfg_save(const app_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    // Best-effort: set all keys, then commit once. First error aborts.
    err = nvs_set_str(h, K_WIFI_SSID,   cfg->wifi_ssid);
    if (err == ESP_OK) err = nvs_set_str(h, K_WIFI_PSK,    cfg->wifi_psk);
    if (err == ESP_OK) err = nvs_set_str(h, K_MQTT_HOST,   cfg->mqtt_host);
    if (err == ESP_OK) err = nvs_set_u16(h, K_MQTT_PORT,   cfg->mqtt_port);
    if (err == ESP_OK) err = nvs_set_str(h, K_MQTT_USER,   cfg->mqtt_user);
    if (err == ESP_OK) err = nvs_set_str(h, K_MQTT_PASS,   cfg->mqtt_pass);
    if (err == ESP_OK) err = nvs_set_str(h, K_MQTT_PREFIX, cfg->mqtt_prefix);
    if (err == ESP_OK) err = nvs_set_str(h, K_ADMIN_PASS,  cfg->admin_pass);
    if (err == ESP_OK) err = nvs_set_str(h, K_AP_PASS,     cfg->fallback_ap_pass);
    if (err == ESP_OK) err = nvs_set_u16(h, K_AP_TIMEOUT,  cfg->fallback_ap_timeout_s);
    if (err == ESP_OK) err = nvs_set_u8(h, K_LAST_PROG,    cfg->last_program);
    if (err == ESP_OK) err = nvs_set_u16(h, K_SCHEMA_VER,  SCHEMA_VER);
    if (err == ESP_OK) err = nvs_commit(h);

    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "appcfg_save failed: %s", esp_err_to_name(err));
        return err;
    }

    s_cfg = *cfg;   // refresh cache only after a durable commit
    ESP_LOGI(TAG, "config saved");
    return ESP_OK;
}

esp_err_t appcfg_set_last_program(uint8_t program)
{
    if (s_cfg.last_program == program) {
        return ESP_OK;   // no change; spare the flash
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(h, K_LAST_PROG, program);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) {
        s_cfg.last_program = program;
    }

    return err;
}

bool appcfg_is_provisioned(void)
{
    return s_cfg.wifi_ssid[0] != '\0';
}

void appcfg_factory_reset(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    set_defaults(&s_cfg);
    ESP_LOGW(TAG, "config factory reset");
}
