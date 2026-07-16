// web - status page + JSON API + config/control over HTTP.
//
// A lightweight esp_http_server on the STA interface, started when WiFi connects
// and stopped when it drops (so it never clashes with the SoftAP captive portal).
// Reads are open (status page, GET /api/state); config, control and OTA-adjacent
// writes require HTTP Basic auth with the admin credential. Best-effort:
// never blocks BLE/MQTT.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Start the web task. Requires appcfg/wifi/charger_state to be initialised.
void web_start(void);

#ifdef __cplusplus
}
#endif
