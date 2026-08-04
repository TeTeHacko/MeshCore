# Field tools

Scripts for talking to a node that is already deployed — or about to be. They
exist because the same handful of mistakes kept costing real time and, once,
real airtime on the live mesh. Each trap below is encoded in a script so it
cannot be repeated by hand.

| tool | what it does |
|---|---|
| `ble_pair.py` | bond to a node by PIN, non-interactively — the prerequisite for both tools below |
| `ble_cli.py` | the text console over BLE (Nordic UART), for nodes with no cable |
| `ble_dfu.py` | firmware update over BLE (legacy Nordic DFU, Adafruit nRF52 bootloader) |
| `provision/` | command files: what to send, and what the answers must look like |
| `build_version.py` | PlatformIO pre-script that stamps a real version into the binary |
| `xiao_uf2_flash.sh` | flash a named XIAO through its UF2 drive, and prove it took |

The usual sequence against a node you have never talked to before:

```sh
ble_pair.py <MAC>                  # bond (PIN from platformio.local.ini)
bluetoothctl disconnect <MAC>      # pairing leaves it connected = not advertising
ble_dfu.py firmware.zip <MAC>      # or ble_cli.py <MAC> ...
```

## Rule 0 — address boards by serial number

```sh
# right
/dev/serial/by-id/usb-Seeed_XIAO_nRF52840_Sense_B69F86518175CBA3-if00

# wrong
/dev/ttyACM1
```

`ttyACM*` numbering is assigned in enumeration order, so it changes on every
reflash and every replug. With more than one board on the bus, a script keyed
on `ttyACM1` will eventually write to the wrong one.

`by-id` is not automatically safe either: the *name* part of the path changes
when a board enters its bootloader (`XIAO_nRF52840` ↔ `XIAO_nRF52840_Sense`,
`Seeed_Studio_...` ↔ `Seeed_...`), so a hard-coded full path breaks mid-run.
Match on the **serial number** substring and glob the rest:

```sh
ls /dev/serial/by-id/ | grep B69F86518175CBA3
```

Over BLE the equivalent identity is the **address**, and it is equally
mandatory — see `ble_dfu.py` below.

## `ble_pair.py` — bond by PIN

```sh
ble_pair.py DC:28:1D:8A:04:1A
ble_pair.py DC:28:1D:8A:04:1A CE:72:14:CD:FD:02      # several in one run
```

Registers a KeyboardOnly agent with bluetoothctl and answers the passkey
prompt with `[secrets] ble_pin` from `platformio.local.ini`. It is state-driven,
not timed: an earlier version sent `pair` before the agent was registered, the
prompt went to the default agent, and the node stayed unpaired with no error
anywhere.

**Bonding is not only for the console.** The buttonless-DFU control point
carries the same `SECMODE_ENC_WITH_MITM`
(`src/helpers/nrf52/SerialBLEInterface.cpp`), so `ble_dfu.py` phase 1 cannot
write its trigger to an unpaired node either — it fails as an ordinary write
error that looks nothing like "you are not bonded".

**Disconnect afterwards.** bluetoothctl leaves the node connected once pairing
succeeds, and a connected peripheral stops advertising, so the very next
`ble_dfu.py` run dies on `APP nenalezen ve scanu`.

**Dual USB/BLE companions only advertise while undocked.** On T1000-E and Wio
L1 builds, dock-quiet suppresses advertising whenever a USB host holds DTR high
(`SerialDualInterface`). Unplug USB — or, if the cable has to stay in, hold the
port open with DTR deasserted:

```python
s = serial.Serial(); s.port = PORT; s.dtr = False; s.rts = False
s.open(); s.dtr = False          # node starts advertising within a second
```

Merely closing the port is not always enough; observed on a T1000-E that stayed
dark until something explicitly drove DTR low.

## `ble_cli.py` — console over BLE

```sh
ble_cli.py <MAC> ver                              # one command
ble_cli.py <MAC> "get radio" "get name"           # several, one connection
ble_cli.py <MAC> -f provision/repeater-cz-silent.txt
ble_cli.py <MAC> --per-command "gps on" "set af 9"   # reconnect each time
ble_cli.py <MAC> -w 8 rxlog                       # longer wait for paged output
```

Prints `command<TAB>reply` per line, so output is greppable.

**The 20-byte trap.** A NUS write carries at most `ATT_MTU - 3` bytes and BlueZ
will not split it for you. MeshCore nodes routinely stay at the default MTU of
23, so anything over 20 bytes is rejected with
`[org.bluez.Error.Failed] Failed to initiate write` — which reads like a dropped
link, not "your command was too long". Commands up to 18 characters work and
everything longer fails, deterministically. `ble_cli.py` chunks every write, so
this cannot bite you through the tool; it *will* bite you in an ad-hoc snippet.

**Pairing.** Console characteristics are `SECMODE_ENC_WITH_MITM`, so bond first:

```sh
bluetoothctl --agent KeyboardOnly     # then: pair <MAC>, enter the BLE PIN
```

`erase` on the node wipes the bond too (bonds live in InternalFS) — re-pair
after one.

**One console at a time.** USB and BLE read into the same firmware command
buffer (`examples/simple_repeater/main.cpp`), so two consoles interleave into
one line and can take the node down.

## `ble_dfu.py` — firmware update over BLE

```sh
ble_dfu.py .pio/build/<env>/firmware.zip <MAC>
ble_dfu.py firmware.zip <MAC> phase2      # node already sits in the bootloader
```

Phase 1 writes the buttonless-DFU trigger to the running application; phase 2
reconnects to the bootloader and runs START / INIT / IMAGE / VALIDATE /
ACTIVATE.

**The target address is mandatory, and that is the point.** An earlier version
matched any device whose name contained "DFU" and took the first scan hit. With
several boards in the bootloader at once it connected to the wrong one — target
`CE:72:…:02`, connected `FE:11:…:C4`. It was caught only because a human was
reading the log; seconds later `START_DFU` would have overwritten an unrelated
board. `find()` now requires the address and logs every device it ignored.

**Chunk size is negotiated, not assumed.** The image is sent in `MTU - 3` byte
writes, but some bootloaders reject anything above 20 even after advertising a
larger MTU. On `ChunkTooBig` the tool falls back to 20-byte writes — via a
`RESET` and a fresh connection, because a second `START_DFU` inside the same
connection returns status 2 (`INVALID_STATE`).

**End of transfer is not an error.** The bootloader answers the final block
with `RESP(RECEIVE_FW, SUCCESS)` instead of another packet receipt. Read as a
receipt that looks like a failure at ~95 %; it is the normal end.

### How each board enters its bootloader

Observed, not inferred from documentation — the differences are real and each
one changes which flashing path works:

| board | trigger | bootloader appears as | UF2 disk? |
|---|---|---|---|
| XIAO nRF52840 | 1200-baud touch on USB | CDC only, PID `2886:0045` | **no** — serial DFU only |
| XIAO nRF52840 | double-tap reset | UF2 mass storage | yes |
| XIAO nRF52840 | buttonless over BLE | `AdaDFU`, same MAC, MTU 247 | yes |
| SenseCap Solar | buttonless over BLE | `AdaDFU`, same MAC, MTU stays 23 | — |
| SenseCap Solar (mast unit) | buttonless over BLE | `SCAP_DFU`, **MAC+1**, MTU 247 | — |

The first row is the one that wastes an afternoon: the 1200-baud touch *works*,
the board *is* in the bootloader, and no UF2 drive ever appears — because the
touch requests Adafruit's serial-DFU mode specifically. Flash it with
`adafruit-nrfutil` instead of waiting for a disk:

```sh
adafruit-nrfutil dfu serial -pkg firmware.zip \
    -p /dev/serial/by-id/<...matched on serial...> -b 115200 --singlebank
```

A SenseCap that has just taken a 1200-baud touch takes **60–300 s** to
re-enumerate. That is not a wedge — wait before concluding anything.

## `build_version.py` — make the node able to answer "what is on you?"

Every env that gets flashed must pull it in:

```ini
extra_scripts = ${nrf52_base.extra_scripts}
  pre:tools/build_version.py
```

Keep `${nrf52_base.extra_scripts}` — that is `create-uf2.py`, and dropping it
means no `.uf2` artifact.

Without the script the node reports the fallback literals compiled into
`examples/*/MyMesh.h` — `v1.16.0` and a build date of `6 Jun 2026` — and those
are byte-for-byte identical in every build ever produced. The failure mode is
silent: the env builds fine and the node lies about itself.

It costs a full rebuild every time (the define changes on each build), which is
10–30 s. Worth it.

**What it stamps:** `<upstream tag>-tth-<git sha>`, with a trailing `+` when the
tree is dirty, plus the compile timestamp. The base version comes from the
newest upstream release tag HEAD descends from, not from a literal in the
script — upstream tags per example (`companion-v1.16.0`, `repeater-v1.16.0`), so
anything matching `*v[0-9]*` counts and the prefix is stripped. Our own
`+tth.<timestamp>` release tags are excluded so they cannot shadow the base.

**Flash from a clean tree.** A trailing `+` on a node tells you which commit it
was near, but not what else was in the working copy at the time.

**The stamp has to fit the wire.** `RESP_CODE_DEVICE_INFO` carries the version in
20 bytes and the build date in 12, NUL included — 19 and 11 usable characters —
and anything longer is silently truncated before the client ever sees it. Hence
the terse `<tag>-tth<sha7>[+]` and `yymmdd hhmm`. The repeater's text console has
no such limit, but one format everywhere beats two. The budget never gives up the
base version or the dirty `+`; it takes characters off the SHA, and warns rather
than truncating quietly if it still cannot fit.

## `xiao_uf2_flash.sh` — flash one named board, and prove it

```sh
tools/xiao_uf2_flash.sh .pio/build/Xiao_x1_rpt/firmware.uf2 3     # fleet number
tools/xiao_uf2_flash.sh fw.uf2 67901109B61E604A                   # or the serial
```

Get the board into UF2 mode with **`dfu`** on its console — deterministic, no
buttons — or by double-tapping RESET. A companion build has no console and
therefore no `dfu`, so those boards can only be tapped by hand; give yourself
room with `XIAO_UF2_WAIT=300` (default 90 s, which is shorter than the round
trip of asking someone to press a button).

It refuses to do the things that have actually gone wrong here:

- **flash an image older than the build that made it.** `pio run` refreshes
  `firmware.zip`, `pio run -t create_uf2` refreshes `firmware.uf2`, and neither
  touches the other. Flashing one board from each silently gives them different
  firmware — one board here ended up two commits behind the fleet, missing a
  command the others had, with both flashes reporting success.
- **guess the target.** Every UF2 drive is labelled `XIAO-SENSE`, numbered in
  plug order, so the label proves nothing; drives are resolved through sysfs by
  USB serial, and with several in UF2 mode it stops and asks.
- **accept a non-UF2 file.** A `.hex` or `.zip` dropped on the drive is ignored
  by the bootloader and looks exactly like a successful flash.
- **claim success it did not verify.** It reads the version back off the board
  and compares it against the image. An earlier version only checked that *some*
  XIAO reappeared on USB — and duly reported x4's port after flashing x3. The
  read-back speaks both dialects: `ver` on the console, and `CMD_DEVICE_QUERY`
  on a `companion_radio` build, which has no text console at all. Before that it
  called a perfectly good companion flash "unverified" and exited 2.

Mount and version read-back both retry: the drive appears a second before the
automounter gets to it, and the port enumerates before the firmware answers on
it. A check that cries wolf is worse than no check, because you learn to ignore
it.

## `fleet_test.py` — regression smoke test for the whole bench

```sh
tools/fleet_test.py                        # every XIAO on USB
tools/fleet_test.py --boards 0,1,2,4 --rounds 3 --json run.json
```

Run it after any firmware change that touches the radio, the dispatcher or the
console. Exit code is non-zero if anything failed.

The design rule behind it: **a node's own counters cannot certify that node.**
`sent` only means the firmware believes it transmitted, which is exactly what a
dead DIO1 line breaks — x0 reported `sent 0` for a week while another board
heard every frame it sent. So every RF claim is made by a *different* board than
the one under test, and the result is an N×N matrix.

Checks, in order:

1. **identity** — version and build date per board. Hard-fails an unstamped
   image (`v1.16.0` with no `-tth<sha>`, the literal that is identical in every
   build ever made) and one built from a dirty tree (`+`). Several *different*
   builds on the bench is only a note: this fleet legitimately runs repeater,
   bot and companion side by side, so asserting one version would cry wolf every
   run and train you to skip the summary.
2. **config** — freq/bw/sf/cr must agree, names must be unique. Without this the
   matrix measures nothing: two boards on different spreading factors cannot
   hear each other, which looks exactly like broken hardware.
3. **matrix** — each board sends a **zero-hop** advert in turn and every other
   board must count it. Zero-hop is the point: nobody rebroadcasts it, so "B
   heard A" cannot be satisfied by C relaying A — and the test never has to
   touch `repeat` prefs. RSSI/SNR at each receiver is printed, which is how an
   unplugged antenna shows up.
4. **hygiene** — `tx_timeout`, `tx_start_fail` and `recv_errors` must not move
   during the run, on any board.
5. **commands** — console builds only: the surface answers, hard-failing on the
   few every repeater build must have. `dfu` is deliberately never probed.

Nothing it does writes prefs, changes radio settings or reboots anything.

Speaks **both** bench dialects and detects which is which: the text console
(repeater/analyzer) and the binary companion protocol (`companion_radio` has no
text console at all). The fleet-number table is parsed out of
`xiao_uf2_flash.sh` rather than copied, because two copies would drift and
mixing up which board is which is the most expensive mistake on this bench.

**Do not leave a BLE client connected to a board under test.** The USB and BLE
consoles share one `command[160]` buffer, so two live consoles interleave
characters into a single line and the node executes the result. An A/B run was
invalidated exactly that way.

## `provision/` — command files with expected answers

`repeater-cz-silent.txt` configures a repeater for the live CZ preset and
leaves it silent; `verify-cz-silent.txt` reads back every value that matters and
documents what each answer must be.

```sh
ble_cli.py <MAC> -f provision/repeater-cz-silent.txt
ble_cli.py <MAC> -f provision/verify-cz-silent.txt
```

Two things worth knowing before you trust the output:

**Prefs in InternalFS override build-time defaults.** `ADVERT_NAME`, `LORA_FREQ`
and friends seed a *fresh* install only. Flashing new firmware onto an
already-configured node does not rename or retune it. The tell is a
contradiction: BLE advertises the new name (that comes from the build, via
`BLE_DEVICE_NAME`) while `get name` returns the old one (that comes from prefs).
The fix is `erase` → reboot → re-pair → re-provision, which also wipes the BLE
bond and the node's identity key — fine on a bench node, not something to do to
a node already known to the mesh.

**`ENABLE_ADVERT_ON_BOOT=0` does not make a node silent.** It suppresses the
advert at boot; the periodic timer keeps running with its default interval, so
an `erase`d node is back on the air within about two minutes. Silence has to be
a property of the firmware:

```
-D ADVERT_INTERVAL_DEFAULT=0
-D FLOOD_ADVERT_INTERVAL_DEFAULT=0
-D DISABLE_FWD_DEFAULT=1
```

This is not hypothetical. A node built without those defaults transmitted for
~44 s on the live 869.432 from the bench, and showed up on the public map.

`stats-radio`'s `tx_air_secs` staying at 0 is the only direct evidence that a
node is actually silent. Check it twice, minutes apart, before shipping one.
