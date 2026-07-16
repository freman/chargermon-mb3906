// charger_ble - NimBLE central that connects to the MB3906 charger, authenticates,
// and streams decoded telemetry via a callback. See docs/PROTOCOL.md.
#pragma once

#include "charger_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CB_LINK_DISCONNECTED = 0,
    CB_LINK_SCANNING,
    CB_LINK_CONNECTING,
    CB_LINK_AUTHENTICATING,
    CB_LINK_READY,
} charger_ble_link_t;

// Invoked (from the NimBLE host task) whenever a telemetry field updates.
// `opcode` is the field that changed; `st` is the full latest state.
typedef void (*charger_ble_state_cb)(const cp_state_t *st, uint8_t opcode, void *ctx);

// Start the BLE central. NVS must already be initialised. Non-blocking; the
// stack runs in its own task.
void charger_ble_start(charger_ble_state_cb cb, void *ctx);

// Current link state (for the UI / MQTT availability, later).
charger_ble_link_t charger_ble_link(void);

// Periodic health check - call regularly (e.g. from the state owner tick). If the
// link is READY but no notification has arrived for RX_STALL_MS, the connection is
// a dead ghost; this tears it down so the scan/reconnect path recovers.
void charger_ble_tick(void);

// Send a framed command to fff1. Fire-and-forget: the frame is queued and
// written on the BLE host task, serialized so writes never overlap.
// Requires the link to be READY. Returns 0 on enqueue, negative otherwise.
// There is no write-level confirmation - control confirms via the telemetry
// stream. `len` must be 1..8.
int charger_ble_send(const uint8_t *data, uint8_t len);

// One-shot getter poll of the discrete-state opcodes (0x21 state, 0x22 error,
// 0x23 program, 0x24 step, 0x25 enable). The charger broadcasts these on change,
// so a client that connects mid-session (after the last change) would otherwise
// miss them; this snapshots current values right after auth. Ongoing updates
// arrive on the stream - no periodic re-poll needed. No-op unless link is READY.
void charger_ble_poll_discrete(void);

#ifdef __cplusplus
}
#endif
