#include "charger_state.h"

#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "state";

// Tuning.
#define TICK_MS        500      // owner-task wake interval (Ah integration + link poll)
#define KEEPALIVE_MS   10000    // publish at least this often even without change
#define MAX_OBSERVERS  4
#define EVQ_DEPTH      16

// Battery-present / removed heuristic (PROTOCOL.md §11): present when the charger
// reports state 2 with real voltage; removed on the collapse to state 0 / ~0V.
#define V_PRESENT_MIN  1.0f
#define V_ABSENT_MAX   1.0f
#define I_ACTIVE_MIN   0.05f    // current above this counts as "charging"

// One queued telemetry event: a full decoded snapshot plus which field changed.
typedef struct {
    cp_state_t st;
    uint8_t    opcode;
} ev_t;

static QueueHandle_t     s_q;
static SemaphoreHandle_t s_mtx;

static charger_state_t   s_cur;         // owned by the state task
static charger_state_t   s_pub;         // last published (for change-detection)
static int64_t           s_last_pub_us;
static int64_t           s_last_integ_us;

static struct {
    charger_state_observer cb;
    void                  *ctx;
} s_obs[MAX_OBSERVERS];
static int s_obs_n;

const char *cs_link_name(charger_ble_link_t l)
{
    switch (l) {
    case CB_LINK_DISCONNECTED:   return "disconnected";
    case CB_LINK_SCANNING:       return "scanning";
    case CB_LINK_CONNECTING:     return "connecting";
    case CB_LINK_AUTHENTICATING: return "authenticating";
    case CB_LINK_READY:          return "ready";
    default:                     return "?";
    }
}

static int field_of(uint8_t op)
{
    switch (op) {
    case CP_OP_VOLTAGE:  return CS_F_VOLTAGE;
    case CP_OP_CURRENT:  return CS_F_CURRENT;
    case CP_OP_CAPACITY: return CS_F_CAPACITY;
    case CP_OP_STATE:    return CS_F_STATE;
    case CP_OP_PROGRAM:  return CS_F_PROGRAM;
    case CP_OP_STEP:     return CS_F_STEP;
    case CP_OP_ENABLE:   return CS_F_ENABLE;
    case CP_OP_ERROR:    return CS_F_ERROR;
    default:             return -1;
    }
}

// Merge a decoded event. cp_state_t already carries the latest of every field,
// so copy the raw values wholesale and stamp the one that changed.
static void merge(const ev_t *ev, int64_t now)
{
    s_cur.voltage_v     = ev->st.voltage_v;
    s_cur.current_a     = ev->st.current_a;
    s_cur.capacity_pct  = ev->st.capacity_pct;
    s_cur.charger_state = ev->st.state;
    s_cur.program       = ev->st.program;
    s_cur.step          = ev->st.step;
    s_cur.enable        = ev->st.enable;
    s_cur.error         = ev->st.error;
    s_cur.seen_mask     = ev->st.seen_mask;

    int f = field_of(ev->opcode);
    if (f >= 0) {
        s_cur.updated_at_us[f] = now;
    }
}

// Integrate current over elapsed time into the session Ah counter.
// Only accrue on a live link with real current; always advance the clock so a
// disconnected gap never dumps a spurious lump when the link returns.
static void integrate_ah(int64_t now)
{
    double dt_h = (double)(now - s_last_integ_us) / 3.6e9;  // us -> hours
    if (s_cur.link == CB_LINK_READY && s_cur.current_a > I_ACTIVE_MIN &&
        s_cur.battery_present && dt_h > 0.0) {
        s_cur.charge_delivered_ah += (float)(s_cur.current_a * dt_h);
    }

    s_last_integ_us = now;
}

// Derive battery-present / charging; reset the Ah session when a battery appears.
static void derive(int64_t now)
{
    bool prev_present = s_cur.battery_present;

    if (s_cur.charger_state == 2 && s_cur.voltage_v > V_PRESENT_MIN) {
        s_cur.battery_present = true;
    } else if (s_cur.charger_state == 0 && s_cur.voltage_v < V_ABSENT_MAX) {
        s_cur.battery_present = false;
    }
    // Ambiguous probing states hold the previous verdict (hysteresis).

    if (!prev_present && s_cur.battery_present) {
        s_cur.charge_delivered_ah = 0.0f;
        s_cur.session_start_us    = now;
    }

    s_cur.charging = (s_cur.step >= 1 && s_cur.step <= 8) &&
                     (s_cur.current_a > I_ACTIVE_MIN);
}

// Publish when a field crosses its threshold. charge_delivered_ah
// is deliberately excluded - it drifts every tick and would defeat throttling;
// it rides along on any other change and on the keepalive.
static bool meaningful(const charger_state_t *a, const charger_state_t *b)
{
    return fabsf(a->voltage_v - b->voltage_v) > 0.05f ||
           fabsf(a->current_a - b->current_a) > 0.05f ||
           a->capacity_pct    != b->capacity_pct ||
           a->charger_state   != b->charger_state ||
           a->program         != b->program ||
           a->step            != b->step ||
           a->enable          != b->enable ||
           a->error           != b->error ||
           a->link            != b->link ||
           a->battery_present != b->battery_present;
}

static void notify(const charger_state_t *snap)
{
    for (int i = 0; i < s_obs_n; i++) {
        s_obs[i].cb(snap, s_obs[i].ctx);
    }
}

static void state_task(void *arg)
{
    (void)arg;
    for (;;) {
        ev_t ev;
        bool got = xQueueReceive(s_q, &ev, pdMS_TO_TICKS(TICK_MS));

        xSemaphoreTake(s_mtx, portMAX_DELAY);

        charger_ble_tick();   // recover a stalled (dead-but-connected) BLE link

        int64_t now = esp_timer_get_time();
        s_cur.link  = charger_ble_link();
        if (got) {
            merge(&ev, now);
        }
        derive(now);
        integrate_ah(now);

        bool change    = meaningful(&s_cur, &s_pub);
        bool keepalive = (now - s_last_pub_us) >= (int64_t)KEEPALIVE_MS * 1000;
        bool publish   = change || keepalive;

        charger_state_t snap;
        if (publish) {
            s_pub         = s_cur;
            s_last_pub_us = now;
            snap          = s_cur;
        }

        xSemaphoreGive(s_mtx);

        if (publish) {
            notify(&snap);
        }
    }
}

void charger_state_init(void)
{
    if (s_q) {
        return;   // already initialised
    }

    s_mtx = xSemaphoreCreateMutex();
    s_q   = xQueueCreate(EVQ_DEPTH, sizeof(ev_t));
    if (!s_mtx || !s_q) {
        ESP_LOGE(TAG, "alloc failed (mtx=%p q=%p)", s_mtx, s_q);
        return;
    }

    memset(&s_cur, 0, sizeof(s_cur));
    memset(&s_pub, 0, sizeof(s_pub));
    s_cur.link      = CB_LINK_DISCONNECTED;
    s_last_integ_us = esp_timer_get_time();

    xTaskCreate(state_task, "state", 4096, NULL, 5, NULL);
}

void charger_state_submit(const cp_state_t *st, uint8_t opcode)
{
    if (!s_q) {
        return;
    }
    ev_t ev = { .st = *st, .opcode = opcode };
    // Non-blocking: drop rather than stall the BLE host task if the queue backs
    // up. cp_state_t is cumulative, so a dropped event only delays a field.
    if (xQueueSend(s_q, &ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full, dropping opcode 0x%02x", opcode);
    }
}

bool charger_state_add_observer(charger_state_observer cb, void *ctx)
{
    if (!cb) {
        return false;
    }

    bool ok = false;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_obs_n < MAX_OBSERVERS) {
        s_obs[s_obs_n].cb  = cb;
        s_obs[s_obs_n].ctx = ctx;
        s_obs_n++;
        ok = true;
    }
    xSemaphoreGive(s_mtx);

    return ok;
}

void charger_state_get(charger_state_t *out)
{
    if (!out) {
        return;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    *out = s_cur;
    xSemaphoreGive(s_mtx);
}
