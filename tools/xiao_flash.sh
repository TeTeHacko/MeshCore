#!/usr/bin/env bash
# Flash a XIAO -- pick the right route automatically, and prove where it landed.
#
#   tools/xiao_flash.sh <env> [target]
#
#   target   USB serial (full or unique suffix), or a fleet number 0..4.
#            Omit only when exactly one XIAO is plugged in.
#
# WHY A DISPATCHER. Which route works depends on what the board is doing right
# now, and AGENTS.md spells the rules out in prose -- which is exactly the kind
# of thing that gets skipped mid-task. On 2026-09-01 all three were skipped in
# one session: `pio -t upload` was sent to a board already in DFU (it touches
# unconditionally, so it kicks it back out), a hand-rolled nrfutil was sent to a
# board in the application (it does NOT touch, so there was nothing listening),
# and a failed upload was retried instead of asking for a replug -- which is how
# the image ended up on a board nobody named. So the rules live here now:
#
#   state of the target            route
#   ---------------------------    -------------------------------------------
#   application (text/companion/   `pio run -t upload`. The XIAO env has
#   kiss/openhop_modem)            `upload_protocol = nrfutil`, so PlatformIO
#                                  does the 1200-baud touch itself and streams
#                                  over CDC. ~35 s, no buttons.
#   bootloader WITH a UF2 drive    hand off to xiao_uf2_flash.sh
#   bootloader, CDC but NO drive   hand-rolled nrfutil (it does not touch, and
#   (serial-only, after a touch)   the board is already where it needs to be)
#
# And regardless of route: the whole bus is fingerprinted before and after, the
# target must come back with the new version READ OFF THE BOARD, and every other
# board must be untouched. "Device programmed" is not evidence -- it was printed
# by both flashes that went wrong that day.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
die() { echo "CHYBA: $*" >&2; exit 1; }

ENV_NAME="${1:-}"; TARGET="${2:-}"
[ -n "$ENV_NAME" ] || { echo "usage: $(basename "$0") <env> [serial|0..4]" >&2; exit 1; }

# Fleet shorthand -- parsed out of xiao_uf2_flash.sh so the table lives in ONE
# place. Two copies would drift, and picking the wrong board is the single most
# expensive mistake on this bench.
case "$TARGET" in
  [0-4])
    TARGET="$(sed -n "s/^\s*${TARGET})\s*TARGET=\([0-9A-F]\{16\}\)\s*;;/\1/p" \
              "$HERE/xiao_uf2_flash.sh" | head -1)"
    [ -n "$TARGET" ] || die "cislo desky nenalezeno v xiao_uf2_flash.sh"
    ;;
esac

PIO="$HOME/.platformio/penv/bin/pio"
[ -x "$PIO" ] || PIO="$(command -v pio)" || die "pio nenalezeno"

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
STATE="$(echo "$BEFORE" | awk -v t="$TARGET" '$1 ~ t {print $2}')"
PORT="/dev/serial/by-id/$(ls /dev/serial/by-id/ | grep -- "$TARGET" | head -1)"
echo "== cil $TARGET je ve stavu: $STATE"

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

RC=0
case "$STATE" in
  text|companion|kiss|modem)
    echo "== cesta: pio -t upload (deska je v APLIKACI, touch si udela sam)"
    "$PIO" run -e "$ENV_NAME" -t upload --upload-port "$PORT" || RC=$?
    ;;
  bootloader)
    if has_uf2_drive; then
      echo "== cesta: UF2 disk -> predavam xiao_uf2_flash.sh"
      "$PIO" run -e "$ENV_NAME" -t create_uf2 >/dev/null 2>&1 || die "create_uf2 selhal"
      exec "$HERE/xiao_uf2_flash.sh" ".pio/build/$ENV_NAME/firmware.uf2" "$TARGET"
    fi
    echo "== cesta: rucni nrfutil (bootloader bez UF2 disku = serial-only rezim)"
    "$PIO" run -e "$ENV_NAME" >/dev/null 2>&1 || die "build selhal"
    NRF="$HOME/.platformio/packages/tool-adafruit-nrfutil"
    # adafruit-nrfutil umi skoncit tracebackem a PRESTO vratit 0 -- videno
    # 1. 9. 2026 na "No data received on serial port". Na navratovy kod se tu
    # tedy spolehnout nejde, cte se i vystup.
    out="$(PYTHONPATH="$NRF/site-packages" "$HOME/.platformio/penv/bin/python" \
      "$NRF/adafruit-nrfutil.py" dfu serial \
      -pkg ".pio/build/$ENV_NAME/firmware.zip" -p "$PORT" -b 115200 --singlebank 2>&1)" || RC=$?
    echo "$out" | tail -5
    case "$out" in
      *NordicSemiException*|*"Not able to proceed"*|*Traceback*) RC=1 ;;
    esac
    ;;
  *)
    die "stav '$STATE' neumim naflashovat -- deska neodpovida zadnemu protokolu.
       Vytahni ji z USB a zapoj zpatky, pak spust znovu."
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
