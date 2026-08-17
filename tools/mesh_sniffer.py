#!/usr/bin/env python3
"""Passive MeshCore sniffer -- every frame the radio hears, decoded on the host.

  tools/mesh_sniffer.py --board 0 --preset cz [--jsonl cap.jsonl] [--pcap cap.pcap]
                        [--psk <hex>] [--seconds 600]

Needs a board running the openHop modem firmware (openhop-dev/openhop_modem) and
`pip install openhop-core` -- the Python reimplementation of the MeshCore stack.
The board is a dumb SX1262 PHY: it hands up EVERY frame that passes CRC, tagged
with RSSI/SNR, and all decoding happens here.

Why this exists, when we already have a node + BLE bridge feeding the analyzer:

  * A companion node only ever surfaces what ITS firmware decided to process --
    its own channels and DMs, adverts, packets addressed to it. Foreign-PSK
    channel traffic and DIRECT packets for other destinations are dropped before
    the companion protocol ever sees them. The modem has no such opinion, so
    "SDR sees twice what the observer decodes" (docs, measured) becomes a
    testable claim instead of a mystery: the modem's own rx_count is printed
    next to the number of frames we managed to parse.
  * No BLE anywhere on the path -- no dock-quiet interaction, no BlueZ holding
    the link, no MC_BLE_OFF_INTERVAL windows costing 42 % of the capture.

RECEIVE ONLY, by construction. There is no call to radio.send() in this file and
no dispatcher/handler is registered, so nothing can answer, ACK or forward. That
matters on 869.432: an unannounced node that starts answering confuses routing.
The modem is left with tx_power at its floor as a second line of defence.

Two traps that shaped the code, both worth keeping in mind if you extend it:

  * openhop_core's own Dispatcher runs a PacketFilter that swallows byte-identical
    frames for 30 s. That is right for a repeater and wrong for a sniffer -- two
    neighbours relaying the same flood is exactly the observation we are after.
    So this script parses packets itself instead of going through the dispatcher,
    and counts duplicates instead of hiding them.
  * USBLoRaRadio's rx_callback is handed only the payload; RSSI/SNR live in
    radio.last_rssi/last_snr, which the NEXT packet overwrites before a queued
    callback runs. So we override _dispatch_frame and take the metadata straight
    out of the CMD_RX_PACKET frame, where it is atomic with the bytes.
"""

from __future__ import annotations

import argparse
import glob
import hashlib
import hmac
import json
import os
import socket
import struct
import sys
import threading
import time
from collections import Counter

try:
    from openhop_core.hardware.usb_radio import CMD_RX_PACKET, USBLoRaRadio
    from openhop_core.protocol import decode_appdata, parse_advert_payload
    from openhop_core.protocol.packet import Packet
except ImportError:
    sys.exit(
        "chybi openhop-core: python3 -m venv .venv && .venv/bin/pip install openhop-core pyserial"
    )

# Fleet shorthand, same numbering as tools/xiao_uf2_flash.sh. Keyed on serial
# number because the by-id NAME changes with the firmware: the modem build
# renames the port from usb-Seeed_Studio_XIAO_nRF52840_<sn> to
# usb-Seeed_XIAO-Wio-SX1262_<sn>, so anything matching on the name breaks the
# moment the board becomes a modem.
FLEET = {
    "0": "30911219DA28411D",
    "1": "5ECC11205C68623B",
    "2": "B69F86518175CBA3",
    "3": "67901109B61E604A",
    "4": "208DBAF462432133",
}

# freq_hz, bandwidth_hz, sf, cr -- mirrored from platformio.ini / .local.ini so a
# capture cannot silently run on the wrong parameters. Two boards on different
# spreading factors simply cannot hear each other.
PRESETS = {
    "cz": (869_432_000, 62_500, 7, 5),  # live CZ mesh, [cz_radio]
    "bench": (866_500_000, 62_500, 8, 5),  # test band, [test_radio] + committed defaults
    "prod": (869_618_000, 62_500, 8, 5),  # committed platformio.ini default
}

ROUTE_TYPES = {0: "TRANSPORT_FLOOD", 1: "FLOOD", 2: "DIRECT", 3: "TRANSPORT_DIRECT"}
PAYLOAD_TYPES = {
    0x00: "REQ",
    0x01: "RESPONSE",
    0x02: "TXT_MSG",
    0x03: "ACK",
    0x04: "ADVERT",
    0x05: "GRP_TXT",
    0x06: "GRP_DATA",
    0x07: "ANON_REQ",
    0x08: "PATH",
    0x09: "TRACE",
    0x0A: "MULTIPART",
    0x0B: "CONTROL",
    0x0F: "RAW_CUSTOM",
}
ADVERT_ROLES = {0x01: "chat", 0x02: "repeater", 0x03: "room", 0x04: "sensor"}

LINKTYPE_USER0 = 147  # what openhop_core's own wireshark example uses


def resolve_port(board: str, port: str | None) -> str:
    """Map a fleet number or serial number to a /dev/serial/by-id path."""
    if port:
        return port
    serial_no = FLEET.get(board, board)
    matches = [p for p in glob.glob("/dev/serial/by-id/*") if serial_no in p]
    if not matches:
        sys.exit(f"zadny port pro sn {serial_no} -- pripojena deska? (ls /dev/serial/by-id/)")
    if len(matches) > 1:
        sys.exit(f"vic portu pro sn {serial_no}: {matches}")
    return matches[0]


class SnifferRadio(USBLoRaRadio):
    """USBLoRaRadio that hands RX frames straight to a callback, metadata included.

    See the module docstring: the stock rx_callback loses the RSSI/SNR pairing
    under load, and it is scheduled onto an asyncio loop we do not want to run.
    """

    def __init__(self, *args, on_frame=None, **kwargs):
        super().__init__(*args, **kwargs)
        self._on_frame = on_frame

    def _dispatch_frame(self, cmd: int, payload: bytes):
        if cmd == CMD_RX_PACKET and len(payload) >= 6:
            # Keep the driver's own bookkeeping up to date -- get_status() is our
            # honest denominator -- but do NOT delegate to super() here: with no
            # rx_callback set it logs "RX packet but no callback registered" for
            # every single frame, which is noise in a capture.
            rssi, snr_x10, signal_rssi = struct.unpack("<hhh", payload[0:6])
            self.last_rssi, self.last_snr = rssi, snr_x10 / 10.0
            self.last_signal_rssi = signal_rssi
            self._rx_count += 1
            if self._on_frame:
                self._on_frame(payload[6:], rssi, snr_x10 / 10.0, signal_rssi)
            return
        super()._dispatch_frame(cmd, payload)


class PcapWriter:
    """Minimal pcap writer/streamer. Payload is the raw MeshCore frame.

    LINKTYPE_USER0 means Wireshark shows the bytes but does not dissect them --
    good enough for eyeballing lengths and repeats, and the decoded fields are in
    the JSONL anyway. A Lua dissector for USER0 would be the next step.
    """

    def __init__(self, path: str | None, udp: str | None):
        self.fh = None
        self.sock = None
        self.dest = None
        header = struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, LINKTYPE_USER0)
        if path:
            self.fh = open(path, "wb")
            self.fh.write(header)
        if udp:
            host, _, port = udp.partition(":")
            self.dest = (host, int(port or 5555))
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.sock.sendto(header, self.dest)

    def write(self, data: bytes, ts: float):
        if not (self.fh or self.sock):
            return
        rec = struct.pack("<IIII", int(ts), int((ts % 1) * 1e6), len(data), len(data)) + data
        if self.fh:
            self.fh.write(rec)
            self.fh.flush()
        if self.sock and self.dest:
            self.sock.sendto(rec, self.dest)

    def close(self):
        if self.fh:
            self.fh.close()
        if self.sock:
            self.sock.close()


def channel_keys(psk_hex: str) -> tuple[int, bytes]:
    """(channel hash, 32B secret) from a hex PSK, matching the firmware.

    The firmware hashes only the first 16 bytes when the second 16 are zero, so a
    128-bit key written out as 32 hex bytes still derives the right hash.
    """
    secret = bytes.fromhex(psk_hex)
    if len(secret) >= 32 and secret[16:32] == b"\x00" * 16:
        hashed = secret[:16]
    else:
        hashed = secret[:32]
    master = hashlib.sha256(hashed).digest()
    secret32 = (secret + b"\x00" * 32)[:32]
    return master[0], secret32


def try_decrypt_channel(payload: bytes, keys: dict[int, list[bytes]]) -> dict | None:
    """MeshCore group text: hash(1) | mac(2) | AES-128-ECB ciphertext."""
    if len(payload) < 4:
        return None
    chan_hash, mac, ciphertext = payload[0], payload[1:3], payload[3:]
    for secret32 in keys.get(chan_hash, []):
        if hmac.new(secret32, ciphertext, hashlib.sha256).digest()[:2] != mac:
            continue  # 1-byte hash collides; the HMAC is the real test
        from Crypto.Cipher import AES  # pycryptodome, pulled in by openhop-core

        padded = ciphertext + b"\x00" * (-len(ciphertext) % 16)
        plain = AES.new(secret32[:16], AES.MODE_ECB).decrypt(padded)[: len(ciphertext)]
        if len(plain) < 5:
            return None
        return {
            "timestamp": int.from_bytes(plain[:4], "little"),
            "flags": plain[4],
            "text": plain[5:].decode("utf-8", errors="replace").rstrip("\x00"),
        }
    return None


def decode(data: bytes, keys: dict[int, list[bytes]]) -> dict:
    """Parse one raw frame into a flat dict. Never raises."""
    out: dict = {"len": len(data), "raw": data.hex()}
    pkt = Packet()
    try:
        pkt.read_from(data)
    except Exception as exc:  # truncated, bad version, bogus path_len
        out["error"] = str(exc)
        return out

    hash_size = pkt.get_path_hash_size()
    hash_count = pkt.get_path_hash_count()
    payload_type = pkt.get_payload_type()
    path = bytes(pkt.path)
    out.update(
        {
            "route": ROUTE_TYPES.get(pkt.get_route_type(), "?"),
            "type": PAYLOAD_TYPES.get(payload_type, f"0x{payload_type:02X}"),
            "ver": pkt.get_payload_ver(),
            # Hash width is chosen by the SENDER and carried in bits 6-7 of
            # path_len, so a 2B/3B-path-hash node decodes here without a rebuild.
            "hash_size": hash_size,
            "hops": hash_count,
            "path": ":".join(
                path[i : i + hash_size].hex() for i in range(0, hash_size * hash_count, hash_size)
            ),
        }
    )
    if pkt.has_transport_codes():
        out["transport_codes"] = list(pkt.transport_codes)

    payload = bytes(pkt.payload)
    name = out["type"]
    try:
        if name == "ADVERT":
            adv = parse_advert_payload(payload)
            app = decode_appdata(adv["appdata"]) if adv["appdata"] else {}
            out["advert"] = {
                "pubkey": adv["pubkey"],
                "path_hash": adv["pubkey"][: hash_size * 2],
                "timestamp": adv["timestamp"],
                "name": app.get("node_name"),
                "role": ADVERT_ROLES.get(app.get("flags", 0) & 0x0F),
                "lat": app.get("latitude"),
                "lon": app.get("longitude"),
            }
        elif name in ("GRP_TXT", "GRP_DATA"):
            out["channel_hash"] = f"{payload[0]:02x}"
            decrypted = try_decrypt_channel(payload, keys)
            if decrypted:
                out["channel_msg"] = decrypted
        elif name == "ACK":
            out["ack_crc"] = payload[:4].hex()
        elif name in ("TXT_MSG", "RESPONSE", "REQ"):
            # dest hash | src hash | mac | ciphertext, hashes as wide as the sender chose
            out["dest"] = payload[:hash_size].hex()
            out["src"] = payload[hash_size : hash_size * 2].hex()
        elif name == "PATH":
            out["dest"] = payload[:hash_size].hex()
        elif name == "TRACE":
            tag, auth, flags = struct.unpack("<IIB", payload[:9])
            out["trace"] = {"tag": f"{tag:08x}", "auth": f"{auth:08x}", "flags": flags}
        elif name == "ANON_REQ":
            out["dest"] = payload[:hash_size].hex()
            out["src_pubkey"] = payload[hash_size : hash_size + 32].hex()
    except Exception as exc:
        out["decode_error"] = f"{name}: {exc}"
    return out


def format_line(rec: dict) -> str:
    """One packet, one terminal line, most-identifying field last."""
    head = (
        f"{time.strftime('%H:%M:%S', time.localtime(rec['ts']))} "
        f"{rec['rssi']:>4}dBm {rec['snr']:>5.1f}dB {rec['len']:>3}B "
        f"{rec.get('route', '?'):<15} {rec.get('type', '?'):<9} "
        f"{rec.get('hops', 0)}hop/{rec.get('hash_size', 0)}B"
    )
    if rec.get("dup"):
        head += f" dup#{rec['dup']}"
    if "error" in rec:
        return f"{head}  UNPARSED: {rec['error']}  {rec['raw'][:48]}"
    tail = []
    if rec.get("path"):
        tail.append(f"path={rec['path']}")
    adv = rec.get("advert")
    if adv:
        tail.append(f"{adv.get('role') or 'node'} {adv.get('name') or '?'} [{adv['path_hash']}]")
        if adv.get("lat"):
            tail.append(f"{adv['lat']:.4f},{adv['lon']:.4f}")
    if rec.get("channel_hash"):
        tail.append(f"chan={rec['channel_hash']}")
    if rec.get("channel_msg"):
        tail.append(f"msg={rec['channel_msg']['text']!r}")
    for key in ("dest", "src", "ack_crc"):
        if rec.get(key):
            tail.append(f"{key}={rec[key]}")
    if rec.get("trace"):
        tail.append(f"trace tag={rec['trace']['tag']}")
    return f"{head}  " + " ".join(tail)


def main() -> int:
    ap = argparse.ArgumentParser(description=(__doc__ or "").split("\n")[0])
    ap.add_argument("--board", default="0", help="fleet number or serial number (default 0)")
    ap.add_argument("--port", help="explicit /dev/serial/by-id path, overrides --board")
    ap.add_argument("--preset", choices=sorted(PRESETS), default="cz")
    ap.add_argument("--freq", type=float, help="MHz, overrides preset")
    ap.add_argument("--bw", type=float, help="kHz, overrides preset")
    ap.add_argument("--sf", type=int, help="overrides preset")
    ap.add_argument("--cr", type=int, help="overrides preset")
    ap.add_argument("--psk", action="append", default=[], help="channel PSK hex, repeatable")
    ap.add_argument("--jsonl", help="append decoded packets here, one JSON per line")
    ap.add_argument("--pcap", help="write raw frames as pcap (LINKTYPE_USER0)")
    ap.add_argument("--wireshark", help="also stream pcap over UDP, host:port")
    ap.add_argument("--seconds", type=float, help="stop after N seconds")
    ap.add_argument("--status-interval", type=float, default=60.0, help="0 disables")
    ap.add_argument("--quiet", action="store_true", help="no per-packet stdout")
    args = ap.parse_args()

    freq, bw, sf, cr = PRESETS[args.preset]
    if args.freq:
        freq = int(args.freq * 1e6)
    if args.bw:
        bw = int(args.bw * 1000)
    sf, cr = args.sf or sf, args.cr or cr
    port = resolve_port(args.board, args.port)

    keys: dict[int, list[bytes]] = {}
    for psk in args.psk:
        chan_hash, secret32 = channel_keys(psk)
        keys.setdefault(chan_hash, []).append(secret32)

    stats = Counter()
    seen: dict[str, int] = {}
    lock = threading.Lock()
    jsonl = open(args.jsonl, "a") if args.jsonl else None
    pcap = PcapWriter(args.pcap, args.wireshark)

    def on_frame(data: bytes, rssi: int, snr: float, signal_rssi: int):
        ts = time.time()
        with lock:
            stats["heard"] += 1
            rec = decode(data, keys)
            rec.update({"ts": ts, "rssi": rssi, "snr": snr, "signal_rssi": signal_rssi})
            digest = hashlib.sha256(data).hexdigest()[:16]
            seen[digest] = seen.get(digest, 0) + 1
            if seen[digest] > 1:
                rec["dup"] = seen[digest]
                stats["dup"] += 1
            stats["parsed" if "error" not in rec else "unparsed"] += 1
            stats[f"type:{rec.get('type', 'BAD')}"] += 1
            pcap.write(data, ts)
            if jsonl:
                jsonl.write(json.dumps(rec, default=str) + "\n")
                jsonl.flush()
            if not args.quiet:
                print(format_line(rec), flush=True)

    print(
        f"== port {os.path.basename(port)}\n"
        f"== {freq / 1e6:.3f} MHz  BW {bw / 1000:g} kHz  SF{sf}  CR{cr}"
        f"  ({args.preset}){'  +' + str(len(args.psk)) + ' PSK' if args.psk else ''}",
        flush=True,
    )
    radio = SnifferRadio(
        port=port,
        frequency=freq,
        bandwidth=bw,
        spreading_factor=sf,
        coding_rate=cr,
        tx_power=2,  # receive-only tool; keep the floor as a second safety net
        lbt_enabled=False,
        on_frame=on_frame,
    )
    if not radio.begin():
        sys.exit(f"modem na {port} neodpovedel -- bezi na desce openHop modem firmware?")
    print("== RX only, nic nevysilam. Ctrl-C ukonci a vypise souhrn.\n", flush=True)

    t0 = time.time()
    next_status = t0 + args.status_interval if args.status_interval else None
    try:
        while True:
            time.sleep(0.25)
            now = time.time()
            if args.seconds and now - t0 >= args.seconds:
                break
            if next_status and now >= next_status:
                next_status = now + args.status_interval
                # The modem's own counter is the honest denominator: it counted a
                # CRC-clean frame even when nothing here could parse it.
                modem = radio.get_status()
                with lock:
                    print(
                        f"-- {now - t0:6.0f}s  modem rx={modem.get('rx_count')} "
                        f"crc_err={modem.get('crc_errors')}  heard={stats['heard']} "
                        f"parsed={stats['parsed']} unparsed={stats['unparsed']} "
                        f"dup={stats['dup']}",
                        flush=True,
                    )
    except KeyboardInterrupt:
        pass
    finally:
        radio.cleanup()
        pcap.close()
        if jsonl:
            jsonl.close()

    elapsed = time.time() - t0
    print(f"\n== {elapsed:.0f}s, {stats['heard']} frames heard, {stats['dup']} duplicates")
    print(f"== parsed {stats['parsed']}, unparsed {stats['unparsed']}")
    for key, count in sorted(stats.items()):
        if key.startswith("type:"):
            print(f"   {key[5:]:<10} {count}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
