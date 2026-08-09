#!/usr/bin/env python3
"""Text console of a MeshCore node over BLE (Nordic UART Service).

The same console you get on USB serial, but wireless -- which is the only way
in once a node is mounted somewhere without a cable. Works against any build
that has `BLE_PIN_CODE` set (repeater, room server, companion_radio_ble).

Why this exists as a tool rather than a snippet: the NUS write has a size limit
that is easy to miss and fails in a misleading way (see CHUNK below), and the
BlueZ/bleak connect path needs retries to be usable for anything longer than a
single command. Every ad-hoc script re-learned both the hard way.

    # single command
    ble_cli.py DC:28:1D:8A:04:1A ver

    # several, one connection
    ble_cli.py DC:28:1D:8A:04:1A "get radio" "get name" stats-radio

    # from a file, one command per line, '#' comments ignored
    ble_cli.py DC:28:1D:8A:04:1A -f provision.txt

    # {epoch} expands to the host's current unix time -- the point being that a
    # provisioning FILE can carry `time {epoch}`, which a shell cannot reach
    ble_cli.py DC:28:1D:8A:04:1A "time {epoch}"

    # keep reconnecting per command (slow, but survives a flaky link)
    ble_cli.py DC:28:1D:8A:04:1A --per-command "gps on" "set af 9"

PAIRING: the console characteristics are SECMODE_ENC_WITH_MITM, so the node
must be bonded first or the connect pops a passkey dialog on the desktop:

    bluetoothctl --agent KeyboardOnly    # then: pair <MAC>, enter the BLE PIN

`erase` on the node wipes the bond too (bonds live in InternalFS), so re-pair
after one.

CAUTION: one console at a time. USB and BLE read into the same command buffer
in the firmware (`simple_repeater/main.cpp`, "commands share the buffer"), so
two consoles at once interleave into one line and can crash the node.
"""
import argparse
import asyncio
import sys
import time

from bleak import BleakClient, BleakScanner

NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # write  (host -> node)
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # notify (node -> host)

# A NUS write carries at most ATT_MTU-3 bytes, and BlueZ will not split it for
# you. At the default MTU of 23 that is 20 bytes -- and MeshCore nodes are
# routinely left at 23. A longer write is rejected with
# "[org.bluez.Error.Failed] Failed to initiate write", which reads like a
# dropped link rather than "your command was too long": commands up to 18
# characters work and everything longer fails, deterministically. Hence chunking
# every write, always.
CHUNK = 20


def log(*a):
    print(*a, file=sys.stderr, flush=True)


async def send(client, buf, cmd, wait):
    buf.clear()
    # {epoch} -> host unix time, expanded HERE rather than at parse time so that
    # it is the time the command is actually sent: --per-command reconnects cost
    # seconds each, and a whole -f file can take minutes.
    #
    # It exists for `time {epoch}` in a provisioning file. -f reads lines verbatim,
    # so a `$(date +%s)` in one is sent as literal text and the node answers
    # "Unknown command" -- which is why the clock line in tools/provision/*.txt
    # was a commented-out note telling you to substitute the epoch by hand. Every
    # clock in this firmware is volatile without GPS or an I2C RTC, and a node
    # whose RTC still says May 2024 has its adverts dropped as replays.
    cmd = cmd.replace("{epoch}", str(int(time.time())))
    data = (cmd + "\r\n").encode()
    for i in range(0, len(data), CHUNK):
        await client.write_gatt_char(NUS_RX, data[i:i + CHUNK], response=False)
        await asyncio.sleep(0.15)
    await asyncio.sleep(wait)
    out = bytes(buf).decode("utf-8", "replace").strip()
    # the node echoes nothing, but replies are prefixed "-> " or "> "
    for p in ("-> ", "> "):
        if out.startswith(p):
            out = out[len(p):]
    # the EXPANDED command goes back too, so the printout says what was really
    # sent rather than the `{epoch}` template
    return cmd, out.strip()


async def run(mac, cmds, wait, per_command, tries):
    async def connect():
        last = None
        for attempt in range(tries):
            try:
                dev = await BleakScanner.find_device_by_address(mac, timeout=20)
                if dev is None:
                    # a connected peripheral stops advertising; a stale link on
                    # the adapter is the usual reason it cannot be found
                    raise RuntimeError(f"{mac} not advertising (already connected?)")
                c = BleakClient(dev, timeout=30)
                await c.connect()
                return c
            except Exception as e:                       # noqa: BLE001
                last = e
                log(f"  [connect {attempt + 1}/{tries}] {type(e).__name__}: {e}")
                await asyncio.sleep(4)
        raise SystemExit(f"cannot connect to {mac}: {last}")

    results = []
    if per_command:
        for cmd in cmds:
            c = await connect()
            try:
                buf = bytearray()
                await c.start_notify(NUS_TX, lambda _h, d: buf.extend(d))
                results.append(await send(c, buf, cmd, wait))
            finally:
                await c.disconnect()
            await asyncio.sleep(2)
    else:
        c = await connect()
        try:
            buf = bytearray()
            await c.start_notify(NUS_TX, lambda _h, d: buf.extend(d))
            for cmd in cmds:
                results.append(await send(c, buf, cmd, wait))
        finally:
            await c.disconnect()
    return results


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mac", help="BLE address of the node")
    ap.add_argument("cmds", nargs="*", help="console commands")
    ap.add_argument("-f", "--file", help="read commands from a file, one per line")
    ap.add_argument("-w", "--wait", type=float, default=3.0,
                    help="seconds to collect the reply (default 3; raise for "
                         "paged output like rxlog/nodes)")
    ap.add_argument("--per-command", action="store_true",
                    help="reconnect for every command -- slower, but survives a "
                         "link that drops mid-sequence")
    ap.add_argument("--tries", type=int, default=5, help="connect attempts")
    a = ap.parse_args()

    cmds = list(a.cmds)
    if a.file:
        with open(a.file) as fh:
            cmds += [ln.strip() for ln in fh
                     if ln.strip() and not ln.lstrip().startswith("#")]
    if not cmds:
        ap.error("no commands given")


    for cmd, out in asyncio.run(run(a.mac, cmds, a.wait, a.per_command, a.tries)):
        print(f"{cmd}\t{out}")


if __name__ == "__main__":
    main()
