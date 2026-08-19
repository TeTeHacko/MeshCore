#!/usr/bin/env bash
# Ostrý flash MeshCore uzlu přes BLE OTA, celý postup na jeden příkaz.
#
# Existuje proto, že samotné `ble_dfu.py` je jen přenos obrazu. Kolem něj je
# pokaždé stejných šest věcí, a každá z nich už jednou něco stála:
#
#   1. ČISTÝ STROM. Verze ze špinavého stromu má `+` a na stožáru pak nepoznáš,
#      co na uzlu je. Skript odmítne pokračovat.
#   2. PŘEDLETOVÁ KONTROLA prefs. Prefs v InternalFS přebíjejí build flagy a
#      flash je NEPŘEPÍŠE — takže se musí přečíst PŘED i PO a porovnat.
#   3. TICHO u uzlu s odpojenou antenou (`--require-silence`). Vysílání do
#      odpojené antény poškozuje PA; build je mlčící z konstrukce, ale prefs to
#      přebijou, takže se ticho ověřuje na uzlu, ne v ini.
#   4. UVOLNĚNÍ LINKU. Zastavit službu (most) NESTAČÍ, spojení drží BlueZ.
#   5. OVĚŘENÍ VERZE proti obrazu, ne "vypadá to dobře".
#   6. VRÁCENÍ SLUŽBY do provozu a kontrola, že zase teče.
#
# Použití:
#   tools/ble_flash_node.sh --env SenseCap_hreb_rpt --mac CE:72:14:CD:FD:02 \
#       --require-silence
#
#   tools/ble_flash_node.sh --env SenseCap_Solar_repeater_ble \
#       --mac FA:4F:30:E3:1B:7A --via dopey.doma \
#       --python /opt/meshcore-ble-bridge/venv/bin/python \
#       --stop-service meshcore-ble-bridge.service
#
#   --check-only   jen přečte stav uzlu a skončí (nic nemění, nic neflashuje)
#
# `--via` flashuje ze vzdáleného hostu, protože BLE má metry: tth-ltm je
# v dosahu dopey, ne pracovní stanice. Nic se tam neinstaluje — použije se
# python, který na hostu už je (`--python`).
set -euo pipefail

ENV_NAME=""; MAC=""; VIA=""; PY="python3"; STOP_SVC=""; REQUIRE_SILENCE=0; CHECK_ONLY=0
REPO="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"

while [ $# -gt 0 ]; do
  case "$1" in
    --env) ENV_NAME="$2"; shift 2 ;;
    --mac) MAC="$2"; shift 2 ;;
    --via) VIA="$2"; shift 2 ;;
    --python) PY="$2"; shift 2 ;;
    --stop-service) STOP_SVC="$2"; shift 2 ;;
    --require-silence) REQUIRE_SILENCE=1; shift ;;
    --check-only) CHECK_ONLY=1; shift ;;
    -h|--help) sed -n '2,32p' "$0"; exit 0 ;;
    *) echo "neznámý argument: $1" >&2; exit 2 ;;
  esac
done
[ -n "$MAC" ] || { echo "chybí --mac" >&2; exit 2; }
[ -n "$ENV_NAME" ] || [ "$CHECK_ONLY" = 1 ] || { echo "chybí --env" >&2; exit 2; }

PIO="${PIO:-$HOME/.platformio/penv/bin/pio}"
CLI_CMDS=(ver "get name" "get repeat" "get advert.interval" "get flood.advert.interval" "get freq")

# Konzole uzlu: buď lokálně, nebo na hostu z --via (tam se nahrají nástroje).
node_cli() {
  if [ -n "$VIA" ]; then
    # shellcheck disable=SC2029
    ssh "$VIA" "$PY /tmp/ble_cli.py $MAC $(printf '%q ' "$@")" 2>/dev/null
  else
    "$PY" "$REPO/tools/ble_cli.py" "$MAC" "$@" 2>/dev/null
  fi
}

pref() { grep -E "^$1[[:space:]]" <<< "$2" | cut -f2- | tr -d '\r'; }

if [ -n "$VIA" ]; then
  scp -q -p "$REPO/tools/ble_cli.py" "$VIA:/tmp/" || { echo "scp ble_cli.py selhalo" >&2; exit 1; }
fi

# Uvolnit link MUSÍ být PŘED předletovou kontrolou: když linku drží most, na
# konzoli se nedostaneš ani ty. A když cokoli dál selže, služba se musí vrátit
# do provozu — proto trap, ne "restart na konci".
SVC_STOPPED=0
restore_svc() {
  [ "$SVC_STOPPED" = 1 ] || return 0
  echo "== vracím $STOP_SVC ==" >&2
  ssh "$VIA" "systemctl start $STOP_SVC" >/dev/null 2>&1 || true
  SVC_STOPPED=0
}
trap restore_svc EXIT

if [ -n "$STOP_SVC" ] && [ "$CHECK_ONLY" = 0 ]; then
  echo "== uvolnění linku ($STOP_SVC) =="
  # Zastavit službu NESTAČÍ — spojení drží BlueZ, další připojení pak selže.
  ssh "$VIA" "systemctl stop $STOP_SVC; bluetoothctl disconnect $MAC >/dev/null 2>&1 || true; sleep 3"
  SVC_STOPPED=1
fi

echo "== stav uzlu $MAC =="
BEFORE="$(node_cli "${CLI_CMDS[@]}")" || { echo "uzel neodpovídá na BLE konzoli" >&2; exit 1; }
echo "$BEFORE" | sed 's/^/   /'

V_BEFORE="$(pref ver "$BEFORE")"
REPEAT_BEFORE="$(pref 'get repeat' "$BEFORE")"
ADV_BEFORE="$(pref 'get advert.interval' "$BEFORE")"
FADV_BEFORE="$(pref 'get flood.advert.interval' "$BEFORE")"

if [ "$REQUIRE_SILENCE" = 1 ]; then
  if [ "$ADV_BEFORE" != "0" ] || [ "$FADV_BEFORE" != "0" ] || [ "$REPEAT_BEFORE" != "off" ]; then
    cat >&2 <<EOF
STOP: --require-silence, ale uzel není tichý
  advert.interval=$ADV_BEFORE  flood.advert.interval=$FADV_BEFORE  repeat=$REPEAT_BEFORE
U uzlu s odpojenou antenou to znamená vysílání do odpojené antény po rebootu.
Umlč ho a spusť znovu:
  set advert.interval 0 / set flood.advert.interval 0 / set repeat off
EOF
    exit 1
  fi
  echo "   ticho ověřeno (0/0/off)"
fi

[ "$CHECK_ONLY" = 1 ] && { echo "== --check-only, končím =="; exit 0; }

echo "== build $ENV_NAME =="
DIRTY="$(cd "$REPO" && git status --porcelain | wc -l)"
if [ "$DIRTY" != "0" ]; then
  echo "STOP: strom není čistý ($DIRTY změn) — verze by dostala '+' a na uzlu" >&2
  echo "      by pak nešlo poznat, co na něm je. Commitni, nebo stashni." >&2
  exit 1
fi
(cd "$REPO" && "$PIO" run -e "$ENV_NAME" >/dev/null) || { echo "build selhal" >&2; exit 1; }
ZIP="$REPO/.pio/build/$ENV_NAME/firmware.zip"
ELF="$REPO/.pio/build/$ENV_NAME/firmware.elf"
V_EXPECT="$(strings "$ELF" | grep -m1 -E 'v[0-9]+\.[0-9]+\.[0-9]+-tth')"
[ -n "$V_EXPECT" ] || { echo "STOP: v obrazu není stampovaná verze (env bez stampingu?)" >&2; exit 1; }
echo "   obraz: $V_EXPECT  ($(stat -c%s "$ZIP") B)"
echo "   na uzlu teď: $V_BEFORE"

echo "== DFU =="
if [ -n "$VIA" ]; then
  scp -q -p "$ZIP" "$REPO/tools/ble_dfu.py" "$VIA:/tmp/"
  ssh "$VIA" "cd /tmp && nohup timeout 1500 $PY /tmp/ble_dfu.py /tmp/firmware.zip $MAC > /tmp/dfu_$MAC.log 2>&1 &" || true
  until ssh "$VIA" "grep -qE 'HOTOVO|SELHALO|Traceback' /tmp/dfu_$MAC.log" 2>/dev/null; do sleep 20; done
  ssh "$VIA" "tail -4 /tmp/dfu_$MAC.log" | sed 's/^/   /'
  ssh "$VIA" "grep -q HOTOVO /tmp/dfu_$MAC.log" || { echo "DFU SELHALO — zotavení: ble_dfu.py … phase2" >&2; exit 1; }
else
  "$PY" "$REPO/tools/ble_dfu.py" "$ZIP" "$MAC" | tail -4 | sed 's/^/   /'
fi

echo "== ověření po flashi =="
for _ in $(seq 1 30); do
  sleep 10
  AFTER="$(node_cli "${CLI_CMDS[@]}" || true)"
  [ -n "$(pref ver "$AFTER")" ] && break
done
echo "$AFTER" | sed 's/^/   /'

V_AFTER="$(pref ver "$AFTER")"
FAIL=0
case "$V_AFTER" in
  "$V_EXPECT"*) echo "   verze OK" ;;
  *) echo "   CHYBA: uzel hlásí '$V_AFTER', čekáno '$V_EXPECT'" >&2; FAIL=1 ;;
esac
# Prefs flash nepřepisuje, takže rozdíl = něco je špatně (nebo si je uzel resetoval).
for p in repeat:REPEAT advert.interval:ADV flood.advert.interval:FADV; do
  key="${p%%:*}"; var="${p##*:}"
  b_var="${var}_BEFORE"; before="${!b_var}"
  after="$(pref "get $key" "$AFTER")"
  if [ "$before" != "$after" ]; then
    echo "   CHYBA: $key se změnil: '$before' -> '$after'" >&2; FAIL=1
  fi
done
[ "$FAIL" = 0 ] && echo "   prefs beze změny"

if [ "$SVC_STOPPED" = 1 ]; then
  echo "== vrácení služby =="
  restore_svc
  sleep 15
  ssh "$VIA" "systemctl is-active $STOP_SVC" | sed 's/^/   /'
  echo "   a ověř DATA, ne jen službu:"
  echo "   ssh $VIA \"journalctl -u $STOP_SVC --since -5min | grep 'full scrape'\""
fi

[ "$FAIL" = 0 ] || exit 1
echo "== HOTOVO: $MAC na $V_AFTER =="
