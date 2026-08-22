---
name: node-diagnose
description: Uzel nereaguje, nevysílá, nepřijímá, zmizel z USB nebo z BLE — zjistit, CO je vlastně rozbité, než se do toho sáhne. Použij VŽDY na "deska je mrtvá", "nevysílá", "nevidím ho", "spadl z USB", "neodpovídá", "nejde se připojit", i když to vypadá jednoznačně.
---

# Diagnostika uzlu, který nefunguje

## „Deska je mrtvá" NENÍ diagnóza

Většina zdejších „mrtvých desek" byla zdravá deska ve špatné kombinaci nebo
špatně pozorovaná. **Než cokoliv resetuješ nebo flashneš, urči režim a mít
svědka** — reset i flash zahodí důkazy a `recover_to_bootloader` v cyklu umí
domnělou mrtvou desku uštvat doopravdy.

**Fyzický stav řekne uživatel.** Když říká, že deska je zapojená nebo že leží
pod stolem v krabici, tak to tak je — podívej se, ale neodporuj mu z paměti.

## 1. Urči režim, ne stav

```bash
ls -l /dev/serial/by-id/                     # VŽDY by-id, ttyACM* se přečísluje
lsusb | grep -iE "2886|239a"                 # 0044 bootloader, 8044/8029/0057 aplikace
lsblk -o NAME,LABEL,TRAN | grep usb          # UF2 disk = bootloader
```

Bootloader se na USB hlásí **bez „Studio"** v názvu portu a nese UF2 disk.

## 2. Rozliš, kde uzel mlčí

| mlčí na | co to znamená |
|---|---|
| **jen na BLE** | dock-quiet: USB drží DTR ⇒ BLE neadvertuje. Nebo připojený klient — připojený peripheral přestane advertovat. |
| **jen na USB** | připojený BLE klient ⇒ USB companion mlčí (by design). |
| **jen po meshi** | rádio nebo IRQ linka; jdi na bod 3. |
| **na BLE i po meshi naráz** | **nejspíš visí v bootloaderu** — ten nemá LoRa ani BLE aplikaci. Připoj USB a přečti `idProduct`. `phase2` nepomůže, v UF2 režimu BLE nezapne. |

Uzel „mrtvý na obou" je nejčastěji zdravý uzel ve špatné kombinaci.

## 3. RF vada: svědkem musí být jiná deska

Countery na testované desce nic necertifikují — `sent` znamená „firmware si
myslí, že vysílal". Postup: oba uzly `set repeat off`, pak
1. **RX test** — třetí uzel vyšle N advertů, sleduj `recv` u obou.
2. **TX test** — `advert` na testovaném, sleduj `recv` u svědka.

**Signatura mrtvé DIO1:** testovaný hlásí `sent 0`, roste `tx_timeout`,
`errors=16` (`ERR_EVENT_TX_TIMEOUT`, bitmaska 1=FULL 2=CAD 4=STARTRX 8=TX_START
16=TX_TIMEOUT) — **ale svědek rámce SLYŠÍ**, a `recv` je 0 i při silném signálu.
SX1262 vysílá i přijímá, jen se do MCU nedostane IRQ.

**Předfiltr ještě před flashem:** `noise_floor` v `stats-radio`. Normální hodnota
(−106 dBm) na „mrtvé" desce znamená, že SPI, NSS i vstupní díl přijímače žijí
a vada je zúžená na IRQ linku.

**Mrtvá DIO1 už není rozsudek smrti.** `-D LORA_POLL_IRQ=1` čte dokončení z IRQ
registru po SPI a linku obejde — ověřeno plnou obousměrnou funkcí. Diagnostický
příkaz `dio1` (za `-D PIN_DIAG=1`): `TX_DONE + pin HIGH` = linka OK ·
`TX_DONE + pin LOW` = mrtvá linka · `žádný TX_DONE` = vada NENÍ na IRQ lince.
**Poll je nadřazený drátku** — když neožije poll, neožije ani přepojení.

## 4. Zdroj pravdy o uzlu na dálku

Že uzel žije, dokazuje `relay_count_1h`/`last_relayed` v komunitním analyzeru,
i když jeho vlastní advert je den starý. Detaily a zdroje: skill `mesh-evidence`.
**Restart counter služby NENÍ důkaz poruchy** — most na tth-ltm vypadal na
crash-loop a přitom měl 46 úspěšných cyklů za 6 h; orákulum je `full scrape`
v journalu.

## 5. Co NEDĚLAT

- **Port `disable` jako recovery** — sundá i zdravou desku bez cesty zpět.
- **Druhý 1200baudový touch v jedné power session** — vyřadí desku z USB i BLE
  bez self-recovery. Když upload selže, chtěj replug, ne druhý pokus.
- **`pio run -t upload` na desku v bootloaderu** — touch ji vykopne ven
  (`Couldn't find a board`) a je to zároveň ten zakázaný druhý touch.
- **Reset před ověřením.** Co potřebuješ změřit, změř PŘED resetem; softwarový
  reset shodí desku z USB až do replugu asi v polovině případů.
- **Diagnostikovat z jednoho pokusu o BLE connect.** BLE je flaky: `TimeoutError`
  při funkční konzoli na tomtéž uzlu není mrtvá deska (viz `flash-node`).

„Spadlo z USB" jde vyrobit i na naprosto zdravé desce zásahem na hostu — stejný
podpis (`USB disconnect`, pak nic, port `not attached`). Čísla: `tools/README.md`.

## 6. Než ohlásíš závěr

Řekni, **který krok ho podepřel** — svědek, `idProduct`, `dio1`, nebo cizí
observer. Diagnóza bez orákula se tady už několikrát ukázala jako mylná
a pokaždé stála zbytečný zásah do železa.
