// charger_proto - framing, decode and stream reassembly for the Powertech MB3906
// BLE charger protocol. Pure C, no ESP dependencies (host-testable).
//
// See docs/PROTOCOL.md for the wire format. Framing: opcode, len, payload..., xor.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- Opcodes -------------------------------------------------------------
// Telemetry (charger -> us, notifications on fff1)
#define CP_OP_STATE     0x21  // 0/1 probing, 2 battery detected
#define CP_OP_ERROR     0x22  // boolean in practice: 0 ok, non-zero fault
#define CP_OP_PROGRAM   0x23  // battery-type / program (see cp_program_t)
#define CP_OP_STEP      0x24  // charge step 0=idle, 1..8
#define CP_OP_ENABLE    0x25  // charge-enable state 0/1
#define CP_OP_VOLTAGE   0x60  // LE u16 / 10.0 -> volts
#define CP_OP_CURRENT   0x61  // LE u16 / 100.0 -> amps  (NB: /100, not /10)
#define CP_OP_CAPACITY  0x62  // percent

// Commands (us -> charger, written to fff1). write opcode = read opcode | 0x80.
#define CP_OP_ANSWER    0x00  // per-packet ack: 00 01 00 01
#define CP_OP_APPSTATUS 0xA0  // "app present" flag
#define CP_OP_SETPROG   0xA3  // set program / start (data=program, 0=standby=stop)
#define CP_OP_ENABLE_W  0xA5  // enableBattery(on): data = on?0:1
#define CP_OP_HANDSHAKE 0xFF  // FF 01 00 FE

// ---- Programs (0x23) -----------------------------------------------------
typedef enum {
    CP_PROG_STANDBY   = 0,
    CP_PROG_6V        = 1,
    CP_PROG_6V_COLD   = 2,
    CP_PROG_CAR       = 3,
    CP_PROG_CAR_COLD  = 4,
    CP_PROG_BIKE      = 5,
    CP_PROG_BIKE_COLD = 6,
    CP_PROG_LITHIUM   = 7,
    CP_PROG_RECOVERY  = 8,
} cp_program_t;

const char *cp_program_name(uint8_t program);
const char *cp_step_name(uint8_t step);

// ---- Decoded state -------------------------------------------------------
typedef struct {
    float    voltage_v;      // 0x60
    float    current_a;      // 0x61
    uint8_t  capacity_pct;   // 0x62
    uint8_t  state;          // 0x21
    uint8_t  program;        // 0x23
    uint8_t  step;           // 0x24
    bool     enable;         // 0x25
    uint8_t  error;          // 0x22
    uint32_t seen_mask;      // bit(opcode & 0x1f) set once a field is seen
} cp_state_t;

// The 6-byte default password ("123456"), written raw (unframed) to fff2.
extern const uint8_t CP_PASSWORD[6];

// ---- Frame building ------------------------------------------------------
// Build "opcode,len,payload...,xor" into out. Returns total length, or 0 if
// out_cap is too small.
size_t cp_frame(uint8_t opcode, const uint8_t *payload, uint8_t plen,
                uint8_t *out, size_t out_cap);

// Convenience builders. out must have room for >= 4 bytes. Return length.
size_t cp_handshake(uint8_t *out);              // FF 01 00 FE
size_t cp_answer(uint8_t *out);                 // 00 01 00 01
size_t cp_set_program(uint8_t program, uint8_t *out);  // A3 01 <p> <xor>
size_t cp_enable(bool on, uint8_t *out);        // A5 01 <on?0:1> <xor>

// True if a received opcode is a discrete state packet the app ack's with answer().
bool cp_opcode_needs_ack(uint8_t opcode);

// ---- Decoding ------------------------------------------------------------
// Validate + decode one frame into st. Returns the opcode handled, or 0.
uint8_t cp_decode(const uint8_t *frame, size_t len, cp_state_t *st);

// ---- Stream reassembler --------------------------------------------------
// Notifications arrive concatenated/fragmented during charging; buffer bytes
// and emit whole validated frames.
typedef void (*cp_frame_cb)(const uint8_t *frame, size_t len, void *ctx);

typedef struct {
    uint8_t buf[64];
    size_t  len;
} cp_reassembler_t;

void cp_reassembler_reset(cp_reassembler_t *r);
void cp_feed(cp_reassembler_t *r, const uint8_t *data, size_t n,
             cp_frame_cb cb, void *ctx);

#ifdef __cplusplus
}
#endif
