#include "charger_ble.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nimble/nimble_npl.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "charger_ble";

// 16-bit charger UUIDs (see docs/PROTOCOL.md).
static const ble_uuid16_t UUID_SVC  = BLE_UUID16_INIT(0xFFF0);
static const ble_uuid16_t UUID_FFF1 = BLE_UUID16_INIT(0xFFF1);  // telemetry + commands
static const ble_uuid16_t UUID_FFF2 = BLE_UUID16_INIT(0xFFF2);  // password only

// ---- module state --------------------------------------------------------
static charger_ble_state_cb s_cb;
static void                 *s_ctx;
static charger_ble_link_t    s_link = CB_LINK_DISCONNECTED;

static uint8_t  s_own_addr_type;
static uint16_t s_conn      = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_svc_start, s_svc_end;
static uint16_t s_fff1_val, s_fff2_val;

// Notification watchdog: the charger streams 0x60/0x61/0x62 continuously (~1 Hz),
// so a live link always receives something within a second. If we reach READY but
// no notification arrives for RX_STALL_MS, the link is dead (a "connected" ghost) -
// tear it down so the scan/reconnect path recovers. See charger_ble_tick().
#define RX_STALL_MS 5000
static int64_t s_last_rx_us;   // esp_timer_get_time() of the last NOTIFY_RX / READY

static cp_reassembler_t s_rx;
static cp_state_t       s_state;

// ---- outbound TX (commands to fff1) --------------------------------------
// Every outbound frame funnels through this queue and is written one at a time
// on the NimBLE host task, so writes never overlap. Both control
// commands and per-packet answer() acks (below) use it.
#define TX_Q_DEPTH 8
typedef struct { uint8_t data[8]; uint8_t len; } tx_item_t;
static QueueHandle_t        s_tx_q;
static struct ble_npl_event s_tx_ev;
static bool                 s_tx_busy;

// Replicate the vendor app's per-packet answer()/ack on discrete state
// notifications (PROTOCOL.md §9). The read-only slice ran fine without
// it; flip to false to return to that behaviour if a unit misbehaves.
static const bool s_answer_ack = true;

// A chained write sequence: enable each CCCD, then handshake, then password.
// NimBLE runs one GATT procedure at a time, so we advance on each completion.
typedef struct { uint16_t handle; uint8_t data[8]; uint8_t len; } action_t;
static action_t s_seq[6];
static int      s_seq_n, s_seq_i;
static uint16_t s_cccd[4];
static int      s_cccd_n;

static void start_scan(void);
static int  gap_event(struct ble_gap_event *event, void *arg);

// ---- outbound TX ---------------------------------------------------------
// Completion callback (host task): free the slot and pump the next frame.
static int on_tx_written(uint16_t conn, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    if (error && error->status != 0) {
        ESP_LOGW(TAG, "tx write failed: %d", error->status);
    }
    s_tx_busy = false;
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_tx_ev);
    return 0;
}

// Pump one queued frame (runs on the host task via the default event queue).
static void tx_pump(struct ble_npl_event *ev)
{
    (void)ev;
    if (s_tx_busy) {
        return;   // a write is in flight; its completion re-pumps
    }
    if (s_link != CB_LINK_READY || s_conn == BLE_HS_CONN_HANDLE_NONE || !s_fff1_val) {
        tx_item_t junk;
        while (xQueueReceive(s_tx_q, &junk, 0) == pdTRUE) { }   // drop stale
        return;
    }

    tx_item_t it;
    if (xQueueReceive(s_tx_q, &it, 0) != pdTRUE) {
        return;
    }

    s_tx_busy = true;
    int rc = ble_gattc_write_flat(s_conn, s_fff1_val, it.data, it.len,
                                  on_tx_written, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "tx write_flat rc=%d", rc);
        s_tx_busy = false;
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_tx_ev);   // try next
    }
}

int charger_ble_send(const uint8_t *data, uint8_t len)
{
    if (!s_tx_q || s_link != CB_LINK_READY) {
        return -1;
    }
    if (len == 0 || len > sizeof(((tx_item_t *)0)->data)) {
        return -1;
    }

    tx_item_t it;
    memcpy(it.data, data, len);
    it.len = len;
    if (xQueueSend(s_tx_q, &it, 0) != pdTRUE) {
        ESP_LOGW(TAG, "tx queue full, dropping command");
        return -2;
    }

    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_tx_ev);
    return 0;
}

void charger_ble_poll_discrete(void)
{
    if (s_link != CB_LINK_READY) {
        return;
    }
    // Getter frame = read opcode with a single 0x00 payload byte (op 01 00 xor).
    static const uint8_t opcodes[] = {
        CP_OP_STATE, CP_OP_ERROR, CP_OP_PROGRAM, CP_OP_STEP, CP_OP_ENABLE,
    };
    for (size_t i = 0; i < sizeof(opcodes); i++) {
        uint8_t g[4];
        const uint8_t zero = 0x00;
        size_t n = cp_frame(opcodes[i], &zero, 1, g, sizeof(g));
        charger_ble_send(g, (uint8_t)n);
    }
}

// ---- telemetry ingest ----------------------------------------------------
static void on_frame(const uint8_t *frame, size_t len, void *ctx)
{
    (void)ctx;
    uint8_t op = cp_decode(frame, len, &s_state);
    if (op && s_cb) {
        s_cb(&s_state, op, s_ctx);
    }
    // Answer/ack each discrete state notification like the vendor app does
    // (PROTOCOL.md §9). High-rate 0x60/0x61 streams are not acked - cp_opcode_
    // needs_ack encodes exactly which opcodes qualify.
    if (s_answer_ack && op && cp_opcode_needs_ack(op)) {
        uint8_t ack[4];
        size_t n = cp_answer(ack);
        charger_ble_send(ack, (uint8_t)n);
    }
}

// ---- subscribe + auth sequence ------------------------------------------
static void run_next_action(void);

static int on_action_written(uint16_t conn, const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr, void *arg)
{
    (void)attr; (void)arg;
    if (error && error->status != 0) {
        ESP_LOGW(TAG, "write step %d failed: %d", s_seq_i, error->status);
    }
    if (conn != s_conn) {
        return 0;
    }
    s_seq_i++;
    run_next_action();
    return 0;
}

static void run_next_action(void)
{
    if (s_seq_i >= s_seq_n) {
        s_link = CB_LINK_READY;
        s_last_rx_us = esp_timer_get_time();   // grace period before watchdog arms
        ESP_LOGI(TAG, "authenticated - telemetry should stream now");
        // Discrete states are broadcast on change, so if we joined after the last
        // change we would miss them; snapshot current values once now. Ongoing
        // updates (and all analog fields) arrive on the stream.
        charger_ble_poll_discrete();
        return;
    }
    action_t *a = &s_seq[s_seq_i];
    int rc = ble_gattc_write_flat(s_conn, a->handle, a->data, a->len,
                                  on_action_written, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gattc_write_flat step %d rc=%d", s_seq_i, rc);
    }
}

static void build_and_run_sequence(void)
{
    s_seq_n = 0;

    // 1) Enable notifications on every CCCD we found (fff1 + fff2).
    for (int i = 0; i < s_cccd_n && s_seq_n < (int)(sizeof(s_seq)/sizeof(s_seq[0])); i++) {
        s_seq[s_seq_n].handle  = s_cccd[i];
        s_seq[s_seq_n].data[0] = 0x01;   // notifications enable, little-endian
        s_seq[s_seq_n].data[1] = 0x00;
        s_seq[s_seq_n].len     = 2;
        s_seq_n++;
    }

    // 2) Handshake -> fff1.
    action_t *hs = &s_seq[s_seq_n++];
    hs->handle = s_fff1_val;
    hs->len    = (uint8_t)cp_handshake(hs->data);

    // 3) Password -> fff2 (raw, unframed). NB: the app waits ~500ms first; the
    //    write-with-response round-trip usually provides enough spacing. If a
    //    unit is picky, add a short delay here.
    action_t *pw = &s_seq[s_seq_n++];
    pw->handle = s_fff2_val;
    memcpy(pw->data, CP_PASSWORD, sizeof(CP_PASSWORD));
    pw->len = sizeof(CP_PASSWORD);

    s_seq_i = 0;
    run_next_action();
}

// ---- GATT discovery ------------------------------------------------------
static int on_disc_dsc(uint16_t conn, const struct ble_gatt_error *error,
                       uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc,
                       void *arg)
{
    (void)chr_val_handle; (void)arg;
    if (conn != s_conn) return 0;

    if (error->status == 0) {
        if (ble_uuid_cmp(&dsc->uuid.u, BLE_UUID16_DECLARE(0x2902)) == 0 &&
            s_cccd_n < (int)(sizeof(s_cccd)/sizeof(s_cccd[0]))) {
            s_cccd[s_cccd_n++] = dsc->handle;
        }
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        ESP_LOGI(TAG, "found %d CCCD(s); subscribing + authenticating", s_cccd_n);
        build_and_run_sequence();
    } else {
        ESP_LOGE(TAG, "dsc disc error %d", error->status);
    }
    return 0;
}

static int on_disc_chr(uint16_t conn, const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    if (conn != s_conn) return 0;

    if (error->status == 0) {
        if (ble_uuid_cmp(&chr->uuid.u, &UUID_FFF1.u) == 0) {
            s_fff1_val = chr->val_handle;
        } else if (ble_uuid_cmp(&chr->uuid.u, &UUID_FFF2.u) == 0) {
            s_fff2_val = chr->val_handle;
        }
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        if (!s_fff1_val || !s_fff2_val) {
            ESP_LOGE(TAG, "missing characteristics (fff1=%u fff2=%u)", s_fff1_val, s_fff2_val);
            ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        s_cccd_n = 0;
        ble_gattc_disc_all_dscs(s_conn, s_fff1_val, s_svc_end, on_disc_dsc, NULL);
    } else {
        ESP_LOGE(TAG, "chr disc error %d", error->status);
    }
    return 0;
}

static int on_disc_svc(uint16_t conn, const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *service, void *arg)
{
    (void)arg;
    if (conn != s_conn) return 0;

    if (error->status == 0) {
        s_svc_start = service->start_handle;
        s_svc_end   = service->end_handle;
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        if (!s_svc_start) {
            ESP_LOGE(TAG, "fff0 service not found");
            ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        ble_gattc_disc_all_chrs(s_conn, s_svc_start, s_svc_end, on_disc_chr, NULL);
    } else {
        ESP_LOGE(TAG, "svc disc error %d", error->status);
    }
    return 0;
}

// ---- GAP -----------------------------------------------------------------
static bool adv_has_fff0(const struct ble_hs_adv_fields *f)
{
    for (int i = 0; i < f->num_uuids16; i++) {
        if (f->uuids16[i].value == 0xFFF0) return true;
    }
    return false;
}

static void reset_link(void)
{
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_svc_start = s_svc_end = 0;
    s_fff1_val = s_fff2_val = 0;
    s_cccd_n = s_seq_n = s_seq_i = 0;
    cp_reassembler_reset(&s_rx);

    // Abandon any in-flight/queued TX; the connection it targeted is gone.
    s_tx_busy = false;
    if (s_tx_q) {
        xQueueReset(s_tx_q);
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {

    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields fields;
        if (ble_hs_adv_parse_fields(&fields, event->disc.data,
                                    event->disc.length_data) != 0) {
            return 0;
        }
        if (!adv_has_fff0(&fields)) {
            return 0;
        }
        ESP_LOGI(TAG, "charger found (rssi %d), connecting", event->disc.rssi);
        ble_gap_disc_cancel();
        s_link = CB_LINK_CONNECTING;
        int rc = ble_gap_connect(s_own_addr_type, &event->disc.addr, 30000,
                                 NULL, gap_event, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "connect rc=%d", rc);
            start_scan();
        }
        return 0;
    }

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            s_link = CB_LINK_AUTHENTICATING;
            ESP_LOGI(TAG, "connected; discovering services");
            ble_gattc_disc_svc_by_uuid(s_conn, &UUID_SVC.u, on_disc_svc, NULL);
        } else {
            ESP_LOGW(TAG, "connect failed: %d", event->connect.status);
            start_scan();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "disconnected: reason=%d", event->disconnect.reason);
        reset_link();
        s_link = CB_LINK_DISCONNECTED;
        start_scan();
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
            start_scan();
        }
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        // Only fff1 carries framed telemetry; fff2 notifies the raw (unframed)
        // auth ack, which must not enter the frame reassembler.
        if (event->notify_rx.attr_handle != s_fff1_val) {
            return 0;
        }
        uint8_t  tmp[64];
        uint16_t copied = 0;
        if (ble_hs_mbuf_to_flat(event->notify_rx.om, tmp, sizeof(tmp), &copied) == 0) {
            s_last_rx_us = esp_timer_get_time();   // feed the watchdog
            cp_feed(&s_rx, tmp, copied, on_frame, NULL);
        }
        return 0;
    }

    default:
        return 0;
    }
}

static void start_scan(void)
{
    s_link = CB_LINK_SCANNING;
    struct ble_gap_disc_params dp = { 0 };
    dp.filter_duplicates = 1;
    dp.passive = 0;   // active scan so we receive the service UUID list
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &dp, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "scanning for charger (fff0)...");
    }
}

// ---- host bring-up -------------------------------------------------------
static void on_sync(void)
{
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    start_scan();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "nimble reset: %d", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void charger_ble_start(charger_ble_state_cb cb, void *ctx)
{
    s_cb  = cb;
    s_ctx = ctx;

    s_tx_q = xQueueCreate(TX_Q_DEPTH, sizeof(tx_item_t));
    if (!s_tx_q) {
        ESP_LOGE(TAG, "tx queue alloc failed");
        return;
    }

    reset_link();
    memset(&s_state, 0, sizeof(s_state));

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %d", ret);
        return;
    }

    // nimble_port_init() populates the npl_funcs table; the event API is only
    // usable after it succeeds.
    ble_npl_event_init(&s_tx_ev, tx_pump, NULL);
    s_tx_busy = false;

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb  = on_sync;

    // Bring up the default GATT server. Even as a central we need it: the charger
    // sends us an ATT Exchange-MTU Request, and without a GATT/ATT server those
    // requests are dropped ("ATT handler not found"), which wedges the peer's ATT
    // bearer and stops the notification stream after ~20-30 s. These init the
    // handlers that auto-answer the MTU exchange and keep the link alive.
    ble_svc_gap_init();
    ble_svc_gatt_init();

    nimble_port_freertos_init(host_task);
}

charger_ble_link_t charger_ble_link(void)
{
    return s_link;
}

void charger_ble_tick(void)
{
    // Recover a dead-but-"connected" link: READY with no notifications for too long.
    if (s_link != CB_LINK_READY || s_conn == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    if (esp_timer_get_time() - s_last_rx_us < (int64_t)RX_STALL_MS * 1000) {
        return;
    }

    ESP_LOGW(TAG, "no telemetry for %d ms - link stalled, reconnecting", RX_STALL_MS);
    // Push the RX stamp forward so we do not re-fire every tick while the
    // teardown is in flight. ble_gap_terminate() is safe from this task; its
    // DISCONNECT event runs reset_link() + start_scan() on the host task.
    s_last_rx_us = esp_timer_get_time();
    ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
}
