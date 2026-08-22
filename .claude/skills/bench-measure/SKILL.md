---
name: bench-measure
description: Změřit něco na lavici — spotřebu uzlu, cenu nastavení, airtime, regresi firmwaru napříč deskami. Použij VŽDY, když se má porovnat "s tím vs bez toho", zjistit kolik něco žere, ověřit že deska po změně firmwaru pořád vysílá, nebo když je potřeba rozhodnout, jestli je naměřený rozdíl skutečný.
---

# Měření na lavici

Nářadí existuje, **nevymýšlej vlastní**: `tools/power_ab.py` (A/B proud po
buňkách), `tools/fleet_test.py` (regrese N×N napříč deskami). Podrobná čísla
a odvození: `docs/power-saving.md`, `docs/ble-lora-coexistence-bench.md`.

## Nulté pravidlo: fyzický stav řekne uživatel

**Co je kde zapojené, ví uživatel — ne já a ne moje paměť.** Když řekne, že
deska je na měřáku nebo na USB, tak tam je; podívej se, nebo se zeptej, ale
neodporuj. Historie tohohle projektu je toho plná („X1 a X2 sou na tom meraku,
tak jsou", „oba RPT mam porad pod stolem", „s2 je pripojeny, to se nemzues
podivat"). Před měřením si nech potvrdit topologii (co na kterém měřáku, co má
článek, co anténu) a **napiš ji do výsledku** — bez ní se čísla nedají znovu
přečíst.

## Vlastní countery uzel NECERTIFIKUJÍ

`sent` znamená jen „firmware si myslí, že vysílal". Mrtvá DIO1 dá `sent 0`,
zatímco druhá deska slyší každý rámec (x0, celý týden). **Každé RF tvrzení musí
vyslovit jiná deska** — proto je `fleet_test.py` matice N×N a proto používá
**zerohop** adverty (nikdo je nerebroadcastuje, takže „B slyšel A" nejde splnit
tím, že to přeposlal C, a test nemusí sahat na `repeat` prefs).

## Pasti, které už jednou zneplatnily celý běh

**Solární uzel přes USB měří NABÍJEČKU, ne uzel.** SenseCAP Solar bere na měřáku
500–700 mA (sběrnice spadne 5,01 → 4,74 V); spotřeba uzlu (5–25 mA) je z toho
2–4 % na driftující hodnotě. Nabíjení nejde zakázat softwarově a klesá pomalu
(0,71 → 0,53 → 0,22 A za ~40 min). Buď dojet nabíjení, vytáhnout článek, nebo
měřit přenositelnou část na XIAO — **P1-Pro má XIAO nRF52840 uvnitř**, takže
delty MCU + SX1262 platí 1:1 a specifická je jen GPS. **INA226 na desce NENÍ**
(ověřeno na třech kusech, `i2c` scan nenajde nic).

**Rozptyl opakovaných buněk NENÍ šum, když něco driftuje.** `gps on` buňky jely
107 → 92 → 88 → 84 mA (doznívající nabíječka) a „rozptyl 23 mA" tím označil
skutečnou 46mA deltu za šum. Design musí být **A/B/A/B** a každá B buňka se
porovnává s **průměrem dvou obklopujících A** — lineární drift se vykrátí.
`power_ab.py` to dělá sám.

**Nečti `meter_current_amps`** — UC96 kvantizuje proud na 10 mA, což je celý
hledaný efekt. Použitelná je akumulace `meter_capacity_ah` (1 mAh) a regrese přes
celý průběh: 5min okno dá ±12 mA z endpointů, ale ~1 mA z fitu.

**Okno kratší než jeden krok čítače nic nedokazuje.** 1 mAh se při 20 mA nasčítá
za 3 min, takže „za 90 s se čítač nehnul" neznamená nulový proud. Stejná chyba
v jiné podobě: `tx_air_secs 0` nedokazuje ticho, orákulum je `sent`.

**Zapnutá `gps duty` znemožní měřit cokoliv pod ~5 mA** — probuzení přijde
náhodně a rozbije buňku (buňka s probuzením 40,9 mA proti 24,1 v sousedních).
Na malé efekty dej `gps off` do KAŽDÉ buňky a po běhu obnov `gps duty`.

**Připojený měřák neadvertuje**, takže ho scan nevidí (`0 UC96 found, 1 active
connections`), a měřáky advertují jen 2–3 min po zapojení. Démon musí běžet dřív,
jinak replug. Nečti měřák po BLE ručně — `uc96-exporter.service` drží linku
a druhá centrála mu ji vytrhne; ber to z exportéru (`localhost:9877/metrics`).

**Připojený BLE klient na měřené desce zneplatní běh** — USB a BLE konzole dělí
jeden `command[160]` buffer, dvě živé konzole si prokládají znaky a uzel vykoná
výsledek.

## Pořadí úspor je změřené, nehádej ho

GPS **46 mA** (párové A/B, datasheetových 25–35 bylo nízko) ≫ vše ostatní.
`powersaving on` ≈ 1 mA a je **neodlišitelné od nuly**, BLE advertising ≈ 1,3 mA
taky na hraně šumu, rxps ≈ 2 mA. Podlaha sestavy s připojeným článkem je 1–2 mA
na buňku. **Po GPS už na solárním uzlu není co měřit** a `ble off` na stožáru se
nedělá kvůli spotřebě (1 mA z 24), jen kvůli bezpečnosti.

## Postup

1. Nech si potvrdit topologii a zapiš ji do výsledku.
2. Vyber orákulum: pro RF **jinou desku**, pro proud `meter_capacity_ah`.
3. A/B/A/B, ne A/B. Buňka aspoň 10 min, u malých efektů víc.
4. Vypni, co střílí náhodně (`gps off`), a po běhu to vrať.
5. Než tvrdíš efekt, ověř, že je větší než opakovatelnost buněk — a řekni
   obojí číslem.
