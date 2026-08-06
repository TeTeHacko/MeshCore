# Power saving on a solar repeater — what it costs, measured

Everything here was measured on the deployment hardware (SenseCAP Solar Node
P1-Pro, nRF52840 + SX1262 + L76K) on 2026-08-05/06 with `tools/power_ab.py`, not
taken from a datasheet. Numbers are input current on the 5 V USB side; the delta
between two states is what the paired design measures, and that is what these
figures are.

## The order that matters

| lever | measured | verdict |
|---|---|---|
| **GPS running continuously** | **46.0 mA** (spread 43.7-47.5) | `gps duty` recovers 42-45 of it |
| BLE advertising + Bluefruit's blinking LED | see below | defaults changed, LED off, 1 s interval |
| `powersaving on` (MCU light sleep) | **~1 mA, not separable from zero** | keep it on (free), but it is not a lever |
| RX duty cycle (EasySkyMesh "rxps") | ~2 mA on our SF7 preset | **not worth pre-release code** whose failure mode is a deaf repeater |
| `radio.rxgain off` | ~0.7 mA | **no** — 1-3 dB of sensitivity is what a mast node is for |
| FEM LNA control | n/a | SenseCap has no FEM, only an RXEN switch |

A whole node in the shipping configuration, with the battery charged and GPS
duty-cycled, sits at **~24 mA**. Continuous GPS alone would nearly triple that.

## GPS is the whole game

The receiver on this board draws 46 mA, and a mast repeater runs GPS for one
reason only: the RTC is volatile, and a node whose clock is wrong has its
replies dropped by clients. It does not need a continuous fix — it needs the
clock set.

`gps duty` (`gps_enabled == 2`, so no prefs-layout change) powers the receiver
only around a sync, at the same 30-minute cadence `gps on` already re-syncs at,
so clock accuracy is unchanged. Verified on hardware in both paths:

- with sky (`tth-ltm`): `duty, asleep, fix, 7 sats` — got a fix, set the clock,
  cut power to the receiver;
- without (indoors, `tth-s1-rpt`): `awake` for exactly 300 s
  (`GPS_DUTY_MAX_AWAKE_SECS`), then `asleep` with an hourly retry, so a blocked
  sky cannot turn duty mode back into "always on".

Worst case (fix never obtained) still saves 42 mA; a normal 30 s fix saves 45.
Knobs: `GPS_DUTY_SYNC_INTERVAL_SECS` (1800), `GPS_DUTY_MAX_AWAKE_SECS` (300),
`GPS_DUTY_RETRY_SECS` (3600).

**One visible side effect:** `querySensors()` only adds the GPS channel to
telemetry while the receiver is powered, so a duty-cycled node reports position
in telemetry only during its sync windows (~2 % of the time). What a repeater
*advertises* is unaffected — that comes from prefs via `gps advert prefs` — and
`node_lat/lon` keep the last fix. Confirmed in the field: `tth-ltm`'s scrape now
carries `voltage/temperature/humidity` and no `gps` field.

## BLE: two defaults that were quietly expensive

- **The blinking LED.** Bluefruit drives `LED_BLUE` while advertising and holds
  it on while connected. On SenseCap Solar that pin is **12 — the same pin as
  `P_LORA_TX_LED`**, so a sealed mast node spent solar charge blinking an LED
  nobody can see, and the LoRa TX indicator lied as a bonus.
  `RPT_BLE_CONN_LED` now defaults to 0.
- **The advertising rate.** Upstream leaves the slow interval at 244 units =
  152.5 ms, i.e. three channels ~6.5 times a second, forever.
  `RPT_BLE_ADV_SLOW` now defaults to 1600 = 1 s; the first
  `RPT_BLE_ADV_FAST_SECS` (30) after boot or disconnect stay fast so discovery
  is still immediate when someone is looking.

Bench boards keep both upstream behaviours (`-D RPT_BLE_CONN_LED=1`,
`-D RPT_BLE_ADV_SLOW=244` in `xiao_analyzer`): on a desk the blinking LED is the
"this thing is alive" indicator.

`ble off` remains the big BLE lever for a deployed node, but the deployment rule
stands: ship with BLE **on**, verify the node on site, and only then turn it off
over the mesh — otherwise a node that does not come up has no way in.

## MCU sleep

`powersaving on` makes the nRF52 wait in WFE between events (it wakes on any
interrupt, including LoRa DIO1). Upstream ships it **off**, and it was missing
from our provisioning, so every node built from that file ran the core at 64 MHz
around the clock. It is now in `tools/provision/repeater-cz-silent.txt`.

**But it buys about 1 mA, not the 3-4 that seemed obvious.** Measured with GPS
held off so its sync windows could not pollute the cells: paired estimate
-0.96 mA over three pairs, spread -1.97 to +0.07, against a measured
cell-to-cell repeatability of 1.16 mA — i.e. the effect is the same size as this
rig's noise and cannot be told from zero. That fits the mechanism: the loop
still polls the SX1262 over SPI on every pass and the SoftDevice wakes the core
for each advertising event, so there is not much idle left to sleep through.
Keep it on (it is free and cannot hurt), but do not budget for it.

One fork-specific fix goes with it: the sleep is also held off while the BLE
console still has queued output (`bleTxPending()`), because on the observer node
a half-drained `rxlog`/`pktlog` page would otherwise wait for the next
connection event — the bridge drains those on a 2-minute duty cycle, so a slower
drain directly costs collected frames.

## Measuring this yourself — two traps

1. **A solar node measured on its USB side measures the CHARGER.** With a
   charging cell the meter reads 500-700 mA, pulsing, and the node's own tens of
   mA ride on top of a drifting number. Wait for the charge to finish (it fell
   0.71 → 0.53 → 0.22 → 0.05 A over about an hour here) or the numbers are
   fiction. There is **no INA226 on this board** to cross-check with — the `i2c`
   command reports nothing at all on the only bus the variant has. Checked on
   two separate P1-Pro units (`tth-s1-rpt`, `tth-hrebecna`), so this is the
   board, not one dead sensor. The cross-check has to be a second *board*.
2. **With `gps duty` enabled you cannot measure anything under ~5 mA.** A sync
   window lands in a random cell and adds ~17 mA to a 10-minute average (seen:
   40.9 mA against 24.1-24.5 in its neighbours). Put `gps off` in every cell of
   such a run and restore `gps duty` afterwards.

See `tools/README.md` (`power_ab.py`) for the alternating-cell design that makes
these deltas trustworthy in the first place.
