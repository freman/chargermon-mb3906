// wifi - station connectivity + first-boot SoftAP captive portal.
//
// Station mode connects from the stored config. Unprovisioned, it opens an OPEN
// captive-portal AP for one-time setup; a provisioned unit that cannot connect
// falls back to a WPA2 AP (config.fallback_ap_pass) that times out back to STA
// retry. All work runs in the wifi task/handlers, so a WiFi outage never blocks
// BLE monitoring. Getters expose state/IP/RSSI for display + MQTT.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_DOWN = 0,      // idle / not started
    WIFI_CONNECTING,    // STA associating / awaiting IP
    WIFI_CONNECTED,     // STA has an IP
    WIFI_PORTAL,        // SoftAP captive portal is up
} wifi_state_t;

// Start WiFi. Non-blocking: brings up netif + the wifi task and returns. Requires
// nvs_flash_init() and config_init() to have run.
void wifi_start(void);

// Current state, for UI / MQTT availability.
wifi_state_t wifi_get_state(void);

// STA IPv4 as a string ("0.0.0.0" when not connected). Copies into out[cap].
void wifi_get_ip(char *out, size_t cap);

// STA RSSI in dBm (0 when not connected). Exposed as an MQTT sensor.
int8_t wifi_get_rssi(void);

// Human-readable state name.
const char *wifi_state_name(wifi_state_t s);

#ifdef __cplusplus
}
#endif
