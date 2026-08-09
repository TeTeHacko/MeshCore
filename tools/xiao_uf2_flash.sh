#!/usr/bin/env bash
# Flash a XIAO nRF52840 through its UF2 drive -- to a named board, and verified.
#
#   tools/xiao_uf2_flash.sh <firmware.uf2> [target]
#
#   target   USB serial (full or unique suffix), or a fleet number 0..4.
#            Omit only when exactly one board is plugged in.
#
#   XIAO_UF2_WAIT=300   seconds to wait for UF2 mode (default 90). Worth raising
#                       when the board has to be double-tapped by hand.
#
# Get the board into UF2 mode with 'dfu' on its console (deterministic), or by
# double-tapping its small RESET button.
#
# What it refuses to do, because each of these has actually gone wrong here:
#   - flash an image older than the build that produced it (pio run and
#     pio run -t create_uf2 refresh different artifacts; x3 silently ended up
#     two commits behind the rest of the fleet that way)
#   - guess which board to write to when several are in UF2 mode (every drive
#     is labelled XIAO-SENSE, in plug order, so the label proves nothing)
#   - write to a board other than the one you named
#   - report success without reading the version back off the board (the first
#     version of this script printed x4's port after flashing x3)
#
# NEVER uses the 1200-baud touch: it only requests the bootloader's SERIAL_ONLY
# mode, so no drive ever appears, and a second touch in one power session wedges
# the board out of USB and BLE with no self-recovery.
set -uo pipefail

die() { echo "CHYBA: $*" >&2; exit 1; }

UF2="${1:-}"; TARGET="${2:-}"
[ -n "$UF2" ] && [ -f "$UF2" ] || { echo "usage: $(basename "$0") <firmware.uf2> [serial|0..4]" >&2; exit 1; }

# Fleet shorthand. Keep in step with the labels physically on the boards.
case "$TARGET" in
  0) TARGET=30911219DA28411D ;;
  1) TARGET=5ECC11205C68623B ;;
  2) TARGET=B69F86518175CBA3 ;;
  3) TARGET=67901109B61E604A ;;
  4) TARGET=208DBAF462432133 ;;
esac

# A .hex or .zip dropped on a UF2 drive is silently ignored by the bootloader and
# looks like a successful flash, so check the container before trusting it.
# UF2 blocks start with the magic 0x0A324655 ("UF2\n").
head -c4 "$UF2" | grep -q 'UF2' || die "$UF2 neni UF2 obraz (chybi magic 'UF2\\n')"
echo "== image: $UF2 ($(stat -c%s "$UF2") B)"

ELF="$(dirname "$UF2")/firmware.elf"
if [ -f "$ELF" ] && [ "$ELF" -nt "$UF2" ]; then
  die "$(basename "$UF2") je STARSI nez firmware.elf -- nalil bys stary obraz.
       Sprav: pio run -e <env> && pio run -e <env> -t create_uf2"
fi

WANT_VER="$(grep -aoE 'v[0-9]+\.[0-9]+\.[0-9]+-tth[0-9a-f]+\+?' "$UF2" | head -1 || true)"
if [ -z "$WANT_VER" ]; then
  die "obraz nema verzi od tools/build_version.py -- po flashi by neslo overit,
       co na desce doopravdy je. Zapoj do envu: extra_scripts = \${stamped.extra_scripts}"
fi
echo "== verze v obrazu: $WANT_VER"
[ -n "$TARGET" ] && echo "== cil: $TARGET"

# Resolve UF2 drives THROUGH SYSFS, never by label: every XIAO drive is called
# XIAO-SENSE (then XIAO-SENSE1, ...) assigned in plug order, so with several
# boards on the bus the label says nothing about which board you are writing to.
find_uf2_boards() {
  local d s blk
  for d in /sys/bus/usb/devices/*/; do
    s="$(cat "$d/serial" 2>/dev/null)" || continue
    [ "$(cat "$d/idVendor" 2>/dev/null)" = "2886" ] || continue
    blk="$(ls -d "$d"*/host*/target*/*/block/* 2>/dev/null | head -1)" || true
    [ -n "$blk" ] && echo "$s /dev/$(basename "$blk")"
  done
}

# Companion builds have no text console, so no `dfu` command: those boards can
# only be double-tapped by hand, and 90 s turned out to be shorter than the round
# trip of asking someone to do it. Overridable rather than just longer, so an
# unattended flash still fails fast.
WAIT="${XIAO_UF2_WAIT:-90}"
echo -n "== cekam na UF2 rezim, ${WAIT}s (\"dfu\" na konzoli, nebo dvojklik RESET) "
SERIAL=""; DISK=""
for _ in $(seq 1 "$WAIT"); do
  mapfile -t found < <(find_uf2_boards)
  if [ -n "$TARGET" ]; then                       # keep only the board asked for
    mapfile -t found < <(printf '%s\n' "${found[@]:-}" | grep -F "$TARGET" || true)
  fi
  if [ "${#found[@]}" -gt 1 ]; then
    echo; { echo "${#found[@]} desek v UF2 rezimu -- nevim, ktera je ta tva:";
            printf '  %s\n' "${found[@]}";
            echo "Zadej cil: $(basename "$0") $UF2 <serial|0..4>"; } >&2
    exit 1
  fi
  if [ "${#found[@]}" -eq 1 ] && [ -n "${found[0]}" ]; then
    SERIAL="${found[0]%% *}"; DISK="${found[0]##* }"; break
  fi
  echo -n "."
  sleep 1
done
echo
[ -n "$DISK" ] || die "do ${WAIT} s se v UF2 rezimu neobjevila${TARGET:+ deska $TARGET}"
echo "== deska: $SERIAL  disk: $DISK"

# Mount with retries: the drive appears a moment before the desktop automounter
# gets to it, and udisksctl can lose that race, so a single attempt fails on a
# board that is perfectly fine. Observed on the very first `dfu`-driven flash.
MNT=""
for _ in $(seq 1 12); do
  MNT="$(lsblk -no MOUNTPOINT "$DISK" | head -1)"
  [ -n "$MNT" ] && break
  udisksctl mount -b "$DISK" >/dev/null 2>&1 || true
  sleep 1
done
[ -n "$MNT" ] || die "disk $DISK se nepodarilo namountovat ani na 12 pokusu
       Zkus rucne: udisksctl mount -b $DISK"

echo "== mount: $MNT -- kopiruju"
# The board resets itself the moment it has all the blocks, so cp/sync failing
# at the very end is normal, not an error.
cp "$UF2" "$MNT/" 2>/dev/null || true
sync 2>/dev/null || true

echo -n "== cekam, az disk zmizi (= bootloader obraz prevzal) "
GONE=0
for _ in $(seq 1 30); do
  find_uf2_boards | grep -q "^$SERIAL " || { GONE=1; break; }
  echo -n "."; sleep 1
done
echo
[ "$GONE" -eq 1 ] || die "disk nezmizel -- flash nejspis neprobehl"

# Wait for THIS board, matched on its serial. "Some XIAO came back on USB" is
# worthless with four of them plugged in.
echo -n "== cekam na navrat desky $SERIAL "
PORT=""
for _ in $(seq 1 45); do
  for d in /sys/bus/usb/devices/*/; do
    [ "$(cat "$d/serial" 2>/dev/null)" = "$SERIAL" ] || continue
    [ "$(cat "$d/idProduct" 2>/dev/null)" = "8044" ] || continue
    PORT="$(ls /dev/serial/by-id/*"$SERIAL"*-if00 2>/dev/null | head -1)" || true
  done
  [ -n "$PORT" ] && break
  echo -n "."; sleep 1
done
echo
[ -n "$PORT" ] || die "$SERIAL se do 45 s neohlasila jako aplikace"

# The actual proof: ask the board what it is running. Anything less and a flash
# that quietly did nothing still reports success.
#
# The same trip also SETS THE CLOCK, because this is the one moment when a host
# is provably talking to the board. Every clock in this firmware is volatile
# unless the board has GPS or an I2C RTC: `VolatileRTCClock` starts in May 2024
# and only ticks from millis(). A node with a wrong clock is not a cosmetic
# problem -- the RTC stamps every advert it transmits, and receivers drop a stale
# timestamp as a replay. Measured 7. 8. 2026: tth-x3 had been running 811 days
# behind since deployment, and bench board x1 was 812 days behind, both simply
# because nobody ever typed `time <epoch>`.
GOT="$(python3 - "$PORT" <<'PY' 2>/dev/null || true
import re, struct, sys, time
try:
    import serial
except ImportError:
    sys.exit(0)
# Retry: the port enumerates a second or two before the firmware is answering on
# it, so a single attempt calls a perfectly good flash "unverified" -- which is
# worse than no check, because it teaches you to ignore the tool.
for attempt in range(5):
    try:
        s = serial.Serial(sys.argv[1], 115200, timeout=1); s.dtr = True; s.rts = True
        time.sleep(2.5); s.reset_input_buffer()
        # Throwaway newline first. The board's own line buffer can still hold a
        # fragment from before the reset, and whatever is sent next gets appended
        # to it -- so the first real command comes back "Unknown command" and the
        # flash is declared unverified even though it landed. Cost this exactly
        # one wrong conclusion about x4.
        s.write(b"\r\n"); s.flush(); time.sleep(0.5); s.reset_input_buffer()
        s.write(b"ver\r\n"); s.flush(); time.sleep(2.0)
        out = s.read(s.in_waiting or 1).decode("utf-8", "replace")
        m = re.search(r"v[0-9]+\.[0-9]+\.[0-9]+-tth[0-9a-f]+\+?", out)
        if m:
            print("VER\t" + m.group(0))
            # Set the clock, then read it back. The read-back is what decides:
            # `time` answers "(ERR: clock cannot go backwards)" when the board is
            # already right -- which happens once the build epoch seeds the RTC,
            # and is SUCCESS, not failure. Never trust the setter's own reply.
            s.reset_input_buffer()
            s.write(("time %d\r\n" % int(time.time())).encode()); s.flush()
            time.sleep(1.5); s.reset_input_buffer()
            s.write(b"clock\r\n"); s.flush(); time.sleep(1.5)
            got = s.read(s.in_waiting or 1).decode("utf-8", "replace")
            c = re.search(r"\d{1,2}:\d{2} - \d{1,2}/\d{1,2}/\d{4} UTC(?: \(epoch \d+\))?", got)
            print("CLOCK\t" + (c.group(0) if c else "?"))
            s.close(); break
        # No text console means this is a companion_radio build, which has no
        # `ver` command at all -- ask in its own language rather than calling a
        # perfectly good flash unverified (it did exactly that on x2).
        # CMD_DEVICE_QUERY(22) -> RESP_CODE_DEVICE_INFO(13); firmware_version is
        # the 20-byte field at offset 60. Framing: '<' + uint16 LE len + payload
        # outbound, '>' + len + payload back.
        s.reset_input_buffer()
        s.write(b"<" + struct.pack("<H", 2) + bytes([22, 10])); s.flush()
        time.sleep(1.0)
        buf = s.read(s.in_waiting or 1)
        # The port stays OPEN here on purpose. It used to be closed on this line,
        # before the reply was even parsed -- which was harmless while the only
        # thing left to do was print, and became a PortNotOpenError the moment
        # the clock was set afterwards (caught by the outer handler, so it looked
        # like four silent retries printing VER over and over). Closed on both
        # exits below instead.
        i = buf.find(b">")
        if i >= 0 and len(buf) >= i + 3:
            ln = struct.unpack("<H", buf[i+1:i+3])[0]
            f = buf[i+3:i+3+ln]
            if len(f) >= 80 and f[0] == 13:
                v = f[60:80].split(b"\0")[0].decode("utf-8", "replace")
                if v:
                    print("VER\t" + v)
                    # Companion builds have no `time` command at all -- the app
                    # sets the clock with CMD_SET_DEVICE_TIME(6). Same guard as
                    # the console (`secs >= curr`), so an ERR frame here is not a
                    # failure either; CMD_GET_DEVICE_TIME(5) -> RESP(9) decides.
                    now = int(time.time())
                    s.reset_input_buffer()
                    fr = bytes([6]) + struct.pack("<I", now)
                    s.write(b"<" + struct.pack("<H", len(fr)) + fr); s.flush()
                    time.sleep(1.0); s.reset_input_buffer()
                    s.write(b"<" + struct.pack("<H", 1) + bytes([5])); s.flush()
                    time.sleep(1.0)
                    rb = s.read(s.in_waiting or 1)
                    shown = "?"
                    j = rb.find(b">")
                    if j >= 0 and len(rb) >= j + 3:
                        rl = struct.unpack("<H", rb[j+1:j+3])[0]
                        rf = rb[j+3:j+3+rl]
                        if len(rf) >= 5 and rf[0] == 9:
                            t = struct.unpack("<I", rf[1:5])[0]
                            shown = time.strftime("%H:%M - %-d/%-m/%Y UTC",
                                                  time.gmtime(t)) + " (epoch %d)" % t
                    print("CLOCK\t" + shown)
                    s.close(); break
        s.close()   # neither dialect answered -- close before the retry reopens
    except Exception:
        pass
    time.sleep(3)
PY
)"

GOT_VER="$(printf '%s\n' "$GOT" | sed -n 's/^VER\t//p' | head -1)"
GOT_CLOCK="$(printf '%s\n' "$GOT" | sed -n 's/^CLOCK\t//p' | head -1)"

if [ -z "$GOT_VER" ]; then
  echo "!! deska neodpovedela ani konzoli, ani companion protokolu -- flash NEOVERENY (drzi konzoli BLE klient?)" >&2
  exit 2
fi
if [ "$GOT_VER" != "$WANT_VER" ]; then
  die "deska hlasi $GOT_VER, ale v obrazu bylo $WANT_VER -- flash NEPROSEL"
fi
echo "== OK: $SERIAL bezi $GOT_VER  ($PORT)"

# Warn rather than fail: a wrong clock does not mean the flash was bad, and this
# script's contract is "the image landed". But it must not pass silently -- an
# unset clock is exactly what went unnoticed on x3 for 811 days.
# Only the year is checked, because that is what a volatile RTC gets wrong: it
# comes up in 2024 and stays there. (Around midnight on 31 December this warns
# for one minute on a board that is actually right. Non-fatal, once a year.)
case "$GOT_CLOCK" in
  "")   echo "!! hodiny se necetly zpatky -- nastav je rucne: time \$(date +%s)" >&2 ;;
  "?")  echo "!! hodiny neodpovedely ve znamem formatu -- zkontroluj 'clock' rucne" >&2 ;;
  *"/$(date -u +%Y)"*)
        echo "== hodiny: $GOT_CLOCK" ;;
  *)    echo "!! hodiny hlasi $GOT_CLOCK -- nesedi rok, nastav je rucne: time \$(date +%s)" >&2 ;;
esac
