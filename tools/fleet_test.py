#!/usr/bin/env python3
"""Regression smoke test for the bench fleet -- run it after every firmware change.

  tools/fleet_test.py [--boards 0,1,2,4] [--rounds 1] [--json out.json]

Grew out of a one-off hw_check.py written to tell a dead radio from a dead IRQ
line. The lesson that made it worth keeping: **a node's own counters cannot
certify the node**. `sent` only says the firmware believes it transmitted, which
is exactly what a dead DIO1 line breaks -- tth-x0 reported `sent 0` for a week
while a second board heard every frame it sent. So every RF claim here is made by
a DIFFERENT board than the one under test.

What it does, in order:

  1. identity   version/build per board; flags unstamped ("v1.16.0" with no -tth,
                identical in every build ever made) and dirty ("+") images, and
                boards that are not on the same build as the rest.
  2. config     freq/bw/sf/cr must agree across the fleet, names must be unique.
                Without this the matrix below measures nothing -- two boards on
                different spreading factors simply cannot hear each other, which
                looks exactly like broken hardware.
  3. matrix     N x N. Each board sends a ZERO-HOP advert in turn; every other
                board must count it. Zero-hop is deliberate: nobody rebroadcasts
                it, so "B heard A" cannot be satisfied by C's relay of A -- and
                it means the test never has to touch `repeat` prefs.
  4. hygiene    tx_timeout / tx_start_fail / recv_errors must not move during the
                run, on any board.
  5. commands   console builds only: the command surface answers (informational,
                plus a hard fail on the handful every repeater build must have).

Nothing here writes prefs, changes radio settings or reboots anything.

Handles both build flavours on the bench: the text console (repeater/analyzer)
and the binary companion protocol (companion_radio, which has no text console).
Detection is automatic.

TRAP: do not leave a BLE client connected to a board under test. The USB and BLE
consoles share one command[160] buffer (simple_repeater/main.cpp), so two live
consoles interleave characters into one line and the node ends up executing
garbage. An earlier A/B run was invalidated exactly that way.
"""
import argparse
import glob
import json
import os
import re
import struct
import sys
import time
from datetime import datetime, timezone

import serial

HERE = os.path.dirname(os.path.abspath(__file__))
FLASH_SH = os.path.join(HERE, "xiao_uf2_flash.sh")

# Companion protocol (examples/companion_radio/MyMesh.cpp)
CMD_APP_START, CMD_SEND_SELF_ADVERT = 1, 7
CMD_DEVICE_QUERY, CMD_GET_STATS = 22, 56
STATS_TYPE_PACKETS = 2
RESP_SELF_INFO, RESP_DEVICE_INFO, RESP_STATS = 5, 13, 24


def fleet_table():
    """Fleet number -> USB serial, read out of xiao_uf2_flash.sh.

    Parsed rather than duplicated on purpose: two copies of this table would
    drift, and mixing up which board is which is the single most expensive
    mistake on this bench (a flash went to x4 while the log said x3).
    """
    try:
        src = open(FLASH_SH).read()
    except OSError as e:
        sys.exit(f"nelze precist {FLASH_SH}: {e}")
    tbl = dict(re.findall(r"^\s*([0-9])\)\s*TARGET=([0-9A-F]{16})\s*;;", src, re.M))
    if not tbl:
        sys.exit(f"v {FLASH_SH} nenalezena tabulka desek -- zmenil se format?")
    return tbl


def discover(wanted):
    """[(fleet_no, serial, port)] for XIAO boards actually on USB.

    Always through /dev/serial/by-id and matched on the SERIAL NUMBER: ttyACM*
    renumbers on every replug, so a positional guess talks to another board.
    """
    tbl = fleet_table()
    found, modems = [], []
    # Hledaji se OBA product stringy, ale znamenaji ruzne desky:
    #
    #   usb-Seeed_Studio_XIAO_nRF52840_<sn>   MeshCore build (PID 8044)
    #   usb-Seeed_XIAO-Wio-SX1262_<sn>        openhop_modem firmware (PID 0044)
    #
    # Prejmenovani dela openhop_modem, ne bootloader -- je to hloupe SX1262 PHY
    # pro openHop demona, takze na MeshCore konzoli ani companion protokol
    # neodpovi a zadnou UF2 mechaniku nevystavi. Viz hlavicku
    # tools/openhop_observer_config.py (radio_type: pymc_usb).
    #
    # Hlasi se zvlast, protoze obe chybne odpovedi na tohle stoji cas: pri globu
    # jen na "XIAO_nRF52840" se 1. 9. 2026 matice smrskla na jednu desku bez
    # vysvetleni, a pri globu na oboji to vypadalo jako dve zaseknute desky
    # (nasledoval replug a pokus o nrfutil, oboje zbytecne -- deska byla v poradku).
    for port in sorted(glob.glob("/dev/serial/by-id/*XIAO-Wio-SX1262*")):
        m = re.search(r"_([0-9A-F]{16})-if00", port)
        if m:
            sn = m.group(1)
            modems.append((next((n for n, s in tbl.items() if s == sn), "?"), sn))
    if modems:
        for no, sn in modems:
            print(f"  x{no} ({sn}): openhop_modem firmware, ne MeshCore uzel -- vynechavam")

    for port in sorted(glob.glob("/dev/serial/by-id/*XIAO_nRF52840*")):
        m = re.search(r"_([0-9A-F]{16})-if00", port)
        if not m:
            continue
        sn = m.group(1)
        no = next((n for n, s in tbl.items() if s == sn), "?")
        if wanted and no not in wanted:
            continue
        found.append((no, sn, port))
    return found


# ---------------------------------------------------------------- transports

class Node:
    """Common bits. Ports are opened once and held: reopening costs ~1.3 s of
    settle time per command (CDC on nRF52 is deaf until DTR is raised), which
    turns an N x N matrix into minutes of dead waiting."""

    def __init__(self, no, sn, port):
        self.no, self.sn, self.port = no, sn, port
        self.name = f"x{no}"
        self.s = serial.Serial(port, 115200, timeout=0)
        self.s.dtr = True
        self.s.rts = True
        time.sleep(1.3)
        self.s.reset_input_buffer()
        self.info = {}

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass


class ConsoleNode(Node):
    """Repeater / analyzer build: line-oriented text console."""

    kind = "console"

    def cmd(self, c, timeout=3.0, quiet=0.25):
        """Send and read until the reply has stopped arriving.

        Idle-based rather than a fixed sleep: paged analyzer replies (rxlog) are
        kilobytes and a fixed wait either truncates them or wastes seconds on
        every one-line answer.
        """
        self.s.reset_input_buffer()
        self.s.write((c + "\r\n").encode())
        buf, deadline, last = b"", time.time() + timeout, None
        while time.time() < deadline:
            chunk = self.s.read(8192)
            if chunk:
                buf += chunk
                last = time.time()
            elif last and time.time() - last > quiet:
                break
            else:
                time.sleep(0.02)
        return buf.decode(errors="replace")

    def stats(self):
        m = re.search(r"\{.*\}", self.cmd("stats-packets"))
        return json.loads(m.group(0)) if m else {}

    def radio_stats(self):
        m = re.search(r"\{.*\}", self.cmd("stats-radio"))
        return json.loads(m.group(0)) if m else {}

    def identify(self):
        ver = self.cmd("ver")
        m = re.search(r"->\s*(\S+)\s*\(Build:\s*([^)]*)\)", ver)
        self.info["version"] = m.group(1) if m else "?"
        self.info["build"] = m.group(2).strip() if m else "?"
        r = re.search(r"->\s*>?\s*([\d.]+),([\d.]+),(\d+),(\d+)", self.cmd("get radio"))
        self.info["radio"] = (f"{float(r.group(1)):.3f}/{float(r.group(2))}/"
                              f"{r.group(3)}/{r.group(4)}") if r else "?"
        n = re.search(r"->\s*>?\s*(.+)", self.cmd("get name"))
        self.info["node"] = n.group(1).strip() if n else "?"

    def advert(self):
        return "OK" in self.cmd("advert.zerohop", timeout=4.0)


class CompanionNode(Node):
    """companion_radio build: binary frames, no text console at all.

    Framing is '<' + uint16 LE length + payload outbound, '>' + length + payload
    inbound (src/helpers/ArduinoSerialInterface.cpp).
    """

    kind = "companion"

    def send(self, payload):
        self.s.write(b"<" + struct.pack("<H", len(payload)) + bytes(payload))
        self.s.flush()

    def recv(self, want, timeout=2.5):
        """Wait for a frame with response code `want`, skipping push frames.

        A companion node emits unsolicited pushes (new message waiting, and so
        on), so 'the next frame' is not necessarily the answer to the question.
        """
        buf, deadline = b"", time.time() + timeout
        while time.time() < deadline:
            buf += self.s.read(4096) or b""
            while True:
                i = buf.find(b">")
                if i < 0 or len(buf) < i + 3:
                    break
                ln = struct.unpack("<H", buf[i + 1:i + 3])[0]
                if len(buf) < i + 3 + ln:
                    break
                frame, buf = buf[i + 3:i + 3 + ln], buf[i + 3 + ln:]
                if frame and frame[0] == want:
                    return frame
            time.sleep(0.02)
        return None

    def handshake(self):
        # RESP_CODE_DEVICE_INFO: code, ver_code, max_contacts/2, max_channels,
        #                        ble_pin[4], build_date[12], manufacturer[40],
        #                        firmware_version[20], client_repeat, path_hash_mode
        self.send(bytes([CMD_DEVICE_QUERY, 10]))
        d = self.recv(RESP_DEVICE_INFO)
        if d and len(d) >= 80:
            self.info["build"] = d[8:20].split(b"\0")[0].decode(errors="replace")
            self.info["version"] = d[60:80].split(b"\0")[0].decode(errors="replace")
        # RESP_CODE_SELF_INFO: code, adv_type, tx_power, max_tx_power,
        #                      pub_key[32], lat[4], lon[4], multi_acks,
        #                      loc_policy, telemetry, manual_contacts,
        #                      freq[4], bw[4], sf, cr, name[...]
        self.send(bytes([CMD_APP_START, 1, 0, 0, 0, 0, 0, 0]) + b"fleet")
        s = self.recv(RESP_SELF_INFO)
        if s and len(s) >= 58:
            freq, bw = struct.unpack("<II", s[48:56])
            self.info["radio"] = f"{freq/1000:.3f}/{bw/1000}/{s[56]}/{s[57]}"
            self.info["node"] = s[58:].decode(errors="replace").strip()
        return bool(s)

    def identify(self):
        if not self.handshake():
            self.info.setdefault("version", "?")

    def stats(self):
        self.send(bytes([CMD_GET_STATS, STATS_TYPE_PACKETS]))
        f = self.recv(RESP_STATS)
        if not f or len(f) < 30 or f[1] != STATS_TYPE_PACKETS:
            return {}
        v = struct.unpack("<7I", f[2:30])
        return dict(zip(("recv", "sent", "flood_tx", "direct_tx",
                         "flood_rx", "direct_rx", "recv_errors"), v))

    def radio_stats(self):
        return {}

    def advert(self):
        self.send(bytes([CMD_SEND_SELF_ADVERT, 0]))   # 0 = zero hop
        return True


def open_node(no, sn, port):
    """Console or companion? Ask, do not assume.

    A companion build ignores stray text and a console build ignores stray
    frames, so probing either way is harmless.
    """
    n = ConsoleNode(no, sn, port)
    if "->" in n.cmd("ver", timeout=2.0):
        return n
    n.close()
    c = CompanionNode(no, sn, port)
    if c.handshake():
        return c
    c.close()
    return None


# -------------------------------------------------------------------- checks

class Report:
    def __init__(self):
        self.rows = []

    def add(self, section, name, ok, detail):
        self.rows.append((section, name, bool(ok), detail))
        mark = "OK  " if ok else "FAIL"
        print(f"  [{mark}] {name}: {detail}", flush=True)

    def note(self, text):
        print(f"         {text}", flush=True)

    @property
    def failed(self):
        return [r for r in self.rows if not r[2]]


def check_identity(nodes, rep):
    print("\n== 1. identita a verze", flush=True)
    for n in nodes:
        v, b = n.info.get("version", "?"), n.info.get("build", "?")
        rep.note(f"x{n.no}  {n.info.get('node','?'):<14} {v}  ({b})  [{n.kind}]")
    vers = {n.info.get("version", "?") for n in nodes}
    stale = [n for n in nodes if not re.search(r"-tth[0-9a-f]+", n.info.get("version", ""))]
    dirty = [n for n in nodes if n.info.get("version", "").endswith("+")]
    rep.add("identity", "vsechny obrazy jsou stampovane", not stale,
            "ok" if not stale else
            f"bez -tth<sha>: {[n.name for n in stale]} -- env nema ${{stamped.extra_scripts}}, "
            "verze je literal shodny ve vsech buildech")
    rep.add("identity", "zadny obraz z spinaveho stromu", not dirty,
            "ok" if not dirty else f"konci na '+': {[n.name for n in dirty]}")
    # Deliberately a note, not a check. The bench legitimately runs several envs
    # at once (repeater, bot, companion), so "all on one version" is not a
    # property this fleet has -- asserting it would cry wolf on every run and
    # train you to ignore the summary. What matters is that you can SEE what is
    # where, and the two things above, which are rule violations in any env.
    if len(vers) > 1:
        by_ver = {}
        for n in nodes:
            by_ver.setdefault(n.info.get("version", "?"), []).append(n.name)
        rep.note("ruzne buildy na lavici: "
                 + " | ".join(f"{v}: {' '.join(b)}" for v, b in sorted(by_ver.items())))


def check_config(nodes, rep):
    print("\n== 2. konfigurace radia", flush=True)
    radios = {}
    for n in nodes:
        radios.setdefault(n.info.get("radio", "?"), []).append(n.name)
    rep.add("config", "shodne parametry radia", len(radios) == 1,
            " | ".join(f"{r}: {v}" for r, v in radios.items()))
    names = [n.info.get("node", "?") for n in nodes]
    dupes = {x for x in names if names.count(x) > 1}
    rep.add("config", "jmena uzlu jsou unikatni", not dupes,
            "ok" if not dupes else f"duplicity: {sorted(dupes)}")
    return len(radios) == 1


def check_matrix(nodes, rep, rounds):
    print(f"\n== 3. krizova matice ({len(nodes)} desek, {rounds}x)", flush=True)
    if len(nodes) < 2:
        rep.add("matrix", "matice potrebuje aspon 2 desky", False,
                f"pripojena je {len(nodes)}")
        return {}

    heard = {t.name: {r.name: 0 for r in nodes if r is not t} for t in nodes}
    rssi = {t.name: {} for t in nodes}
    for rnd in range(rounds):
        for tx in nodes:
            others = [n for n in nodes if n is not tx]
            before = {n.name: n.stats() for n in nodes}
            tx.advert()
            time.sleep(3.0)
            after = {n.name: n.stats() for n in nodes}

            d_sent = after[tx.name].get("sent", 0) - before[tx.name].get("sent", 0)
            d_to = after[tx.name].get("tx_timeout", 0) - before[tx.name].get("tx_timeout", 0)
            got = []
            for rx in others:
                d = after[rx.name].get("recv", 0) - before[rx.name].get("recv", 0)
                if d >= 1:
                    heard[tx.name][rx.name] += 1
                    rs = rx.radio_stats()
                    if rs:
                        rssi[tx.name][rx.name] = (rs.get("last_rssi"), rs.get("last_snr"))
                got.append(f"{rx.name}{'+' if d >= 1 else '-'}")
            rep.note(f"kolo {rnd+1}: {tx.name} vysila -> sent +{d_sent} "
                     f"timeout +{d_to} | slyseli: {' '.join(got)}")

    for tx in nodes:
        misses = [r for r, c in heard[tx.name].items() if c < rounds]
        rep.add("matrix", f"{tx.name} slyseli vsichni ({rounds}/{rounds})", not misses,
                "ok" if not misses else
                f"neslyseli/ne vzdy: {', '.join(f'{r} {heard[tx.name][r]}/{rounds}' for r in misses)}")
    for tx in nodes:
        for rx, v in rssi[tx.name].items():
            if v[0] is not None:
                rep.note(f"{tx.name} -> {rx}: RSSI {v[0]} dBm, SNR {v[1]} dB")
    return heard


def check_hygiene(nodes, base, rep):
    print("\n== 4. countery chyb behem behu", flush=True)
    for n in nodes:
        a, b = n.stats(), base[n.name]
        bad = {k: a.get(k, 0) - b.get(k, 0)
               for k in ("tx_timeout", "tx_start_fail", "recv_errors")
               if a.get(k, 0) - b.get(k, 0) > 0}
        rep.add("hygiene", f"{n.name} bez chybovych counteru", not bad,
                "ok" if not bad else str(bad))


# Rok, pod kterym je jasne, ze hodinam nikdo cas nenastavil: VolatileRTCClock
# startuje v kvetnu 2024 a jinak jen tika z millis().
CLOCK_FLOOR = 1735689600      # 2025-01-01 UTC


def check_clock(nodes, rep):
    print("\n== 6. hodiny desek", flush=True)
    for n in nodes:
        if n.kind != "console":
            rep.note(f"{n.name}: companion build, `clock` neexistuje -- preskoceno")
            continue
        out = n.cmd("clock")
        m = re.search(r"epoch\s+(\d+)", out)
        if m:
            node_epoch = int(m.group(1))
        else:
            m = re.search(r"\b(\d{1,2}):(\d{2})\s*-\s*(\d{1,2})/(\d{1,2})/(\d{4})", out)
            if not m:
                rep.add("clock", f"{n.name} odpovida na clock", False, out.strip()[:60])
                continue
            hh, mm, day, mon, year = (int(g) for g in m.groups())
            try:
                node_epoch = int(datetime(year, mon, day, hh, mm,
                                          tzinfo=timezone.utc).timestamp())
            except ValueError:
                rep.add("clock", f"{n.name} odpovida na clock", False, out.strip()[:60])
                continue
        skew = int(time.time()) - node_epoch
        # Deska, ktere nikdo cas nenastavil, je vada konfigurace, ne vada desky --
        # ale prave proto se to musi hlasit. Na x3 to uniklo 811 dni a na teto
        # lavici to 7. 8. 2026 platilo pro KAZDOU desku. Spatny cas neni kosmetika:
        # RTC stampuje kazdy vyslany advert a prijemce stary timestamp zahodi jako
        # replay. Srovnat: `tools/ble_cli.py <MAC> "time {epoch}"` nebo pres USB.
        rep.add("clock", f"{n.name} ma nastavene hodiny", node_epoch > CLOCK_FLOOR,
                f"{datetime.fromtimestamp(node_epoch, timezone.utc).isoformat()}"
                f" (skew {skew:+d} s = {skew / 86400:.1f} dni)")


# Commands every repeater/analyzer build must answer. `dfu` is deliberately NOT
# probed -- it reboots into the bootloader.
MUST_HAVE = ["ver", "advert.zerohop", "stats-packets", "blink", "txpwr"]
OPTIONAL = ["nodes", "rxlog 999999", "pktlog 999999", "bot", "dio1", "bleconn"]


def check_commands(nodes, rep):
    print("\n== 5. povrch prikazu (jen console buildy)", flush=True)
    for n in nodes:
        if n.kind != "console":
            rep.note(f"{n.name}: companion build, textova konzole neexistuje -- preskoceno")
            continue
        missing = [c for c in MUST_HAVE if "Unknown command" in n.cmd(c)]
        rep.add("commands", f"{n.name} ma zakladni prikazy", not missing,
                "ok" if not missing else f"chybi: {missing}")
        have = [c.split()[0] for c in OPTIONAL if "Unknown command" not in n.cmd(c)]
        rep.note(f"{n.name} volitelne: {' '.join(have) if have else '(zadne)'}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--boards", help="cisla desek, napr. 0,1,2,4 (vychozi: vse na USB)")
    ap.add_argument("--rounds", type=int, default=1, help="kol matice (vychozi 1)")
    ap.add_argument("--json", help="ulozit vysledek jako JSON")
    a = ap.parse_args()

    wanted = set(a.boards.split(",")) if a.boards else None
    found = discover(wanted)
    if not found:
        sys.exit("na USB nenalezena zadna XIAO deska")

    print(f"== nalezeno {len(found)} desek", flush=True)
    nodes = []
    for no, sn, port in found:
        n = open_node(no, sn, port)
        if n is None:
            print(f"  x{no} ({sn}): neodpovida ani konzoli, ani companion protokolu", flush=True)
            continue
        n.identify()
        nodes.append(n)
    if not nodes:
        sys.exit("zadna deska neodpovedela")

    rep = Report()
    try:
        check_identity(nodes, rep)
        same_radio = check_config(nodes, rep)
        if not same_radio:
            rep.note("POZOR: desky nejsou na stejnem radiu, matice nize je tim padem "
                     "test konfigurace, ne hardwaru")
        base = {n.name: n.stats() for n in nodes}
        check_matrix(nodes, rep, a.rounds)
        check_hygiene(nodes, base, rep)
        check_commands(nodes, rep)
        check_clock(nodes, rep)
    finally:
        for n in nodes:
            n.close()

    print("\n===== SOUHRN =====")
    for _sec, name, ok, _detail in rep.rows:
        print(f"  {'OK  ' if ok else 'FAIL'}  {name}")
    n_bad = len(rep.failed)
    print(f"\n{len(rep.rows) - n_bad}/{len(rep.rows)} proslo")

    if a.json:
        with open(a.json, "w") as f:
            json.dump({"boards": [{"no": n.no, "sn": n.sn, "kind": n.kind, **n.info}
                                  for n in nodes],
                       "checks": [{"section": s, "name": nm, "ok": ok, "detail": d}
                                  for s, nm, ok, d in rep.rows]}, f, indent=2)
        print(f"JSON: {a.json}")
    return 1 if n_bad else 0


# The guard is not decoration: this module TRANSMITS. check_matrix() and the
# `advert.zerohop` probe in check_commands() key the radio on every console board
# found on USB, whatever band each one is tuned to. Without the guard, so much as
# `import fleet_test` to reuse discover() ran the whole suite -- which on
# 7. 8. 2026 put two zero-hop adverts on 869.432 out of a bench board configured
# for the live CZ mesh, before the importing script's own first line had run.
if __name__ == "__main__":
    sys.exit(main())
