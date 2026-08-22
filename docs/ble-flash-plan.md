# Ostrý flash přes BLE: tth-ltm, tth-plesivec-abertamy, tth-hrebecna

Plán z 19. 8. 2026. Cílem je dostat na tři produkční SenseCapy aktuální build
(`v1.17.1-tth<sha>`) — nese rozšířený RF status s `tx_start_fail`/`tx_timeout`
a stamping verze, takže **poprvé půjde z uzlu poznat, který build na něm je**.

## Pořadí a proč

1. **hrebecna + Plešivec** (na lavici, anténa odpojená, míří do provozu)
2. **tth-ltm** (na balkoně, v provozu, na ostrém CZ meshi, krmí Grafanu)

Obráceně to nedělej: procedura se má nejdřív protočit na uzlech, u kterých
selhání nic neshodí. tth-ltm je jediný z těch tří, jehož výpadek je vidět
v dashboardu.

## Předpoklad, který je potřeba splnit dřív než cokoliv jiného

**hrebecna i Plešivec advertují** a jsou z black-archu připravené k OTA
(19. 8. 2026, 45s scan): oba stabilně na **−60 dBm**, bondované. tth-ltm ve
scanu z black-archu není a nebude — je u dopey, flashuje se odtud.

POZOR na metodu, stálo to falešný poplach: `bluetoothctl scan on | grep` **nic
nenajde**, protože objevy vypisuje průběžně a jinak, než čekáš. Skenuj přes
bleak s callbackem:

```python
from bleak import BleakScanner
s = BleakScanner(detection_callback=lambda d, ad: print(ad.rssi, d.address, d.name))
await s.start(); await asyncio.sleep(45); await s.stop()
```

Když uzel opravdu nikde není, nejčastější důvod je **dock-quiet** (připojené USB
drží DTR ⇒ BLE neadvertuje — vlastnost, ne závada) nebo vypnuté napájení.

## Nejjednodušší cesta: `tools/ble_flash_node.sh`

Celý postup níž je zabalený do jednoho příkazu, včetně věcí, které se dají
zapomenout (čistý strom, předletová kontrola prefs, uvolnění linku, ověření
verze proti obrazu, vrácení služby). Ověřeno 19. 8. 2026 na hrebecné, Plešivci
a tth-ltm.

```bash
# lavice / uzel s ODPOJENOU antenou
tools/ble_flash_node.sh --env SenseCap_hreb_rpt --mac CE:72:14:CD:FD:02 --require-silence

# tth-ltm: BLE má metry, takže se flashuje Z DOPEY
tools/ble_flash_node.sh --env SenseCap_Solar_repeater_ble --mac FA:4F:30:E3:1B:7A \
    --via dopey.doma --python /opt/meshcore-ble-bridge/venv/bin/python \
    --stop-service meshcore-ble-bridge.service

# jen se podívat, co na uzlu je (nic nemění)
tools/ble_flash_node.sh --mac DC:28:1D:8A:04:1A --check-only
```

Co skript dělá a co ne:

- **Odmítne špinavý strom** — verze by dostala `+` a na stožáru bys nepoznal, co
  na uzlu je.
- **Porovná prefs před a po** (`repeat`, oba advert intervaly). Flash je
  nepřepisuje, takže rozdíl znamená, že se něco stalo.
- `--require-silence` **zastaví celý flash**, pokud uzel není tichý. U desky
  s odpojenou antenou je to ta jediná pojistka, která fakt drží.
- **Službu vrací traphem**, ne až na konci — když spadne build nebo DFU, most
  se nesmí nechat dole.
- Na `--via` hostu **nic neinstaluje**: použije python, který tam už je
  (u dopey `venv` mostu, kde bleak je).
- **Neaktivuje uzel.** `set repeat on` a advert intervaly jsou krok na místě.

Zbytek téhle stránky je to samé ručně, plus proč jednotlivé kroky existují.

## Nástroj a build

```bash
# build (z ČISTÉHO stromu, jinak bude ve verzi '+')
pio run -e SenseCap_hreb_rpt        # nebo _ples_rpt / SenseCap_Solar_repeater_ble
# DFU balíček je .pio/build/<env>/firmware.zip

tools/ble_dfu.py .pio/build/<env>/firmware.zip <BLE-MAC>
tools/ble_dfu.py .pio/build/<env>/firmware.zip <BLE-MAC> phase2   # zotavení
```

`ble_dfu.py` je kanonický, nepiš ho znovu. Fáze 1 překlopí běžící aplikaci do
bootloaderu (buttonless), fáze 2 přenese obraz a sama se retryuje — po pádu
pošle RESET a zkusí to znovu, **bez power-cycle**. `phase2` použij, když uzel
už v bootloaderu visí (advertuje `SCAP_DFU`) po přerušeném přenosu.

**MTU rozhoduje o délce**: s velkým MTU je to ~3 min, s malým ~15. Když to
leze k patnácti, nech to dojet, nepřerušuj.

| uzel | BLE MAC | env |
|---|---|---|
| tth-ltm | `FA:4F:30:E3:1B:7A` | `SenseCap_Solar_repeater_ble` (má v sobě identitu stožáru) |
| tth-plesivec-abertamy | `DC:28:1D:8A:04:1A` | `SenseCap_ples_rpt` |
| tth-hrebecna | `CE:72:14:CD:FD:02` | `SenseCap_hreb_rpt` |

## hrebecna a Plešivec (anténa ODPOJENÁ)

**Hlavní riziko není DFU, ale vysílání do odpojené antény** — to poškozuje PA.
BLE OTA samo je bezpečné (jiná anténa, jiné rádio), problém je, co uzel udělá
po rebootu.

`sensecap_prod_base` je mlčící z konstrukce: `ENABLE_ADVERT_ON_BOOT=0`,
`ADVERT_INTERVAL_DEFAULT=0`, `FLOOD_ADVERT_INTERVAL_DEFAULT=0`,
`DISABLE_FWD_DEFAULT=1`. **Ale to jsou build defaulty a prefs je přebijou** —
platí jen pro uzel bez prefs. Tyhle dva prefs mají, takže:

1. **Před flashem** si přes BLE konzoli ověř a případně vynuť ticho:
   ```
   get advert.interval        → musí být 0
   get flood.advert.interval  → musí být 0
   get repeat                 → musí být off
   ```
   Když ne: `set advert.interval 0`, `set flood.advert.interval 0`, `set repeat off`.
2. Flash (`ble_dfu.py`).
3. **Po flashi totéž zkontroluj znovu** a **nespouštěj `advert`**. Ten příkaz
   vysílá okamžitě.
4. `ver` musí dát `v1.17.1-tth<sha>` — to je akceptační test, že na uzlu je
   build, který si myslíš.
5. `time <epoch>` — po flashi jde RTC do května 2024 a klienti pak zahazují
   odpovědi na timestampu.

Aktivace (anténa, `set repeat on`, advert intervaly) je **samostatný krok na
místě**, ne součást flashe.

Fallback: oba jsou na lavici, takže když BLE selže, je tu USB DFU. Počítej
s tím, že SenseCap se po DFU re-enumeruje 60–300 s — nesahej na to dřív.

## tth-ltm (v provozu)

Tady je navíc most, který drží BLE spojení, a **zastavit službu NESTAČÍ** —
spojení drží BlueZ:

```bash
ssh dopey.doma 'systemctl stop meshcore-ble-bridge.service'
ssh dopey.doma 'bluetoothctl disconnect FA:4F:30:E3:1B:7A'
```

Flashuj **z dopey** (je v BLE dosahu, bond tam je). Uzel je na balkoně, tedy
fyzicky dosažitelný — proto je tohle akceptovatelné riziko a ne výprava na
stožár.

Po flashi:

1. `ver` → `v1.17.1-tth<sha>`
2. ticho ověřovat nemusíš (anténa je připojená a uzel MÁ vysílat), ale zkontroluj
   `get repeat` → `on`, jinak přestane přeposílat.
3. `time <epoch>` jako výš.
4. `systemctl start meshcore-ble-bridge.service` na dopey.
5. **Ověř data, ne jen službu**: v Grafaně musí naskočit `up{job="meshcore_map"}`
   a svěží `meshcore_*` s `node="tth-ltm.meshcore.cz"`. Most sám hlásí
   `full scrape: N sousedů | stats: {...}` do journalu.
6. Bonus, který tím zapneš: RF status začne nést `tx_start_fail`/`tx_timeout`.
   Konzument je ještě nečte — parser knihovny se u 56 B zastaví, potřebuje
   těch šest řádků z `docs/` (viz commit `ec20d77e`).

Během výpadku mostu bude v dashboardu díra. To je čekané.

**Provedeno 19. 8. 2026**: `v1.17.1-ttha0f3ce9` → `v1.17.1-ttha29ed62`
(Build: 260819 2121), DFU 408388/408388 B, IMAGE i VALIDATE ok. Prefs přežily
(`repeat on`, `flood.advert.interval 25`, jméno i `freq 869.432`), hodiny
19:22 UTC. Po nahození mostu `full scrape` do dvou minut a v Prometheu spadl
`meshcore_uptime_seconds` z 370279 na 105 — tedy ověřeno až v datech, ne jen
podle běžící služby.

## Co se může pokazit a co s tím

| symptom | co to je | co dělat |
|---|---|---|
| scan nic nevidí | dock-quiet (USB drží DTR) nebo uzel nejede | vytáhnout USB, power-cycle |
| DFU spadne v půlce | dropnutá receipt notifikace, BLE DFU je flaky | `ble_dfu.py … phase2`, bez power-cycle |
| uzel advertuje `SCAP_DFU` | visí v bootloaderu | totéž — `phase2` |
| `ver` hlásí literál bez `-tth` | flashnul jsi upstreamový env | přeflashovat správným lokálním envem |
| po flashi mlčí i s antenou | prefs (`repeat off`, intervaly 0) přežily flash | `set repeat on`, nastavit intervaly |
| login z jiného uzlu tiše selhává | v prefs je STARÉ `ADMIN_PASSWORD` | z konzole `password <nové>` |

## Aktivace na místě — provedeno na hrebecné 21. 8. 2026

Uzel je na chatě u Hřebečné, tedy **z black-archu nedosažitelný**. BLE dosah na
něj má `sneezy.chata`, takže konzole jde odtamtud:

```bash
# na sneezy: bleak + obě BLE utility (venv, ne systémový python)
python3 -m venv /opt/meshcore-tools/venv && /opt/meshcore-tools/venv/bin/pip install bleak
scp tools/ble_cli.py tools/ble_pair.py sneezy.chata:/opt/meshcore-tools/tools/
# PIN se NIKDY nevypisuje — jen se propíše rourou do souboru 0600 vedle tools/
grep -E '^\s*ble_pin\s*=' platformio.local.ini | \
  ssh sneezy.chata 'umask 077; { echo "[secrets]"; cat; } > /opt/meshcore-tools/platformio.local.ini'
```

**Bond je per-adaptér.** Uzel byl bondovaný s black-archem, sneezy je pro něj
cizí adaptér — takže znovu `ble_pair.py`, a pak `bluetoothctl disconnect`, jinak
uzel neadvertuje a `ble_cli.py` ho nenajde.

```bash
ssh sneezy.chata 'python3 /opt/meshcore-tools/tools/ble_pair.py CE:72:14:CD:FD:02'
ssh sneezy.chata 'bluetoothctl disconnect CE:72:14:CD:FD:02'
ssh sneezy.chata '/opt/meshcore-tools/venv/bin/python /opt/meshcore-tools/tools/ble_cli.py \
    CE:72:14:CD:FD:02 -f /opt/meshcore-tools/tools/verify-cz-silent.txt'     # předletová
ssh sneezy.chata '... ble_cli.py CE:72:14:CD:FD:02 -f .../activate-cz-mast.txt'
```

`ble_pair.py` potřebuje jen stdlib a `bluetoothctl`, takže běží systémovým
pythonem; `ble_cli.py` chce bleak, tedy ten venv.

Naměřeno: `ver` = `v1.17.1-ttha29ed62`, prefs přesně jak je nechal flash
(`repeat off`, oba intervaly 0, `af 9`, `powersaving on`, `gps duty/prefs`,
`path.hash.mode 1`), hodiny **správné bez zásahu** (`clock` na minutu sedělo
s UTC, přestože `gps` hlásí `duty, asleep, no fix` — fix tedy někdy předtím
proběhnout musel). Po aktivaci `repeat on / 120 / 25`.

**Ověřuj to na cizím rádiu, ne na counterech uzlu.** Svědek byl openHop observer
`tth-ob2`, který stojí na téže chatě:

- ruční `advert` → ob2 ho zachytil jako ADVERT (`packet_type 4`, RSSI −14 dBm,
  SNR 12,2), v raw je čitelné `tth-hrebecna` a payload začíná pubkey `5361aa08…`;
- za dalších 5 minut ob2 viděl **tři cizí flood TXT dvakrát**: jednou od
  původce (0 hopů, RSSI −48) a hned nato tutéž `hash` s cestou `5361` na začátku
  — to je hrebecna, jak je přeposílá. `stats-packets` to potvrdil z druhé
  strany: `flood_tx` +3;
- CoreScope si ji do minuty založil jako `repeater 5361aa0892…`, `hash_size 2`.
  Prefix `5361` je proti 887 známým uzlům nekolizní.

**Pozice: 22. 8. 2026 přenastavena na 50.3750/12.8290.** Do té doby uzel
advertoval 50.382/12.827, vedené v poznámkách jako záměrně hrubá poloha ~1 km.
Jenže je to pouhé zaokrouhlení skutečných souřadnic chaty na 4 desetinná místa,
tedy **36 m od domu**, a na mapě to sedělo přímo na něj. **Anonymita musí
vzniknout POSUNEM, ne zaokrouhlením.** Nová hodnota leží v Polesí mezi Hřebečnou
a Abertamy, mimo silnice i budovy, 826 m od chaty. Mění se za běhu po meshi
(`set lat` / `set lon` zapisují rovnou do prefs, bez rebootu), ale patří i do
`ADVERT_LAT`/`ADVERT_LON` v envu — jinak to příští flash vrátí zpátky.

**Čím uzel zatím NENÍ**: `neighbors` hlásí jediného souseda, `025C68B0` (tth-ltm)
ve stáří 20 h — což je ještě z lavice v Litoměřicích. Z chaty přímého souseda na
ostrém meshi zatím nemá. Ten −48 dBm původce je podle úrovně signálu blízko
(22 dBm a volný prostor dá −48 dBm někde kolem 300 m; na 20 km by to bylo −89),
takže je to nejspíš něčí lokální uzel, ne cesta do sítě. **Dokud nepůjde nahoru
Plešivec, počítej s tím, že hrebecna může být ostrov** — repeat na ní pak nemá
co přeposílat směrem k meshi.

### Sonda dosahu: dva detektory, žádné nové nářadí

Po aktivaci se hodí vědět, jestli uzel někam dosáhne, aniž bys musel slyšet
protistranu. Recept:

```bash
# 1. detektor: co je slyšet na místě (observer publikuje syrové pakety)
mosquitto_sub -h stor.grg -p 1883 -t 'meshcore/<IATA>/+/packets' -v

# 2. detektor: slyšel nás někdo VENKU? (bez tokenu, stejné API jako CoreScope)
curl -s 'https://analyzer.meshcore.cz/api/nodes?limit=5000'

ble_cli.py <MAC> -w 45 -f <(printf 'advert\nadvert\nadvert\n')   # tři floody
```

Druhý detektor je ten cenný: **komunitní analyzer chytne i případ, kdy náš
advert někdo přepošle dál, ale zpátky k nám to nedoletí.** Když se uzel po
sérii advertů v `/api/nodes` neobjeví, nikdo z MQTT-přemostěných uzlů ho
neslyšel. Naopak `last_relayed` + `relay_count_1h` u cizího uzlu dokazují, že
**žije a přeposílá**, i když je jeho vlastní `last_heard` den starý (u
`flood.advert.interval 25` je to normální).

Cestu poznáš z `raw_hex`: druhý bajt je `path_length` — spodních 6 bitů je
počet hopů, horní dva `šířka hashe − 1`. Paket s `hops ≥ 1` je vždycky
retransmise, originál má 0.

**Změřeno 21. 8. 2026 na hrebecné (anténa na půdě pod střechou):** tři adverty,
**nula přeposlání, v komunitním analyzeru se neobjevila**. Sedí to s DEM
analýzou z července — chata → Klínovec je blokovaná hřbetem 1012 m, spoj vede
46 m pod terénem (≈ −1,6 F1, tedy ~20 dB difrakce navíc k 111 dB na 10,1 km).
Výkonově by ta trasa byla pohodlná; **problém je čistě terén** a nespraví ho
ani anténa na střeše. Odemyká to až repeater na Plešivci (3,07 km z chaty,
LOS 169 % F1).

## Co v tomhle plánu záměrně není

- **Změna identity ani jména.** Envy je mají zadrátované; `SenseCap_Solar_repeater_ble`
  nese identitu stožáru, takže ho nepoužívej na nic jiného.
- **Zapnutí vysílání na Plešivci.** To je aktivace na místě, viz sekci výš —
  pro hrebecnou je hotová, Plešivec čeká na výlez.
- **Flash x1.** Je zaseklá po flashi upstreamovým envem (přišla tím o BLE konzoli)
  a chce ruce: dvojklik na reset nebo replug, pak `pio run -e Xiao_x1_rpt -t upload`.
