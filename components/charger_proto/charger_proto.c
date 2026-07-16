#include "charger_proto.h"
#include <string.h>

const uint8_t CP_PASSWORD[6] = { '1', '2', '3', '4', '5', '6' };

static uint8_t xor_of(const uint8_t *p, size_t n)
{
    uint8_t x = 0;
    for (size_t i = 0; i < n; i++) x ^= p[i];
    return x;
}

const char *cp_program_name(uint8_t program)
{
    switch (program) {
    case CP_PROG_STANDBY:   return "standby";
    case CP_PROG_6V:        return "6V";
    case CP_PROG_6V_COLD:   return "6V-cold";
    case CP_PROG_CAR:       return "car";
    case CP_PROG_CAR_COLD:  return "car-cold";
    case CP_PROG_BIKE:      return "bike";
    case CP_PROG_BIKE_COLD: return "bike-cold";
    case CP_PROG_LITHIUM:   return "lithium";
    case CP_PROG_RECOVERY:  return "recovery";
    default:                return "?";
    }
}

const char *cp_step_name(uint8_t step)
{
    switch (step) {
    case 0: return "idle";
    case 1: return "check";
    case 2: return "recovery";
    case 3: return "soft-start";
    case 4: return "bulk";
    case 5: return "absorption";
    case 6: return "maintenance-1";
    case 7: return "maintenance-2";
    case 8: return "trickle";
    default: return "?";
    }
}

size_t cp_frame(uint8_t opcode, const uint8_t *payload, uint8_t plen,
                uint8_t *out, size_t out_cap)
{
    size_t total = (size_t)plen + 3;
    if (out_cap < total) {
        return 0;
    }

    out[0] = opcode;
    out[1] = plen;
    if (plen && payload) {
        memcpy(out + 2, payload, plen);
    }
    out[total - 1] = xor_of(out, total - 1);

    return total;
}

size_t cp_handshake(uint8_t *out)
{
    const uint8_t d = 0x00;
    return cp_frame(CP_OP_HANDSHAKE, &d, 1, out, 4);
}

size_t cp_answer(uint8_t *out)
{
    const uint8_t d = 0x00;
    return cp_frame(CP_OP_ANSWER, &d, 1, out, 4);
}

size_t cp_set_program(uint8_t program, uint8_t *out)
{
    return cp_frame(CP_OP_SETPROG, &program, 1, out, 4);
}

size_t cp_enable(bool on, uint8_t *out)
{
    // enableBattery(on) encodes data as (on ^ 1): true -> 0x00, false -> 0x01
    const uint8_t d = on ? 0x00 : 0x01;
    return cp_frame(CP_OP_ENABLE_W, &d, 1, out, 4);
}

bool cp_opcode_needs_ack(uint8_t opcode)
{
    switch (opcode) {
    case CP_OP_STATE:
    case CP_OP_ERROR:
    case CP_OP_PROGRAM:
    case CP_OP_STEP:
    case CP_OP_ENABLE:
    case 0x27:            // model (not present on MB3906, but the app ack's it)
    case 0x28:            // preselection mode
    case CP_OP_CAPACITY:
        return true;
    default:
        return false;
    }
}

uint8_t cp_decode(const uint8_t *frame, size_t len, cp_state_t *st)
{
    if (len < 4) {
        return 0;
    }
    uint8_t plen = frame[1];
    if ((size_t)plen + 3 != len) {
        return 0;
    }
    if (xor_of(frame, len - 1) != frame[len - 1]) {
        return 0;
    }

    uint8_t op = frame[0];
    const uint8_t *d = frame + 2;

    switch (op) {
    case CP_OP_VOLTAGE:
        if (plen >= 2) st->voltage_v = (float)((d[1] << 8) | d[0]) / 10.0f;
        break;
    case CP_OP_CURRENT:
        if (plen >= 2) st->current_a = (float)((d[1] << 8) | d[0]) / 100.0f;
        break;
    case CP_OP_CAPACITY:
        st->capacity_pct = d[0];
        break;
    case CP_OP_STATE:
        st->state = d[0];
        break;
    case CP_OP_PROGRAM:
        st->program = d[0];
        break;
    case CP_OP_STEP:
        st->step = d[0];
        break;
    case CP_OP_ENABLE:
        st->enable = (d[0] != 0);
        break;
    case CP_OP_ERROR:
        st->error = d[0];
        break;
    default:
        return 0;   // unhandled opcode (e.g. the charger's answer echo)
    }

    st->seen_mask |= (1u << (op & 0x1f));
    return op;
}

void cp_reassembler_reset(cp_reassembler_t *r)
{
    r->len = 0;
}

void cp_feed(cp_reassembler_t *r, const uint8_t *data, size_t n,
             cp_frame_cb cb, void *ctx)
{
    // Append, guarding against overflow (frames are tiny; overflow only if we
    // are wedged on garbage - in which case dropping the buffer is correct).
    if (r->len + n > sizeof(r->buf)) {
        r->len = 0;
        if (n > sizeof(r->buf)) {
            data += (n - sizeof(r->buf));
            n = sizeof(r->buf);
        }
    }
    memcpy(r->buf + r->len, data, n);
    r->len += n;

    // Extract whole frames.
    while (r->len >= 4) {
        uint8_t ln = r->buf[1];
        size_t flen = (size_t)ln + 3;

        if (ln == 0 || flen > 24) {          // implausible - resync by one byte
            memmove(r->buf, r->buf + 1, --r->len);
            continue;
        }
        if (r->len < flen) {
            break;                            // need more bytes
        }
        if (xor_of(r->buf, flen - 1) == r->buf[flen - 1]) {
            if (cb) cb(r->buf, flen, ctx);
            memmove(r->buf, r->buf + flen, r->len - flen);
            r->len -= flen;
        } else {
            memmove(r->buf, r->buf + 1, --r->len);
        }
    }
}
