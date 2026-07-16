// charger_state - single owner of the authoritative ChargerState.
//
// The BLE host-task callback submits decoded telemetry via charger_state_submit();
// a dedicated owner task merges it, derives battery-present / charging / Ah, runs
// change-detection, and fans the result out to registered observers. Pull
// consumers (web, OLED) read a consistent snapshot with charger_state_get().
#pragma once

#include "charger_proto.h"
#include "charger_ble.h"   // reuse charger_ble_link_t for link state

#ifdef __cplusplus
extern "C" {
#endif

// Per-field last-update timestamps let consumers tell live from stale.
typedef enum {
    CS_F_VOLTAGE = 0,
    CS_F_CURRENT,
    CS_F_CAPACITY,
    CS_F_STATE,
    CS_F_PROGRAM,
    CS_F_STEP,
    CS_F_ENABLE,
    CS_F_ERROR,
    CS_F_COUNT,
} cs_field_t;

// The authoritative state. Raw telemetry mirrors cp_state_t; the rest is derived
// or link/bookkeeping. Snapshots are copied by value, so this is safe to hold.
typedef struct {
    charger_ble_link_t link;            // BLE link state (availability)

    // Raw telemetry (latest of each field seen).
    float    voltage_v;                 // 0x60
    float    current_a;                 // 0x61
    uint8_t  capacity_pct;              // 0x62
    uint8_t  charger_state;             // 0x21: 0/1 probing, 2 detected
    uint8_t  program;                   // 0x23
    uint8_t  step;                      // 0x24: 0 idle, 1..8
    bool     enable;                    // 0x25
    uint8_t  error;                     // 0x22: non-zero = fault

    // Derived.
    bool     battery_present;           // derived from state + voltage
    bool     charging;                  // active step with real current
    float    charge_delivered_ah;       // per charge session

    // Bookkeeping.
    uint32_t seen_mask;                 // bit(field) set once seen
    int64_t  updated_at_us[CS_F_COUNT]; // esp_timer_get_time() per field
    int64_t  session_start_us;          // start of the current Ah session
} charger_state_t;

// Called by an observer whenever a published (meaningful or keepalive) update
// occurs. `snap` is a private copy, valid only for the call. Runs in the state
// task context; keep it quick and non-blocking (post to a queue for real work).
typedef void (*charger_state_observer)(const charger_state_t *snap, void *ctx);

// Create the queue, mutex and owner task. Call once, after NVS init.
void charger_state_init(void);

// Submit a decoded telemetry update. Safe from any task (BLE host task in
// particular). `st` is copied; `opcode` is the field that changed (per cp_decode).
void charger_state_submit(const cp_state_t *st, uint8_t opcode);

// Register a fan-out observer. Returns false if the (small) table is full.
bool charger_state_add_observer(charger_state_observer cb, void *ctx);

// Copy a consistent snapshot for pull consumers (web/OLED).
void charger_state_get(charger_state_t *out);

// Human-readable BLE link name.
const char *cs_link_name(charger_ble_link_t l);

#ifdef __cplusplus
}
#endif
