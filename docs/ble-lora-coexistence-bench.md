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

## Still open

1. **Board and concurrent workload.** `tth-ltm` is a SenseCap Solar: InternalFS
   file log (`log start` is a known source of deaf windows — a flash write
   blocks the loop for tens of ms), INA226/solar telemetry, GPS, different SPI
   layout. None of that exists on the XIAO. Two SenseCap Solar nodes are now on
   the bench for exactly this.
2. **Loaded LoRa channel.** Ours is idle; the mast relays flood traffic. This
   one is capped by regulation, see below.

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
- **Runtime `set tx` does not change radiated LoRa power** on XIAO+Wio-SX1262:
  +2 dBm gave RSSI −44.1, −9 dBm gave −44.0, and the node confirmed the setting.
  Attenuate physically or rely on frequency for isolation.
