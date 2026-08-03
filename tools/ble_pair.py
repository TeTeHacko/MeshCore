#!/usr/bin/env python3
"""Non-interactive BLE pairing by PIN, driving bluetoothctl over a pty.

A MeshCore node's console characteristics are SECMODE_ENC_WITH_MITM, so the
first connection to an unpaired node raises a passkey dialog on the desktop --
useless from a script. This registers its own KeyboardOnly agent and answers
the prompt.

    ble_pair.py DC:28:1D:8A:04:1A               # one node
    ble_pair.py DC:28:1D:8A:04:1A CE:72:14:CD:FD:02

The PIN comes from platformio.local.ini ([secrets] ble_pin) and is never
printed. That is the same value the firmware was built with, so a node whose
PIN does not match here was built from a different tree.

WHY A SCRIPT, AND WHY STATE-DRIVEN: the first version marched through
bluetoothctl on a fixed schedule and sent `pair` before the agent was
registered. The passkey prompt then went to the default agent and the node
stayed unpaired -- with no error anywhere. Every step here waits for the
output that proves the previous one landed.

BONDING IS NOT ONLY FOR THE CONSOLE. The buttonless-DFU control point is
declared with the same SECMODE_ENC_WITH_MITM
(`helpers/nrf52/SerialBLEInterface.cpp`), so `ble_dfu.py` phase 1 cannot write
its trigger to an unpaired node either. It fails as a plain write error, which
does not look like "you are not bonded".

AFTERWARDS, DISCONNECT BEFORE FLASHING. bluetoothctl leaves the node
*connected* once pairing succeeds, and a connected peripheral stops
advertising -- so `ble_dfu.py` immediately fails its scan with "APP nenalezen
ve scanu". Run `bluetoothctl disconnect <MAC>` first.

    ble_pair.py <MAC> && bluetoothctl disconnect <MAC> && ble_dfu.py fw.zip <MAC>

DOCK-QUIET NODES ADVERTISE ONLY WHEN UNDOCKED. On dual USB/BLE companion
builds (T1000-E, Wio L1), BLE advertising is suppressed while a USB host holds
DTR high. Unplug USB, or hold the port open with DTR deasserted, or nothing on
this page will find the node.
"""
import os
import pty
import re
import select
import sys
import time

INI = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "platformio.local.ini")


def read_pin():
    try:
        with open(INI) as fh:
            lines = fh.readlines()
    except FileNotFoundError:
        sys.exit(f"{INI} not found -- it holds the real PIN and is git-ignored")
    pin = None
    for line in lines:                     # last one wins, same as PlatformIO
        m = re.match(r"\s*ble_pin\s*=\s*(\d+)", line)
        if m:
            pin = m.group(1)
    if not pin:
        sys.exit(f"no [secrets] ble_pin in {INI}")
    return pin


def main():
    macs = sys.argv[1:]
    if not macs:
        sys.exit(f"usage: {os.path.basename(sys.argv[0])} <MAC> [MAC...]")
    pin = read_pin()

    pid, fd = pty.fork()
    if pid == 0:
        os.execvp("bluetoothctl", ["bluetoothctl"])

    def drain(seconds, want=None, mac=None):
        """Read for `seconds` (or until `want` matches), answering prompts."""
        end = time.time() + seconds
        hit = None
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.4)
            if not r:
                continue
            try:
                chunk = os.read(fd, 4096).decode("utf-8", "replace")
            except OSError:
                break
            clean = re.sub(r"\x1b\[[0-9;?]*[a-zA-Z]", "", chunk)
            for ln in clean.splitlines():
                t = ln.strip()
                if not t or "RSSI" in t or t.startswith("[NEW] Device"):
                    continue                      # scan noise
                print("  bt|", t[:110], flush=True)
            low = clean.lower()
            if "passkey" in low or "enter pin" in low or "pin code" in low:
                os.write(fd, (pin + "\n").encode())
                print("  -> PIN sent", flush=True)
            elif "(yes/no)" in low:
                os.write(fd, b"yes\n")
                print("  -> confirmed", flush=True)
            if want and mac and re.search(want, clean, re.I) and mac in clean:
                hit = True
                break
            if "failed" in low and mac and mac.lower() in low:
                hit = False
                break
        return hit

    def send(cmd, wait=1.5, want=None, mac=None):
        os.write(fd, (cmd + "\n").encode())
        print(f"$ {cmd}", flush=True)
        return drain(wait, want, mac)

    send("agent KeyboardOnly", 2)
    send("default-agent", 2)
    failed = []
    for mac in macs:
        print(f"===== {mac} =====", flush=True)
        send(f"remove {mac}", 3)          # a stale bond makes `pair` a no-op
        send("scan on", 1)
        drain(6)                          # let the adapter see it again
        ok = send(f"pair {mac}", 45, want=r"Bonded: yes|Pairing successful", mac=mac)
        print(f"  paired: {ok}", flush=True)
        if not ok:
            failed.append(mac)
        send("scan off", 1)
        send(f"trust {mac}", 3)
    send("quit", 1)
    os.close(fd)

    if failed:
        print("NOT PAIRED: " + ", ".join(failed))
        print("A wrong PIN and a node that stopped advertising look the same "
              "here -- check both.")
        sys.exit(1)
    print("done -- disconnect before running ble_dfu.py")


if __name__ == "__main__":
    main()
