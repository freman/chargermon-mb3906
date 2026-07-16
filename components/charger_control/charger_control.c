#include "charger_control.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"

#include "charger_proto.h"
#include "charger_ble.h"
#include "charger_state.h"
#include "appcfg.h"

static const char *TAG = "control";

#define CMD_Q_DEPTH     8
#define CONFIRM_TRIES   5      // resend-until-readback attempts (PROTOCOL.md §9)
#define CONFIRM_WAIT_MS 400    // wait for the telemetry stream to reflect a change

typedef enum {
    CMD_SET_PROGRAM,
    CMD_ENABLE,
    CMD_REFRESH,
} cmd_kind_t;

typedef struct {
    cmd_kind_t kind;
    uint8_t    arg;
} cmd_t;

static QueueHandle_t s_cmd_q;
static volatile bool s_auto_stop;

// ---- enqueue (guard rails run here for fast caller feedback) --------------
static cc_result_t enqueue(cmd_kind_t kind, uint8_t arg)
{
    if (!s_cmd_q) {
        return CC_ERR_QUEUE;
    }
    if (charger_ble_link() != CB_LINK_READY) {
        return CC_ERR_NOT_READY;
    }

    cmd_t c = { .kind = kind, .arg = arg };

    return (xQueueSend(s_cmd_q, &c, 0) == pdTRUE) ? CC_OK : CC_ERR_QUEUE;
}

cc_result_t charger_control_set_program(uint8_t program)
{
    if (program > CP_PROG_RECOVERY) {
        return CC_ERR_RANGE;
    }

    return enqueue(CMD_SET_PROGRAM, program);
}

cc_result_t charger_control_start(uint8_t program)
{
    if (program < 1 || program > CP_PROG_RECOVERY) {
        return CC_ERR_RANGE;
    }

    return enqueue(CMD_SET_PROGRAM, program);
}

cc_result_t charger_control_stop(void)
{
    return enqueue(CMD_SET_PROGRAM, CP_PROG_STANDBY);
}

cc_result_t charger_control_start_last(void)
{
    uint8_t p = appcfg_get()->last_program;
    if (p < 1 || p > CP_PROG_RECOVERY) {
        return CC_ERR_RANGE;   // nothing stored yet; select a program first
    }

    return enqueue(CMD_SET_PROGRAM, p);
}

cc_result_t charger_control_enable(bool on)
{
    return enqueue(CMD_ENABLE, on ? 1 : 0);
}

cc_result_t charger_control_refresh(void)
{
    return enqueue(CMD_REFRESH, 0);
}

void charger_control_set_auto_stop(bool on)
{
    s_auto_stop = on;
}

// ---- execution (control task) --------------------------------------------
static void send_frame(const uint8_t *f, uint8_t n)
{
    int rc = charger_ble_send(f, n);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble send rc=%d", rc);
    }
}

// A set_program is confirmed when the readback reflects it: for stop, standby
// program and idle step; otherwise the program byte matches (PROTOCOL.md §8/§9).
static bool program_confirmed(uint8_t want)
{
    charger_state_t s;
    charger_state_get(&s);

    if (want == CP_PROG_STANDBY) {
        return s.program == CP_PROG_STANDBY && s.step == 0;
    }

    return s.program == want;
}

static void do_set_program(uint8_t program)
{
    uint8_t f[4];
    size_t  n = cp_set_program(program, f);

    for (int i = 0; i < CONFIRM_TRIES; i++) {
        if (charger_ble_link() != CB_LINK_READY) {
            ESP_LOGW(TAG, "link lost mid-command; abandoning program %u", program);
            return;
        }

        send_frame(f, (uint8_t)n);
        vTaskDelay(pdMS_TO_TICKS(CONFIRM_WAIT_MS));

        if (program_confirmed(program)) {
            ESP_LOGI(TAG, "program %u (%s) confirmed", program, cp_program_name(program));
            // Remember the last real program so the HA Charging
            // switch ON can resume it. Standby (stop) is not remembered.
            if (program != CP_PROG_STANDBY) {
                appcfg_set_last_program(program);
            }
            return;
        }
    }

    ESP_LOGW(TAG, "program %u (%s) not confirmed after %d tries",
             program, cp_program_name(program), CONFIRM_TRIES);
}

static void do_enable(bool on)
{
    uint8_t f[4];
    size_t  n = cp_enable(on, f);

    // Secondary command; no confirm loop (ineffective alone).
    send_frame(f, (uint8_t)n);
    ESP_LOGI(TAG, "enableBattery(%d) sent", on);
}

static void do_refresh(void)
{
    // Getters "<op> 01 00 <xor>" to fff1: status, error, step (PROTOCOL.md §5).
    static const uint8_t ops[] = { CP_OP_STATE, CP_OP_ERROR, CP_OP_STEP };

    for (unsigned i = 0; i < sizeof(ops); i++) {
        uint8_t       f[4];
        const uint8_t zero = 0x00;
        size_t        n = cp_frame(ops[i], &zero, 1, f, sizeof(f));

        send_frame(f, (uint8_t)n);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void control_task(void *arg)
{
    (void)arg;
    for (;;) {
        cmd_t c;
        if (xQueueReceive(s_cmd_q, &c, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        // Re-check the link: it may have dropped between enqueue and now.
        if (charger_ble_link() != CB_LINK_READY) {
            ESP_LOGW(TAG, "dropping command %d: link not ready", c.kind);
            continue;
        }

        switch (c.kind) {
        case CMD_SET_PROGRAM: do_set_program(c.arg);      break;
        case CMD_ENABLE:      do_enable(c.arg != 0);      break;
        case CMD_REFRESH:     do_refresh();               break;
        }
    }
}

// ---- link watcher: boot/reconnect auto-stop policy ------------
static void on_state(const charger_state_t *s, void *ctx)
{
    (void)ctx;
    static charger_ble_link_t prev = CB_LINK_DISCONNECTED;

    if (s->link == CB_LINK_READY && prev != CB_LINK_READY && s_auto_stop) {
        ESP_LOGI(TAG, "auto-stop on connect: setBatteryMode(0)");
        charger_control_stop();
    }

    prev = s->link;
}

void charger_control_init(void)
{
    if (s_cmd_q) {
        return;
    }

    s_cmd_q = xQueueCreate(CMD_Q_DEPTH, sizeof(cmd_t));
    if (!s_cmd_q) {
        ESP_LOGE(TAG, "command queue alloc failed");
        return;
    }

    charger_state_add_observer(on_state, NULL);
    xTaskCreate(control_task, "control", 4096, NULL, 5, NULL);
}
