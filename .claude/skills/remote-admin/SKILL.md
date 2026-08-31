---
name: remote-admin
description: Spravovat uzel, který není fyzicky na dosah (chata, stožár, balkon) — číst/měnit konfiguraci přes RF nebo přes BLE cizího hosta, přihlásit se, srovnat hodiny, a nezavřít si poslední dveře. Použij VŽDY, když se má na dálku poslat get/set/advert/reboot, rotovat heslo, zapínat/vypínat BLE, nebo se jen "zeptat uzlu, jak se má".
---

# Vzdálená správa uzlů

## Cesty k uzlům (stav 31. 8. 2026)

| uzel | primární cesta | záložní | pozn. |
|---|---|---|---|
| tth-hrebecna (chata, aktivní RPT) | RF: `mesh_admin.py 5361aa08` přes sneezy:5012 | BLE ze sneezy (`ble_cli.py CE:72:14:CD:FD:02`, bond ✔, BLE on) | BLE = lokální konzole ⇒ jdou i `stats-*` |
| tth-plesivec (stožár) | RF: `mesh_admin.py 5f14d8c9` přes sneezy:5012 | BLE jen fyzicky na místě | BLE pref on |
| tth-ltm (balkon doma) | BLE z dopey (STOP most + `bluetoothctl disconnect`!) | RF přes dopey:5011/5012 | krmí Grafanu — po zásahu vrátit most a ověřit data |
| tth-probe (karta, mobilní) | BLE companion `C0:AE:B9:97:A1:34` (bond ✔ na e5570) | RF: `mesh_admin.py d9f0ef62` | **companion build ⇒ textovou konzoli NEMÁ**, jen binární rámce |
| tth-card (karta, doma) | BLE companion `F5:C2:0C:74:A0:67` (bond ✔ na e5570) | USB konzole tam, kde zrovna je | dtto — po odchodu RemoteTermu z karty (25. 8.) je volná |
| TTH-L1 (companion) | USB / bot na #tth-test | — | admin CLI nemá |

## BLE odsud nejede — black-arch má hluchý scanner

**Na BLE operace používej `e5570.doma`, ne black-arch.** Zjištěno 31. 8. 2026:
scan na black-archu (MediaTek `0e8d:0616`) vrací nula až jedno zařízení, i když
`Discovering: yes` a firmware se při power-cyclu reloaduje. Restart
`bluetooth.service` ani power-cycle přes DBus to nespraví. Notebook (Intel) ve
stejné chvíli vidí desítky zařízení včetně `tth-x1` a obou karet.

Past, na kterou jsem naletěl: „scan nic nevidí" jsem chtěl vyložit jako „uzly
nejsou v dosahu". Kontrolní skupina je povinná — a XIAO s DTR-low jako kontrola
NESTAČÍ, dokud nevíš, že ten build vůbec má BLE.

Na e5570 chybí bleak. Instalovat ho tam netřeba: je tam `python-dbus`, takže
BlueZ se dá obsloužit přímo (`nus_dbus.py` pro textovou konzoli, `companion_dbus.py`
pro binární rámce — obojí ve scratchpadu session, ~90 řádků).

```bash
rfkill unblock bluetooth        # BT tam bývá soft-blocked
bluetoothctl --timeout 30 scan le >/dev/null
bluetoothctl info <MAC> | grep RSSI    # RSSI = v dosahu a advertuje
```

## RF admin: `tools/mesh_admin.py`

```bash
PW=$(sed -n 's/^[[:space:]]*admin_pw[[:space:]]*=[[:space:]]*//p' platformio.local.ini | head -1)
ssh sneezy.chata 'sudo systemctl stop meshcore-exporter.service'      # port 5012 = JEDEN klient!
printf '%s\n' "$PW" | ssh sneezy.chata \
  "MC_NAME=hreb /opt/meshcore-tools/venv/bin/python /opt/meshcore-tools/tools/mesh_admin.py 5361aa08 ver 'get repeat'"
ssh sneezy.chata 'sudo systemctl start meshcore-exporter.service'     # VŽDY vrátit — nejlíp trapem
```

- **Na companion port smí JEN JEDEN klient** — druhému démon dotaz odešle, ale
  odpověď nedoručí. Proto stop exportéru před a start po, s trapem na EXIT.
- Heslo VÝHRADNĚ stdin (argv vidí `ps`; konzole uzlu příkaz echuje).
- **Přes RF NEJDE** (gated na lokální konzoli): `erase`, `log`, `stats-packets`,
  `stats-radio`, `stats-core`, `get acl`, `set freq`, `set prv.key`. Countery
  na dálku = exportér v Mimiru, ne stats příkazy.
- `login neprošel, ale příkazy jdou` = žije stará ACL session, NEBO uzel má
  v prefs staré heslo (pak pomůže jen lokální konzole `password <nové>`).

## Zlaté pravidlo posledních dveří

**Neposílej `ble off`, `reboot` ani `set` měnící dosažitelnost, dokud sis
TOUHLE session neověřil, že uzel odpovídá i druhou cestou.** `ble off` je
persistentní; reboot umí uzel shodit z USB do replugu; a na stožár se nechodí.
Stejně tak nikdy neměň `freq`/rádio na dálku — to je jeden typo od uzlu, který
už neuslyší ani opravu (`tempradio` se aspoň sám vrátí).

## Hodiny

Po rebootu jde RTC do května 2024 a klienti pak zahazují odpovědi na timestampu
(a admin login je na timestampu taky). Mast-jednotky mají `gps duty`, srovnají
se samy; jinak `time <epoch>`. Mosty na dopey to dělají automaticky.

## Heslo

Jen v `platformio.local.ini [secrets] admin_pw`, max 15 znaků (delší se TIŠE
uřízne a login padá). Rotace VÝHRADNĚ `mesh_admin.py <uzel> 'password!'`
(nové heslo = 2. řádek stdin, odpověď REDIGUJE). Nikdy přes ble_cli/konzoli,
která echuje — 22. 8. tak heslo uteklo do transkriptu a rotovalo se celé.

## Dohled bez sahání na uzly

Mimir (`mimir`, uid `a36e61cb-…`): `meshcore_uptime_seconds`,
`meshcore_battery_millivolts`, `meshcore_last_scrape_success_timestamp`,
countery — `pod="sneezy.chata"` = chatové uzly přes RF, `pod="localhost"` =
tth-ltm z dopey. Uptime spadlý k nule = reboot; chybějící série ≠ uzel dole
(nejdřív zkontroluj exportér a hosta — zmizelá `up` série není `up=0`).
Kdo koho slyší a kudy: skill `mesh-evidence`.
