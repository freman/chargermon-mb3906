// ota - over-the-air firmware update.
//
// A push .bin upload to POST /api/ota (registered by the web component, gated by
// the same admin Basic-auth) is streamed into the inactive A/B slot, then booted.
// The new image comes up in "pending verify" and is only confirmed after a short
// healthy uptime; a boot loop before that auto-rolls back to the previous slot.
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// Call early in app_main. If the running image is pending-verify, schedules the
// health confirmation (marks the slot valid after a short delay, else rollback).
void ota_init(void);

// HTTP handler body for POST /api/ota: streams the request body into the next OTA
// partition, sets it bootable, responds, and reboots. Rejects a concurrent upload.
// The caller (web) enforces auth first.
esp_err_t ota_perform(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
