#!/usr/bin/env bash
# PreToolUse/Bash: příkaz, kterým uzel začne vysílat do ostrého CZ meshe, si
# nechá potvrdit člověkem. NEBLOKUJE — vrací "ask", takže na stožáru se prostě
# odklikne.
#
# Proč: 22. 8. 2026 jsem spustil `activate-cz-mast.txt` na desce ležící na stole
# a udělal z Plešivce nepřihlášený repeater vysílající do cizí sítě. Ten soubor
# má „On-site activation" a „ONLY WITH THE ANTENNA CONNECTED" v hlavičce — takže
# přečtení dokumentace to nezachytilo, protože jsem ji přečetl a pak podle ní
# neuvažoval. Hook se nedá přehlédnout.
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

echo "$CMD" | grep -qiE "activate-cz-mast|set repeat on|set advert\.interval [1-9]|set flood\.advert\.interval [1-9]|(^|[\"' ])advert([\"' ]|$)|set freq" || exit 0

jq -n '{hookSpecificOutput:{
  hookEventName:"PreToolUse",
  permissionDecision:"ask",
  permissionDecisionReason:"Tímhle uzel ZAČNE VYSÍLAT do ostrého CZ meshe (869.432). Potvrď, že: (1) deska je fyzicky NA MÍSTĚ s připojenou antenou — ne na lavici, ne na stole; (2) vysílání do odpojeného konektoru poškozuje PA. Aktivace je krok na místě, ne součást flashe (viz skill live-mesh-safety)."
}}'
