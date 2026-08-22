#!/usr/bin/env bash
# PreToolUse/Bash+Read: brání únikům secretů do transkriptu.
#
# Proč hook, když je to v instrukcích: transkripty drží secrety NEŠIFROVANĚ
# a už to dvakrát bylo horké — 22. 8. uzel odechoval nové heslo do výpisu
# (`password now: …`), a exporter vault heslo se našlo v plaintextu ve starém
# transkriptu. Instrukce se dají přehlédnout, hook ne.
set -uo pipefail
IN=$(cat)
TOOL=$(jq -r '.tool_name // ""' <<<"$IN")

SECRET_FILES='platformio\.local\.ini|meshcore-node-identities/keys\.yml|\.ansible_vault_password|\.vault_pass|prv\.key|api-users\.conf|/credentials(\b|$)|\.pem(\b|$)|\.env(\b|$)'

deny() { jq -n --arg r "$1" '{hookSpecificOutput:{hookEventName:"PreToolUse",permissionDecision:"deny",permissionDecisionReason:$r}}'; exit 0; }
ask()  { jq -n --arg r "$1" '{hookSpecificOutput:{hookEventName:"PreToolUse",permissionDecision:"ask",permissionDecisionReason:$r}}'; exit 0; }

if [ "$TOOL" = "Read" ]; then
  FP=$(jq -r '.tool_input.file_path // ""' <<<"$IN")
  echo "$FP" | grep -qE "$SECRET_FILES" && deny "Soubor se secrety se nečte Read nástrojem — obsah by skončil v transkriptu (ten je nešifrovaný). Strukturu ověř grep -c/stat, env sekce čti sed/awk na rozsah řádků, hodnoty POUZE rourou (sed -n 's/^klic=//p' | ssh …)."
  exit 0
fi

[ "$TOOL" = "Bash" ] || exit 0
CMD=$(jq -r '.tool_input.command // ""' <<<"$IN")

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

# dump nástroje na soubory se secrety -- vždycky existuje lepší vzor
echo "$CMD" | grep -qE "\b(cat|bat|batcat|less|more|head|tail|nl|tac|od|xxd|hexdump|strings|vim?|nano|emacs|micro)\b[^|;&>]*($SECRET_FILES)" \
  && deny "Dump souboru se secrety by skončil v transkriptu. Vzory: struktura = grep -c/stat; hodnota = sed -n 's/^klic=//p' ROUROU do ssh/programu, nikdy na stdout; výpis sekce = sed s redakcí (s/=.*/= <REDIGOVÁNO>/)."

# docker inspect vypíše env kontejneru vč. tokenů -- globální zákaz
echo "$CMD" | grep -qE '\bdocker\s+inspect\b' \
  && deny "docker inspect je zakázaný (vypíše env kontejneru včetně tokenů). Použij docker ps / docker logs / cílený --format na ne-env pole, a to jen po potvrzení."

# ykman fido chce PIN interaktivně -- tady to nejde, jen mate
echo "$CMD" | grep -qE '\bykman\s+fido\b' \
  && deny "ykman fido chce PIN na terminálu — tady ho nemá kdo zadat. Řekni uživateli, ať to pustí v reálném terminálu (třeba přes '! ykman fido …')."

# heslo přes echující konzoli -- uzel ho vrátí do výpisu (stalo se 22. 8.)
echo "$CMD" | grep -qE '(ble_cli|prov|card|console)\.py[^|;&]*\bpassword\b' \
  && ask "Konzole uzlu příkaz ECHUJE zpátky — heslo by skončilo v transkriptu (22. 8. se přesně tohle stalo). Rotaci dělej přes mesh_admin.py token 'password!' (bere nové heslo ze stdin a REDIGUJE odpověď). Pokračuj jen pokud fakt víš proč."

exit 0
