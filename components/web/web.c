#include "web.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "mbedtls/base64.h"

#include "appcfg.h"
#include "wifi.h"
#include "mqtt_ha.h"
#include "charger_state.h"
#include "charger_control.h"
#include "charger_proto.h"
#include "ota.h"

static const char *TAG = "web";

static httpd_handle_t s_httpd;

// SSE (server-sent events) push. Each connected browser holds one detached
// (async) request; the web task diffs the state ~2/s and pushes on change plus a
// periodic keepalive. Sends run on the web task, never the BLE/state tasks, so a
// slow client cannot stall telemetry.
#define MAX_SSE 3
static httpd_req_t     *s_sse[MAX_SSE];
static SemaphoreHandle_t s_sse_lock;

// ==========================================================================
// Helpers
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

static bool form_get(const char *body, const char *key, char *out, size_t cap)
{
    if (httpd_query_key_value(body, key, out, cap) != ESP_OK) {
        return false;
    }
    url_decode(out);
    return true;
}

// Read the request body into a malloc'd NUL-terminated buffer (caller frees).
static char *read_body(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 3000) {
        return NULL;
    }
    char *b = malloc(total + 1);
    if (!b) {
        return NULL;
    }
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, b + got, total - got);
        if (r <= 0) {
            free(b);
            return NULL;
        }
        got += r;
    }
    b[total] = '\0';
    return b;
}

// HTTP Basic auth against the admin password (user "admin"). Open if unset.
static bool authed(httpd_req_t *req)
{
    const char *pw = appcfg_get()->admin_pass;
    if (!pw[0]) {
        return true;   // no credential configured -> writes open
    }
    char hdr[160];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) {
        return false;
    }
    char cred[96];
    int cl = snprintf(cred, sizeof(cred), "admin:%s", pw);
    unsigned char b64[160];
    size_t olen = 0;
    mbedtls_base64_encode(b64, sizeof(b64), &olen, (const unsigned char *)cred, cl);
    char expect[176];
    snprintf(expect, sizeof(expect), "Basic %s", b64);
    return strcmp(hdr, expect) == 0;
}

static esp_err_t require_auth(httpd_req_t *req)
{
    if (authed(req)) {
        return ESP_OK;
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Charger Monitor\"");
    httpd_resp_send(req, "auth required", HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
}

// ==========================================================================
// GET /api/state  (open) - the single source of truth for the page
// ==========================================================================
static int build_state_json(char *j, size_t cap)
{
    charger_state_t s;
    charger_state_get(&s);
    char ip[16];
    wifi_get_ip(ip, sizeof(ip));

    return snprintf(j, cap,
        "{\"voltage\":%.1f,\"current\":%.2f,\"capacity\":%u,"
        "\"step\":%u,\"step_text\":\"%s\",\"state\":%u,"
        "\"program\":%u,\"program_text\":\"%s\",\"error\":%u,\"ah\":%.3f,"
        "\"enable\":%s,\"charging\":%s,\"battery\":%s,\"link\":\"%s\",\"ble_ready\":%s,"
        "\"wifi\":\"%s\",\"ip\":\"%s\",\"mqtt\":%s,"
        // volatile fields last so the SSE diff can ignore them (see web_task)
        "\"rssi\":%d,\"uptime\":%lld}",
        s.voltage_v, s.current_a, s.capacity_pct,
        s.step, cp_step_name(s.step), s.charger_state,
        s.program, cp_program_name(s.program), s.error, s.charge_delivered_ah,
        s.enable ? "true" : "false",
        s.charging ? "true" : "false", s.battery_present ? "true" : "false",
        cs_link_name(s.link), s.link == CB_LINK_READY ? "true" : "false",
        wifi_state_name(wifi_get_state()), ip,
        mqtt_ha_connected() ? "true" : "false",
        wifi_get_rssi(), (long long)(esp_timer_get_time() / 1000000));
}

static esp_err_t api_state(httpd_req_t *req)
{
    char j[512];
    int n = build_state_json(j, sizeof(j));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, j, n);
    return ESP_OK;
}

// ==========================================================================
// GET /api/events  (open) - SSE push of the state JSON on change + keepalive
// ==========================================================================
static esp_err_t api_events(httpd_req_t *req)
{
    // Detach first, then do every send on the copy - sending on the original and
    // then detaching orphans the buffered chunk (headers go out, body doesn't).
    httpd_req_t *copy = NULL;
    if (httpd_req_async_handler_begin(req, &copy) != ESP_OK) {
        return ESP_FAIL;
    }
    httpd_resp_set_type(copy, "text/event-stream");
    httpd_resp_set_hdr(copy, "Cache-Control", "no-cache");
    // First chunk sets chunked transfer + the browser's reconnect delay, then an
    // immediate snapshot so the page paints without waiting for a change.
    char j[560];
    int n = snprintf(j, sizeof(j), "retry: 3000\ndata: ");
    n += build_state_json(j + n, sizeof(j) - n);
    n += snprintf(j + n, sizeof(j) - n, "\n\n");
    if (httpd_resp_send_chunk(copy, j, n) != ESP_OK) {
        httpd_req_async_handler_complete(copy);
        return ESP_OK;
    }

    xSemaphoreTake(s_sse_lock, portMAX_DELAY);
    int slot = -1;
    for (int i = 0; i < MAX_SSE; i++) {
        if (!s_sse[i]) { s_sse[i] = copy; slot = i; break; }
    }
    xSemaphoreGive(s_sse_lock);

    if (slot < 0) {
        httpd_req_async_handler_complete(copy);   // table full
        return ESP_OK;
    }
    ESP_LOGI(TAG, "SSE client attached (slot %d)", slot);
    return ESP_OK;
}

// Push one SSE frame to every client; drop any whose socket has died. Runs on
// the web task only.
static void sse_broadcast(const char *json)
{
    char frame[560];
    int n = snprintf(frame, sizeof(frame), "data: %s\n\n", json);
    xSemaphoreTake(s_sse_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_SSE; i++) {
        if (!s_sse[i]) {
            continue;
        }
        if (httpd_resp_send_chunk(s_sse[i], frame, n) != ESP_OK) {
            httpd_req_async_handler_complete(s_sse[i]);
            s_sse[i] = NULL;
            ESP_LOGI(TAG, "SSE client gone (slot %d)", i);
        }
    }
    xSemaphoreGive(s_sse_lock);
}

static void sse_close_all(void)
{
    xSemaphoreTake(s_sse_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_SSE; i++) {
        if (s_sse[i]) {
            httpd_req_async_handler_complete(s_sse[i]);
            s_sse[i] = NULL;
        }
    }
    xSemaphoreGive(s_sse_lock);
}

// ==========================================================================
// GET /api/info  (open) - running firmware version + partition
// ==========================================================================
static esp_err_t api_info(httpd_req_t *req)
{
    const esp_app_desc_t *a = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();
    char j[320];
    int n = snprintf(j, sizeof(j),
        "{\"project\":\"%s\",\"version\":\"%s\",\"build\":\"%s %s\","
        "\"idf\":\"%s\",\"slot\":\"%s\",\"uptime\":%lld}",
        a->project_name, a->version, a->date, a->time, a->idf_ver,
        run ? run->label : "?", (long long)(esp_timer_get_time() / 1000000));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, j, n);
    return ESP_OK;
}

// ==========================================================================
// POST /api/ota  (auth) - push firmware to the inactive A/B slot
// ==========================================================================
static esp_err_t post_ota(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_OK;
    }
    return ota_perform(req);
}

// ==========================================================================
// GET /  (open) - self-contained status page that polls /api/state
// ==========================================================================
static const char PAGE[] =
"<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>Charger Monitor</title><style>"
"*{box-sizing:border-box}"
":root{--surface:#fcfcfb;--ink:#0b0b0b;--sub:#52514e;--muted:#898781;--hollow:#e1e0d9;"
"--blue:#2a78d6;--blue-hi:#6da7ec;--blue-lo:#1c5cab;--ring:rgba(11,11,11,.10)}"
"@media(prefers-color-scheme:dark){:root{--surface:#1a1a19;--ink:#fff;--sub:#c3c2b7;"
"--muted:#898781;--hollow:#2c2c2a;--blue:#3987e5;--blue-hi:#5598e7;--blue-lo:#184f95;"
"--ring:rgba(255,255,255,.10)}}"
"body{background:var(--surface);color:var(--ink);font-family:system-ui,-apple-system,sans-serif;"
"max-width:640px;margin:0 auto;padding:14px}h1{font-size:1.15em;margin:.2em 0 .6em}"
"svg{width:100%;height:auto;display:block}"
".cap{fill:var(--muted);font-size:12px;letter-spacing:.14em;text-transform:uppercase}"
".ink{fill:var(--ink)}.sub{fill:var(--sub)}.muted{fill:var(--muted)}"
".done{fill:var(--blue)}.active{fill:var(--blue)}"
".future{fill:var(--surface);stroke:var(--hollow);stroke-width:2}"
".cdone{stroke:var(--blue)}.cfuture{stroke:var(--hollow)}.tile{fill:var(--surface);stroke:var(--ring)}"
".vessel{fill:var(--hollow);opacity:.45}.vesselout{fill:none;stroke:var(--ring);stroke-width:1.5}"
".ringr{fill:none;stroke:var(--blue);stroke-width:2;opacity:.35}.dot{fill:var(--surface)}"
".fillhi{fill:var(--blue-hi);opacity:.9}"
"#warn{background:#c0392b;color:#fff;padding:.5em;border-radius:8px;text-align:center;"
"display:none;margin-bottom:.6em}.ctl{display:flex;gap:.5em;flex-wrap:wrap;margin-top:.6em}"
"select,button{background:var(--surface);color:var(--ink);border:1px solid var(--ring);"
"border-radius:8px;padding:.55em .8em;font-size:1em}button{cursor:pointer}"
"footer{color:var(--muted);font-size:.8em;margin-top:1em}a{color:inherit}"
"</style><h1>Charger Monitor</h1><div id=warn>charger disconnected</div>"
"<svg viewBox='0 0 820 375'>"
"<defs><linearGradient id='liquid' x1='0' y1='0' x2='0' y2='1'>"
"<stop offset='0' style='stop-color:var(--blue-hi)' /><stop offset='1' style='stop-color:var(--blue-lo)' />"
"</linearGradient><clipPath id='drop'>"
"<path d='M150 250 C150 250 90 316 90 346 C90 376 118 393 150 393 C182 393 210 376 210 346 C210 316 150 250 150 250 Z' />"
"</clipPath></defs>"
"<text class=cap x=8 y=22>Charge stage</text>"
// Everything below the caption is drawn in the mockup's original coordinates,
// lifted 65 units to close the dead space under the heading (viewBox shrinks
// to match). The JS addresses elements in these local coords, so no math changes.
"<g transform='translate(0 -65)'>"
"<g stroke-width=3.5 fill=none stroke-linecap=round>"
"<line id=k1 class=cfuture x1=86 y1=150 x2=151 y2=150 /><line id=k2 class=cfuture x1=183 y1=150 x2=248 y2=150 />"
"<line id=k3 class=cfuture x1=280 y1=150 x2=345 y2=150 /><line id=k4 class=cfuture x1=377 y1=150 x2=442 y2=150 />"
"<line id=k5 class=cfuture x1=474 y1=150 x2=539 y2=150 /><line id=k6 class=cfuture x1=571 y1=150 x2=636 y2=150 />"
"<line id=k7 class=cfuture x1=668 y1=150 x2=733 y2=150 /></g>"
"<circle id=n1 class=future cx=70 cy=150 r=13 /><circle id=n2 class=future cx=167 cy=150 r=13 />"
"<circle id=n3 class=future cx=264 cy=150 r=13 /><circle id=n4 class=future cx=361 cy=150 r=13 />"
"<circle id=n5 class=future cx=458 cy=150 r=13 /><circle id=n6 class=future cx=555 cy=150 r=13 />"
"<circle id=n7 class=future cx=652 cy=150 r=13 /><circle id=n8 class=future cx=749 cy=150 r=13 />"
"<circle id=ring class=ringr cx=70 cy=150 r=20 style='display:none' />"
"<circle id=dot class=dot cx=70 cy=150 r=5 style='display:none' />"
"<g font-size=10.5 text-anchor=middle class=sub>"
"<text x=70 y=184>Check</text><text x=167 y=184>Recover</text><text x=264 y=184>Soft</text>"
"<text x=361 y=184>Bulk</text><text x=458 y=184>Absorb</text><text x=555 y=184>Maint 1</text>"
"<text x=652 y=184>Maint 2</text><text x=749 y=184>Trickle</text></g>"
"<text class=sub x=150 y=238 font-size=13 font-weight=600 text-anchor=middle>Capacity</text>"
"<path class=vessel d='M150 250 C150 250 90 316 90 346 C90 376 118 393 150 393 C182 393 210 376 210 346 C210 316 150 250 150 250 Z' />"
"<g clip-path='url(#drop)'><rect id=fill x=82 y=393 width=136 height=0 fill='url(#liquid)' />"
"<rect id=fillhi class=fillhi x=82 y=393 width=136 height=2.5 /></g>"
"<path class=vesselout d='M150 250 C150 250 90 316 90 346 C90 376 118 393 150 393 C182 393 210 376 210 346 C210 316 150 250 150 250 Z' />"
"<text id=cap class=ink x=150 y=352 font-size=34 font-weight=700 text-anchor=middle>-</text>"
"<rect class=tile x=300 y=244 width=150 height=66 rx=12 stroke-width=1 />"
"<text class=cap x=318 y=270 font-size=11>Voltage</text><text id=v class=ink x=318 y=298 font-size=24 font-weight=700>-</text>"
"<rect class=tile x=466 y=244 width=150 height=66 rx=12 stroke-width=1 />"
"<text class=cap x=484 y=270 font-size=11>Current</text><text id=cur class=ink x=484 y=298 font-size=24 font-weight=700>-</text>"
"<rect class=tile x=632 y=244 width=150 height=66 rx=12 stroke-width=1 />"
"<text class=cap x=650 y=270 font-size=11>Total</text><text id=tot class=ink x=650 y=298 font-size=24 font-weight=700>-</text>"
"<rect class=tile x=300 y=326 width=316 height=66 rx=12 stroke-width=1 />"
"<text class=cap x=318 y=352 font-size=11>Charger state</text><text id=cst class=ink x=318 y=380 font-size=20 font-weight=700>-</text>"
"<rect class=tile x=632 y=326 width=150 height=66 rx=12 stroke-width=1 />"
"<text class=cap x=650 y=352 font-size=11>Program</text><text id=prog class=ink x=650 y=380 font-size=20 font-weight=700>-</text>"
"</g>"
"</svg>"
"<div class=ctl><select id=sel></select>"
"<button onclick=\"ctl('program',sel.value)\">Set</button>"
"<button onclick=\"ctl('stop')\">Stop</button>"
"<button onclick=\"ctl('refresh')\">Refresh</button></div>"
"<footer>BLE <span id=ble>-</span> &middot; WiFi <span id=wifi>-</span> "
"(<span id=rssi>-</span>dBm) &middot; MQTT <span id=mqtt>-</span> &middot; "
"up <span id=up>-</span>s &middot; <a href=/config>configure</a></footer>"
"<script>"
"var PROG=['standby','6V','6V-cold','car','car-cold','bike','bike-cold','lithium','recovery'];"
"var XS=[70,167,264,361,458,555,652,749];"
"function g(id){return document.getElementById(id)}"
"var sel=g('sel');PROG.forEach(function(p){sel.add(new Option(p,p))});"
"function step(s){for(var i=1;i<=8;i++)g('n'+i).setAttribute('class',i<s?'done':(i==s?'active':'future'));"
"for(var j=1;j<=7;j++)g('k'+j).setAttribute('class',j<s?'cdone':'cfuture');"
"var r=g('ring'),d=g('dot');if(s>=1){r.style.display=d.style.display='';"
"r.setAttribute('cx',XS[s-1]);d.setAttribute('cx',XS[s-1])}else{r.style.display=d.style.display='none'}}"
"function gauge(p){var t=393-p/100*143;g('fill').setAttribute('y',t);g('fill').setAttribute('height',393-t);"
"g('fillhi').setAttribute('y',t);g('cap').textContent=p+'%'}"
"function ctl(cmd,arg){var b='cmd='+cmd+(arg?'&arg='+encodeURIComponent(arg):'');"
"fetch('/api/control',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b})"
".then(function(r){if(r.status==401)alert('auth required (admin / your write password)')})}"
"function render(d){"
"g('warn').style.display=d.ble_ready?'none':'block';step(d.step);gauge(d.capacity);"
"g('v').textContent=d.voltage.toFixed(1)+' V';g('cur').textContent=d.current.toFixed(2)+' A';"
"g('prog').textContent=d.program_text;g('tot').textContent=d.ah.toFixed(2)+' Ah';"
"var st=!d.ble_ready?'Disconnected':(d.charging?'Charging':(d.battery?'Idle':'No battery'));"
"if(d.ble_ready&&d.step>0)st+=' \\u00b7 '+d.step_text;g('cst').textContent=st;"
"g('ble').textContent=d.link;g('wifi').textContent=d.wifi;g('rssi').textContent=d.rssi;"
"g('mqtt').textContent=d.mqtt?'connected':'down';g('up').textContent=d.uptime}"
"function poll(){fetch('/api/state').then(function(r){return r.json()}).then(render).catch(function(e){})}"
"poll();"                    // immediate paint
"if(window.EventSource){var es=new EventSource('/api/events');"   // live push
"es.onmessage=function(e){try{render(JSON.parse(e.data))}catch(x){}}}"
"else{setInterval(poll,2000)}"                                    // fallback
"</script>";

static esp_err_t get_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PAGE, sizeof(PAGE) - 1);
    return ESP_OK;
}

// ==========================================================================
// POST /api/control  (auth) - routes through the same control guard rails
// ==========================================================================
static esp_err_t api_control(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_OK;
    }
    char *body = read_body(req);
    if (!body) {
        return httpd_resp_send_500(req);
    }
    char cmd[16] = "", arg[24] = "";
    form_get(body, "cmd", cmd, sizeof(cmd));
    form_get(body, "arg", arg, sizeof(arg));
    free(body);

    cc_result_t r = CC_ERR_RANGE;
    if (!strcmp(cmd, "program")) {
        for (int i = 0; i <= 8; i++) {
            if (!strcmp(arg, cp_program_name(i))) { r = charger_control_set_program(i); break; }
        }
    } else if (!strcmp(cmd, "start")) {
        r = charger_control_start(atoi(arg));
    } else if (!strcmp(cmd, "stop")) {
        r = charger_control_stop();
    } else if (!strcmp(cmd, "refresh")) {
        r = charger_control_refresh();
    }
    ESP_LOGI(TAG, "control cmd='%s' arg='%s' -> %d", cmd, arg, r);

    httpd_resp_set_type(req, "application/json");
    char out[32];
    int n = snprintf(out, sizeof(out), "{\"result\":%d}", r);
    httpd_resp_send(req, out, n);
    return ESP_OK;
}

// ==========================================================================
// GET/POST /config  (auth) - re-provision WiFi/MQTT/admin without the portal
// ==========================================================================
static esp_err_t get_config(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_OK;
    }
    const app_config_t *c = appcfg_get();
    char *b = malloc(3072);
    if (!b) {
        return httpd_resp_send_500(req);
    }
    int n = snprintf(b, 3072,
        "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Configure</title><style>body{font:15px system-ui,sans-serif;max-width:26em;"
        "margin:1.5em auto;padding:0 1em}label{display:block;margin:.6em 0 .2em}"
        "input{width:100%%;padding:.5em;box-sizing:border-box}fieldset{margin:1em 0}"
        "button{margin-top:1em;padding:.6em 1.2em}</style><h1>Configure</h1>"
        "<form method=POST action=/config>"
        "<fieldset><legend>WiFi</legend>"
        "<label>SSID<input name=wifi_ssid value='%s'></label>"
        "<label>Password<input name=wifi_psk type=password placeholder='(unchanged)'></label></fieldset>"
        "<fieldset><legend>MQTT</legend>"
        "<label>Host<input name=mqtt_host value='%s'></label>"
        "<label>Port<input name=mqtt_port type=number value='%u'></label>"
        "<label>Username<input name=mqtt_user value='%s'></label>"
        "<label>Password<input name=mqtt_pass type=password placeholder='(unchanged)'></label>"
        "<label>Discovery prefix<input name=mqtt_prefix value='%s'></label></fieldset>"
        "<fieldset><legend>Admin / OTA</legend>"
        "<label>Admin password<input name=admin_pass type=password placeholder='(unchanged)'></label></fieldset>"
        "<button>Save &amp; reboot</button></form><p><a href='/'>&larr; status</a></p>",
        c->wifi_ssid, c->mqtt_host, c->mqtt_port, c->mqtt_user, c->mqtt_prefix);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, b, n);
    free(b);
    return ESP_OK;
}

static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
}

static esp_err_t post_config(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_OK;
    }
    char *body = read_body(req);
    if (!body) {
        return httpd_resp_send_500(req);
    }
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
    free(body);

    if (appcfg_save(&cfg) != ESP_OK) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, "<!doctype html><meta http-equiv=refresh content='6;url=/'>"
        "<body style='font:15px system-ui;text-align:center;margin-top:3em'>"
        "<h1>Saved</h1><p>Rebooting...</p>");
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// ==========================================================================
// Server lifecycle - only while the STA link is up (avoids the portal's httpd)
// ==========================================================================
static void start_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 10;
    cfg.stack_size       = 8192;    // OTA flash writes run in the handler task
    cfg.recv_wait_timeout = 15;     // tolerate a slow firmware upload
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        return;
    }
    httpd_uri_t uris[] = {
        { .uri = "/",            .method = HTTP_GET,  .handler = get_root },
        { .uri = "/api/state",   .method = HTTP_GET,  .handler = api_state },
        { .uri = "/api/events",  .method = HTTP_GET,  .handler = api_events },
        { .uri = "/api/info",    .method = HTTP_GET,  .handler = api_info },
        { .uri = "/api/control", .method = HTTP_POST, .handler = api_control },
        { .uri = "/api/ota",     .method = HTTP_POST, .handler = post_ota },
        { .uri = "/config",      .method = HTTP_GET,  .handler = get_config },
        { .uri = "/config",      .method = HTTP_POST, .handler = post_config },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_httpd, &uris[i]);
    }
    char ip[16];
    wifi_get_ip(ip, sizeof(ip));
    ESP_LOGI(TAG, "web up at http://%s/", ip);
}

static void stop_server(void)
{
    if (s_httpd) {
        sse_close_all();
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
}

// Length of the "meaningful" JSON prefix - everything before the volatile
// rssi/uptime tail - so the SSE diff pushes on real state changes, not on the
// clock ticking or RSSI jitter.
static int meaningful_len(const char *j)
{
    const char *p = strstr(j, ",\"rssi\"");
    return p ? (int)(p - j) : (int)strlen(j);
}

static void web_task(void *arg)
{
    static char last[512];
    (void)arg;
    int64_t last_push_us = 0;
    for (;;) {
        bool up = wifi_get_state() == WIFI_CONNECTED;
        if (up && !s_httpd) {
            start_server();
            last[0] = '\0';
        } else if (!up && s_httpd) {
            stop_server();
        }

        bool have_clients = false;
        for (int i = 0; i < MAX_SSE; i++) {
            if (s_sse[i]) { have_clients = true; break; }
        }
        if (s_httpd && have_clients) {
            char cur[512];
            build_state_json(cur, sizeof(cur));
            int64_t now = esp_timer_get_time();
            int ml = meaningful_len(cur);
            bool changed = ml != meaningful_len(last) || strncmp(cur, last, ml) != 0;
            if (changed || now - last_push_us > 10000000) {   // change or 10s keepalive
                sse_broadcast(cur);
                strlcpy(last, cur, sizeof(last));
                last_push_us = now;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void web_start(void)
{
    s_sse_lock = xSemaphoreCreateMutex();
    xTaskCreate(web_task, "web", 4096, NULL, 4, NULL);
}
