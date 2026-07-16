#include "wifi.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"

#include "appcfg.h"

static const char *TAG = "wifi";

// --- tuning ---------------------------------------------------------------
#define MAX_STA_FAILS   10     // consecutive STA failures before fallback AP
#define BACKOFF_CAP_MS  30000  // reconnect backoff ceiling
#define AP_IP           "192.168.4.1"

// --- event-group bits (handlers -> control task) --------------------------
#define EV_GOT_IP       BIT0
#define EV_DISCONNECTED BIT1
#define EV_AP_CLIENT    BIT2

// --- module state ---------------------------------------------------------
static wifi_state_t       s_state;
static EventGroupHandle_t s_events;
static esp_netif_t       *s_sta_netif;
static esp_netif_t       *s_ap_netif;
static httpd_handle_t     s_httpd;
static volatile bool      s_dns_run;
static int                s_dns_sock = -1;
static char               s_ip[16] = "0.0.0.0";
static volatile int       s_ap_clients;

// ==========================================================================
// Event handling
// ==========================================================================
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_DISCONNECTED:
            strcpy(s_ip, "0.0.0.0");
            xEventGroupSetBits(s_events, EV_DISCONNECTED);
            break;
        case WIFI_EVENT_AP_STACONNECTED:
            s_ap_clients++;
            xEventGroupSetBits(s_events, EV_AP_CLIENT);
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            if (s_ap_clients > 0) s_ap_clients--;
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        esp_ip4addr_ntoa(&e->ip_info.ip, s_ip, sizeof(s_ip));
        ESP_LOGI(TAG, "STA got IP %s", s_ip);
        xEventGroupSetBits(s_events, EV_GOT_IP);
    }
}

// ==========================================================================
// Captive-portal DNS: answer every A query with the AP IP so any lookup lands
// on our HTTP server.
// ==========================================================================
static void dns_task(void *arg)
{
    uint8_t buf[512];
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(53),
                              .sin_addr.s_addr = htonl(INADDR_ANY) };
    s_dns_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_dns_sock < 0 || bind(s_dns_sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        ESP_LOGW(TAG, "DNS socket setup failed");
        if (s_dns_sock >= 0) close(s_dns_sock);
        s_dns_sock = -1;
        vTaskDelete(NULL);
        return;
    }

    struct in_addr apip;
    inet_aton(AP_IP, &apip);

    while (s_dns_run) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(s_dns_sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (n < 12) {
            continue;   // too small to be a DNS query (or socket closed)
        }
        // Build a response over the request: flags = standard response, 1 answer,
        // then append an A record pointing at the AP. Question section is echoed.
        buf[2] = 0x81; buf[3] = 0x80;   // QR=1, RD/RA
        buf[6] = 0x00; buf[7] = 0x01;   // ANCOUNT = 1
        buf[8] = buf[9] = buf[10] = buf[11] = 0;   // NS/AR counts = 0
        int len = n;
        uint8_t ans[] = {
            0xc0, 0x0c,             // name -> pointer to question
            0x00, 0x01, 0x00, 0x01, // type A, class IN
            0x00, 0x00, 0x00, 0x3c, // TTL 60s
            0x00, 0x04,             // rdlength 4
            0, 0, 0, 0,             // rdata (filled below)
        };
        memcpy(ans + 12, &apip.s_addr, 4);
        if (len + (int)sizeof(ans) <= (int)sizeof(buf)) {
            memcpy(buf + len, ans, sizeof(ans));
            len += sizeof(ans);
            sendto(s_dns_sock, buf, len, 0, (struct sockaddr *)&from, fl);
        }
    }

    if (s_dns_sock >= 0) close(s_dns_sock);
    s_dns_sock = -1;
    vTaskDelete(NULL);
}

// ==========================================================================
// Captive-portal HTTP: config form
// ==========================================================================
static void url_decode(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '+') {
            *o++ = ' ';
        } else if (*p == '%' && p[1] && p[2]) {
            int hi = p[1], lo = p[2];
            hi = hi <= '9' ? hi - '0' : (hi | 0x20) - 'a' + 10;
            lo = lo <= '9' ? lo - '0' : (lo | 0x20) - 'a' + 10;
            *o++ = (char)((hi << 4) | lo);
            p += 2;
        } else {
            *o++ = *p;
        }
    }
    *o = '\0';
}

// Extract and URL-decode one x-www-form-urlencoded field from `body` into out.
static bool form_get(const char *body, const char *key, char *out, size_t cap)
{
    if (httpd_query_key_value(body, key, out, cap) != ESP_OK) {
        return false;
    }
    url_decode(out);
    return true;
}

static const char FORM_HEAD[] =
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Charger Monitor setup</title>"
    "<style>body{font:16px sans-serif;max-width:26em;margin:2em auto;padding:0 1em}"
    "h1{font-size:1.3em}label{display:block;margin:.7em 0 .2em}"
    "input{width:100%;padding:.5em;box-sizing:border-box}"
    "fieldset{margin:1em 0;border:1px solid #ccc}button{margin-top:1em;padding:.7em 1.4em}"
    "small{color:#666}</style><h1>Charger Monitor setup</h1>"
    "<form method=POST action=/save>";

static esp_err_t get_root(httpd_req_t *req)
{
    const app_config_t *c = appcfg_get();
    char *b = malloc(4096);
    if (!b) {
        return httpd_resp_send_500(req);
    }
    int n = snprintf(b, 4096,
        "%s"
        "<fieldset><legend>WiFi</legend>"
        "<label>SSID<input name=wifi_ssid value='%s' required></label>"
        "<label>Password<input name=wifi_psk type=password placeholder='(unchanged)'></label>"
        "</fieldset>"
        "<fieldset><legend>MQTT</legend>"
        "<label>Broker host<input name=mqtt_host value='%s'></label>"
        "<label>Port<input name=mqtt_port type=number value='%u'></label>"
        "<label>Username<input name=mqtt_user value='%s'></label>"
        "<label>Password<input name=mqtt_pass type=password placeholder='(unchanged)'></label>"
        "<label>Discovery prefix<input name=mqtt_prefix value='%s'></label>"
        "</fieldset>"
        "<fieldset><legend>Admin / OTA</legend>"
        "<label>Admin password<input name=admin_pass type=password placeholder='(unchanged)'></label>"
        "</fieldset>"
        "<fieldset><legend>Fallback AP</legend>"
        "<small>Used if the saved WiFi can't be reached.</small>"
        "<label>AP password (>=8 chars)<input name=ap_pass type=password placeholder='(unchanged)'></label>"
        "<label>AP timeout (seconds)<input name=ap_to type=number value='%u'></label>"
        "</fieldset>"
        "<button>Save &amp; reboot</button></form>",
        FORM_HEAD, c->wifi_ssid, c->mqtt_host, c->mqtt_port, c->mqtt_user,
        c->mqtt_prefix, c->fallback_ap_timeout_s);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, b, n);
    free(b);
    return ESP_OK;
}

static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1200));   // let the HTTP response flush
    esp_restart();
}

static esp_err_t post_save(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 3000) {
        return httpd_resp_send_500(req);
    }
    char *body = malloc(total + 1);
    if (!body) {
        return httpd_resp_send_500(req);
    }
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, body + got, total - got);
        if (r <= 0) {
            free(body);
            return httpd_resp_send_500(req);
        }
        got += r;
    }
    body[total] = '\0';

    // Start from the current config; only overwrite fields that were provided.
    // Blank password fields keep the stored secret (form shows "(unchanged)").
    app_config_t cfg = *appcfg_get();
    char v[64];
    if (form_get(body, "wifi_ssid",   v, sizeof(v))) strlcpy(cfg.wifi_ssid,   v, sizeof(cfg.wifi_ssid));
    if (form_get(body, "wifi_psk",    v, sizeof(v)) && v[0]) strlcpy(cfg.wifi_psk, v, sizeof(cfg.wifi_psk));
    if (form_get(body, "mqtt_host",   v, sizeof(v))) strlcpy(cfg.mqtt_host,   v, sizeof(cfg.mqtt_host));
    if (form_get(body, "mqtt_port",   v, sizeof(v)) && v[0]) cfg.mqtt_port = (uint16_t)atoi(v);
    if (form_get(body, "mqtt_user",   v, sizeof(v))) strlcpy(cfg.mqtt_user,   v, sizeof(cfg.mqtt_user));
    if (form_get(body, "mqtt_pass",   v, sizeof(v)) && v[0]) strlcpy(cfg.mqtt_pass, v, sizeof(cfg.mqtt_pass));
    if (form_get(body, "mqtt_prefix", v, sizeof(v))) strlcpy(cfg.mqtt_prefix, v, sizeof(cfg.mqtt_prefix));
    if (form_get(body, "admin_pass",  v, sizeof(v)) && v[0]) strlcpy(cfg.admin_pass, v, sizeof(cfg.admin_pass));
    if (form_get(body, "ap_pass",     v, sizeof(v)) && v[0]) strlcpy(cfg.fallback_ap_pass, v, sizeof(cfg.fallback_ap_pass));
    if (form_get(body, "ap_to",       v, sizeof(v)) && v[0]) cfg.fallback_ap_timeout_s = (uint16_t)atoi(v);
    free(body);

    esp_err_t err = appcfg_save(&cfg);
    if (err != ESP_OK) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<body style='font:16px sans-serif;text-align:center;margin-top:3em'>"
        "<h1>Saved</h1><p>Rebooting and connecting to WiFi...</p>");

    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// Any unknown path -> redirect to the form (captive-portal probe handling).
static esp_err_t redirect_root(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" AP_IP "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static void start_httpd(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        return;
    }
    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = get_root };
    httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = post_save };
    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &save);
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, redirect_root);
}

static void stop_httpd(void)
{
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
}

// ==========================================================================
// Mode transitions
// ==========================================================================
static void ap_ssid(char *out, size_t cap)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(out, cap, "ChargerMon-%02X%02X", mac[4], mac[5]);
}

static void start_sta(void)
{
    esp_wifi_stop();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    const app_config_t *c = appcfg_get();
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, c->wifi_ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, c->wifi_psk, sizeof(wc.sta.password));
    // Scan every channel and connect to the STRONGEST AP for this SSID, not the
    // first one found (the default WIFI_FAST_SCAN grabs whichever answers first,
    // which on a multi-AP network is often a distant one). Costs a slightly longer
    // scan on (re)connect; worth it for a fixed device with roaming APs.
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_state = WIFI_CONNECTING;
    ESP_LOGI(TAG, "STA connecting to '%s'", c->wifi_ssid);
    esp_wifi_connect();
}

static void start_ap(bool first_boot)
{
    esp_wifi_stop();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    const app_config_t *c = appcfg_get();
    wifi_config_t wc = { 0 };
    ap_ssid((char *)wc.ap.ssid, sizeof(wc.ap.ssid));
    wc.ap.ssid_len       = strlen((char *)wc.ap.ssid);
    wc.ap.channel        = 1;
    wc.ap.max_connection = 4;

    // First boot has no configured password yet -> open. Fallback uses WPA2 with
    // the configured password (>= 8 chars, else fall back to open).
    if (!first_boot && strlen(c->fallback_ap_pass) >= 8) {
        wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
        strlcpy((char *)wc.ap.password, c->fallback_ap_pass, sizeof(wc.ap.password));
    } else {
        wc.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_ap_clients = 0;
    s_dns_run = true;
    xTaskCreate(dns_task, "dns", 3072, NULL, 5, NULL);
    start_httpd();
    s_state = WIFI_PORTAL;
    ESP_LOGW(TAG, "portal up: SSID '%s' (%s), http://%s/", wc.ap.ssid,
             wc.ap.authmode == WIFI_AUTH_OPEN ? "open" : "WPA2", AP_IP);
}

static void stop_ap(void)
{
    stop_httpd();
    s_dns_run = false;
    if (s_dns_sock >= 0) {
        close(s_dns_sock);   // unblock recvfrom so dns_task exits
        s_dns_sock = -1;
    }
}

// ==========================================================================
// Control task
// ==========================================================================
static uint32_t backoff_ms(int fails)
{
    uint32_t ms = 1000u << (fails > 5 ? 5 : fails);
    return ms > BACKOFF_CAP_MS ? BACKOFF_CAP_MS : ms;
}

// Run STA until MAX_STA_FAILS consecutive failures; returns (fallback needed).
static void run_sta_until_fail(void)
{
    xEventGroupClearBits(s_events, EV_GOT_IP | EV_DISCONNECTED);
    start_sta();

    int fails = 0;
    while (1) {
        EventBits_t b = xEventGroupWaitBits(s_events, EV_GOT_IP | EV_DISCONNECTED,
                                            pdTRUE, pdFALSE, portMAX_DELAY);
        if (b & EV_GOT_IP) {
            fails = 0;
            s_state = WIFI_CONNECTED;
            continue;
        }
        // EV_DISCONNECTED
        s_state = WIFI_CONNECTING;
        if (++fails >= MAX_STA_FAILS) {
            ESP_LOGW(TAG, "%d STA failures; opening fallback AP", fails);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(backoff_ms(fails)));
        esp_wifi_connect();
    }
}

// Portal until a client provisions (reboot) or, for the fallback AP, the timeout
// elapses with no client connected -> tear down and return to STA retry.
static void run_portal(bool first_boot)
{
    start_ap(first_boot);
    uint32_t to_s = appcfg_get()->fallback_ap_timeout_s;
    if (to_s == 0) to_s = 120;

    while (1) {
        TickType_t wait = first_boot ? portMAX_DELAY : pdMS_TO_TICKS(to_s * 1000);
        EventBits_t b = xEventGroupWaitBits(s_events, EV_AP_CLIENT, pdTRUE, pdFALSE, wait);
        if (b & EV_AP_CLIENT) {
            continue;                 // client present; keep the portal open
        }
        if (!first_boot && s_ap_clients == 0) {
            ESP_LOGW(TAG, "portal timeout, no client; retrying STA");
            stop_ap();
            return;
        }
        // first_boot never reaches here (infinite wait); loop otherwise
    }
}

static void wifi_task(void *arg)
{
    while (1) {
        if (appcfg_is_provisioned()) {
            run_sta_until_fail();   // returns when fallback is needed
            run_portal(false);      // WPA2 fallback; returns on timeout -> retry STA
        } else {
            run_portal(true);       // open first-boot portal; only exits via reboot
        }
    }
}

// ==========================================================================
// Public API
// ==========================================================================
void wifi_start(void)
{
    s_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(e);
    }
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    // We own credentials in NVS via the config component; keep the WiFi driver's
    // own copy in RAM only.
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL, NULL));

    xTaskCreate(wifi_task, "wifi", 4096, NULL, 5, NULL);
}

wifi_state_t wifi_get_state(void)
{
    return s_state;
}

void wifi_get_ip(char *out, size_t cap)
{
    strlcpy(out, s_ip, cap);
}

int8_t wifi_get_rssi(void)
{
    wifi_ap_record_t ap;
    if (s_state == WIFI_CONNECTED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return ap.rssi;
    }
    return 0;
}

const char *wifi_state_name(wifi_state_t s)
{
    switch (s) {
    case WIFI_DOWN:       return "down";
    case WIFI_CONNECTING: return "connecting";
    case WIFI_CONNECTED:  return "connected";
    case WIFI_PORTAL:     return "portal";
    default:              return "?";
    }
}
