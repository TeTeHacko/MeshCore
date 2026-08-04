# BLE/LoRa coexistence — bench reproduction attempt

Working notes for the DIRECT-forward loss seen on `tth-ltm` (SenseCap Solar,
`SenseCap_Solar_repeater_ble`). Written down because the result is mostly
**negative**, and a negative result is only worth anything if the method and its
limits are recorded with it.

## The claim under test

Commit `f6e9aeca` widened the BLE connection interval from 15–30 ms to 60–80 ms
after an A/B on the mast showed ~40 % of DIRECT-route forwards silently lost
with an **idle** BLE connection at 15–30 ms. The stated mechanism is that the
SoftDevice's per-connection-event preemption disturbs the SX126x TX path
(`startTransmit` fail / send-complete timeout).

The fix is deployed. What was never established is whether a bench rig can
**detect** the effect at all — a measurement that reads zero in both the
treatment and the control has shown nothing.

## Rig

Isolated test mesh on **866.5 MHz** (BW 62.5, SF 8, CR 5). Isolation is by
frequency, not by tx power: a hilltop repeater with `RX_BOOSTED_GAIN` hears a
few mW just fine. The override is compile-time (`platformio.local.ini`) because
`MyMesh.cpp` seeds `_prefs.freq` from `LORA_FREQ`, so the firmware cannot come
up on the production frequency even after `erase`.

| node | board | role |
|---|---|---|
| `tth-x1-rpt` | XIAO nRF52840 + Wio-SX1262 | forwarder under test |
| `tth-x4-rpt` | same | paired control / radiating witness |
| `tth-x2-cmp`, `tth-x3-cmp` | same | probe sources |
| `tth-s1-rpt` | SenseCap Solar Node | forwarder, mast-identical hardware |
| `tth-s2-rpt` | same | paired control |

Paired controls differ **only** in `ADVERT_NAME`, verified by diffing strings in
the ELF. Any other difference invalidates them.

### Oracle

The measured quantity is the node's own `direct_rx − direct_tx` from
`stats-packets`: how many DIRECT packets it accepted for forwarding versus how
many it actually sent. This is self-normalising — it does not depend on channel
conditions or on a receiver hearing anything. `tx_start_fail` and `tx_timeout`
are the mechanism-level counters. A second repeater with `repeat off` acts as a
radiating witness, so "counted a TX" can be distinguished from "actually
radiated".

Route forcing: a contact is injected into the sender's companion with an
explicit `out_path`, so the DM must traverse the node under test.
**`out_path_len` is not a hop count** — it is a packed byte, top two bits =
hash size − 1, low six = hop count (`Packet.h:85`). One hop with a 2-byte hash
is `0x41`, not `1`. The receiver reads the size out of the packet
(`Mesh.cpp:93`), so a forced path works regardless of the node's
`path.hash.mode`.

## What was measured

All cells at a **verified** 22.50 ms connection interval — `bleconn` reports the
parameters actually negotiated, because `bleOnSecured()` only requests them and
the central may refuse. Without that read-back none of the null results below
would mean anything.

| `blepwr` | ≈RSSI at central | link up | direct_rx | direct_tx | lost | tx_timeout |
|---:|---:|---:|---:|---:|---:|---:|
| BLE off | — | — | 8 | 8 | 0 | 0 |
| 0 dBm | −67 | 100 % | 8 | 8 | 0 | 0 |
| −8 | −75 | 100 % | 8 | 8 | 0 | 0 |
| −12 | −79 | 100 % | 8 | 8 | 0 | 0 |
| −16 | −83 | 87 % | 8 | 8 | 0 | 0 |
| −20 | −87 | 27 % | 8 | 8 | 0 | 0 |

The −20 dBm row is a bonus condition: the link held only a quarter of the
window, so the node spent the rest advertising and re-connecting — close to the
reconnect churn the mast reports as HCI 0x3E.

**Ruled out as the missing factor: BLE link margin.** The whole −75..−90 window
the mast sits in is covered, at the known-bad interval, with zero loss.

### Why idle + weak link does nothing

In idle the connection events carry empty PDUs, whose retransmission costs
almost no radio time. The cost of a retransmission scales with payload size, and
the mast has a bridge actively draining the console. Hence the `poll` mode in
the harness.

Direction matters too, and it is the reason to weaken **our** side rather than
the central's: a connection event ends as soon as either side misses a packet,
so if the central misses our PDU we must re-send it at the next anchor — an
extra slave TX. If we miss the central's PDU we never transmit in that event at
all, and the SoftDevice uses *less* radio.

## On mast hardware, with the file log on

Two SenseCap Solar nodes (`tth-s1-rpt` forwarder, `tth-s2-rpt` paired control) —
the same board as the mast, so InternalFS logging, INA226/solar telemetry and
the SenseCap SPI layout are all in play. Again at a verified 22.50 ms interval
(`bleconn`).

| file log | BLE | direct_rx | direct_tx | lost | tx_timeout | witness |
|---|---|---:|---:|---:|---:|---:|
| off | off | 8 | 8 | 0 | 0 | +39 |
| off | idle, 88 % of window | 8 | 8 | 0 | 0 | +37 |
| **on** | off | 8 | 8 | 0 | 0 | +39 |
| **on** | idle, 90 % of window | 8 | 8 | 0 | 0 | +40 |

The logging condition was real, not nominal: `log` afterwards held 69 entries
with RX/TX lines timestamped inside the measurement window, i.e. an InternalFS
write per packet — the mechanism behind the known deaf windows.

**Ten cells, 80 forced forwards, zero lost, `tx_timeout` and `tx_start_fail` at
zero throughout.** Against the mast's reported ~40 % loss rate this is not a
weak null: a single 8-probe cell would miss a true 40 % effect with probability
0.6⁸ ≈ 1.7 %, and ten cells make it vanishing. Whatever the mast is doing, it is
not reproduced by the connection interval, the BLE link margin, the board, or
InternalFS logging — alone or together.

## Still open

**Loaded LoRa channel** is the one uncontrolled difference left. Ours is idle;
the mast relays flood traffic, so its forwards contend for a busy channel and
the TX window lands on a BLE connection event far more often. This cannot be
tested legally here: 866.5 is capped at 1 % duty and the 10 % sub-band
(869.4–869.65) is where production lives.

Practical consequence: **f6e9aeca can be neither confirmed nor refuted on the
bench.** Deciding whether the 60–80 ms interval is still needed requires
measuring on `tth-ltm` itself — the counters (`stats-packets`, and now
`bleconn` to prove which interval is actually in force) are all reachable over
its BLE console.

## Airtime is a hard limit, and the firmware will not enforce it

866.5 MHz sits in 865–868 (CZ VO-R/10): 25 mW ERP, **1 % duty cycle = 36 s of
transmission per hour**. The firmware does not police this — `airtime_factor`
defaults to 0 (= 100 %) and is clamped to 9, so the lowest duty MeshCore can
enforce is 10 %. The first sweep consumed `tx_air_secs` 27 s in 18 minutes of
uptime, a rate of ~2.5 %. Budget the probes: `stats-radio.tx_air_secs` is the
ground truth, and the harness aborts before crossing a configured cap.

`stats-core.queue_len` and `errors` are a required control, not decoration: when
the airtime budget runs out the Dispatcher **defers** a forward
(`next_tx_time`, `Dispatcher.cpp:101`), which looks identical to loss in the
counters. Both were zero throughout.

## Traps worth remembering

- **USB and BLE consoles share one `command[160]`.** Two concurrent consoles
  interleave into one line and crash the node. The harness keeps a strict
  sequence: counters over USB → close USB → BLE client → probes → stop BLE →
  counters over USB.
- **`Bluefruit.setTxPower()` is advertising-only** (see the commit message for
  `blepwr`); the connection runs at the SoftDevice default 0 dBm.
- **Weakening the link can lock you out.** Below about −16 dBm the node's
  replies stop arriving and the value is re-applied on every reconnect. Hence
  the auto-revert on `blepwr`.
- **~~Runtime `set tx` does not change radiated LoRa power~~ — retracted.** The
  original reading (+2 dBm → RSSI −44.1, −9 dBm → −44.0) was made with an
  instrument that could not report failure: `RadioLibWrapper::setTxPower()`
  discarded RadioLib's status, so a refused request looked exactly like an
  accepted one. Re-measured 2026-08-04 on the repeater build with `txpwr`
  confirming each step, x1 transmitting and x4 as witness:

  | `txpwr` | witness `last_rssi` |
  |---:|---:|
  | +22 | 0 dBm *(saturated)* |
  | +14 | −1 dBm *(saturated)* |
  | +6 | −7 dBm |
  | 0 | −18 dBm |
  | −9 | −28 dBm |

  31 dB of command produced 28 dB of RSSI, and the witness `recv` counter rose
  by one at every step, so no point was missed. Radiated power does follow the
  setting. The top two points are saturated — the boards were ~10 cm apart — so
  the usable range of that particular rig starts around +6 dBm; characterising
  the top end needs attenuation or distance, not a bigger number.

  The lesson is not about tx power. It is that a null result from an instrument
  which cannot distinguish "refused" from "no effect" is not a result at all.

## The two commands this bench added

Both live in `examples/simple_repeater/main.cpp` and are repeater-only.

### `bleconn` — what the link is actually doing

```
bleconn
-> BLE conn: interval 75.00 ms (req 60-80), latency 0, sup timeout 4000 ms [live]
```

Reports the **negotiated** connection parameters, not the requested ones. The
central is free to ignore a peripheral's request, so this is the only way to
know whether a change like `f6e9aeca` (widening the requested interval to
60–80 ms) is in force on a node you cannot see. The trailing `[live]` /
`[last]` says whether a client is connected right now or these are the values
from the last one; before any client has ever connected it answers
`nothing seen yet`.

### `blepwr [dBm [hold]]` — connection TX power, with a way back

```
blepwr             # report: BLE conn tx: 0 dBm, adv tx: 4 dBm
blepwr -20         # weaken the link; auto-reverts after 5 minutes
blepwr -20 hold    # ... unless you opt out
```

The reply carries how many live connections accepted the value
(`OK - BLE conn tx -20 dBm (1/1 conn), auto-revert armed`) because the
nRF52840 only takes −40, −20, −16, −12, −8, −4, 0 and 2..8 dBm; anything else
is rejected by `setTxPower()` and is *not* latched, so a typo cannot arm itself
for the next reconnect.

Sets `BLE_GAP_TX_POWER_ROLE_CONN`, which is **independent of**
`Bluefruit.setTxPower()` — that one only affects advertising, despite reading
like a global. The connection otherwise runs at the SoftDevice default of
0 dBm; the compile-time starting value is `RPT_BLE_CONN_TX_POWER`.

The value is re-applied on every 'secured' event, so a setting weak enough to
break the link would persist across reconnects and lock you out of a node with
no cable — which is exactly what happened at −40 dBm during this bench. Hence
the five-minute auto-revert: worst case you wait it out instead of driving to
the node.
