#!/usr/bin/env python3
"""Direct-from-PC BLE monitor / driver for the Powertech MB3906 charger.

Connects to the charger with the host Bluetooth adapter (bypassing the ESP32),
replicates the vendor app's bootstrap, then prints every decoded notification.
Used to reverse-engineer / verify the protocol - see docs/PROTOCOL.md.

Usage:
    python ble_monitor.py                 # connect, bootstrap, stream forever
    python ble_monitor.py --seconds 60    # stream for 60s then exit
    python ble_monitor.py --send A3 01 03 # after bootstrap, send a frame (auto-xor)
                                          #   e.g. A3 01 03 = start car mode
    python ble_monitor.py --no-password   # handshake only, skip password

Requires: bleak (pip install bleak). Only one BLE central may hold the charger
at a time - close the Android app and unplug the ESP module first.
"""
import argparse
import asyncio
import time

from bleak import BleakClient, BleakScanner

ADDR = "4C:72:74:5C:ED:EF"
SVC = "0000fff0-0000-1000-8000-00805f9b34fb"
FFF1 = "0000fff1-0000-1000-8000-00805f9b34fb"  # telemetry IN + command writes OUT
FFF2 = "0000fff2-0000-1000-8000-00805f9b34fb"  # password write (+ auth ack)

HANDSHAKE = bytes([0xFF, 0x01, 0x00, 0xFE])
PASSWORD = b"123456"

PROG = {0: "standby", 1: "6V", 2: "6V-cold", 3: "car", 4: "car-cold",
        5: "bike", 6: "bike-cold", 7: "lithium", 8: "recovery"}
STEP = {0: "idle", 1: "check", 2: "recovery", 3: "soft-start", 4: "bulk",
        5: "absorption", 6: "maintenance-1", 7: "maintenance-2", 8: "trickle"}

t0 = time.time()


def ts():
    return f"{time.time() - t0:7.2f}s"


def xor(b):
    x = 0
    for v in b:
        x ^= v
    return x


def decode(frame):
    """Return a human string for one validated frame, or None."""
    if len(frame) < 4 or frame[1] + 3 != len(frame) or xor(frame[:-1]) != frame[-1]:
        return f"(bad frame {frame.hex(' ')})"
    op, d = frame[0], frame[2:-1]
    if op == 0x60:
        return f"voltage = {((d[1] << 8) | d[0]) / 10.0:.1f} V"
    if op == 0x61:
        return f"current = {((d[1] << 8) | d[0]) / 100.0:.2f} A"
    if op == 0x62:
        return f"capacity = {d[0]} %"
    if op == 0x21:
        return f"state = {d[0]} ({'battery detected' if d[0] == 2 else 'probing'})"
    if op == 0x22:
        return f"error = {d[0]}"
    if op == 0x23:
        return f"program = {d[0]} ({PROG.get(d[0], '?')})"
    if op == 0x24:
        return f"step = {d[0]} ({STEP.get(d[0], '?')})"
    if op == 0x25:
        return f"enable = {d[0]}"
    if op == 0x00:
        return "ack/answer"
    return f"op 0x{op:02x} data={d.hex(' ')}"


class Reassembler:
    """Notifications arrive concatenated and/or split; buffer and extract frames."""

    def __init__(self):
        self.buf = bytearray()

    def feed(self, data):
        self.buf += data
        out = []
        while len(self.buf) >= 4:
            ln = self.buf[1]
            flen = ln + 3
            if ln == 0 or flen > 24:
                del self.buf[0]
                continue
            if len(self.buf) < flen:
                break
            frame = bytes(self.buf[:flen])
            if xor(frame[:-1]) == frame[-1]:
                out.append(frame)
                del self.buf[:flen]
            else:
                del self.buf[0]
        return out


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--address", default=ADDR)
    ap.add_argument("--seconds", type=float, default=0, help="0 = run until Ctrl-C")
    ap.add_argument("--no-password", action="store_true")
    ap.add_argument("--send", nargs="+", metavar="BYTE",
                    help="hex bytes to send after bootstrap; xor appended if omitted")
    args = ap.parse_args()

    print(f"{ts()} scanning for {args.address} ...")
    dev = await BleakScanner.find_device_by_address(args.address, timeout=15)
    if not dev:
        print("device not found - is the app disconnected and the charger on?")
        return

    rx = Reassembler()

    def on_notify(char, data):
        src = "fff1" if str(char.uuid).lower().startswith("0000fff1") else "fff2"
        for f in rx.feed(bytes(data)):
            print(f"{ts()} [{src}] {data.hex(' '):<20} -> {decode(f)}")
        if str(char.uuid).lower().startswith("0000fff2"):
            print(f"{ts()} [fff2] {bytes(data).hex(' '):<20} -> auth/pwd ack")

    async with BleakClient(dev) as client:
        print(f"{ts()} connected: {client.is_connected}")
        # enable notifications on both characteristics
        await client.start_notify(FFF1, on_notify)
        await client.start_notify(FFF2, on_notify)
        print(f"{ts()} notifications enabled on fff1 + fff2")

        # bootstrap: handshake -> fff1, then password -> fff2
        await client.write_gatt_char(FFF1, HANDSHAKE, response=True)
        print(f"{ts()} sent handshake {HANDSHAKE.hex(' ')}")
        await asyncio.sleep(0.6)
        if not args.no_password:
            await client.write_gatt_char(FFF2, PASSWORD, response=True)
            print(f"{ts()} sent password '123456'")

        if args.send:
            await asyncio.sleep(0.8)
            b = [int(x, 16) for x in args.send]
            frame = bytes(b if len(b) >= 4 else b + [xor(b)])
            await client.write_gatt_char(FFF1, frame, response=True)
            print(f"{ts()} sent frame {frame.hex(' ')}")

        print(f"{ts()} streaming (Ctrl-C to stop) ...")
        deadline = t0 + args.seconds if args.seconds else None
        try:
            while deadline is None or time.time() < deadline:
                await asyncio.sleep(0.5)
        except (KeyboardInterrupt, asyncio.CancelledError):
            pass
        print(f"{ts()} done")


if __name__ == "__main__":
    asyncio.run(main())
