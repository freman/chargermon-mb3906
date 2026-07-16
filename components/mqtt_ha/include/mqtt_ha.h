// mqtt_ha - MQTT client + Home Assistant discovery.
//
// Connects to the configured broker, publishes retained HA discovery configs for
// the charger's sensors + controls, streams state as one JSON per change, and
// routes command topics to the control layer. Availability follows the BLE link
// - entities go unavailable when the charger is not connected, and the
// MQTT LWT covers a full device drop. No-op if no broker is configured.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Start the MQTT client. Requires appcfg_init(), wifi_start(), charger_state_init()
// to have run. Non-blocking; esp-mqtt reconnects on its own once the network is up.
void mqtt_ha_start(void);

// True while connected to the broker (for the web status page / diagnostics).
bool mqtt_ha_connected(void);

#ifdef __cplusplus
}
#endif
