#!/usr/bin/env bash
# Flash a XIAO nRF52840 by waiting for its UF2 drive and copying the image.
#
#   tools/xiao_uf2_flash.sh .pio/build/Xiao_x1_rpt/firmware.uf2
#
# You put the board in UF2 mode by DOUBLE-TAPPING its small RESET button; the
# drive shows up within a few seconds. Then this copies, waits for the drive to
# vanish (which is what proves the bootloader took the image) and for the
# application to come back on USB.
#
# WHY NOT THE 1200-BAUD TOUCH: on a running MeshCore repeater build it is about
# a coin flip. Measured on 2026-08-04: it worked on two boards, then wedged x4,
# then wedged x1 -- and a wedged board is gone from BOTH USB and BLE, does not
# come back on its own (waited 400 s), and needs a physical replug. It is not a
# "too soon after boot" effect either; x1 had been up 23 minutes. So: the touch
# is fine for a board you can reach, useless for one you cannot, and this script
# exists so the reliable path is the easy one.
#
# The other reliable path is BLE OTA (tools/ble_dfu.py) -- no buttons at all, but
# ~14 minutes per board at the 20-byte chunk size the XIAO bootloader forces.
#
# CAUTION: with several boards in UF2 mode at once the drives are all labelled
# XIAO-SENSE (and XIAO-SENSE1, ...). This script refuses to guess -- do one board
# at a time, or resolve the drive through sysfs by serial number yourself.
set -uo pipefail

UF2="${1:-}"
if [ -z "$UF2" ] || [ ! -f "$UF2" ]; then
  echo "usage: $(basename "$0") <firmware.uf2>" >&2
  exit 1
fi
echo "== image: $UF2 ($(stat -c%s "$UF2") B)"

# STALENESS GUARD. `pio run` builds firmware.zip, `pio run -t create_uf2` builds
# firmware.uf2, and NEITHER refreshes the other. Flashing one board from the .uf2
# and another from the .zip then silently gives them different firmware -- which
# is exactly how x3 ended up 50 minutes behind the rest of the fleet, missing a
# command the others had. Compare against the .elf, which every build relinks.
ELF="$(dirname "$UF2")/firmware.elf"
if [ -f "$ELF" ] && [ "$ELF" -nt "$UF2" ]; then
  echo "CHYBA: $(basename "$UF2") je STARSI nez firmware.elf -- nalil bys stary obraz." >&2
  echo "       Sprav: pio run -e <env> && pio run -e <env> -t create_uf2" >&2
  exit 1
fi
# The version string is stamped by tools/build_version.py; show it so what lands
# on the board is on the record before it lands, not guessed afterwards.
VER="$(grep -aoE 'v[0-9]+\.[0-9]+\.[0-9]+-tth[0-9a-f]+\+?' "$UF2" | head -1 || true)"
[ -n "$VER" ] && echo "== verze v obrazu: $VER"

find_disks() { ls -d /dev/disk/by-label/XIAO* 2>/dev/null; }

echo -n "== double-tap the RESET button now; waiting for the UF2 drive "
DISK=""
for _ in $(seq 1 60); do
  mapfile -t found < <(find_disks)
  if [ "${#found[@]}" -gt 1 ]; then
    echo
    echo "CHYBA: ${#found[@]} UF2 disku najednou -- nevim, ktery je ktery:" >&2
    printf '  %s\n' "${found[@]}" >&2
    echo "Nech v UF2 rezimu jen jednu desku." >&2
    exit 1
  fi
  if [ "${#found[@]}" -eq 1 ]; then DISK="${found[0]}"; break; fi
  echo -n "."
  sleep 1
done
echo
[ -n "$DISK" ] || { echo "CHYBA: UF2 disk se neobjevil" >&2; exit 1; }
echo "== disk: $DISK"

MNT="$(lsblk -no MOUNTPOINT "$DISK" | head -1)"
if [ -z "$MNT" ]; then
  udisksctl mount -b "$DISK" >/dev/null 2>&1 || true
  sleep 1
  MNT="$(lsblk -no MOUNTPOINT "$DISK" | head -1)"
fi
[ -n "$MNT" ] || { echo "CHYBA: disk se nepodarilo namountovat" >&2; exit 1; }

echo "== mount: $MNT -- kopiruju"
# The board resets itself the moment it has all the blocks, so cp/sync failing
# at the very end is normal, not an error.
cp "$UF2" "$MNT/" 2>/dev/null || true
sync 2>/dev/null || true

echo -n "== cekam, az disk zmizi (= bootloader obraz prevzal) "
GONE=0
for _ in $(seq 1 30); do
  [ -z "$(find_disks)" ] && { GONE=1; break; }
  echo -n "."
  sleep 1
done
echo
[ "$GONE" -eq 1 ] || { echo "CHYBA: disk nezmizel -- flash nejspis neprobehl" >&2; exit 1; }

echo -n "== cekam na navrat aplikace na USB "
for _ in $(seq 1 45); do
  P="$(ls /dev/serial/by-id/*XIAO* 2>/dev/null | head -1)"
  [ -n "$P" ] && { echo; echo "== OK: $P"; exit 0; }
  echo -n "."
  sleep 1
done
echo
echo "!! aplikace se do 45 s neohlasila -- zkontroluj desku" >&2
exit 1
