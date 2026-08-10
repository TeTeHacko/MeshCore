# Merge upstreamu v1.17.0 a rozvoz na flotilu (9.–10. 8. 2026)

Záznam toho, co merge změnil na chování, jak se konflikty rozhodly a co při
rozvozu na uzly vylezlo. Cílem není historie commitů (ta je v gitu), ale ty věci,
které by se jinak musely znovu vypátrat.

## Který tag je vlastně "v1.17.0"

Oficiální MeshCore verzuje **per-role**: `companion-v1.17.0`, `repeater-v1.17.0`,
`room-server-v1.17.0`. Všechny tři sedí na commitu `727fc051`.

Tagy `v1.23`, `v1.23.1`, `v1.24` v tomhle repu **nejsou z oficiálního MeshCore**,
ale ze Solo forku (MarekZegare4) — jiná verzovací linie. Nezaměňovat.

Podstatné: strom `727fc051` je **bit-identický** s `origin/main` (`efc527a3`),
jen historie je jiná (upstream si `main` skládá tak, že commity z `dev` v něm
nejsou dosažitelné). Cíl merge je proto prostě `origin/main`.

## Rozsah a proč merge, ne rebase

Náš základ byl `bbb58cce` (7. 7. 2026), tedy **350 souborů / +11 263 / −1 640**
upstream změn. Vlastních commitů 26 na `main` a dalších 61 na
`feat/ble-diag-and-2b-path-hash`.

Zvolen **merge**: 87 commitů opakovaně sahá do `Mesh.cpp` a `CommonCLI.cpp`, takže
rebase by tentýž konflikt řešil pořád dokola, a historie je pushnutá na `fork`
i `rfa` — rebase by ji přepsal pod rukama. Reálná cena merge byla **12 hunků /
~326 řádků** v 9 souborech.

Zpětné tagy: `backup/main-pre-v117-merge`, `backup/feat-ble-diag-pre-v117-merge`.

## Jak se rozhodly konflikty

| soubor | rozhodnutí |
|---|---|
| `src/Mesh.cpp` | Upstream rozdělil dedup na `wasSeen()` + explicitní `markSeen()`. Vzato **obojí**: jejich nová dvojice **a** náš bounds-check na čtení path hashe z TRACE. |
| `examples/companion_radio/main.cpp` | Upstream si napsal vlastní `MultiSerialInterface` — nadmnožina našeho `SerialDualInterface` (backport PR #1779). Vzat jejich celý. |
| `ui-new/UITask.cpp` | `isSerialEnabled()` se upstream jmenuje `isBluetoothEnabled()`, `DisplayDriver::RED` → `UIColor::warning_txt`. Záměr stejný: BT pin na displeji jen když je Bluetooth zapnutý. |
| `src/helpers/NRF52Board.cpp` | Upstream nezávisle došel ke **stejnému** `sd_temp_get()` fixu pro SoftDevice-chráněný TEMP. Vzata jejich forma, ponechán náš komentář — je to jediný záznam **proč** (APP_MEMACC fault → reboot při telemetrii). |
| `EnvironmentSensorManager.cpp` | Upstream opravil přetečení `millis()` na GPS timeru přesně v řádku, kde sedí náš `gpsDutyLoop()`. Obojí, s jejich wrap-safe porovnáním. |
| `RadioLibWrappers.{h,cpp}`, `simple_repeater/main.cpp` | add/add. Jejich `configSideDetectors()`/`performChannelScan()`/external watchdog vedle našeho `irqDoneMask()`/`probeDio1()`/BLE pumpy/WDT feedu. |
| `.vscode/extensions.json` | Upstream smazal a je v `.gitignore`; naše změna byl jen komentář. Smazáno. |

## Co to udělalo s USB companionem (regrese, kterou merge přinesl)

`MultiSerialInterface` registruje BLE + USB + WiFi + Ethernet vedle sebe, ale
upstream envs `*_companion_radio_ble` **USB nezapínají** — kdežto náš backport ho
tam přidával zdarma. Důsledek: **BLE companion buildy po merge nemají USB
konzoli.** Projeví se jako

```
$ meshcli -s /dev/serial/by-id/… infos
ERROR: Are you sure your node is a serial companion ?
```

Týká se `T1000E_cmp_cz`, který je teď dostupný jen po BLE. Vrací to
`-D ENABLE_USB_INTERFACE`, ale **upstream se chová jinak než náš dual**:

* `writeFrame()` **broadcastuje rámec na všechna aktivní rozhraní**, takže USB
  klient vidí i odpovědi určené BLE klientovi (náš dual si pamatoval, odkud dotaz
  přišel),
* **dock-quiet nemá vůbec.**

`src/helpers/SerialDualInterface.h` je proto ponechán ve stromu jako mrtvý kód —
ta logika je v něm, kdyby ji bylo potřeba vrátit.

## Stamping verzí: dvě pasti, obě opravené

`tools/build_version.py` skládá verzi z nejnovějšího upstream tagu, ze kterého
HEAD vychází. Při tomhle mergi to selhalo dvakrát:

1. **`backup/*` tagy stínily upstream.** `backup/feat-ble-diag-pre-v117-merge`,
   zakládaný jako pojistka minutu před mergem, matchoval `*v[0-9]*` a jako
   nejnovější tag vyhrál. Proto T1000-E nalitý ten večer hlásí
   `v117-merge-tth7c50c` — sha je správná, base je jméno tagu. Pojistka nikdy
   nesmí pojmenovat firmware, který chránila. Přidán `--exclude 'backup/*'`.
2. **`tag.find("v")` chytilo „ser*v*er"** v `room-server-v1.17.0` → base
   `ver-v1.17.0`. Dřívější upstream tagy (`companion-`, `repeater-`) žádné „v"
   v prefixu neměly, takže to vylezlo až teď. Teď `re.search(r"v[0-9]")`.

A do třetice: **release tag nebyl náš předek**, takže `git describe` v1.17.0
neviděl vůbec a spadl na cokoli dalšího. Dotaženo přes

```
git merge -s ours companion-v1.17.0     # obsah se nezmění ani o bajt
```

`-s ours` je tady správný nástroj: obsahová otázka je vyřešená (náš strom přišel
z `origin/main`, který má stejný strom jako tag), jde jen o to, aby `describe`
release viděl. Ověřeno porovnáním tree hashe před a po.

## Rozvoz na flotilu

Postaveno z čistého stromu, verze `v1.17.0-tth4c9b4b9`, flashováno **po jedné
a s kontrolou mezi tím**.

| uzel | env | cesta |
|---|---|---|
| x0 `tth-x0` | `Xiao_x0_rpt` | `dfu uf2` z konzole → `xiao_uf2_flash.sh` |
| x1 `tth-x1` | `Xiao_x1_bot` | totéž |
| x4 `tth-x4` | `Xiao_x4_rpt` | totéž |
| x2 `tth-x2-cmp` | `Xiao_x2_cmp` | **serial DFU** (`pio run -t upload`) |
| x3 `tth-x3` | `Xiao_x3_obs` | UF2 na dopey, přes SSH |
| tth-ltm | `SenseCap_Solar_repeater_ble` | BLE OTA z dopey |
| TTH-L1 | `WioTrackerL1_companion_solo_dual` (Solo, `v1.24-tth753df1b`) | `flash-l1.sh` |

Hodiny srovnány všude (`xiao_uf2_flash.sh` to dělá sám, jinak `time <epoch>` /
`meshcli clock sync`).

### Co při rozvozu překvapilo

* **x2 (companion) nešla dvojklikem**, a nemá `dfu`, protože nemá textovou
  konzoli. Cesta je `pio run -e Xiao_x2_cmp -t upload --upload-port …` — serial
  DFU přes `tool-adafruit-nrfutil`, **36 s na první pokus**. Viz `AGENTS.md`.
* **Vzdálené uzly: zastavit most nestačí.** U tth-ltm po
  `systemctl stop meshcore-ble-bridge` hlásil `ble_cli.py` pětkrát
  `not advertising (already connected?)`, protože spojení držel dál BlueZ na dopey
  (bonded+trusted). Rozadvertoval ho až `bluetoothctl disconnect <MAC>`.
* **Lokální `bluetoothctl devices` není dosah.** tth-ltm se z cache vypisoval
  i tady, ale scan ho nenašel — pozná se podle chybějícího RSSI. Flashovat se
  musel z dopey.
* **Délka BLE DFU závisí na MTU, ne na velikosti obrazu.** tth-ltm: 407 kB za
  ~3 min (~244 B chunky). T1000-E: 372 kB za ~14 min (MTU 23 → 20 B chunky).
* **`scp` bez `-p` rozbije staleness guard.** `xiao_uf2_flash.sh` odmítne obraz
  starší než `firmware.elf`; bez `-p` dostanou soubory čas přenosu v pořadí
  kopírování a `.elf` vyjde novější. Správně je `scp -p`.
* **Most si reboot uzlu pozná sám** a přehraje ring od nuly
  (`cursor > newest — uzel rebootoval (seq reset)`), takže se sběr neztratí.

## Co zůstalo otevřené

* `T1000E_cmp_cz` běží `v117-merge-tth7c50c` (nekonvenční base) a **bez USB
  companionu**. Oprava obojího = reflash, tj. ~15 min OTA.
* **`main` stampuje verzi po staru.** Má `variants/sensecap_solar/build_version.py`
  s hardcoded literálem `v1.16.0` — přesun do `tools/`, base z git tagu i oba
  dnešní fixy jsou jen na `feat/ble-diag-and-2b-path-hash`. Build z `main` by se
  tedy hlásil jako `v1.16.0-tth-<sha>`, což je od téhle chvíle lež. Release tag
  je na `main` dotažený (`-s ours`), aby ho `describe` viděl, ale samotný stamping
  se srovná až sloučením s feature větví. **Flashuj z feature větve.**
* `MeshCore-solo/flash-l1.sh` je netrackovaný a ověřuje jen návrat portu, ne
  verzi. Stálo by za to dotáhnout ho do gitu a přidat čtení verze, jak to dělá
  `xiao_uf2_flash.sh`.
* `build.sh` v Solo repu dělá `rm -rf out` **před** validací argumentů, takže
  spadne na chybějící `FIRMWARE_VERSION` a stejně ti smaže předchozí artefakt.
