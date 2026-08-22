---
name: flash-node
description: Naflashovat MeshCore uzel (SenseCAP solární, T1000-E, XIAO, Wio L1) — vybrat správnou cestu podle hardwaru, ověřit výsledek na desce a nechat uzel ve správném stavu. Použij VŽDY, když se má flashovat, upgradovat firmware, dostat deska do bootloaderu (DFU/OTA/UF2), nebo když uzel po zásahu nereaguje.
---

# Flashování uzlu

Rozsáhlé podklady jsou v `tools/README.md` a `docs/ble-flash-plan.md`. Tenhle
skill je **postup**, ne encyklopedie — jeho účel je, aby se nemusely znovu
objevovat věci, které už jednou stály hodiny.

## 1. Nejdřív zjisti, kde deska je a v jakém režimu

Bez tohohle se nedá vybrat cesta a většina ztraceného času šla odsud.

```bash
ls -l /dev/serial/by-id/          # VŽDY by-id, ttyACM* se přečísluje
lsusb | grep -iE "2886|239a"      # 8044/8029/0057 aplikace, 0044 bootloader
lsblk -o NAME,LABEL,TRAN | grep usb   # UF2 disk = deska v bootloaderu
```

**Uzel, který mlčí na BLE _i_ po meshi, není mrtvý — nejspíš visí
v bootloaderu.** Ten nemá LoRa ani BLE aplikaci, takže zmizí z obou cest naráz.
Mrtvé rádio mlčí jen na meshi, dock-quiet jen na BLE. Připoj USB a přečti
`idProduct`; `phase2` takové desce nepomůže, protože v UF2 režimu BLE nezapne.

## 2. Vyber cestu podle hardwaru

| deska | cesta | pozn. |
|---|---|---|
| SenseCAP solární (hrebecná, Plešivec, tth-ltm) | **BLE OTA** `tools/ble_flash_node.sh` nebo `ble_dfu.py` | bootloader `AdaDFU`/`SCAP_DFU`, u mast unit na **MAC+1** |
| T1000-E (karta) | `dfu uf2` na USB konzoli → **`adafruit-nrfutil dfu serial`** na CDC port | BLE OTA NENÍ (repeater build BLE nemá); UF2 disk nepotřebuješ. POZOR: „-BOOT" v názvu portu má APLIKACE, bootloader je bez něj |
| XIAO nRF52840 | `tools/xiao_uf2_flash.sh`, nebo `pio run -t upload` (aplikace!) | na desku v bootloaderu ruční nrfutil, `-t upload` ji vytouchuje ven |
| Wio Tracker L1 | 1× krátce RST → UF2 disk `TRACKER L1` | jediná deska, kde UF2 disk fakt naskočí |

**Preferuj cestu, která nepotřebuje root.** Mount UF2 disku chce root a `sudo`
přes SSH bez terminálu se nemá koho zeptat na heslo; na CDC port píše běžný
uživatel. Když UF2 disk mountovat musíš, **nikdy na `/mnt` samotné** — deska se
po zápisu rebootne, mount po ní zůstane viset a `/mnt` je pak mrtvé
(`Input/output error` i na `mkdir`). Náprava: `sudo umount -R -l /mnt`.

## 3. Před flashem

- **Čistý strom.** Špinavý dá verzi s `+` a na stožáru pak nepoznáš, co na uzlu je.
- **Ticho, když je anténa odpojená** (`--require-silence`). Vysílání do
  odpojeného konektoru poškozuje PA. Build je mlčící z konstrukce, ale
  **prefs v InternalFS build flagy přebijou**, takže se ticho ověřuje na uzlu.
- **Uvolni BLE link.** Zastavit službu (most) NESTAČÍ, spojení drží BlueZ:
  `bluetoothctl disconnect <MAC>`. A po párování taky — připojený peripheral
  přestane advertovat.
- **Bond je per-adaptér.** Z jiného hosta se musí `ble_pair.py` znovu.

## 4. Flash

BLE DFU je flaky a **jeden pokus o connect nestačí**. Když fáze 1 padá na
`TimeoutError`, ale `ble_cli.py <MAC> ver` na tomtéž uzlu projde, není to mrtvá
deska ani slabý signál — je to ta flakiness. `ble_dfu.py` má od 22. 8. 2026
retry (`connect_app`, 5 pokusů). **Nediagnostikuj z toho mrtvou desku a nespouštěj
opakovaně `recover_to_bootloader`** — ten po každém pádu posílá RESET, čímž se
domnělá mrtvá deska dá uštvat doopravdy.

MTU rozhoduje o délce: 247 ≈ 3 min, 23 ≈ 15 min. Když to leze k patnácti, nech
to dojet.

## 5. Ověř na desce, ne podle běžící služby

```
ver                       → v1.17.1-tth<sha>, BEZ '+'
get name / get freq       → identita a kmitočet přežily (prefs flash nepřepíše)
get path.hash.mode        → 1 (= 2 bajty, flotilní standard)
get repeat / oba intervaly
```

U uzlu s mostem navíc **data**, ne službu: v Prometheu musí spadnout
`meshcore_uptime_seconds{node="…"}` k nule a most hlásit `full scrape` do
journalu. Hodiny po flashi jdou do května 2024 — `time <epoch>`, jinak klienti
zahazují odpovědi na timestampu.

## 6. Uzel nech ve správném stavu

**Aktivace (`set repeat on`, advert intervaly, `advert`) je krok NA MÍSTĚ, ne
součást flashe.** `tools/provision/activate-cz-mast.txt` se pouští až s antenou
na stožáru; na desce ležící na stole z toho je nepřihlášený repeater, který
mate routing na ostrém CZ meshi. Pro desku, která se někam ponese, je ten správný
soubor `provision/repeater-cz-silent.txt`.

**Jednotky advert intervalů se liší a chyba je tichá:** `set advert.interval`
bere **minuty**, `set flood.advert.interval` **hodiny** (3–168). Špatná jednotka
skončí `Error: interval range is 3-168 hours` a **nechá tam původní hodnotu** —
takže po ztišení zůstane nula a uzel přestane flood adverty posílat úplně. Vždy
`get` zpátky, nespoléhej na `OK`.

## 7. Po sobě ukliď

Ptej se **„co jsem tady změnil"**, ne „co tady ještě běží" — proces může být
dávno mrtvý a stav po něm zůstat. Zkontroluj: vrácené služby (most, exportér),
`findmnt /mnt`, a na hostu, kde se párovalo, `bluetoothctl show | grep -E
"Pairable|Discovering"`. `ble_pair.py` zabitý uprostřed nechával registrovaného
KeyboardOnly agenta, který odpovídá i na CIZÍ párovací požadavky (opraveno
`e5b4572b`, ale na starších kopiích na jiných hostech to platí dál).
