#!/usr/bin/env bash
# Flash a XIAO over USB serial DFU (`pio run -t upload`) -- to a named board,
# and with proof that no OTHER board was touched.
#
#   tools/xiao_serial_flash.sh <env> [target]
#
#   target   USB serial (full or unique suffix), or a fleet number 0..4.
#            Omit only when exactly one XIAO is plugged in.
#
# Why this exists next to xiao_uf2_flash.sh: that one guards the UF2-drive route
# and refuses to guess which board to write to. The serial DFU route had NO such
# guard -- and on 1. 9. 2026 it cost a board. `pio run -e Xiao_nrf52_kiss_modem
# -t upload --upload-port <x4>` reported FAILED, and the KISS modem image ended
# up on **x2**, a board that was not named anywhere on that command line. Both
# boards dropped off USB together, x2 then answered neither the text console nor
# the companion protocol (a KISS modem has neither), and three replugs could not
# help because nothing was actually wrong with it.
#
# The upload itself still goes through PlatformIO -- the point is what happens
# around it:
#
#   - the target is resolved from the SERIAL NUMBER, never from "the only port"
#   - every XIAO on the bus is fingerprinted BEFORE the flash
#   - after the flash the target must report the expected new version, read back
#     off the board
#   - and every OTHER board must still have the fingerprint it had before.
#     That is the check that was missing; it is what turns "upload failed" into
#     "upload failed AND it wrote somewhere else".
#
# A failed upload is NOT retried here. One 1200-baud touch per power session is
# the rule (AGENTS.md); a second one is how boards get wedged out of USB. The
# script says so and stops.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
die() { echo "CHYBA: $*" >&2; exit 1; }

ENV_NAME="${1:-}"; TARGET="${2:-}"
[ -n "$ENV_NAME" ] || { echo "usage: $(basename "$0") <env> [serial|0..4]" >&2; exit 1; }

# Fleet shorthand -- parsed out of xiao_uf2_flash.sh so the table lives in ONE
# place. Two copies of it would drift, and picking the wrong board is the single
# most expensive mistake on this bench.
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

# Cíl musí být mezi nimi, a musí být právě jeden.
if [ -n "$TARGET" ]; then
  hits="$(echo "$BEFORE" | grep -c -- "$TARGET" || true)"
  [ "$hits" = "1" ] || die "cil $TARGET je na sbernici ${hits}x -- upresni seriove cislo"
  PORT="$(ls /dev/serial/by-id/ | grep -- "$TARGET" | head -1)"
  [ -n "$PORT" ] || die "port pro $TARGET nenalezen"
  PORT="/dev/serial/by-id/$PORT"
else
  n="$(echo "$BEFORE" | grep -c . || true)"
  [ "$n" = "1" ] || die "na sbernici je $n desek -- rekni ktera: $(basename "$0") $ENV_NAME <serial|0..4>"
  TARGET="$(echo "$BEFORE" | awk '{print $1}')"
  PORT="/dev/serial/by-id/$(ls /dev/serial/by-id/ | grep -- "$TARGET" | head -1)"
fi
echo "== cilovy port: $PORT"

echo "== upload"
if ! "$PIO" run -e "$ENV_NAME" -t upload --upload-port "$PORT"; then
  echo >&2
  echo "UPLOAD SELHAL. NEOPAKUJ ho -- jeden 1200baudovy touch na power session." >&2
  echo "Vytahni desku z USB, zapoj zpatky, a spust tenhle skript znovu." >&2
  echo >&2
  echo "Kontroluji, jestli to nezapsalo jinam:" >&2
  "$HERE/board_probe.py" --all --compare <(echo "$BEFORE") >&2
  exit 1
fi

echo "== otisk desek PO flashi (necham CDC nabehnout)"
sleep 6
"$HERE/board_probe.py" --all --compare <(echo "$BEFORE") --expect-changed "$TARGET"
