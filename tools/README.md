# Field tools

Scripts for talking to a node that is already deployed — or about to be. They
exist because the same handful of mistakes kept costing real time and, once,
real airtime on the live mesh. Each trap below is encoded in a script so it
cannot be repeated by hand.

| tool | what it does |
|---|---|
| `ble_cli.py` | the text console over BLE (Nordic UART), for nodes with no cable |
| `ble_dfu.py` | firmware update over BLE (legacy Nordic DFU, Adafruit nRF52 bootloader) |
| `provision/` | command files: what to send, and what the answers must look like |

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
