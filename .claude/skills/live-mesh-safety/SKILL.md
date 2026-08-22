---
name: live-mesh-safety
description: Než se sáhne na uzel, který je na ostrém komunitním CZ meshi (869.432) nebo se tam má dostat — ztišení, přeladění, aktivace, zásah do cizího uzlu, vysílání s odpojenou antenou. Použij VŽDY před `set repeat`, `set freq`, `advert`, změnou advert intervalů, aktivací uzlu, nebo když se má měnit cokoliv na uzlu, který není náš bench.
---

# Zásah na ostrém CZ meshi

Ostrý mesh je cizí infrastruktura, kterou používají jiní lidé. Chyba se
neprojeví na lavici, ale v jejich routingu — a nikdo nám to nepřijde říct.

## Aktivace je krok NA MÍSTĚ, ne součást flashe

**`tools/provision/activate-cz-mast.txt` se pouští až s antenou na stožáru.**
Má to dvakrát v hlavičce („On-site activation", „RUN THIS ONLY WITH THE ANTENNA
CONNECTED") a i tak se 22. 8. 2026 stalo, že jsem ho spustil na desce ležící na
stole — z Plešivce se tím na chvíli stal nepřihlášený repeater vysílající do
ostrého meshe. **Než ten soubor pustíš, řekni si nahlas, kde ta deska fyzicky je,**
a ověř to u uživatele; on to ví, moje paměť ne.

Pro desku, která se někam ponese, je ten správný soubor
`provision/repeater-cz-silent.txt`.

## Ztišení před přeladěním a před přenášením

```
set repeat off
set advert.interval 0
set flood.advert.interval 0
```

Nepřihlášený repeater mate routing: sousedi si dají jeho hash do cest, které
neumí rozřešit zpátky na uzel.

**Na diagnostický poslech je lepší `tempradio <freq>,<bw>,<sf>,<cr>,<min>`** —
aplikuje se hned bez rebootu, nezapisuje prefs a sám se po N minutách vrátí.
`set freq` je jeden typo od advertování do cizí sítě.

## Jednotky intervalů se liší a chyba je TICHÁ

`set advert.interval` bere **minuty**, `set flood.advert.interval` **hodiny**
(rozsah 3–168; ukládá se to půlené resp. přímo). Špatná jednotka skončí
`Error: interval range is 3-168 hours` a **nechá tam původní hodnotu** — takže po
ztišení zůstane nula a uzel přestane flood adverty posílat úplně. **Vždy `get`
zpátky, nespoléhej na `OK`.**

## Prefs v InternalFS přebíjejí build flagy, a mlčí o tom

Flash nastavení nepřepíše. `sensecap_prod_base` je mlčící z konstrukce
(`ENABLE_ADVERT_ON_BOOT=0`, oba intervaly 0, `DISABLE_FWD_DEFAULT=1`), ale na
uzlu, který už prefs má, to **nezaručuje nic**. U desky s odpojenou antenou si
ticho ověř na uzlu (`get advert.interval`, `get flood.advert.interval`,
`get repeat`), ne v ini.

## Vysílání do odpojené antény poškozuje PA

Každá řádka aktivačního souboru končí vysíláním. U desky bez antény je
`--require-silence` v `ble_flash_node.sh` ta jediná pojistka, která fakt drží,
a `advert` se **nespouští vůbec**.

## Cizí uzly a záměrné odchylky

**Runtime prefs na cizím uzlu neměň bez potvrzení.** Některé konfigurace jsou
odlišné SCHVÁLNĚ — 3bajtový path hash na jednom našem uzlu kvůli testu, starý
conn interval na `x1_rpt_oldble`. **Odchylka není automaticky chyba** a z provozu
se cizí konfigurace soudit nedá (CoreScope `hash_size` je jen dolní hranice).

Naše flotila jede na **2 B** (`path.hash.mode 1`). 1 B u cizího uzlu je jeho
volba; šířku volí odesílatel, takže náš repeater k jeho paketu správně připíše
taky 1 bajt.

## Airtime a duty cycle jsou změřené

Advert na SF8/BW62,5 = **795–918 ms**, takže limit 1 % na 866,5 MHz znamená
~40 advertů/hod. Na 869,432 je duty 8,5 %. Nová identita se **vždycky mele
nekolizní** (`tools/gen_node_id.py`) — path hash je prefix pubkey, na 1 bajt
koliduje většina repeaterů meshe a `00`/`ff` jsou zakázané. Mletí je zadarmo
a uzel, který ještě nevysílal, nemá co ztratit.

## Po zásahu ověř čísly

`get repeat`, oba intervaly, `get freq`, `get path.hash.mode` — a u uzlu, který
má být tichý, ověř ticho **na uzlu**. „OK" v odpovědi nestačí; dneska právě „OK"
zakrylo, že flood interval zůstal na nule.
