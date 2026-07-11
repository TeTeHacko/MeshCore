# MeshCore Passive Mesh Analyzer

A zero-infrastructure way to pull **everything a node hears on the radio** out
over BLE/serial and plot it on a map — the "MeshSense for MeshCore" that does
not exist yet (see prior-art gap analysis below).

```
  repeater FW  ──(text console: BLE-NUS / USB serial)──▶  bridge  ──▶  MQTT + map
  (this repo)        nodes / rxlog  commands                 (separate agent)
```

The **firmware half is implemented in this repo** (repeater, `SenseCap_Solar_repeater_ble`).
The **bridge + map half is a separate project** for another agent; this document
is its spec. Everything the bridge needs to know about the FW protocol is here,
so the bridge author does not need to read the firmware source.

---

## 1. Firmware protocol (implemented)

Two RAM-only, opt-in tables are fed from the normal RX path and exposed as
**text CLI commands** on the repeater console. The console is request/reply with
a **~160-byte reply cap**, so both commands are **paged** and carry a trailer so
the caller can drain them and detect if it fell behind. Reachable over USB
serial, the PIN-paired BLE Nordic-UART console, and the admin mesh CLI — exactly
like the existing `neighbors` command.

Enabled per-variant via build flags (compiled out otherwise):

```ini
-D MAX_HEARD_NODES=48   ; size of the heard-repeater/room registry
-D RXLOG_SIZE=96        ; size of the circular "everything heard" ring
```

### 1.1 `nodes [offset]` — heard repeater/room-server registry

One row per **distinct repeater or room server** whose (signature-verified)
advert we heard. **Chat clients and sensors are intentionally NOT recorded** —
they are personal/mobile devices, so mapping them is a privacy concern and just
churns the table. Sorted newest-heard first.

Per-node line:

```
N,<prefix>,<type>,<lat>,<lon>,<snr>,<rssi>,<age>,<path>,<name>
```

| Field    | Meaning                                                              |
|----------|---------------------------------------------------------------------|
| `prefix` | first **6 bytes** of the node's Ed25519 pubkey, hex (12 chars). The 1st byte is its path-hash. |
| `type`   | advert node type: `2`=repeater, `3`=room server (only these appear) |
| `lat`    | latitude × 1e6, signed integer (`0` = unknown)                      |
| `lon`    | longitude × 1e6, signed integer (`0` = unknown)                     |
| `snr`    | last-heard SNR **× 4** (0.25 dB units); divide by 4 for dB          |
| `rssi`   | last-heard RSSI in dBm, signed                                      |
| `age`    | seconds since last heard                                            |
| `path`   | hex of the hop-hash trail the advert arrived by (empty = 0-hop / direct). See §1.3. |
| `name`   | node name (UTF-8). **Everything after the 9th comma is the name** — it may itself contain commas. |

Trailer line: `E,<next_offset>,<total>`
- `next_offset` — pass as the next `offset`; when it equals `total`, you are done.
- `total` — total non-empty rows in the registry.

Example:

```
> nodes
N,a3f19c02bb71,2,50548100,14128500,48,-95,37,,tth-ltm.meshcore.cz
N,10c4de00aa02,2,50510000,14200000,24,-108,410,a3,Repeater-Hill
E,2,2
```

### 1.2 `rxlog [cursor]` — firehose of every frame heard

A circular buffer capturing **every parsed frame** the radio decoded (any type,
whether or not it was for us), fed synchronously so SNR/RSSI belong to that
exact frame. Returns records with `seq > cursor`, oldest first.

Per-record line:

```
R,<seq>,<when>,<pkthash>,<hdr>,<snr>,<rssi>,<path>
```

| Field     | Meaning                                                             |
|-----------|--------------------------------------------------------------------|
| `seq`     | monotonic capture sequence (the paging cursor)                     |
| `when`    | capture time, epoch seconds (node RTC; may be wrong until GPS/clock sync) |
| `pkthash` | FNV-1a hash of (type‖payload), hex (8 chars) — cheap dedup / correlation across nodes running this FW. **Not** the MeshCore SHA packet-hash (avoided to keep the radio hot path light). |
| `hdr`     | raw header byte, hex. Decode: see §1.3                              |
| `snr`     | SNR × 4 (0.25 dB units)                                            |
| `rssi`    | RSSI in dBm, signed                                                |
| `path`    | hex of the hop-hash trail (empty = 0-hop). See §1.3.               |

Trailer line: `E,<last_seq>,<oldest_seq>,<newest_seq>`
- `last_seq` — highest seq returned this call; pass it as the next `cursor`.
- `oldest_seq` / `newest_seq` — the seq range still held in the ring.
- **Drop detection:** if your previous cursor `+ 1 < oldest_seq`, the ring
  overwrote `(oldest_seq − cursor − 1)` records before you read them — poll faster
  or grow `RXLOG_SIZE`.

Example:

```
> rxlog 40
R,41,1799887654,ab12cd34,11,48,-95,
R,42,1799887656,7f0e1122,09,20,-110,a310c4
E,42,17,42
```

### 1.3 Decoding reference

**Header byte** (`hdr`):
- bits 0-1 = route type: `0`=transport-flood, `1`=flood, `2`=direct, `3`=transport-direct
- bits 2-5 = payload type (table below)
- bits 6-7 = payload version

**Payload types:** `0`=REQ, `1`=RESPONSE, `2`=TXT_MSG, `3`=ACK, `4`=**ADVERT**,
`5`=GRP_TXT, `6`=GRP_DATA, `7`=ANON_REQ, `8`=**PATH**, `9`=**TRACE**,
`0xA`=MULTIPART, `0xB`=CONTROL, `0xF`=RAW_CUSTOM.

**Path field** (`nodes` and `rxlog`): a flood packet accumulates one hop-hash per
repeater it traverses. Each hop-hash is the **leading byte(s) of that repeater's
pubkey**. Hash width is 1, 2 or 3 bytes (v1.14+ "multibyte path hash"); most
traffic is 1 byte. The `nodes` registry gives you the pubkey→prefix table to
resolve these bytes back to named/located repeaters — but 1-byte hashes collide
(1 in 256), so resolve against the local `nodes` set and, where ambiguous, keep
the edge as "unknown hop". A truncated path (very long routes) is capped at 16
bytes stored.

**Units:** SNR in the wire is × 4 (0.25 dB); RSSI is plain dBm; lat/lon are
degrees × 1e6.

---

## 2. Bridge + map spec (for the other agent)

### 2.1 Transport & poll loop
- Reuse the existing dopey BLE bridge (meshcore-ble-bridge, bleak, NUS on
  `FA:4F:30:E3:1B:7A`; write cmd to `6e400002` in ≤20-byte chunks + `\r`, read
  notifications on `6e400003`). The console echoes replies prefixed with `  -> `.
- Persist two cursors across restarts: `nodes` offset is re-derived each poll
  (walk offset 0→total, it's small); `rxlog` keep the last `seq` you consumed.
- Suggested cadence for a quiet home mesh: poll `rxlog` every ~5 s draining until
  `last_seq == cursor`; poll `nodes` every ~60 s (it changes slowly). Honour the
  drop-detection rule and log/counter it (do **not** silently skip).

### 2.2 Parsing
- Split each reply into lines; dispatch on the first char (`N`/`R`/`E`).
- For `N` lines, treat everything after the 9th comma as the name.
- Convert `snr/4` → dB, keep RSSI as dBm, `lat/1e6`,`lon/1e6` → degrees.

### 2.3 Hash → node resolution
- Build a local map: `pubkey-prefix-byte(s) → {name, type, lat, lon}` from the
  `nodes` registry.
- Optionally enrich against the public registry
  `https://map.meshcore.io/api/v1/nodes` (keyed on full pubkey + location) to
  resolve hops this node hasn't personally heard an advert from.
- Mark unresolved / colliding hops explicitly rather than guessing.

### 2.4 MQTT (optional, for interop)
- Publish `rxlog` records in a shape compatible with `meshcoretomqtt`
  (`meshcore/{region}/{pubkey}/packets`, JSON with snr, rssi, type, route,
  path, hash) so existing tools (CoreScope, MeshCore Hub) can ingest it.

### 2.5 Map (Leaflet, self-contained)
- **Dots**: nodes from `nodes` lat/lon, icon by type (repeater vs room).
- **Edges**: draw the hop path of each advert / rxlog record as a polyline
  between resolved repeaters; colour/width by the SNR/RSSI of the final hop to us.
- **Coverage heatmap**: anchored at this repeater's own position, from the
  SNR/RSSI of everything it hears — a genuine local RF-truth / drive-test view.
- **Live traffic feed** + **VCR-style replay** over the `rxlog` timeline (see
  CoreScope for UX).

### 2.6 Prior art to study / stay compatible with
- **CoreScope** (github.com/Kpa-clawbot/meshcore-analyzer) — open-source packet
  viz + VCR replay; MQTT-ingest.
- **MeshCore Hub** (github.com/ipnet-mesh/meshcore-hub) — path-hash→node
  resolution UX.
- **MeshSense** (github.com/Affirmatech/MeshSense) — the single-node-BLE analyzer
  product shape (Meshtastic only; this fills the MeshCore gap).
- **MeshMap** (meshmap.net) — dots + neighbour-lines + traceroute map UX.
- The official **map.meshcore.io** does dots only (opt-in manual uploads); the
  observer analyzers require a USB-tethered node + MQTT broker + server. The
  differentiator here is **portable, offline, single-node BLE capture with local
  RF truth** — lean into that.

---

## 3. Firmware data model (reference)

`HeardNode` (registry row): `pub_prefix[6]`, `name[24]`, `lat`/`lon` (int32 ×1e6),
`heard_timestamp`, `advert_timestamp`, `snr` (×4), `rssi` (dBm), `type`,
`path_len` (bit-packed), `path[16]`.

`RxLogRec` (ring record): `seq`, `when`, `pkt_hash` (4 B), `snr` (×4), `rssi`,
`header`, `path_len` (bit-packed), `path[16]`.

Implementation: `examples/simple_repeater/MyMesh.{h,cpp}` (`putHeardNode`,
`captureRx`, `formatNodesReply`, `formatRxLogReply`, fed from `onAdvertRecv` and
`logRx`). Per-packet RSSI comes from `Packet::_rssi`, captured at RX in
`Dispatcher::checkRecv`.
