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

# JEN JEDEN FLASH NAJEDNOU. Dva soubezne behy si lezou do cesty na sdilenych
# zdrojich -- jeden BLE adapter, jedna USB sbernice, jeden .pio/build adresar.
# 2. 9. 2026 se kvuli tomu spustily TRI instance zaraz (spousteny na pozadi, bez
# kontroly, jestli predchozi dobehla): kazda si zvlast retryovala BLE connect,
# takze se karta porad dokola pripojovala a odpojovala a zaplavila plochu
# notifikacemi. Nebyla to chyba retry logiky, ale toho, ze bezely tri.
exec 9>"${TMPDIR:-/tmp}/.meshcore-flash-node.lock"
if ! flock -n 9; then
  die "uz bezi jiny flash (zamek ${TMPDIR:-/tmp}/.meshcore-flash-node.lock).
       Pockej, az dobehne -- soubezne flashovani si leze do cesty na BLE i na USB."
fi

# Desky, na ktere se podle AGENTS.md nesaha bez vyslovneho zadani. Drzi identitu,
# historii nebo produkcni roli, a prepsat je omylem je draha chyba.
PROTECTED="B612AE3898A81CCA"   # domaci T1000-E: identita, historie wedge, RemoteTerm

# Cil se da zadat i jako BLE MAC, takze gate musi znat OBOJI. Do 2. 9. 2026 se
# kontrola delala az po dispatchi a obe BLE cesty (`flash_over_ble` a vetev pro
# MAC) z ni utekly `exit`em driv, nez na ni dosla rada -- chranenou domaci
# T1000-E slo pres BLE prepsat bez --force-protected. Ta karta uz o identitu
# jednou nenavratne prisla.
is_protected() {
  local t="$1" sn mac
  for sn in $PROTECTED; do
    case "$t" in *"$sn"*) return 0 ;; esac
    mac="$(ble_mac_for "$sn")"
    [ -n "$mac" ] && [ "$(printf %s "$t" | tr a-f A-F)" = "$mac" ] && return 0
  done
  return 1
}

guard_protected() {
  is_protected "$1" || return 0
  [ "$FORCE" = "1" ] || die "$1 je CHRANENA deska (AGENTS.md: bez vyslovneho
       zadani na ni nesahat). Kdyz to fakt chces: --force-protected"
}

# BLE adresy desek, ktere nemaji textovou konzoli. Companion build se do
# bootloaderu neda poslat prikazem `dfu` (zadna konzole neni), takze jediny zpusob
# je buttonless DFU pres BLE. Kdyz je pak deska ZAROVEN na USB, samotny prenos jde
# po USB: ~20 s misto 3-15 min po BLE (podle vyjednaneho MTU).
ble_mac_for() {
  case "$1" in
    B3F1601DCECE9D11) echo "C0:AE:B9:97:A1:34" ;;   # T1000-E probe
    B612AE3898A81CCA) echo "F5:C2:0C:74:A0:67" ;;   # T1000-E domaci (CHRANENA)
    30911219DA28411D) echo "CB:35:76:3A:FB:03" ;;   # x0
    5ECC11205C68623B) echo "D9:E7:5E:E5:72:28" ;;   # x1
    B69F86518175CBA3) echo "E2:74:BE:B3:EC:30" ;;   # x2
    67901109B61E604A) echo "C4:51:68:23:05:7B" ;;   # x3
    208DBAF462432133) echo "FE:11:D8:C2:AB:C4" ;;   # x4
    *) echo "" ;;
  esac
}

# Deska, ktera po flashi "spadla z USB", NENI mrtva -- 2. 9. 2026 x1 i x4 v tom
# stavu normalne advertovaly na BLE. Softwarovy replug pritom neni z ceho udelat:
# vsechny desky visi primo na root hubu (xhci), a ten neumi prepinat napajeni
# portu, takze ani uhubctl by nepomohl. BLE OTA je proto jedina cesta, jak takovou
# desku doflashovat BEZ RUKOU -- a nepotrebuje USB touch, takze ji ani nema jak
# shodit znovu.
#
# Pozor na dock-quiet: deska, ktera JE na USB, drzi DTR a proto neadvertuje
# (potvrzeno tyz den na x2). BLE zachrana tedy funguje prave pro ty desky, ktere
# z USB vypadly -- coz je presne kdyz je potreba.
# Vraci: 0 = MAC videna, 1 = scan bezel a MAC tam nebyla, 2 = ADAPTER NEVIDEL NIC.
# Ten treti stav je potreba rozlisit, protoze scanner na tehle pracovni stanici je
# HLUCHY (MediaTek; `Discovering: yes` a presto nulovy vystup -- viz pamet
# "BLE jen z e5570"). Bez toho rozliseni skript hlasil "deska neadvertuje" i
# tehdy, kdyz neslysi adapter, tedy obvinoval desku z vady hostitele.
ble_reachable() {
  local mac="$1" out
  out="$(timeout 20 bash -c "bluetoothctl --timeout 12 scan on 2>/dev/null" || true)"
  printf %s "$out" | grep -q "$mac" && return 0
  printf %s "$out" | grep -qE "^\s*\[NEW\] Device |Device [0-9A-F]{2}:" || return 2
  return 1
}

flash_over_ble() {
  local mac="$1"
  echo "== cesta: BLE OTA na $mac (bez USB, tedy bez rizika replugu)"
  "$PIO" run -e "$ENV_NAME" >/dev/null 2>&1 || die "build selhal"
  bluetoothctl disconnect "$mac" >/dev/null 2>&1 || true
  sleep 2
  python3 "$HERE/ble_dfu.py" ".pio/build/$ENV_NAME/firmware.zip" "$mac"
}

ENV_NAME=""; TARGET=""; FORCE=0; PASSTHRU=()
for arg in "$@"; do
  case "$arg" in
    --force-protected) FORCE=1 ;;
    # Vse ostatni s -- se predava ble_flash_node.sh (--require-silence, --via,
    # --stop-service, --check-only); tady by to nemelo co delat.
    --*) PASSTHRU+=("$arg") ;;
    *) if [ -z "$ENV_NAME" ]; then ENV_NAME="$arg"; else TARGET="$arg"; fi ;;
  esac
done
[ -n "$ENV_NAME" ] || { echo "usage: $(basename "$0") <env> [serial|0..4] [--force-protected]" >&2; exit 1; }

# Nastroje a pracovni adresar PRED jakoukoli cestou. Do 2. 9. 2026 se PIO
# prirazovalo AZ ZA vetvi pro BLE MAC, ktera ho pouzivala -- se `set -u` to
# znamenalo "PIO: unbound variable" a cela dokumentovana cesta na stozar/chatu
# byla mrtva. `cd "$ROOT"` bylo taky pozdeji, takze build i relativni cesta do
# .pio/build mirily do cwd volajiciho.
PIO="$HOME/.platformio/penv/bin/pio"
[ -x "$PIO" ] || PIO="$(command -v pio)" || die "pio nenalezeno"
NRF="$HOME/.platformio/packages/tool-adafruit-nrfutil"
cd "$ROOT"

case "$TARGET" in
  [0-4]) TARGET="$(sed -n "s/^\s*${TARGET})\s*TARGET=\([0-9A-F]\{16\}\)\s*;;/\1/p" \
                   "$HERE/xiao_uf2_flash.sh" | head -1)"
         [ -n "$TARGET" ] || die "cislo desky nenalezeno v xiao_uf2_flash.sh" ;;
esac

# Cil zadany jako BLE MAC = deska na USB vubec neni (stozar, chata). Predava se
# ble_flash_node.sh, ktery kolem prenosu dela sest veci, jez tady chybely:
# odmitnuti spinaveho stromu, prefs PRED i PO (prefs v InternalFS prebijeji build
# flagy a flash je nepresipe), --require-silence u uzlu s odpojenou antenou,
# uvolneni linku v BlueZ, overeni verze proti obrazu a vraceni sluzby. Presne
# u mastovych/chatovych uzlu, kam tahle vetev miri, je ticho ochrana PA.
case "$TARGET" in
  *:*:*:*:*:*)
    guard_protected "$TARGET"
    BFN="$HERE/ble_flash_node.sh"
    [ -x "$BFN" ] || die "$BFN nenalezen"
    echo "== cil je BLE MAC $TARGET -- deska neni na USB, predavam ble_flash_node.sh"
    echo "   (pokud ma uzel odpojenou antenu, pridej --require-silence)"
    exec "$BFN" --env "$ENV_NAME" --mac "$TARGET" "${PASSTHRU[@]}"
    ;;
esac

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
  if [ "$hits" = "0" ]; then
    # Neni na USB. Drive to znamenalo "rekni si o replug"; kdyz ale deska
    # advertuje, da se doflashovat pres BLE a ruce nejsou potreba.
    guard_protected "$TARGET"
    MAC="$(ble_mac_for "$TARGET")"
    if [ -n "$MAC" ]; then
      echo "== $TARGET neni na USB; zkousim, jestli je na BLE ($MAC)"
      ble_reachable "$MAC"; BR=$?
      if [ "$BR" = "0" ]; then
        flash_over_ble "$MAC"
        exit $?
      fi
      [ "$BR" = "2" ] && die "$TARGET neni na USB a BLE scan na tomhle hostu NEVIDEL
       ANI JEDNO zarizeni -- to neni vypoved o desce, ale o adapteru. Na tehle
       pracovni stanici je scanner hluchy (MediaTek): `bluetoothctl` hlasi
       Discovering: yes a presto nic. Bud replug po USB, nebo BLE cestu spust
       z hostu, ktery slysi (e5570.doma), nebo pres ble_flash_node.sh --via."
      die "$TARGET neni na USB ani neadvertuje na BLE -- replug.
       POZOR, nez z toho udelas diagnozu: BLE zachrana funguje jen tehdy, kdyz
       ten build BLE vubec MA. USB-only companion (env *_companion_radio_usb,
       tedy i Xiao_x2_cmp) ho nema, takze tam neadvertuje ani zdrava deska --
       overeno 2. 9. 2026 na x2. BLE MAC v tabulce vyse muze byt z doby, kdy na
       desce jel jiny build."
    fi
    die "cil $TARGET neni na sbernici a neznam k nemu BLE adresu."
  fi
  [ "$hits" = "1" ] || die "cil $TARGET je na sbernici ${hits}x -- upresni seriove cislo"
else
  n="$(echo "$BEFORE" | grep -c . || true)"
  [ "$n" = "1" ] || die "na sbernici je $n desek -- rekni ktera: $(basename "$0") $ENV_NAME <serial|0..4>"
  TARGET="$(echo "$BEFORE" | awk '{print $1}')"
fi

guard_protected "$TARGET"

FULL="$(echo "$BEFORE" | awk -v t="$TARGET" '$1 ~ t {print $2}')"
BOARD="${FULL%%/*}"; STATE="${FULL##*/}"
PORT="/dev/serial/by-id/$(ls /dev/serial/by-id/ | grep -- "$TARGET" | head -1)"
echo "== cil: $BOARD, stav: $STATE"

# Stav, ktery NENI zjisteny protokol, znamena "nevim, co na desce bezi" -- a do
# `xiao/*` propadal na `pio -t upload`, tedy 1200baudovy touch naslepo. U stavu
# `obsazeny` je to nejhorsi: board_probe.py se portu, ktery drzi nekdo jiny,
# schvalne NESAHA, protoze 2. 9. 2026 dotek portu, ktery drzel openHop, oznacil
# link za degradovany a vypnul radio natrvalo do repeater.db. Ta pojistka byla
# v probe a do dispatcheru se nepropsala.
case "$STATE" in
  obsazeny)
    die "port desky $TARGET drzi jiny proces ($(echo "$BEFORE" | grep -- "$TARGET" | cut -d' ' -f3-)).
       Nesaham na nej: dotek zive linky umi protistrane vypnout radio natrvalo.
       Zastav toho, kdo port drzi, a spust znovu." ;;
  visi|nic|nedostupny|chyba)
    die "stav desky $TARGET se nepodarilo zjistit (probe hlasi '$STATE'), takze
       nevim, kterou cestou flashovat -- a hadat znamena touch naslepo.
       U T1000-E je 'visi' bezne (pyserial ji neotiskne, viz tools/README.md):
       dostan ji do bootloaderu rucne (dlouhy stisk do 8 s od startu) a spust
       znovu, pak to pujde vetvi t1000e/boot." ;;
esac

# OCEKAVANA VERZE. Bez ni umi otisk rict jen "neco se zmenilo", coz je slabe:
# 2. 9. 2026 pio dvakrat ohlasilo SUCCESS a na desce zustal STARY firmware
# (bootloader nestihl novy obraz aktivovat, protoze deska spadla z USB), a
# jindy naopak flash "selhal" a firmware se pritom nalil. Jedine, cemu se da
# verit, je verze PRECTENA Z DESKY -- a porovnat se musi proti tomu, co je
# v obrazu, ne proti predchozimu stavu.
"$PIO" run -e "$ENV_NAME" >/dev/null 2>&1 || die "build selhal"
WANT_VER="$(strings ".pio/build/$ENV_NAME/firmware.elf" 2>/dev/null \
            | grep -oE "v[0-9]+\.[0-9]+\.[0-9]+-tth[0-9a-f]+\+?" | head -1)"
[ -n "$WANT_VER" ] || die "obraz nema stampovanou verzi -- po flashi by neslo
       overit, co na desce doopravdy je. Zapoj do envu: extra_scripts = \${stamped.extra_scripts}"
echo "== ocekavana verze: $WANT_VER"

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
  # ZADNY dalsi `pio run`: build je jednou nahore, u cteni WANT_VER. Stamping meni
  # FIRMWARE_BUILD_DATE pri kazdem behu, takze kazde volani znamenalo PLNY rebuild
  # (10-30 s) -- a hlavne se pak flashoval jiny artefakt, nez ze ktereho se
  # WANT_VER precetla, cimz ta kontrola prestavala overovat odeslany obraz.
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
      # NE `exec`: tim by se predalo rizeni a preskocila zaverecna kontrola, ze
      # se nezmenila zadna JINA deska -- tedy presne ta pojistka, kvuli ktere
      # tenhle skript vznikl. xiao_uf2_flash.sh si overi svuj cil, my overime zbytek.
      "$HERE/xiao_uf2_flash.sh" ".pio/build/$ENV_NAME/firmware.uf2" "$TARGET" || RC=$?
    # `else` tady CHYBELO, takze po uspesnem UF2 flashi beh propadl do nrfutil na
    # $PORT zachyceny PRED flashem -- ten uz neexistuje, protoze se deska
    # rebootla do aplikace. Kazdy uspesny UF2 flash se tim ohlasil jako selhany,
    # vcetne rady "dvojklikni RESET a spust znovu" pro flash, ktery prosel.
    else
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
    # Companion build nema textovou konzoli, takze `dfu uf2` nema kam poslat.
    # Buttonless DFU pres BLE ho tam dostane -- a protoze deska JE na USB,
    # samotny prenos pak jde po kabelu (~20 s misto minut po BLE).
    MAC="$(ble_mac_for "$TARGET")"
    [ -n "$MAC" ] || die "T1000-E ve stavu '$STATE' nema textovou konzoli a k seriaku
       $TARGET neznam BLE adresu. Doplň ji do ble_mac_for(), nebo desku dostan
       do bootloaderu rucne (dlouhy stisk do 8 s od startu)."
    echo "== cesta: BLE buttonless -> bootloader, pak flash po USB"
    echo "   BLE $MAC (jen prepnuti; prenos pojede po kabelu)"
    "$PIO" run -e "$ENV_NAME" >/dev/null 2>&1 || die "build selhal"
    # Pripojeny BLE klient by uzel drzel a buttonless zapis by nemel kam --
    # zastavit sluzbu NESTACI, spojeni drzi BlueZ (skill flash-node).
    bluetoothctl disconnect "$MAC" >/dev/null 2>&1 || true
    sleep 2
    if ! python3 "$HERE/ble_dfu.py" ".pio/build/$ENV_NAME/firmware.zip" "$MAC" phase1; then
      die "buttonless pres BLE se nepovedl. BLE DFU je flaky a jeden pokus o connect
       nestaci -- ale NESPOUSTEJ to opakovane, RESET po kazdem padu umi desku
       uštvat doopravdy (skill flash-node)."
    fi
    # Bootloader se na USB hlasi BEZ "-BOOT" v nazvu portu -- ano, obracene.
    echo "== cekam na bootloader port"
    BOOTPORT=""
    for _ in $(seq 30); do
      p="$(ls /dev/serial/by-id/ 2>/dev/null | grep -- "$TARGET" | grep -v -- "-BOOT" | head -1)"
      [ -n "$p" ] && { BOOTPORT="/dev/serial/by-id/$p"; break; }
      sleep 1
    done
    [ -n "$BOOTPORT" ] || die "deska se do 30 s neobjevila v bootloaderu na USB"
    echo "   bootloader port: $BOOTPORT"
    run_nrfutil "$BOOTPORT" || RC=1
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
AFTER="$("$HERE/board_probe.py" --all)"
# JEDEN otisk pro obe kontrole. Driv se sbernice otiskovala dvakrat za sebou
# (kazdy az PROBE_DEADLINE dlouhy) a verdikt o zmenach se pocital z jineho
# vzorku, nez overeni verze.
#
# A `|| true` tady BYT NESMI. Do 2. 9. 2026 tam bylo, takze verdikt "zmenila se
# i jina deska" se jen vypsal a skript skoncil nulou -- tedy presne ten nalez,
# pro ktery tenhle nastroj vznikl (obraz mel jit na x4 a pristal na x2), se dal
# prehlednout a kazdy volajici, ktery se ridi exit kodem, videl uspech.
if ! "$HERE/board_probe.py" --all --compare <(echo "$BEFORE") \
       --expect-changed "$TARGET" --now <(echo "$AFTER"); then
  echo >&2
  die "otisk sbernice nesouhlasi (viz vys). Bud se zmenila i JINA deska, nebo se
       cil nezmenil -- obojim smerem to znamena, ze obraz sel jinam, nez mel."
fi

GOT_VER="$(echo "$AFTER" | awk -v t="$TARGET" '$1 ~ t {print $3}')"
# Rozlisuj NEOVERITELNE od OVERENE SPATNE. board_probe umi vratit stavovy text
# misto verze (napr. T1000-E, kterou pyserial neotiskne -- tools/README.md), a
# ten se drive porovnal s WANT_VER a vypsal "FLASH SE NEAPLIKOVAL" po flashi,
# ktery klidne prosel. Dokumentovana cesta `flash_node.sh t1000e_repeater B3F160`
# tak koncila chybou vzdycky.
case "$GOT_VER" in
  ""|"-"|neodpovida|drzi|nenalezen)
    echo >&2
    echo "NEOVERENO: flash probehl bez chyby, ale verzi z $TARGET nejde precist" >&2
    echo "(probe hlasi '$STATE'). NEZNAMENA to, ze flash selhal -- u T1000-E je to" >&2
    echo "bezny stav. Over rucne z konzole: 'ver' ma dat $WANT_VER." >&2
    exit 2 ;;
esac
if [ "$GOT_VER" != "$WANT_VER" ]; then
  echo >&2
  die "FLASH SE NEAPLIKOVAL. Deska hlasi '$GOT_VER', obraz ma '$WANT_VER'.
       Nastroj pred tim mohl hlasit uspech -- 'Device programmed' i 'SUCCESS'
       umi vypsat i beh, po kterem na desce zustal stary firmware (bootloader
       nestihl novy obraz aktivovat). Spust znovu; pokud to vytrva, chce to
       replug."
fi
echo "== OVERENO NA DESCE: $TARGET bezi $GOT_VER"
