#!/usr/bin/env bash
# Stop: na konci turnu zkontroluje, co jsem po sobě nechal běžet nebo
# namountované, a jen v tom případě to vloží do kontextu. Když je čisto, MLČÍ.
#
# Proč: „nebezi ti tu furt neco na pozadi? stopni si vsecky ty watche a podobny
# kraviny" (4. 8. 2026) a „jak dopey neuklidni?" (17. 8.).
#
# PŘESNOST PŘED POKRYTÍM. První verze hlásila uživatelův trvalý NFS mount
# (/mnt/wd_crypt) a `Pairable: yes` na jeho vlastním adaptéru — tedy vyla na
# každý běh, což podle jeho vlastní poznámky z 5. 8. naučí souhrn přehlížet.
# Proto: mounty jen z blokových USB zařízení (/dev/sd*), a BT stav se
# nekontroluje vůbec, protože svoje od uživatelova nerozliším — a ten skutečný
# případ (registrovaný agent po zabitém ble_pair.py) byl na CIZÍM hostu, kam
# tenhle hook nedosáhne. Tam to řeší úklidový trap v samotném ble_pair.py.
set -uo pipefail
FOUND=""

# DVĚ PASTI, obě zjištěné na živém provozu, ne na syntetickém testu:
#  1) `claude-xmpp-client` nese v argumentu TEXT MOJÍ ZPRÁVY — když jsem v ní
#     psal o `ble_pair.py`, hook si našel svůj vlastní kontext. Proto se hledá
#     jen v executable + prvních třech argumentech, ne v celém cmdline.
#  2) Hook matchoval SÁM SEBE: `grep`/`awk` z vlastní pipeline nesou ten pattern
#     v argumentech. Proto se vylučují vlastní potomci podle PPID, a to je
#     spolehlivější než další grep -v (ten se dá obejít každým novým členem
#     pipeline).
#  3) Procesy pod `timeout` se NEhlásí: jsou ohraničené a ukončí se samy —
#     to je záměrný vzorek, ne únik. Bez téhle výjimky hook zavyl TŘIKRÁT za
#     sebou na tentýž 7min mosquitto_sub vzorek (23. 8. 2026) — přesně ta
#     „kontrola, která vyje na každý běh a naučí souhrn přehlížet".
P=$(ps -eo pid=,ppid=,args= 2>/dev/null | awk -v me="$$" '
    {
      npid[NR] = $1; nppid[NR] = $2
      s = ""
      for (i = 3; i <= 6 && i <= NF; i++) s = s $i " "
      argv[NR] = s
      if ($3 == "timeout") istimeout[$1] = 1
    }
    END {
      for (n = 1; n <= NR; n++) {
        if (npid[n] == me || nppid[n] == me) continue
        if (istimeout[npid[n]] || istimeout[nppid[n]]) continue
        s = argv[n]
        if (s ~ /mosquitto_sub|ble_pair\.py|ble_dfu\.py|ble_cli\.py|dtr_low|flight_[a-z]*\.py|power_ab\.py|fleet_test\.py/ \
            && s !~ /claude-|cleanup-check/) print npid[n], s
      }
    }' | head -5)
[ -n "$P" ] && FOUND="${FOUND}nástroje, které mi ještě běží:\n$P\n"

# jen USB bloková zařízení: UF2 disk po flashi zmizí a mount po něm visí,
# načež i `sudo mkdir /mnt/cokoliv` skončí na Input/output error
# POZOR na dva falešné pozitivy, které to hlásilo, než jsem to dotáhl:
# uživatelův trvalý NFS `/mnt/wd_crypt` (proto jen /dev/sd*) a systémový
# `/boot` na /dev/sda1 (proto jen cíle v /mnt, /media a /run/media).
M=$(findmnt -rno TARGET,SOURCE,FSTYPE 2>/dev/null \
    | awk '$2 ~ /^\/dev\/sd/ && $1 ~ /^\/(mnt|media|run\/media)/ {print "  "$1" <- "$2" ("$3")"}' | head -5)
[ -n "$M" ] && FOUND="${FOUND}namountovaný USB disk (po flashi deska zmizí a mount visí, /mnt pak dá I/O error):\n$M\n"

[ -z "$FOUND" ] && exit 0
printf '%b' "$FOUND" | jq -Rs '{hookSpecificOutput:{hookEventName:"Stop",additionalContext:("ÚKLID — po mně zůstalo:\n" + . + "Ukliď to (umount -l, kill), nebo uživateli řekni, že to tam necháváš záměrně a proč.")}}'
