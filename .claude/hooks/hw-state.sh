#!/usr/bin/env bash
# PreToolUse/Bash: než se flashuje, měří nebo diagnostikuje deska, NASYPE do
# kontextu skutečný stav USB a disků.
#
# Proč: nejčastější chyba tohohle projektu (8× napříč 6 sezeními) je, že tvrdím
# něco o fyzickém stavu železa z paměti — „leží v krabici", „musíš to přenést
# k sneezy" — a uživatel mě opraví. Skilly to řeší jen připomínkou; tenhle hook
# dodá FAKT. Nic neblokuje.
set -uo pipefail
CMD=$(jq -r '.tool_input.command // ""' 2>/dev/null) || exit 0

# HEREDOC TELA SE PRED TESTEM VYSTRIHNOU. Bez toho hook zablokoval vlastni
# commit: message v heredocu nesla nazvy zakazanych prikazu a hook matchoval
# TEXT misto prikazu -- stejna chyba, jako kdyz si cleanup hook nasel vlastni
# kontext ve zprave. Testuje se jen kod, ne data v heredocich.
strip_heredocs() {
  awk 'BEGIN{skip=""}
  { if (skip != "") { if ($0 == skip) skip=""; next }
    line=$0
    if (match(line, /<<-?[ \t]*["'"'"']?[A-Za-z_][A-Za-z0-9_]*/)) {
      m=substr(line, RSTART, RLENGTH)
      sub(/<<-?[ \t]*["'"'"']?/, "", m)
      skip=m
    }
    print line }'
}

CMD=$(strip_heredocs <<<"$CMD")

# Spouštěj jen u úkonů, kde na stavu desky záleží. `if` v settings.json na to
# nestačí — permission rule matchuje jen PREFIX, a naše příkazy začínají
# `timeout`/`ssh`, takže se filtruje tady.
echo "$CMD" | grep -qiE 'ble_dfu|ble_flash_node|ble_cli|ble_pair|nrfutil|xiao_uf2_flash|flash-l1|power_ab|fleet_test|pio run.*(upload|create_uf2)|dfu (uf2|serial|ota)|/dev/serial/by-id|/dev/ttyACM' || exit 0

# ttyACM přímo v příkazu = míříš na nestabilní jméno; po každém replugu se
# přečísluje a trefí JINOU desku. Varování + aktuální mapa, ať se dá opravit hned.
WARN=""
if echo "$CMD" | grep -q '/dev/ttyACM'; then
  MAP=$(ls -l /dev/serial/by-id/ 2>/dev/null | awk '/->/{printf "    %s -> %s\n", $(NF-2), $NF}')
  WARN="!! PŘÍKAZ MÍŘÍ NA /dev/ttyACM* — to se po každém replugu PŘEČÍSLUJE a trefíš jinou desku.
Použij /dev/serial/by-id/ (matchuj sériové číslo). Aktuální mapa na TOMHLE hostu:
${MAP:-    (žádné sériové porty)}
Na vzdáleném hostu si mapu vyžádej stejným ls -l tam.

"
fi

SER=$(ls /dev/serial/by-id/ 2>/dev/null | paste -sd', ' -)
USB=$(lsusb 2>/dev/null | grep -iE '2886|239a|1915|303a' | sed 's/^Bus [0-9]* Device [0-9]*: //' | paste -sd' | ' -)
DSK=$(lsblk -o NAME,LABEL,TRAN 2>/dev/null | awk '$3=="usb"{printf "%s(%s) ", $1, $2}')

CTX="${WARN}STAV ŽELEZA na tomhle hostu (hook, ne moje paměť):
  sériové porty: ${SER:-ŽÁDNÉ}
  USB nRF52/ESP:  ${USB:-ŽÁDNÉ}
  USB disky:      ${DSK:-ŽÁDNÉ}
Pozn.: 0044/0045 = bootloader, 8044/8029/0057/0059 = aplikace. UF2 disk = deska
v bootloaderu. Pokud tu deska, o které mluvíš, NENÍ, je na jiném hostu — zjisti
to, netvrď, kde leží. Fyzický stav říká uživatel."

jq -n --arg c "$CTX" '{hookSpecificOutput:{hookEventName:"PreToolUse",additionalContext:$c}}'
