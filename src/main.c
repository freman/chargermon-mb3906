// Charger Monitor - ESP32-C3 firmware entry point.
//
// See docs/PROTOCOL.md for the reverse-engineered charger protocol.
//
// app_main wires together the components: BLE monitor/control, NVS config,
// WiFi (STA + captive-portal provisioning), MQTT + Home Assistant discovery,
// web UI + JSON API + SSE, OTA with A/B rollback, and the OLED/LED ui. See the
// roadmap checklist at the bottom for what lives where.

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

#include "charger_ble.h"
#include "charger_proto.h"
#include "charger_state.h"
#include "charger_control.h"
#include "appcfg.h"
#include "wifi.h"
#include "mqtt_ha.h"
#include "web.h"
#include "ota.h"
#include "ui.h"

static const char *TAG = "main";

// Build stamp - logged at boot and echoed periodically so it is obvious whether
// the firmware actually running is the latest flash. build_info.h is regenerated
// each build by tools/stamp_touch.py (content changes -> real recompile, unlike a
// stale __DATE__/__TIME__). Fallback keeps the file compilable on its own.
#if __has_include("build_info.h")
#include "build_info.h"
#else
#define FW_BUILD_STAMP (__DATE__ " " __TIME__)
#endif

// Runs in the NimBLE host task on every decoded field. Hand off to the state
// owner (queue) and return fast; no work happens on the host task.
static void on_charger_telemetry(const cp_state_t *st, uint8_t opcode, void *ctx)
{
    (void)ctx;
    charger_state_submit(st, opcode);
}

// A state observer that logs each published update. Stands in for the
// MQTT / web / OLED consumers until those components exist; proves the fan-out.
static void log_observer(const charger_state_t *s, void *ctx)
{
    (void)ctx;
    // Re-echo the build stamp roughly every 30 s so the running firmware is
    // always identifiable in the log without needing a reboot.
    static uint32_t last_stamp_ms;
    uint32_t now_ms = esp_log_timestamp();
    if (now_ms - last_stamp_ms >= 30000) {
        last_stamp_ms = now_ms;
        ESP_LOGI(TAG, "firmware build: %s", FW_BUILD_STAMP);
    }
    ESP_LOGI(TAG, "[%s] %.1fV %.2fA %u%% step=%u(%s) prog=%u(%s) %s%s%.3fAh",
             cs_link_name(s->link), s->voltage_v, s->current_a, s->capacity_pct,
             s->step, cp_step_name(s->step), s->program, cp_program_name(s->program),
             s->battery_present ? "batt " : "no-batt ",
             s->error ? "FAULT " : "",
             s->charge_delivered_ah);
}

// -------------------------------------------------------------------------
// TEMPORARY test console - single-keypress trigger to exercise control on real
// hardware. This is NOT the console component; it exists only to drive
// start/stop/program until MQTT/web/console land, then it comes out.
// -------------------------------------------------------------------------
static bool s_auto_stop_on;

static void test_help(void)
{
    char ip[16];
    wifi_get_ip(ip, sizeof(ip));
    printf("\n== charger control test ==\n"
           "  1-8  set program (3=car 5=bike 7=lithium 8=recovery)\n"
           "  0/s  stop (standby)\n"
           "  e/d  enableBattery on/off (secondary)\n"
           "  r    refresh (re-poll status/error/step)\n"
           "  a    toggle auto-stop-on-connect (now: %s)\n"
           "  w    set WiFi creds (ssid psk) then reboot\n"
           "  C    factory-reset config then reboot\n"
           "  ?    this help\n"
           "wifi: %s %s\n"
           "WARNING: 1-8 start a real charge cycle on the connected battery.\n\n",
           s_auto_stop_on ? "on" : "off", wifi_state_name(wifi_get_state()), ip);
}

// Blocking line reader over USB-Serial-JTAG for the recovery commands. Echoes,
// handles backspace, returns the line (without newline) in buf.
static void read_line(const char *prompt, char *buf, size_t cap)
{
    printf("%s", prompt);
    fflush(stdout);
    size_t n = 0;
    for (;;) {
        uint8_t ch;
        if (usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(100)) <= 0) {
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            break;
        }
        if ((ch == 0x7f || ch == 0x08) && n > 0) {   // backspace
            n--;
            printf("\b \b");
            fflush(stdout);
            continue;
        }
        if (ch >= 0x20 && n < cap - 1) {
            buf[n++] = (char)ch;
            putchar(ch);
            fflush(stdout);
        }
    }
    buf[n] = '\0';
    putchar('\n');
}

// Serial recovery: `w` provisions WiFi (ssid psk) and reboots.
static void cmd_set_wifi(void)
{
    char line[128];
    read_line("ssid psk: ", line, sizeof(line));
    char *sp = strchr(line, ' ');
    if (!sp) {
        printf("usage: <ssid> <psk>\n");
        return;
    }
    *sp = '\0';
    app_config_t cfg = *appcfg_get();
    strlcpy(cfg.wifi_ssid, line, sizeof(cfg.wifi_ssid));
    strlcpy(cfg.wifi_psk, sp + 1, sizeof(cfg.wifi_psk));
    if (appcfg_save(&cfg) == ESP_OK) {
        printf("saved; rebooting...\n");
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    } else {
        printf("save failed\n");
    }
}

static void test_console_task(void *arg)
{
    (void)arg;
    test_help();
    for (;;) {
        uint8_t ch;
        if (usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(100)) <= 0) {
            continue;
        }

        cc_result_t r = CC_OK;
        if (ch >= '1' && ch <= '8') {
            r = charger_control_start(ch - '0');
            ESP_LOGI(TAG, "start program %c (%s) -> %d",
                     ch, cp_program_name(ch - '0'), r);
        } else switch (ch) {
        case '0':
        case 's':
        case 'S':
            r = charger_control_stop();
            ESP_LOGI(TAG, "stop -> %d", r);
            break;
        case 'e': r = charger_control_enable(true);  ESP_LOGI(TAG, "enable on -> %d", r);  break;
        case 'd': r = charger_control_enable(false); ESP_LOGI(TAG, "enable off -> %d", r); break;
        case 'r': r = charger_control_refresh();     ESP_LOGI(TAG, "refresh -> %d", r);    break;
        case 'a':
            s_auto_stop_on = !s_auto_stop_on;
            charger_control_set_auto_stop(s_auto_stop_on);
            ESP_LOGI(TAG, "auto-stop-on-connect: %s", s_auto_stop_on ? "on" : "off");
            break;
        case 'w':
            cmd_set_wifi();
            break;
        case 'C':
            appcfg_factory_reset();
            printf("config reset; rebooting...\n");
            vTaskDelay(pdMS_TO_TICKS(300));
            esp_restart();
            break;
        case '?':
        case '\r':
        case '\n':
            test_help();
            break;
        default:
            break;
        }

        if (r == CC_ERR_NOT_READY) {
            ESP_LOGW(TAG, "  (rejected: BLE link not ready)");
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Charger Monitor - Powertech MB3906 bridge (BLE slice)");
    ESP_LOGI(TAG, "firmware build: %s", FW_BUILD_STAMP);

    // NVS is required by the BLE stack (and later WiFi/config).
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // OTA rollback check: if we just booted a freshly-flashed image, confirm it
    // healthy after a short delay, else the bootloader reverts.
    ota_init();

    // Config store + WiFi. WiFi runs in its own task and never blocks BLE;
    // an outage only pauses (future) MQTT publishing.
    appcfg_init();
    wifi_start();

    // State owner first: it must be draining its queue before BLE starts
    // submitting telemetry.
    charger_state_init();
    charger_state_add_observer(log_observer, NULL);

    // Control: serialized writes + boot-safety. Never auto-starts charging;
    // auto-stop-on-connect stays off until a trigger enables it.
    charger_control_init();

    charger_ble_start(on_charger_telemetry, NULL);

    // MQTT + Home Assistant discovery. Registers a state observer; no-op until a
    // broker is provisioned. Reconnects on its own once WiFi is up.
    mqtt_ha_start();

    // Web UI + JSON API on the STA interface (starts once WiFi connects).
    web_start();

    // OLED status display + onboard WiFi LED. No-op for the panel if none is
    // wired (it probes I2C and logs); the LED still reflects WiFi state.
    ui_start();

    // Temporary keypress trigger over native USB-Serial-JTAG (see above).
    usb_serial_jtag_driver_config_t ucfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    if (usb_serial_jtag_driver_install(&ucfg) == ESP_OK) {
        usb_serial_jtag_vfs_use_driver();
        xTaskCreate(test_console_task, "testcon", 4096, NULL, 4, NULL);
    } else {
        ESP_LOGW(TAG, "USB-Serial-JTAG driver install failed; test console off");
    }

    // ---------------------------------------------------------------------
    // ROADMAP - each becomes its own component under components/:
    //
    //   [x] state    - single owner of ChargerState + Ah integrator
    //   [x] control  - serialized writes to fff1: setBatteryMode start/stop (PROTOCOL.md §8)
    //   [x] wifi     - STA + SoftAP captive-portal provisioning
    //   [x] mqtt     - broker client + HA discovery, availability = BLE link
    //   [x] web      - esp_http_server: status page + JSON API (open read,
    //                  basic-auth writes)
    //   [x] ui       - SSD1306 OLED (72x40): capacity, stage n/8, V<->I alternate;
    //                  onboard LED = WiFi state
    //   [x] ota      - password-protected push upload, A/B rollback
    //   [ ] console  - USB serial: PROGRAM/START/STOP, WIFI/MQTT config, RESET
    // ---------------------------------------------------------------------
}
