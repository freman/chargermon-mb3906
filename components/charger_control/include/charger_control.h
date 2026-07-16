// charger_control - serialized outbound control for the MB3906.
//
// Every write to the charger routes through one control task and the single
// BLE TX path. Commands are confirmed by watching the telemetry
// stream, not by a write ack. Guard rails require an authenticated
// link and range-check the program. The firmware never auto-starts
// charging.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CC_OK = 0,
    CC_ERR_NOT_READY,   // BLE link is not authenticated (ready)
    CC_ERR_RANGE,       // program value out of range
    CC_ERR_QUEUE,       // command queue unavailable / full
} cc_result_t;

// Create the command queue and control task; register the link watcher for the
// auto-stop policy. Call after charger_state_init().
void charger_control_init(void);

// Set the charge program (0..8): 0 = standby (stop), 1..8 per cp_program_t.
// This is the primary control (setBatteryMode, PROTOCOL.md §8/§6).
cc_result_t charger_control_set_program(uint8_t program);

// Start a specific program (1..8). Rejects 0 - use charger_control_stop().
cc_result_t charger_control_start(uint8_t program);

// Stop charging: setBatteryMode(0) -> standby.
cc_result_t charger_control_stop(void);

// Start the last user-selected program (persisted in NVS when a start confirms).
// Returns CC_ERR_RANGE if no program has ever been selected. This backs the HA
// Charging switch ON, which is an explicit user action (never an auto-start).
cc_result_t charger_control_start_last(void);

// enableBattery(on). Secondary and unreliable alone; prefer
// set_program. Exposed for completeness / experimentation.
cc_result_t charger_control_enable(bool on);

// Re-poll status/error/step from the charger (the HA "refresh" button).
cc_result_t charger_control_refresh(void);

// Auto-stop policy, off by default. When on, the control layer
// issues a single stop (setBatteryMode 0) on each fresh transition to a ready
// link, so the charger is parked until explicitly started. Never auto-starts.
void charger_control_set_auto_stop(bool on);

#ifdef __cplusplus
}
#endif
