# MeshCore (fork TeTeHacko)

Komunikuj česky.

**Rozcestník domácí infry: `INFRA/INFRA.md`** (symlink na repo `home-scripts`).
Je tam cesta dat od uzlu přes mosty na dopey do analyzeru CoreScope, přehled hostů
a kde má co zdroj pravdy. Mosty na dopey **nejsou v tomhle repu** — jsou
v `home-scripts/meshcore-ble-bridge/`; `/opt/meshcore-ble-bridge` na hostu je jen
deploy cíl.

## Železo a flashování — pravidla, která stála čas

- **Sériové porty VŽDY přes `/dev/serial/by-id/`**, matchuj na **sériové číslo**.
  `ttyACM*` se po každém replugu přečísluje a mířil bys na jinou desku.
- **Nikdy nesahej na `T1000-E-BOOT`.**
- **Do bootloaderu jde `dfu [uf2|serial|ota]`** z konzole (GPREGRET magic, lokální
  jen). 1200baudový touch žádá VÝHRADNĚ serial mode, takže UF2 disk nikdy nepřijde
  — a druhý touch v jedné power session desku vyřadí z USB i BLE bez self-recovery.
- **Flashování XIAO: `tools/xiao_uf2_flash.sh <firmware.uf2> [serial|0..4]`.**
  Odmítá zastaralý obraz, hádání cílové desky a hlásí úspěch až po přečtení verze
  z desky. Kanonický BLE DFU je `tools/ble_dfu.py` — nepiš ho znovu.
- **Build BEZ textové konzole (companion) se flashuje po USB serial DFU:**
  `pio run -e <env> -t upload --upload-port /dev/serial/by-id/...`. XIAO env má
  `upload_protocol = nrfutil`, takže si PlatformIO udělá touch samo a nalije po
  CDC — **36 s, bez tlačítek**. Companion `dfu` příkaz nemá (žádná konzole) a
  dvojklik na x2 opakovaně nezabral; ťukání kvůli tomu stálo čas zbytečně. Touch
  dává serial mode, což je přesně to, co nrfutil chce — potíž nikdy nebyla
  v touchi, ale v čekání na UF2 disk, který v tom režimu nepřijde. Serial DFU
  bylo hluché na **T1000-E**, ne obecně. Pořád platí jeden touch na power
  session: když upload selže, chtěj replug, ne druhý pokus.
- **Wio Tracker L1 má jiný bootloader** — tam touch UF2 disk (`TRACKER L1`)
  naopak DÁ, do ~10 s. Kanonicky `MeshCore-solo/flash-l1.sh`. Ten ale ověřuje jen
  návrat portu, ne verzi, a **port se vrací dřív, než firmware odpovídá**: první
  `meshcli … infos` po flashi selže na „serial companion?" a za dvě minuty projde
  sám. Než z toho uděláš diagnózu, počkej a zkus znovu.
- **Softwarový reset může desku shodit z USB až do replugu** — `reboot` i `dfu`,
  změřeno ~5 z 10. Reset pinem (dvojklik) neselhal ani jednou. Co potřebuješ
  ověřit, ověř PŘED resetem. **Flashuj po jedné a mezi tím kontroluj**; dávka
  tří flashů stála tři replugy. Netlač dál dalším pokusem — k desce, kterou host
  nevidí, se druhý `dfu` nemá jak dostat, chtěj ruce hned.
- **„Deska spadla z USB" NENÍ diagnóza.** Stejný podpis (`USB disconnect` a pak
  nic, bez chyb enumerace, port `not attached`) jde vyrobit i na naprosto zdravé
  desce zásahem na hostu. **Port `disable` jako recovery NEPOUŽÍVAT** — sundá
  zdravou desku bez cesty zpět. Podrobnosti a čísla: `tools/README.md`.
- **Každý flashovaný env MUSÍ mít `${stamped.extra_scripts}`** (`tools/build_version.py`),
  jinak uzel hlásí literál `v1.16.0` stejný ve všech buildech a nepoznáš, co na něm
  doopravdy je. Flashuj z čistého stromu.
- **Základní verzi bere stamping z git tagu, takže si na tagy dávej pozor.**
  Záložní tag `backup/*` před mergem matchoval `*v[0-9]*` a jako nejnovější vyhrál
  → T1000-E hlásí `v117-merge-tth7c50c`. A **upstream release tag nemusí být tvůj
  předek**: v1.17.0 sedí na commitu se stejným stromem jako `origin/main`, ale
  jinou historií, takže ho `git describe` neviděl — dotáhni ho
  `git merge -s ours <tag>` (obsah se nezmění, ověř na tree hashi). Detaily:
  `docs/upstream-v1.17.0.md`.
- **`-U` k přebití děděného `-D` NEFUNGUJE** (SCons dá `-U` až za CPPDEFINES).
  Spoléhej na „poslední `-D` vyhrává" a ověř přes `strings` na ELF.
- **Dock-quiet:** USB drží DTR ⇒ BLE neadvertuje; připojený BLE klient ⇒ USB mlčí.
  Uzel „mrtvý na obou" je většinou zdravý uzel ve špatné kombinaci.
- **Energetika uzlu: `docs/power-saving.md`** (změřená čísla, knoby, pasti měření).
- **Pořadí úspor je změřené, nehádej ho:** GPS **46 mA** (párové A/B na SenseCapu
  5. 8. 2026; datasheetových 25–35 mA bylo nízko) ≫ BLE advertising + blikající
  LED > `powersaving` (MCU spánek) ~jednotky mA > rxps ~2 mA > `rxgain off`
  0,7 mA (za cenu citlivosti ⇒ na stožár NE). `gps duty` z toho ušetří 42–45 mA
  i v případě, že uzel fix nikdy nedostane. Cokoliv jiného řeš až po GPS.
- **Spotřebu solárního uzlu NEMĚŘ přes USB** — měřák tam vidí nabíječku článku
  (500–700 mA, sběrnice klesne na 4,7 V), a uzel sám je 2–4 % z toho na driftující
  hodnotě. Buď počkej, než nabíjení dojde, vytáhni článek, nebo měř přenositelnou
  část na XIAO (P1-Pro má XIAO uvnitř; jen GPS je specifická). **INA226 na desce
  NENÍ** (`i2c` scan: na jediné sběrnici neodpoví nikdo) — `TELEM_INA226_ADDRESS=0x40`
  je jen pojistka proti falešné detekci SHT41 na 0x44. Nářadí: `tools/power_ab.py`.
- **Novou identitu uzlu VŽDY umlít nekolizní** (`tools/gen_node_id.py`). Path hash je
  prefix pubkey a šířku volí ODESÍLATEL paketu, takže kandidátem na hop je každý
  repeater meshe — na 1 bajt jich koliduje 671 z 711. `00`/`ff` nejsou volné, ale
  zakázané (`Identity.cpp:56`). Mletí je zadarmo a uzel, který ještě nevysílal, nemá
  co ztratit.
- **`prv.key` NENÍ `seed‖pubkey`**, ale clampnutý SHA-512(seed) (`lib/ed25519/keypair.c`)
  — posledních 32 bajtů tedy není veřejný klíč, i když tak vypadá. Odvození:
  `tools/gen_node_id.py --derive`. A klíč **neposílej nástrojem, který echuje příkaz**;
  konzole ho echuje taky a skončí ve výpisu.

## Secrety

Skutečný BLE PIN a admin heslo žijí **výhradně v gitignorovaném
`platformio.local.ini`**. Do committed souborů se nepíšou.

## Cizí uzly a záměrné odchylky

Runtime prefs na cizím uzlu **neměň bez potvrzení**. Některé konfigurace jsou
odlišné SCHVÁLNĚ (např. 3bajtový path hash na jednom uzlu kvůli testu) — odchylka
není automaticky chyba.

Na ostrém komunitním CZ meshi (869.432) uzel před přeladěním **ztiš**:
`set repeat off`, `set advert.interval 0`, `set flood.advert.interval 0`.
Nepřihlášený repeater tam mate routing. Na diagnostický poslech je lepší
`tempradio <freq>,<bw>,<sf>,<cr>,<min>` — aplikuje se hned bez rebootu, nezapisuje
prefs a sám se po N minutách vrátí.
