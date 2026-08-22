# MeshCore (fork TeTeHacko)

Komunikuj česky.

**Rozcestník domácí infry: `INFRA/INFRA.md`** (symlink na repo `home-scripts`).
Je tam cesta dat od uzlu přes mosty na dopey do analyzeru CoreScope, přehled hostů
a kde má co zdroj pravdy. Mosty na dopey **nejsou v tomhle repu** — jsou
v `home-scripts/meshcore-ble-bridge/`; `/opt/meshcore-ble-bridge` na hostu je jen
deploy cíl.

## Železo a flashování — pravidla, která stála čas

**Flashuješ, děláš DFU, nebo uzel po zásahu nereaguje? Načti si skill
`flash-node`** (`.claude/skills/flash-node/SKILL.md`). Tenhle seznam je pozadí,
skill je postup — 22. 8. 2026 se ukázalo, že mít pravidla v kontextu nestačí,
když se podle nich neuvažuje v momentě úkonu.

**Ptáš se „prolezlo to?", „slyšel nás někdo?", „kudy to šlo?" — načti si skill
`mesh-evidence`.** Klíčové pravidlo: **pozoruj na tom konci trasy, kde paket
KONČÍ.** 22. 8. 2026 jsem prohlásil letový test za neúspěšný na základě správného
měření na chatě — jenže trasa byla `L1 → Klínovec → mesh → tth-ltm → domů`
a chata v ní nebyla. Důkaz ležel doma v RemoteTermu.

**Aktivace uzlu (`set repeat on`, advert intervaly, `advert`) patří NA MÍSTO
s antenou, nikdy na lavici.** `provision/activate-cz-mast.txt` to má dvakrát
v hlavičce a i tak se to stalo: z desky ležící na stole se stal nepřihlášený
repeater vysílající do ostrého CZ meshe. Pro přenášenou desku je ten správný
soubor `provision/repeater-cz-silent.txt`.


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
  v touchi, ale v čekání na UF2 disk, který v tom režimu nepřijde. Pořád platí
  jeden touch na power session: když upload selže, chtěj replug, ne druhý pokus.
- **„Serial DFU je na T1000-E hluché" platí jen Z APLIKACE, ne z bootloaderu.**
  Doteď tu stálo prosté „hluché na T1000-E" a odvádělo to od cesty, která funguje:
  desce, která už v bootloaderu je (`dfu uf2` z konzole), vystaví UF2 bootloader
  CDC port a `adafruit-nrfutil dfu serial --singlebank` na něj nalije obraz —
  změřeno 22. 8. 2026 na probe kartě, 329 848 B za **20 s**. Je to navíc cesta,
  kterou volit PŘEDNOSTNĚ před UF2 diskem: mount disku chce root a `sudo` přes SSH
  bez terminálu se nemá koho zeptat na heslo, kdežto na CDC port píše běžný uživatel.
- **`pio ... -t upload` patří na desku v APLIKACI, ne v bootloaderu.** Touch dělá
  vždycky, takže desku, která už v DFU je, jím vykopneš ven (`Couldn't find a
  board`) — a je to zároveň ten zakázaný druhý touch. Na desku v bootloaderu
  posílej ruční nrfutil, ten netouchuje:
  `PYTHONPATH=~/.platformio/packages/tool-adafruit-nrfutil/site-packages
  ~/.platformio/penv/bin/python
  ~/.platformio/packages/tool-adafruit-nrfutil/adafruit-nrfutil.py dfu serial
  -pkg .pio/build/<env>/firmware.zip -p <port> -b 115200 --singlebank`.
  Režim poznáš z `idProduct` (`8044` aplikace, `0044` bootloader) a podle toho,
  že bootloader se na USB hlásí bez „Studio" v názvu portu.
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
- **Stamping verze má od 19. 8. 2026 `[nrf52_base]`, takže ho dědí každý nRF52 env.**
  Předtím ho z 585 envů měly DVA a deska na lavici hlásila literál z upstream headeru
  — nešlo poznat, který build na ní je. T1000-E vypadal správně jen proto, že se
  flashuje z lokálního envu. Ověřuj to na desce (`ver` musí dát
  `v1.17.1-tth<sha>`, `+` = špinavý strom), ne v ini. Flashuj z čistého stromu.
- **PREFS V InternalFS PŘEBÍJEJÍ BUILD FLAGY, a mlčí o tom.** Flash nastavení
  nepřepíše — build default platí jen pro uzel BEZ prefs (čerstvý nebo po `erase`).
  Stálo to 19. 8. 2026 hodinu na lavici, protože se to sečetlo do jednoho symptomu
  „deska slyší adverty, ale na dotazy neodpovídá":
  - **`path_hash_mode`**: x4 měl 1 (2 bajty), x2 pořád 0 (1 bajt) i po flashi
    s `PATH_HASH_MODE_DEFAULT=2`. Broadcast advert projde, adresovaný REQ ne.
    Řeší se na kontaktu: `update_contact(c, path="", path_hash_mode=1)`.
  - **`ADMIN_PASSWORD`**: x4 si pamatoval STARÉ heslo z doby před zkrácením na
    15 znaků, takže login tiše selhával. Napravit jde jen z konzole: `password <nové>`.
  Praktický důsledek pro mlčení uzlu: `sensecap_prod_base` je mlčící z konstrukce
  (`ENABLE_ADVERT_ON_BOOT=0`, oba advert intervaly 0, `DISABLE_FWD_DEFAULT=1`), ale
  na uzlu, který už prefs má, to NEZARUČÍ nic — u desky s odpojenou antenou si
  ticho ověř přes `get advert.interval`, `get flood.advert.interval`, `get repeat`.
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
  LED > `powersaving` (MCU spánek) ~jednotky mA > rxps ~2 mA > `set radio.rxgain off`
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
