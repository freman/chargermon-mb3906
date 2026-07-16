// appcfg - NVS-backed device configuration.
//
// One provisioning form (captive portal) fills this whole struct in a single
// save; the wifi component reads its part now, mqtt/web/ota read theirs later.
// A schema version key lets fields be added without wiping the user's settings.
// (Named appcfg_* to avoid the ESP-IDF bt stack's own config_* symbols.)
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Field sizes include room for the NUL terminator. WiFi max SSID is 32,
// WPA2 PSK max 63 (+NUL). WPA2 requires an AP password of at least 8 chars.
typedef struct {
    // WiFi station.
    char     wifi_ssid[33];
    char     wifi_psk[64];

    // MQTT broker + Home Assistant discovery (consumed by the mqtt component).
    char     mqtt_host[64];
    uint16_t mqtt_port;
    char     mqtt_user[33];
    char     mqtt_pass[64];
    char     mqtt_prefix[32];           // HA discovery prefix, e.g. "homeassistant"

    // Web-write / OTA credential (consumed by the web + ota components).
    char     admin_pass[64];

    // Fallback provisioning AP (used when a provisioned STA cannot connect).
    // WPA2-protected with this password; if no client associates within the
    // timeout, the AP closes and STA retry resumes. Password must be >= 8 chars
    // for WPA2, else the fallback AP falls back to open.
    char     fallback_ap_pass[64];
    uint16_t fallback_ap_timeout_s;

    // Last user-selected charge program, persisted by charger_control
    // when a start confirms. 0 = none yet; the HA Charging switch ON resumes it.
    uint8_t  last_program;
} app_config_t;

// Load the cached config from NVS (defaults if absent) and migrate old schemas.
// Call once after nvs_flash_init(), before wifi_start().
void appcfg_init(void);

// Read-only pointer to the cached config. Stable until the next appcfg_save().
const app_config_t *appcfg_get(void);

// Persist the whole config to NVS and refresh the cache. Returns ESP_OK on success.
esp_err_t appcfg_save(const app_config_t *cfg);

// Persist just the last-selected program (single NVS key; avoids rewriting the
// credential fields on every charge start).
esp_err_t appcfg_set_last_program(uint8_t program);

// True once WiFi credentials exist (i.e. we have been provisioned at least once).
bool appcfg_is_provisioned(void);

// Erase all stored config (serial recovery). Cache resets to defaults.
void appcfg_factory_reset(void);

#ifdef __cplusplus
}
#endif
