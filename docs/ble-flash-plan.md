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

Ani jeden z těch tří **teď po BLE neadvertuje** (25s scan z black-archu, 19. 8.
19:xx — nula nálezů). Všechny tři jsou spárované a bondované, RSSI v cache je
jen historie (hrebecna −68, Plešivec −62). Nejpravděpodobnější důvody:

- **dock-quiet**: připojené USB drží DTR ⇒ BLE neadvertuje (je to vlastnost, ne
  závada — viz AGENTS.md). Na lavičních uzlech tedy **vytáhni USB**.
- uzel je vypnutý / bez napájení.

Bez advertování nemá `ble_dfu.py` na co se připojit, takže **krok 0 každé
sekce je scan**:

```bash
timeout 30 bluetoothctl --timeout 20 scan on | grep -i -E 'hrebecna|plesivec|ltm'
```

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

## Co se může pokazit a co s tím

| symptom | co to je | co dělat |
|---|---|---|
| scan nic nevidí | dock-quiet (USB drží DTR) nebo uzel nejede | vytáhnout USB, power-cycle |
| DFU spadne v půlce | dropnutá receipt notifikace, BLE DFU je flaky | `ble_dfu.py … phase2`, bez power-cycle |
| uzel advertuje `SCAP_DFU` | visí v bootloaderu | totéž — `phase2` |
| `ver` hlásí literál bez `-tth` | flashnul jsi upstreamový env | přeflashovat správným lokálním envem |
| po flashi mlčí i s antenou | prefs (`repeat off`, intervaly 0) přežily flash | `set repeat on`, nastavit intervaly |
| login z jiného uzlu tiše selhává | v prefs je STARÉ `ADMIN_PASSWORD` | z konzole `password <nové>` |

## Co v tomhle plánu záměrně není

- **Změna identity ani jména.** Envy je mají zadrátované; `SenseCap_Solar_repeater_ble`
  nese identitu stožáru, takže ho nepoužívej na nic jiného.
- **Zapnutí vysílání na hrebecné/Plešivci.** To je aktivace na místě.
- **Flash x1.** Je zaseklá po flashi upstreamovým envem (přišla tím o BLE konzoli)
  a chce ruce: dvojklik na reset nebo replug, pak `pio run -e Xiao_x1_rpt -t upload`.
