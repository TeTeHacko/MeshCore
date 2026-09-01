#!/usr/bin/env bash
# Flash any board of the fleet -- pick the route from what the board is doing,
# and prove where the image landed.
#
#   tools/flash_node.sh <env> [target] [--force-protected]
#
#   target   USB serial (full or unique suffix), or a XIAO fleet number 0..4.
#            Omit only when exactly one board of the fleet is plugged in.
#
# WHY A DISPATCHER. Which route works depends on the board AND on what it is
# doing right now. The rules are written out in .claude/skills/flash-node
# and AGENTS.md -- prose, i.e. the kind of thing that gets skipped mid-task. On
# 2026-09-01 three of them were skipped in one session: `pio -t upload` went to a
# board already in DFU (it touches unconditionally, kicking it back out), a
# hand-rolled nrfutil went to a board in the application (it does NOT touch, so
# nothing was listening), and a failed upload was retried instead of asking for a
# replug -- which is how an image ended up on a board nobody named. The rules
# therefore live here, where the board's state is actually known:
#
#   board    state              route
#   ------   ----------------   ------------------------------------------------
#   xiao     application        pio -t upload (env has upload_protocol=nrfutil,
#                               so PlatformIO touches; ~35 s, no buttons)
#   xiao     bootloader + disk  hand off to xiao_uf2_flash.sh
#   xiao     bootloader, no     hand-rolled nrfutil (does not touch; the board is
#            disk (serial-only) already where it needs to be)
#   t1000e   application with   `dfu uf2` on the console -> wait for the port to
#            a text console     rename -> hand-rolled nrfutil on the CDC port
#   t1000e   bootloader         hand-rolled nrfutil straight away
#   wio-l1   application        1200-baud touch -> UF2 drive "TRACKER L1"
#   sensecap anywhere           not over USB: BLE OTA, tools/ble_dfu.py
#
# CDC is preferred over the UF2 drive wherever both work: mounting the drive
# needs root, and `sudo` over SSH without a terminal has nobody to ask for a
# password. Writing to a CDC port works as the ordinary user.
#
# Whatever the route: the whole bus is fingerprinted before and after, the target
# must come back with the new version READ OFF THE BOARD, and every other board
# must be untouched. "Device programmed" is not evidence -- both flashes that went
# wrong that day printed it.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
die() { echo "CHYBA: $*" >&2; exit 1; }

# Desky, na ktere se podle AGENTS.md nesaha bez vyslovneho zadani. Drzi identitu,
# historii nebo produkcni roli, a prepsat je omylem je draha chyba.
PROTECTED="B612AE3898A81CCA"   # domaci T1000-E: identita, historie wedge, RemoteTerm

ENV_NAME=""; TARGET=""; FORCE=0
for arg in "$@"; do
  case "$arg" in
    --force-protected) FORCE=1 ;;
    *) if [ -z "$ENV_NAME" ]; then ENV_NAME="$arg"; else TARGET="$arg"; fi ;;
  esac
done
[ -n "$ENV_NAME" ] || { echo "usage: $(basename "$0") <env> [serial|0..4] [--force-protected]" >&2; exit 1; }

case "$TARGET" in
  [0-4]) TARGET="$(sed -n "s/^\s*${TARGET})\s*TARGET=\([0-9A-F]\{16\}\)\s*;;/\1/p" \
                   "$HERE/xiao_uf2_flash.sh" | head -1)"
         [ -n "$TARGET" ] || die "cislo desky nenalezeno v xiao_uf2_flash.sh" ;;
esac

PIO="$HOME/.platformio/penv/bin/pio"
[ -x "$PIO" ] || PIO="$(command -v pio)" || die "pio nenalezeno"
NRF="$HOME/.platformio/packages/tool-adafruit-nrfutil"

cd "$ROOT"

# Spinavy strom da verzi s '+', a na uzlu pak nepoznas, co na nem je (AGENTS.md).
if [ -n "$(git status --porcelain 2>/dev/null)" ]; then
  echo "VAROVANI: strom neni cisty -- obraz dostane verzi s '+'." >&2
fi

echo "== env: $ENV_NAME${TARGET:+  cil: $TARGET}"
echo "== otisk desek PRED flashem"
BEFORE="$("$HERE/board_probe.py" --all)" || die "otisk se nezdaril"
echo "$BEFORE" | sed 's/^/   /'

if [ -n "$TARGET" ]; then
  hits="$(echo "$BEFORE" | grep -c -- "$TARGET" || true)"
  [ "$hits" = "1" ] || die "cil $TARGET je na sbernici ${hits}x -- upresni seriove cislo"
else
  n="$(echo "$BEFORE" | grep -c . || true)"
  [ "$n" = "1" ] || die "na sbernici je $n desek -- rekni ktera: $(basename "$0") $ENV_NAME <serial|0..4>"
  TARGET="$(echo "$BEFORE" | awk '{print $1}')"
fi

case "$PROTECTED" in
  *"$TARGET"*)
    [ "$FORCE" = "1" ] || die "$TARGET je CHRANENA deska (AGENTS.md: bez vyslovneho
       zadani na ni nesahat). Kdyz to fakt chces: --force-protected" ;;
esac

FULL="$(echo "$BEFORE" | awk -v t="$TARGET" '$1 ~ t {print $2}')"
BOARD="${FULL%%/*}"; STATE="${FULL##*/}"
PORT="/dev/serial/by-id/$(ls /dev/serial/by-id/ | grep -- "$TARGET" | head -1)"
echo "== cil: $BOARD, stav: $STATE"

has_uf2_drive() {
  local d s blk
  for d in /sys/bus/usb/devices/*/; do
    s="$(cat "$d/serial" 2>/dev/null)" || continue
    case "$s" in *"$TARGET"*) ;; *) continue ;; esac
    blk="$(ls -d "$d"*/host*/target*/*/block/* 2>/dev/null | head -1)" || true
    [ -n "$blk" ] && return 0
  done
  return 1
}

# nrfutil umi skoncit tracebackem a PRESTO vratit 0 -- videno 1. 9. 2026 na
# "No data received on serial port". Cte se tedy i vystup, ne jen navratovy kod.
run_nrfutil() {
  local port="$1" out
  "$PIO" run -e "$ENV_NAME" >/dev/null 2>&1 || die "build selhal"
  out="$(PYTHONPATH="$NRF/site-packages" "$HOME/.platformio/penv/bin/python" \
        "$NRF/adafruit-nrfutil.py" dfu serial \
        -pkg ".pio/build/$ENV_NAME/firmware.zip" -p "$port" -b 115200 --singlebank 2>&1)" \
    || { echo "$out" | tail -4; return 1; }
  echo "$out" | tail -3
  case "$out" in *NordicSemiException*|*"Not able to proceed"*|*Traceback*) return 1 ;; esac
  return 0
}

# T1000-E: aplikace s textovou konzoli se do bootloaderu posle sama prikazem
# `dfu uf2`. Port se pritom PREJMENUJE z "...T1000-E-BOOT..." na "...T1000-E..."
# (ano, "-BOOT" ma aplikace) a pyserial hodi OSError: [Errno 5] -- to je reboot,
# ne chyba.
t1000e_to_bootloader() {
  echo "== posilam 'dfu uf2' na konzoli, deska se rebootne do bootloaderu"
  python3 - "$PORT" <<'PY' || true
import serial, sys, time
try:
    s = serial.Serial(sys.argv[1], 115200, timeout=0)
    s.dtr = True; s.rts = True
    time.sleep(1.5)
    s.write(b"dfu uf2\r\n"); s.flush()
    time.sleep(2)
    s.close()
except Exception as e:
    print(f"   ({type(e).__name__} pri odesilani -- deska se rebootuje)")
PY
  for _ in $(seq 30); do
    local p
    p="$(ls /dev/serial/by-id/ 2>/dev/null | grep -- "$TARGET" | grep -v -- "-BOOT" | head -1)"
    [ -n "$p" ] && { PORT="/dev/serial/by-id/$p"; echo "   bootloader port: $PORT"; return 0; }
    sleep 1
  done
  return 1
}

RC=0
case "$BOARD/$STATE" in
  xiao/boot)
    if has_uf2_drive; then
      echo "== cesta: UF2 disk -> predavam xiao_uf2_flash.sh"
      "$PIO" run -e "$ENV_NAME" -t create_uf2 >/dev/null 2>&1 || die "create_uf2 selhal"
      exec "$HERE/xiao_uf2_flash.sh" ".pio/build/$ENV_NAME/firmware.uf2" "$TARGET"
    fi
    echo "== cesta: rucni nrfutil (bootloader bez UF2 disku = serial-only rezim)"
    if ! run_nrfutil "$PORT"; then
      # Videno 1. 9. 2026 na x2: bootloader vystavuje CDC (USB tridy 02+0a, zadne
      # mass storage), ale na DFU protokol neodpovida -- nrfutil skonci na "No
      # data received", se `--singlebank` i bez nej. Deska neni mrtva a replug
      # s tim nehne, protoze v bootloaderu zustane; chce to DVOJKLIK na RESET,
      # kterym se bootloader prepne do UF2 rezimu a VYSTAVI DISK. Pak uz tenhle
      # skript sam zvoli druhou cestu (xiao_uf2_flash.sh).
      echo >&2
      echo "Bootloader neodpovida na serial DFU, a UF2 disk nevystavuje." >&2
      echo "DVOJKLIKNI na male tlacitko RESET -- tim se prepne do UF2 rezimu" >&2
      echo "a objevi se disk. Pak spust tenhle skript znovu, uz pujde diskem." >&2
      RC=1
    fi
    ;;
  xiao/*)
    echo "== cesta: pio -t upload (deska je v APLIKACI, touch si udela sam)"
    "$PIO" run -e "$ENV_NAME" -t upload --upload-port "$PORT" || RC=$?
    ;;
  t1000e/boot)
    echo "== cesta: rucni nrfutil (T1000-E uz je v bootloaderu)"
    run_nrfutil "$PORT" || RC=1
    ;;
  t1000e/text)
    t1000e_to_bootloader || die "deska se do 30 s neprepnula do bootloaderu"
    run_nrfutil "$PORT" || RC=1
    ;;
  t1000e/*)
    die "T1000-E ve stavu '$STATE' nema textovou konzoli, kterou by sla poslat do
       bootloaderu. Companion build se flashuje pres BLE OTA (tools/ble_dfu.py),
       nebo desku dostan do bootloaderu rucne (dlouhy stisk do 8 s od startu)."
    ;;
  wio-l1/*)
    echo "== cesta: Wio L1 -> predavam MeshCore-solo/flash-l1.sh (touch + disk TRACKER L1)"
    L1="$HOME/projects/MeshCore-solo/flash-l1.sh"
    [ -x "$L1" ] || die "$L1 nenalezen"
    "$PIO" run -e "$ENV_NAME" -t create_uf2 >/dev/null 2>&1 || die "create_uf2 selhal"
    "$L1" "$ROOT/.pio/build/$ENV_NAME/firmware.uf2" || RC=$?
    ;;
  *)
    die "kombinaci '$BOARD/$STATE' neumim. Deska neodpovida zadnemu protokolu --
       vytahni ji z USB, zapoj zpatky a spust znovu."
    ;;
esac

if [ "$RC" != "0" ]; then
  echo >&2
  echo "FLASH SELHAL. NEOPAKUJ ho -- jeden 1200baudovy touch na power session." >&2
  echo "Vytahni desku z USB, zapoj zpatky, a spust tenhle skript znovu." >&2
  echo >&2
  echo "Kontroluji, jestli to nezapsalo jinam:" >&2
  "$HERE/board_probe.py" --all --compare <(echo "$BEFORE") >&2
  exit 1
fi

echo "== otisk desek PO flashi (necham CDC nabehnout)"
sleep 8
"$HERE/board_probe.py" --all --compare <(echo "$BEFORE") --expect-changed "$TARGET"
