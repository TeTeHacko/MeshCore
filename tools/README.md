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
| `power_ab.py` | A/B current measurement of a bench node through the UC96 meter's exporter |
| `mesh_sniffer.py` | passive capture of every frame on the air, decoded on the host to JSONL/pcap |
| `openhop_observer_config.py` | config for an openHop observer (`mode: no_tx`) over a modem board |

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

**A repeater build on a T1000-E has no BLE at all** — `t1000e_repeater` answers
`Unknown command` to `ble`, so there is no buttonless DFU service to write to and
BLE OTA is not an option. Dock-quiet does not apply either (it lives in
`SerialDualInterface`, which a repeater does not use). The way in is the USB
console: `dfu uf2` puts it in the UF2 bootloader in about ten seconds, the port
renames itself from `…T1000-E-BOOT…` to `…T1000-E…`, and a `T1000-E` disk shows
up. The `OSError: [Errno 5]` from pyserial as the command lands is the board
rebooting, not a failure.

Do **not** reach for the magnetic-cable double-tap from `docs/faq.md` here: it
was tried on 2026-08-22 and does nothing on this build. Note also that anything
holding the port open (e.g. the DTR trick above) makes the console look dead —
kill it first, or `ver` returns nothing at all and it looks like a wedged board.

## `ble_cli.py` — console over BLE

```sh
ble_cli.py <MAC> ver                              # one command
ble_cli.py <MAC> "get radio" "get name"           # several, one connection
ble_cli.py <MAC> -f provision/repeater-cz-silent.txt
ble_cli.py <MAC> --per-command "gps on" "set af 9"   # reconnect each time
ble_cli.py <MAC> -w 8 rxlog                       # longer wait for paged output
```

Prints `command<TAB>reply` per line, so output is greppable.

**`{epoch}` expands to the host's unix time**, at the moment the command is sent
rather than when arguments are parsed — `--per-command` reconnects cost seconds
each and a whole `-f` file can take minutes. It exists so a provisioning *file*
can carry `time {epoch}`: `-f` reads lines verbatim, so a `$(date +%s)` in one
goes out as literal text and the node answers `Unknown command`. That is why the
clock line in `provision/repeater-cz-silent.txt` used to be a commented-out note
telling you to substitute the number by hand. The printed command shows the
expanded value, so the log says what was really sent.

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

**Connect once is not enough — retry it.** Phase 1 used to scan, connect, and
give up on the first failure. On 2026-08-22 that cost an hour on hrebecna: three
DFU runs in a row died on `TimeoutError` out of
`local_disconnect_monitor_event.wait()`, while `ble_cli.py ver` against the *same
node from the same host* answered immediately. The node, the bond and the
distance (−64 dBm) were all fine — `ble_cli.py` simply wraps its connect in a
retry loop and `ble_dfu.py` did not. A dead-looking board is the symptom.
`connect_app()` now retries five times, 4 s apart, like the console does.

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
| T1000-E | buttonless over BLE | `AdaDFU`, same MAC, MTU 23 | — |
| T1000-E, repeater build | **`dfu uf2` on the USB console** | — | **yes**, `T1000-E`, in ~10 s |
| Wio Tracker L1 | 1200-baud touch on USB | UF2 mass storage `TRACKER L1` | **yes**, in ~10 s |

The first row is the one that wastes an afternoon: the 1200-baud touch *works*,
the board *is* in the bootloader, and no UF2 drive ever appears — because the
touch requests Adafruit's serial-DFU mode specifically. Flash it with
`adafruit-nrfutil` instead of waiting for a disk:

```sh
adafruit-nrfutil dfu serial -pkg firmware.zip \
    -p /dev/serial/by-id/<...matched on serial...> -b 115200 --singlebank
```

**In practice you don't have to run that by hand.** `variants/xiao_nrf52` sets
`upload_protocol = nrfutil`, so PlatformIO does the touch and the serial DFU for
you:

```sh
pio run -e Xiao_x2_cmp -t upload \
    --upload-port /dev/serial/by-id/usb-Seeed_Studio_XIAO_nRF52840_<serial>-if00
```

**This is THE path for companion builds.** They have no text console, so they
have no `dfu` command, and `xiao_uf2_flash.sh` can only wait for a disk somebody
has to produce by hand. On 10. 8. 2026 the double-tap on x2 refused to take
(board healthy — a replug brought it back as the application, `idProduct=8044`,
no drive), and the upload above landed it in **36 seconds on the first try**.
The lesson is not "the touch is unreliable": serial DFU was deaf on the
**T1000-E**, and that single case got generalised to the whole nRF52 fleet.

Still true, and the reason not to just retry: **one touch per power session.**
A second one in the same session takes the board off USB *and* BLE with no
self-recovery. If the upload fails, ask for a replug.

The last row is the opposite case: the L1's bootloader is not Adafruit's, and
there the touch *does* produce a drive. Canonically `MeshCore-solo/flash-l1.sh`.
Two caveats — it verifies only that the port came back, not what version is on
the board, and the port comes back **before the firmware answers**: the first
`meshcli -s … infos` fails with `Are you sure your node is a serial companion ?`
and passes on its own a couple of minutes later. Don't diagnose that, wait.

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

**It also stamps `FIRMWARE_BUILD_EPOCH`** — the same instant as an integer, used
by `VolatileRTCClock` as its starting `base_time` instead of upstream's hardcoded
15 May 2024 (see the define in `src/MeshCore.h`). This is the only thing that
gives a node a sane clock with **no GPS, no I2C RTC, nothing carrying a correct
time on air and no host attached**: a freshly flashed board is right to within
"time since compile", which on the bench is seconds. Costs no extra rebuilds —
`FIRMWARE_BUILD_DATE` already forces a full one every time.

It is `str(epoch)` and **not** `StringifyMacro`, because the compiler has to see
the integer `1786098575`, not the string `"1786098575"`.

Verifying an integer define is not `strings` on the ELF — that only works for
string literals. Dump `.text` and look for the word:

```sh
arm-none-eabi-objdump -s -j .text firmware.elf   # find the epoch as a LE word,
                                                 # and check 1715770351 is GONE
```

The `#ifndef` fallback in `src/MeshCore.h` is not optional: this script reaches
7 of the 16 envs in `platformio.local.ini` and is nRF52-only, so upstream envs
never define it and must keep compiling.

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

**It also sets the clock**, on the same console trip that reads the version back,
and prints what the board says afterwards:

```
== OK: 5ECC11205C68623B bezi v1.16.0-tthbe64653+  (/dev/serial/by-id/usb-...)
== hodiny: 10:34 - 7/8/2026 UTC (epoch 1786098856)
```

This is the one moment a host is provably talking to the board, and every clock
in this firmware is volatile without GPS or an I2C RTC. Setting it used to be a
manual step written down in two places and performed in neither: measured
7. 8. 2026, `tth-x3` had been running **811 days** behind since it was deployed
and every board on this bench was **812 days** behind. A wrong clock is not
cosmetic — the RTC stamps every advert the node transmits and a receiver drops a
stale timestamp as a replay.

Both dialects again: `time <epoch>` on the console, `CMD_SET_DEVICE_TIME` on a
companion build. A mismatched year is a **warning, not a failure** — the script's
contract is "the image landed", and a wrong clock does not mean a bad flash. But
it must not pass silently, which is exactly how x3 went unnoticed.

`(ERR: clock cannot go backwards)` from `time` is **success**: it means the board
was already right, which is now the normal case straight after a flash because
`build_version.py` seeds the clock with the build epoch. The read-back decides,
never the setter's reply.

## Flashing the nodes that live on dopey

Two of them are not on this bench: `tth-x3` hangs off dopey by USB, and `tth-ltm`
is only in dopey's Bluetooth range. Both have a bridge holding them open, so the
sequence is stop bridge → flash → verify → start bridge. `sudo` needs no password
there, and `udisksctl`/`lsblk`/`pyserial` are all present, so
`xiao_uf2_flash.sh` runs unmodified (the drive mounts at `/media/root/XIAO-SENSE`).

```sh
ssh dopey 'sudo systemctl stop meshcore-serial-bridge'   # x3, USB
ssh dopey 'sudo systemctl stop meshcore-ble-bridge'      # tth-ltm, BLE
```

**Copy with `scp -p`.** Without it the files get the time of transfer in the order
they are copied, `firmware.elf` ends up newer than `firmware.uf2`, and the
staleness guard refuses the flash — correctly, for a false reason.

**Stopping the bridge is not enough for BLE.** After the unit had been stopped,
`ble_cli.py` still reported `not advertising (already connected?)` five times over:
the connection was being held by BlueZ itself, because the node is bonded and
trusted. It only started advertising after

```sh
ssh dopey 'bluetoothctl disconnect FA:4F:30:E3:1B:7A'
```

**And check you are even in range.** `bluetoothctl devices` lists `tth-ltm` on this
workstation too, from cache — but a scan never finds it. The tell is a missing
RSSI. Cached name is not reachability; that node can only be flashed from dopey.

**DFU duration tracks the MTU, not the image size.** tth-ltm took ~3 minutes for
407 kB (~244 B chunks); the T1000-E took ~14 minutes for 372 kB, because its
bootloader stays at MTU 23 and falls back to 20 B chunks. A DFU that looks slow is
usually just a small MTU.

Restarting the bridge afterwards needs no cursor surgery — it notices the reboot
and replays the ring from zero:

```
WARNING rxlog: cursor 137936 > newest 38 — uzel rebootoval (seq reset), jedu od začátku ringu
```

## When a board vanishes from USB and only a replug helps

Measured on this bench 4. 8. 2026, after calling it "the board fell off USB"
three times in a row, which is a description and not a diagnosis.

**What the kernel log looks like.** A good bootloader entry and a bad one differ
only in what does *not* happen:

```
good:  usb 7-9: USB disconnect  ->  3 s later  idProduct=0045 (bootloader)
bad:   usb 7-9: USB disconnect  ->  nothing, ever
```

No `error -71`, no "unable to enumerate", nothing. The port then reports
`state=not attached` in
`/sys/bus/usb/devices/usb7/7-0:1.0/usb7-portN/`.

**Do not read that as a dead board.** The identical signature was reproduced on
a *known good* board by writing `1` then `0` to that port's `disable` from the
host — the board was healthy, doing nothing unusual, and went to `not attached`
and stayed there. So the log proves the host stopped seeing a device; it says
nothing about why. A missing error message is not evidence.

**There is no software recovery on this machine.** The port `disable` toggle is
the only candidate and it makes things worse: it takes healthy boards off the
bus with no way back. Do not use it. The boards hang off the xHCI *root hub*,
whose ports do not do real power switching, so `uhubctl` has nothing to work
with either.

**What does work, every time (5/5):** double-tapping RESET. A reset through the
pin has never failed; software resets fail in both directions —
`dfu` (app to bootloader) failed 5 of 10 before the `USBPULLUP` change, and the
bootloader's own reset back into the application has failed several times, which
is a path no firmware change of ours can reach.

**Operational rules that follow:**

- Flash one board at a time and check the result before starting the next. A
  batch of three cost three replugs in one go.
- Do not loop flashes to "make sure". Every reset is another chance to lose the
  board.
- If a board does vanish, it wants hands. Say so immediately instead of
  retrying — a second `dfu` cannot reach a board the host cannot see.
- **The structural fix is a powered hub with per-port power switching**
  (uhubctl-compatible). Then "replug" becomes a command and the bench is
  recoverable without being in the room. On the root hub it is not.

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
6. **clock** — console builds only: the year must be current. A board nobody set
   the clock on is a configuration fault rather than a hardware one, which is
   precisely why it needs reporting: it went unnoticed on x3 for 811 days, and on
   7. 8. 2026 it was true of *every* board on this bench. The check only reads.

Nothing it does writes prefs, changes radio settings or reboots anything.

**It DOES transmit, and importing it used to be enough.** The matrix and the
`advert.zerohop` probe key the radio on every console board found on USB,
whatever band each is tuned to. The file ended in a bare `sys.exit(main())` with
no `if __name__ == "__main__"` guard, so `import fleet_test` to reuse
`discover()` ran the whole suite before the importing script's first line — which
on 7. 8. 2026 put two zero-hop adverts on **869.432, the live CZ mesh**, out of a
bench board configured for it. The guard is now there. If you reuse anything from
this module, that guard is what makes it safe.

Speaks **both** bench dialects and detects which is which: the text console
(repeater/analyzer) and the binary companion protocol (`companion_radio` has no
text console at all). The fleet-number table is parsed out of
`xiao_uf2_flash.sh` rather than copied, because two copies would drift and
mixing up which board is which is the most expensive mistake on this bench.

**Do not leave a BLE client connected to a board under test.** The USB and BLE
consoles share one `command[160]` buffer, so two live consoles interleave
characters into a single line and the node executes the result. An A/B run was
invalidated exactly that way.

## `power_ab.py` — what a setting actually costs, in mA

```sh
tools/power_ab.py --serial 85D35458CC870126 --minutes 15 \
    --cell "baseline=" --cell "ps=powersaving on" --cell "duty=gps duty"
```

Applies each cell's commands, lets the node settle, measures, and prints the
delta against the first cell. Written for the power-saving work on the solar
repeaters, where the whole question is whether a setting is worth its risk.

**It reads the exporter, not the meter.** `uc96-exporter.service` owns the BLE
link to the UC96; a second central connecting to the meter steals that link and
the service reconnect-loops. So the script polls
`http://localhost:9877/metrics`. (The same data reaches Mimir through alloy, so
a finished run stays inspectable — but the exporter is live, which is tighter.)
Which meters that host owns is Ansible-managed in
`chytra-budka/power-meter/inventory.yml` (`uc96_meter_allowlist`) — not in the
`uc96-exporter` repo, which is only a mirror. Three meters were added to
black-arch on 2026-08-05 for bench work; `--meter <mac-without-colons>` picks one.

**A solar node measured on its USB side measures the CHARGER.** Put a SenseCap
Solar on the meter and it draws 500-700 mA with the bus sagging to 4.7 V: that is
the cell charging, and the 5-25 mA the node itself uses is 2-4 % of it, riding on
a current that drifts as the cell fills. The first run against `tth-s1-rpt` was
invalid for exactly this reason. Options, in order of preference: wait for the
charge to taper (watch `meter_current_amps` fall — it went 0.71 → 0.53 → 0.22 A
over ~40 min), pull the cell, or measure the transferable part on a XIAO (a
P1-Pro has a XIAO nRF52840 inside, so MCU + SX1262 deltas carry over 1:1; only
GPS is SenseCap-specific).

**And there is no INA226 to cross-check against.** The `i2c` command on
`tth-s1-rpt` reports nothing at all on the bus, and the variant has a single I2C
interface (`WIRE_INTERFACES_COUNT (1)`), so that is the whole story. The
`TELEM_INA226_ADDRESS=0x40` build flag is only a guard against false-detecting
SHT41 at the library default 0x44 — and that SHT41 is an external Grove sensor on
`tth-ltm`, not something the board carries. Cross-verification has to come from a
second board (`s2` is the paired control), not a second sensor.

**Never read `meter_current_amps` for this.** The UC96 quantises current to
10 mA: the same 0.02 covers a node drawing 15 mA and one drawing 24. The savings
being measured here are 1-4 mA, i.e. entirely inside one count. What the script
uses instead is the accumulating `meter_capacity_ah` counter (1 mAh steps),
sampled through the window and fitted with least squares — the *timing* of each
step carries far more than the count at the ends. Measured on the bench: a 5 min
window gives ±12 mA from the endpoints and ~1 mA from the fit.

**Alternate cells when the rig drifts.** A charging cell tapering underneath made
the `gps on` cells read 107 → 92 → 88 → 84 mA over an hour, which is 23 mA of
spread across repeats of the *same* cell — enough to bury the 46 mA effect being
measured if that spread is taken for noise. So run `A B A B A B` and let the tool
compare each B against the mean of its two neighbouring A cells; a linear trend
cancels exactly. The summary prints that as `PAROVY ODHAD` and labels a
monotone spread `drift, NE sum` instead of using it as a noise floor. Measured
this way, GPS on a SenseCap Solar costs **46.0 mA** (spread 43.7-47.5 over three
pairs) — where a single before/after pair would have said anything from 55 to 68.

`--reanalyse run.json` re-runs the analysis on a finished run, no hardware needed.

**A cell whose setting did not apply is refused, not measured.** The console can
drop mid-run (Errno 5); it is re-opened and retried, and the setting is then
verified on the node (`gps` must answer `duty`, `powersaving` must answer `on`).
This exists because it went wrong: `gps off` was lost on the wire, the run
carried on, and two cells silently measured `gps on` again — producing a
plausible "12 mA saving" that was pure charger taper.

Every cell prints the residual spread and the airtime counters. Residuals over
about half a step mean the load moved during the window (traffic burst, charging,
a hand near the antenna) — the mean is then hiding something and the cell should
be repeated rather than believed. Airtime is context, not the claim: a cell that
relayed a flood burns more than an idle one, and without that column it looks
like a regression.

## `gen_node_id.py` — an identity whose path hash collides with nobody

```sh
tools/gen_node_id.py --out new.key                     # 2-byte prefix free
tools/gen_node_id.py --first-byte 5f --out new.key     # ...and a chosen first byte
tools/gen_node_id.py --known https://analyzer.meshcore.cz/api/analytics/hash-sizes
tools/gen_node_id.py --derive <128 hex>                # pubkey out of an existing prv.key
```

A node's path hash is nothing but a prefix of its public key (`Identity.h:20`), and
**the width is chosen by whoever originated the packet, not by the forwarder** —
`Mesh.cpp:346` appends `packet->getPathHashSize()` bytes, so a repeater configured
for two bytes still writes one byte into a one-byte packet. The set of nodes a hop
could mean is therefore *every repeater on the mesh*, never just the ones sharing
your setting. Measured against the community analyzer (711 repeaters, 5 Aug 2026):

| width | repeaters sharing a prefix |
|---|---|
| 1 byte | **671 of 711** (213 colliding prefixes) |
| 2 bytes | 6 |
| 3 bytes | 0 |

One-byte uniqueness is unobtainable by pigeonhole — 711 repeaters, 256 values — and
of the three unused values `00` and `ff` are not free but **forbidden**:
`Identity.cpp:56` rejects any key whose public part starts with either. That left
exactly one unclaimed first byte on the whole CZ mesh, `5f`, and
`tth-plesivec-abertamy` now holds it. Everyone else should aim for a free *two*-byte
prefix, which is nearly free to find: 934 of 65536 taken.

Grinding costs nothing (~54k keys/s), so there is no reason to accept a random
prefix on a node that has not yet transmitted — nothing references its old key.

**The private key is not `seed || pubkey`.** MeshCore stores the 64 bytes that
`lib/ed25519/keypair.c` produces: a **clamped SHA-512 of the seed**, whose second
half is the hash's other half and *not* the public key. Reading `get prv.key` and
taking the last 32 bytes yields a plausible-looking value that is not the node's
identity — a mistake worth avoiding, hence `--derive`, which does the real
`ge_scalarmult_base(prv[0:32])`. Feeding the wrong format in is at least loud:
`set prv.key` answers `Error, bad key` because `validatePrivateKey()` re-derives the
public key and compares.

Loading it: `set prv.key <hex>` on a repeater console, `set private_key <hex>` via
`meshcore-cli` on a companion; both answer with the new public key and need a
reboot. **Do not pass the key through a tool that echoes the command** — the serial
console echoes what it receives, so the key lands in the scrollback and in any
transcript. Write it straight to the port and filter the echo out.

Related: the community's 1-byte-to-2-byte campaign
(<https://meshcore.cz/repeatery:twobytesonecup>) is `set path.hash.mode 1`, where the
mode is **width − 1** (0=1 B, 1=2 B, 2=3 B) and firmware ≤v1.13 drops 2-byte packets.
TRACE packets encode the width differently — `flags & 3` giving `1 << n`, so 1/2/4/8
bytes and no such thing as three (`Mesh.cpp:54`). Do not carry one rule over to the
other.

## `mesh_sniffer.py` — every frame on the air, decoded on the host

```sh
python3 -m venv .venv && .venv/bin/pip install openhop-core pyserial
.venv/bin/python tools/mesh_sniffer.py --board 0 --preset cz --jsonl cz.jsonl --pcap cz.pcap
.venv/bin/python tools/mesh_sniffer.py --board 0 --preset bench --psk <hex>   # + channel text
```

The board runs the **openHop modem** firmware (`openhop-dev/openhop_modem`), which turns
the SX1262 into a dumb PHY on USB-CDC at 921600: it hands up every frame that passes
CRC with RSSI/SNR attached, and MeshCore is decoded here in Python by
[`openhop-core`](https://github.com/openhop-dev/openhop_core), a reimplementation of
our own C++ stack (MIT). Both projects understand the multi-byte path hash, so a
2-byte or 3-byte sender decodes without a rebuild.

Why this exists next to the observer + BLE bridge, which already feed the analyzer:

* **A companion node only surfaces what its own firmware chose to process** — its
  channels, its DMs, adverts, packets addressed to it. Foreign-PSK channel traffic and
  DIRECT packets for other destinations are dropped long before the companion protocol
  sees them. The modem has no opinion, so `docs`' open question ("the SDR sees twice
  what the observer decodes") becomes measurable: the status line prints the modem's own
  `rx_count` next to the number of frames that actually parsed.
* **No BLE on the path** — no dock-quiet interaction, no BlueZ holding the link, no
  `MC_BLE_OFF_INTERVAL` windows costing 42 % of the capture.

**Receive-only by construction.** No `radio.send()` anywhere in the file, no dispatcher
and no handler registered, so nothing can answer, ACK or forward; `tx_power` is left at
its floor as a second line of defence. That is what makes it safe to point at 869.432
without announcing a node.

### Getting a modem board (measured on x0, 17. 8. 2026)

`firmware/xiao_nrf52_wio/firmware.{uf2,zip,hex}` is prebuilt for our exact board (XIAO
nRF52840 + Wio-SX1262, SKU 102010710) — but it is **fully DIO1-interrupt driven**
(`setDio1Action`, every RX/TX/CAD completion waits on the ISR flag), so on **x0** it is
deaf and mute: that board's DIO1 net died in the 29. 7. breadboard short.
`tools/openhop-modem-pollirq.patch` adds `[env:xiao_nrf52_wio_pollirq]` and reads the
SX1262 IRQ register over SPI instead — the same repair as `LORA_POLL_IRQ` in our tree,
for the same reason (it does not care which end of the line is broken).

```sh
git clone https://github.com/openhop-dev/openhop_modem.git && cd openhop_modem
git apply /path/to/MeshCore/tools/openhop-modem-pollirq.patch
cd firmware && pio run -e xiao_nrf52_wio_pollirq \
    -t upload --upload-port /dev/serial/by-id/usb-Seeed_Studio_XIAO_nRF52840_30911219DA28411D-if00
```

11.5 s, one touch, no buttons — the board must be in the APPLICATION for this, as always.
Stock (healthy) boards want `-e xiao_nrf52_wio` and no patch.

Three things the flash changes, all of which cost time if unexpected:

* The port is **renamed** to `usb-Seeed_XIAO-Wio-SX1262_<sn>`, so anything matching on
  the by-id *name* breaks. `mesh_sniffer.py` matches on the serial number (Rule 0).
* **`idProduct` becomes `0x0044`** — the value AGENTS.md reads as "bootloader". The
  board JSON claims both `0x0044` and `0x8044`, and this application enumerates as the
  first, so the mode heuristic simply does not apply to a modem board.
* The first `begin()` right after DFU failed with `Could not configure port: (5, 'Input/
  output error')`; the same call a minute later worked. Give the CDC a moment to settle
  before diagnosing anything.

The board stops being a MeshCore node while it carries this firmware — no identity, no
prefs, no console. Reflash `Xiao_x0_rpt` to get x0 back (its key before the experiment:
`6905…`, name `tth-x0`, 866.5 MHz).

### What it proved

x4 sending three zero-hop adverts on the bench band: **3/3 decoded** at −40 dBm / 13.5 dB,
`hash_size=2`, `path_hash=c327` — the first two bytes of x4's public key, i.e. our
2-byte default read straight off the air.

Then seven minutes on 869.432, 17. 8. 2026, from the bench antenna:

| | |
|---|---|
| frames | **69 heard, 69 parsed, 0 CRC errors** |
| types | GRP_TXT 24, REQ 14, ADVERT 12, PATH 10, RESPONSE 8, ANON_REQ 1 |
| path hash width | 53 × 2 bytes, 16 × 1 byte — both on the same air, same run |
| hops | 2 … **32** |
| distinct nodes named in adverts | 7 (`mraveniste.meshcore.cz`, `CESKAKANADA.SOLAR`, `OK2ZAW MeshGate Zdar`, …) |

The interesting split is by signal: **35 of the 69 arrived at −52…−53 dBm and every one
of them has `tth-ltm` as its last hop** — 26 as `025c`, 9 as `02`, the same node under
both hash widths, because the width is the *sender's* choice and not the forwarder's.
The other 34 came in at −111…−115 dBm straight from distant repeaters, never through
`tth-ltm` at all. So this is a **host-side witness of tth-ltm's forwarding** — its own
counters cannot certify it, and here is a second board that sees each relay with its
path and RSSI, without touching the node. Several floods also show up twice seconds
apart with different tails, which is exactly the observation a dedupe window eats.

Traps encoded in the script:

* **openhop_core's own `Dispatcher` is the wrong tool for this.** Its `PacketFilter`
  drops byte-identical frames for 30 s, which is right for a repeater and wrong for a
  sniffer. The script parses packets itself and *counts* repeats instead.
* **`set_rx_callback()` loses the signal metadata.** The callback is handed only the
  payload; RSSI/SNR live in `radio.last_rssi`/`last_snr`, which the next frame overwrites
  before a queued callback runs. `SnifferRadio` takes them out of the `CMD_RX_PACKET`
  frame, where they are atomic with the bytes.
* Presets are mirrored from `platformio.ini` / `.local.ini` (`cz` = 869.432/62.5/SF7/CR5,
  `bench` = 866.5/62.5/SF8/CR5): two radios on different spreading factors cannot hear
  each other, and that looks exactly like broken hardware.

## `provision/` — command files with expected answers

`repeater-cz-silent.txt` configures a repeater for the live CZ preset and
leaves it silent; `verify-cz-silent.txt` reads back every value that matters and
documents what each answer must be. `activate-cz-mast.txt` is the other end of
the trip: it turns that silent node into a repeater once the antenna is bolted
on, and reads the three values back in the same session.

```sh
ble_cli.py <MAC> -f provision/repeater-cz-silent.txt
ble_cli.py <MAC> -f provision/verify-cz-silent.txt
ble_cli.py <MAC> -f provision/activate-cz-mast.txt      # antenna connected!
```

An option *between* the MAC and the commands — `<MAC> -w 8 rxlog`, the form the
examples above use — used to exit with `unrecognized arguments: rxlog` on
Python ≤ 3.12, while working on a 3.13+ workstation. That is the wrong way
round: the Debian hosts that run this tool for a mast node (dopey, sneezy) are
the 3.11 ones. `ble_cli.py` now parses with `parse_intermixed_args()`, so every
argument order works everywhere; the point of the note is that this class of bug
does not reproduce locally.

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

## `openhop_observer_config.py` — config pro openHop observer

```sh
tools/openhop_observer_config.py --name tth-ob1 --iata ULK \
    --key out/identities/tth-ob1.key --serial 67901109B61E604A \
    --storage /var/lib/openhop-ob1 --out out/openhop/ob1/config.yaml
```

Vygeneruje `config.yaml` (0600) pro `openhop_repeater` v roli **pasivního
observeru**: `mode: no_tx`, adverty 0, discovery off, LBT off, tx_power 2 dBm,
CZ preset, `radio_type: pymc_usb` na desce s firmwarem openhop_modem, lokální
CoreScope zapnutý a komunitní brokery `CZ 1`/`CZ 2` zapsané s `enabled: false`.

Existuje jako skript, protože **démon si config.yaml sám přepisuje** (uloží do
něj vygenerovaný JWT secret a znormalizuje celý YAML) — komentáře v tom souboru
nepřežijí ani první start, takže zdrojem pravdy musí být generátor.

Identitu si vezme z 64bajtového `prv.key` v hexu (výstup `gen_node_id.py`) a
vloží ji jako `repeater.identity_key`, tedy tu samou cestu, kterou používá
`convert_firmware_key.sh` z openhopu. Pozor na rozdíl: `identity_file`, který si
démon generuje sám, je base64 **32** bajtů, kdežto MeshCore klíč má 64
(scalar‖nonce) — proto `identity_key`, ne `identity_file`.

Nasazení, pasti a co se ztratí přeflashováním desky na modem:
`INFRA/openhop-observer/README.md`.
