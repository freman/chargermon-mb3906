# Powertech MB3906 BLE Protocol

Reverse-engineered BLE protocol for the **Powertech MB3906** "8 Step Bluetooth
Intelligent Lead Acid and Lithium Battery Charger" (Electus Distribution / Jaycar,
AU). Should also apply to close siblings that share the vendor app.

**Status:** hardware-verified against a live unit (2026-07-12), cross-checked
against the decompiled vendor app and the printed manual. Everything below has been
observed working unless explicitly marked otherwise.

**Provenance / how to reproduce:** a PC BLE central ([bleak](https://github.com/hbldh/bleak))
driving the charger, plus the vendor Android app `com.cchip.charge` ("BT Charger
2.0") decompiled with [androguard](https://github.com/androguard/androguard).

---

## 1. Device identity

| | |
|---|---|
| Product | Powertech MB3906 (8-step, 6V / 12V / 12.8V LiFePO4) |
| Vendor app | `com.cchip.charge` - "BT Charger 2.0", model shown as `MW LF-CC040LCDBT` |
| App device family | **"V2"** (the app also supports V1/V3/V4 families with different GATT) |
| BLE advertised name | `BTC` |
| Advertised service | `0000fff0-0000-1000-8000-00805f9b34fb` |
| BLE module | Beken `BK-BLE-1.0` (per Device Information service) - a clone of a TI example profile |
| Charge currents | 0.9A / 4.0A / 3.0A (max 4.0A) |
| Sibling | MB3908 = `MW LF-CC075LCDBT` |

---

## 2. GATT profile

Primary service `0000fff0-0000-1000-8000-00805f9b34fb`:

| Characteristic | UUID | Properties | Role |
|---|---|---|---|
| `fff1` | `0000fff1-…` | read, write, notify | **Telemetry notifications IN, and ALL command writes OUT** |
| `fff2` | `0000fff2-…` | read, write, notify | **Password writes ONLY** (+ auth ack notifies here) |

> ### ⚠ The #1 gotcha: commands go to `fff1`, not `fff2`
>
> Only the 6-byte password is written to `fff2`. **Every framed command** (handshake,
> answer/ack, set-program, start/stop) is written to **`fff1`** - the same
> characteristic that streams telemetry back. Writing commands to `fff2` produces
> no error and no effect; the charger silently ignores them. In the decompiled app
> this is buried four hops deep:
> `sendData -> writeDataDirect -> getV2Gatt -> getWriteCharacteristic = CHARACTERISTIC_DATA_V2 = fff1`,
> while the password path uses `getWritePwdCharacteristic = fff2`.

Both characteristics also expose Device Information (`180a`) and a TI OAD firmware
service (`f000ffc0…`, `ffc1`/`ffc2`) for the charger's own firmware - not used here.

Enable notifications on both `fff1` and `fff2` (the auth ack arrives on `fff2`,
telemetry on `fff1`).

---

## 3. Packet framing

```
byte 0    : command opcode
byte 1    : len          - number of PAYLOAD bytes (NOT total frame length)
byte 2..  : payload[0 .. len-1]
last byte : xor checksum - XOR of every preceding byte (opcode, len, payload)
```

- Total frame length = `len + 3`.
- Validate received frames by recomputing the trailing XOR.
- Example: voltage frame `60 02 7C 00 1E` -> `0x60 ^ 0x02 ^ 0x7C ^ 0x00 = 0x1E`. OK.

### Reassembly (important during charging)

During an active charge the charger emits packets fast enough that BLE
notifications arrive **concatenated and/or split across notification boundaries**.
A naive "one notification = one packet" parser mis-splits them. Buffer incoming
bytes and extract frames by the `opcode / len / … / xor` structure, validating each
checksum and re-syncing by one byte on failure.

---

## 4. Authentication

After enabling notifications:

1. **Handshake** -> `fff1`: `FF 01 00 FE`. Charger replies `00 01 00 01` on `fff1`.
2. Wait ~500-800 ms.
3. **Password** -> `fff2`: ASCII `"123456"` = `31 32 33 34 35 36` (6 raw bytes, **no**
   framing/checksum). Charger acks `01` on `fff2`.

Telemetry only begins streaming after auth. The analog fields (`0x60`/`0x61`/`0x62`)
then stream continuously; the discrete states (`0x21`-`0x25`) are broadcast on change
(see §5). The default password is `123456`; the app lets the user change it per-device.

> ### ⚠ The charger sends *us* an ATT Exchange-MTU Request - you must answer it
> On connect the charger (as an ATT client to our GATT server) issues an
> `Exchange MTU Request` (ATT opcode `0x02`). The central **must** have a GATT/ATT
> server able to auto-answer it. If it does not, the request is dropped, the
> charger's ATT bearer keeps the transaction outstanding, and **~30 s later that
> transaction times out - after which the charger stops sending all notifications**
> (an ATT timeout forbids further operations on the bearer). Symptom: telemetry
> streams fine for ~20-30 s then goes silent while the link still looks connected.
> Verified 2026-07-13. On ESP-IDF NimBLE this means the **peripheral role must stay
> enabled** (`CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=y`) and `ble_svc_gatt_init()` /
> `ble_svc_gap_init()` must run - a "central-only" build silently breaks the stream.
> BlueZ/bleak answer the MTU request automatically, which is why a PC monitor never
> sees this.

---

## 5. Telemetry (charger -> app, notifications on `fff1`)

| Opcode | len | Field | Decode |
|---|---|---|---|
| `0x60` | 2 | Voltage | little-endian `u16 / 10.0` -> volts |
| `0x61` | 2 | Current | little-endian `u16 / 100.0` -> amps  ⚠ **/100, not /10** |
| `0x62` | 1 | Capacity | `payload[0]` percent |
| `0x21` | 1 | Charger state | `0`/`1` = probing, `2` = battery detected |
| `0x22` | 2 | Error | boolean in practice: `0` = OK, non-zero = fault (see notes) |
| `0x23` | 1 | Program | see §6 |
| `0x24` | 1 | Charge step | `0` = idle, `1`-`8` = 8-step strategy, see §7 |
| `0x25` | 1 | Charge-enable state | `0`/`1` |

> ### ⚠ The #2 gotcha: voltage and current use different scales
> Voltage is `/10`, current is `/100`. Raw current `397` is **3.97 A**, not 39.7 A
> (the charger maxes at 4 A). Confirmed by the app's own code dividing by the float
> constants `10.0` and `100.0`, and by the app UI showing e.g. `4.01A`.

### On getters and errors

- ✅ **The charger streams everything autonomously - but the two classes have
  different cadences.** Verified on hardware 2026-07-13 with a direct PC-to-charger
  capture ([tools/ble_monitor.py](../tools/ble_monitor.py), no getters sent):
  - **Analog fields** - `0x60` voltage, `0x61` current, `0x62` capacity - stream
    **continuously** (~1-2 Hz) once auth completes.
  - **Discrete state fields** - `0x21` state, `0x22` error, `0x23` program,
    `0x24` step, `0x25` enable - are **edge-triggered**: the charger broadcasts one
    only when its value *changes*. `0x25` also pulses during standby probing.
  - Observed on the wire the instant a battery was connected (no getter, no program):
    `21 01 02` (state 2 = detected) then `60 02 80 00` (12.8 V). Starting the cycle
    then broadcast `23` (program) and a rising `24` (step): check -> bulk -> absorption.
- ⚠ **The edge-trigger has a gotcha for late joiners.** If you connect *after* a
  discrete value last changed (e.g. attach mid-charge to a charger already sitting in
  absorption), you never see that field until it next changes - so program/step/state
  read as their initial `0` until then. A **one-shot getter poll right after auth**
  fixes this by fetching the current snapshot; continuous re-polling is **not** needed
  and is best avoided (it adds write traffic that competes with the inbound stream).
- **Getter frame** = the read opcode with a single `0x00` payload byte:
  `<op> 01 00 <xor>`, written to `fff1`. E.g. state = `21 01 00 20`, step =
  `24 01 00 25`. The charger replies once per getter with the matching notification.
- The `A0 01 <b>` "app present" flag was tested (2026-07-13) and is **not** required -
  the stream flows without it.
- The vendor app treats `0x22` as a **boolean** (`!= 0` -> show one generic error
  icon); it does not decode individual codes. The manual's named faults
  (battery-not-connected / battery-fault / incorrect-polarity) appear on the
  charger's LCD, not decoded from this byte. Reverse-mapping specific codes would
  require deliberate fault injection.

---

## 6. Programs / battery types (`0x23`)

Values mapped from the vendor app's string resources, cross-checked against the
physical MODE button labels. The **odd/even split is Normal/Cold** (the snowflake
variant):

| `0x23` | Program | Current |
|---|---|---|
| 0 | Standby | - |
| 1 | 6V (Normal) | 4.0A |
| 2 | 6V Cold | 4.0A |
| 3 | Car / 12V (Normal) | 4.0A |
| 4 | Cold Car | 4.0A |
| 5 | Bike / Motorcycle / 12V | 0.9A |
| 6 | Cold Bike | 0.9A |
| 7 | Lithium 12.8V | 3.0A |
| 8 | Recovery (deep-discharge repair) | ~4.0A nominal |

"Cold" variants raise the target voltage (14.5V -> 14.7V).

---

## 7. Charge steps (`0x24`) - the "8 step" strategy

`0x24` is the current step number: `0` = idle, `1`-`8` as below.

| `0x24` | Step |
|---|---|
| 0 | Idle / standby |
| 1 | Check (verify connection & battery stable) |
| 2 | Recovery (pulse desulfation) |
| 3 | Soft Start |
| 4 | Bulk (to ~80%) |
| 5 | Absorption |
| 6 | Maintenance 1 (100%) |
| 7 | Maintenance 2 (voltage-triggered top-up) |
| 8 | Trickle / pulse maintenance (indefinite) |

---

## 8. Control (app -> charger, written to `fff1`)

All framed `opcode len payload xor`. Elegant pattern: **write opcode = read opcode
`| 0x80`** (`0x25`->`0xA5`, `0x23`->`0xA3`, `0x28`->`0xA8`, `0x20`->`0xA0`).
Control commands are **not acked** - confirm by watching the telemetry stream change.

| Action | Bytes | Notes |
|---|---|---|
| **Start a program** | `A3 01 <m> <xor>` | `setBatteryMode(m)`, `m` per §6. Selecting a program starts that charge cycle. |
| **Stop (standby)** | `A3 01 00 A2` | `setBatteryMode(0)`. This is the reliable stop (what the app's stop button does). |
| Enable output | `A5 01 00 A4` | `enableBattery(true)`. Secondary - did not reliably start/stop on its own; prefer `setBatteryMode`. |
| Disable output | `A5 01 01 A5` | `enableBattery(false)`. Secondary (see above). |
| Set app-status | `A0 01 <b> <xor>` | "app present" flag; the app sends it. Tested 2026-07-13: **not** required - the stream flows without it. |
| **Getter (poll)** | `<op> 01 00 <xor>` | Request one notification for that opcode. Useful as a one-shot snapshot right after auth (discrete states are edge-triggered, so a late joiner otherwise misses them until they next change). E.g. step = `24 01 00 25`. |
| Set preselection | `A8 01 <m> <xor>` | Untested. |
| Answer / ACK | `00 01 00 01` | See §9. |

Verified live: `A3 01 03 A1` (`setBatteryMode(3)`) starts a 4A "car" charge;
`A3 01 00 A2` returns to standby (step -> idle, current -> 0).

---

## 9. Answer/ACK protocol

The vendor app replies with `answer()` = **`00 01 00 01`** (to `fff1`) after **each
discrete state notification**: `0x21`, `0x22`, `0x23`, `0x24`, `0x25`, `0x27`,
`0x28`, `0x62`. The high-rate `0x60`/`0x61` (voltage/current) streams are **not**
acked. `enableBattery`/`setBatteryMode` are sent by the app through a resend loop
until the matching read-back reflects the new value.

In testing, a bare `setBatteryMode` took effect **without** faithfully running this
ACK loop, so it may not be strictly required - but replicating it is the safe choice
if you want to match the app's behaviour exactly.

---

## 10. Battery detection thresholds (from the manual)

Explains the charger's behaviour on connect (it probes before committing):

| Measured voltage | Behaviour |
|---|---|
| < 2.0V or > 14V | "not suitable / defective" - fault, no charge |
| 2.0 - 7.0V | detected as a 6V battery |
| 7.0 - 10.5V | ambiguous (charged 6V vs deep-flat 12V): pulses Recovery; if it doesn't rise past 10.5V in ~2h -> fault, standby |
| Recovery mode | accepts 2.0-14V as 12V; if not risen to 12V in ~3h -> fault, standby |

A clinically-dead cell will pulse-probe forever and never commit to a cycle.

---

## 11. Client lifecycle - the whole dance, in order

Everything above as one ordered recipe. Each step references the section with the
details.

0. **Get the charger to yourself.** The charger accepts **one central at a time**.
   If the vendor app (or another monitor) is connected, the charger stops
   advertising and a scan finds nothing. Close the app first.
1. **Scan** for the advertised service UUID `fff0` (§1). The name `BTC` is
   generic; the service UUID is the reliable selector.
2. **Connect** - and make sure your stack can **answer the charger's ATT
   Exchange-MTU Request** (§4). PC stacks do this for you; embedded
   "central-only" builds often cannot, and the failure is invisible for ~30 s.
3. **Discover** `fff1` and `fff2`, then **enable notifications on both** (write
   `01 00` to each CCCD). Telemetry arrives on `fff1`, the auth ack on `fff2` (§2).
4. **Handshake:** write `FF 01 00 FE` to `fff1`; expect `00 01 00 01` back on
   `fff1` (§4).
5. **Wait ~500-800 ms** (the vendor app does; a write-with-response round-trip
   usually suffices).
6. **Password:** write the raw 6 bytes `"123456"` to `fff2` - no framing, no
   checksum; expect a `01` ack on `fff2` (§4).
7. **Telemetry starts.** Analog fields stream ~1-2 Hz; run the byte-stream
   **reassembler** (§3), not a one-notification-one-packet parser.
8. **Snapshot the discretes** with a one-shot getter poll of
   `0x21/0x22/0x23/0x24/0x25` (§5) - they are edge-triggered, so a late joiner
   never sees them otherwise. Do not re-poll continuously.
9. **Ack discrete notifications** with `00 01 00 01` to `fff1` if you want to
   match the app exactly (§9); the stream flows without it.
10. **Watchdog the stream.** The analog stream is the only liveness signal: if a
    "connected" link goes >~5 s without a notification, it is dead - tear down and
    reconnect. (This is also how the unanswered-MTU failure of step 2 manifests.)
11. **On any disconnect, start over from step 2.** Auth is per-connection; nothing
    persists on the charger side.
12. **Control** (§8): write framed commands to `fff1`, never `fff2`. Commands are
    not acked - confirm by watching the telemetry reflect the change
    (`0x23`/`0x24`), resending until it does.

### Behaviour to expect (not bugs)

- **Battery removal raises no error.** Yank a terminal mid-charge and `0x22`
  stays 0; the charger just collapses to standby within ~1-2 s (`0x21`→0,
  `0x23`→0, `0x24`→0, voltage/current/capacity→0). Infer "battery removed" from
  that collapse, not from an error packet.
- **`0x25` self-cycles 0/1 during standby probing** - it is not a heartbeat and
  not useful as a charging indicator; derive "charging" from step + current.
- **Capacity (`0x62`) wanders** while the charger re-estimates under load
  (25→80% swings mid-cycle are normal).

---

## Quick reference

```
Service   fff0
  fff1    telemetry notifications IN  +  ALL command writes OUT
  fff2    password write ONLY (+ auth ack)

Auth      fff1: FF 01 00 FE      (handshake, reply 00 01 00 01)
          fff2: "123456"         (ack 01)

Frame     opcode, len, payload..., xor(all prev)

Read      60=V/10  61=I/100  62=cap%   <- stream continuously (~1-2 Hz)
          21=state 22=err(bool) 23=program 24=step(0..8) 25=enable
                                        <- broadcast on CHANGE (edge-triggered)

Poll→fff1   <op> 01 00 <xor>   getter, one reply each (e.g. 24 01 00 25)
            optional: one-shot after auth to snapshot discretes (late joiner)

Write→fff1  A3 01 <m>  start program m (0=stop/standby)
            A5 01 00 / A5 01 01  enable/disable (secondary)
            00 01 00 01          answer/ack per discrete packet
```
