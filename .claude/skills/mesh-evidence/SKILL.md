---
name: mesh-evidence
description: Dokázat, jestli paket/zpráva prolezla meshem a kudy — vybrat správný zdroj dat (RemoteTerm, komunitní analyzer, MQTT, CoreScope, observery), dotázat se ho správně a nevyvodit "neprolezlo" z pozorování na špatném konci trasy. Použij VŽDY, když se ptáš "prolezlo to?", "slyšel nás někdo?", "kudy to šlo?", "kolik hopů?", nebo když se ověřuje dosah, nová trasa či cizí uzel.
---

# Dokazování průchodu meshem

## Pravidlo, které stálo celý experiment

**Pozoruj na tom konci trasy, kde paket KONČÍ, ne kde začíná.** 22. 8. 2026 jsem
prohlásil letový test za neúspěšný na základě správného měření: hledal jsem hash
Klínovce na `ob2` na chatě a naměřil 0 paketů za 4 h. Jenže trasa byla
`L1 → Klínovec → CZ mesh → tth-ltm → black-arch` a **chata v ní vůbec nebyla**.
Důkaz celou dobu ležel doma v RemoteTermu. Než řekneš „neprolezlo", vyjmenuj,
kudy by ten paket LETĚL, a zkontroluj **koncový** uzel té cesty.

**„Nevidím to" má tři různé významy** a je potřeba je rozlišit: (a) nešlo to,
(b) šlo to jinudy, (c) ptám se špatného zdroje nebo špatným dotazem.

## Zdroje, od nejvíc vypovídajícího

### RemoteTerm doma — jediné místo s CESTAMI přijatých zprav
```bash
curl -s "http://127.0.0.1:8011/api/messages?limit=800"   # jen loopback black-archu
curl -s "http://127.0.0.1:8011/api/contacts"             # prefix -> jméno
```
`systemctl --user status remoteterm`. Každá zpráva nese `paths[]` s
`{path: hex, path_len: POČET HOPŮ, rssi, snr}` — **šířka hashe = (len(hex)/2) /
path_len**. Tohle je jediný zdroj, kde je vidět, kudy zpráva domů dorazila, a je
tam historie dnů. Sbírá i když o něm nikdo neví.

### Komunitní analyzer — co viděli cizí, bez tokenu
```bash
curl -s "https://analyzer.meshcore.cz/api/nodes?limit=2000"    # BEZ limitu jen 50!
curl -s "https://analyzer.meshcore.cz/api/packets?limit=3000"
curl -s "https://analyzer.meshcore.cz/api/stats"
```
**PAST: `/api/nodes` bez `?limit=2000` vrátí 50 uzlů z 983** a chybí v nich i uzly,
které tam jsou — málem jsem z toho usoudil, že v komunitních datech nejsme vůbec.

`/api/packets` má `_parsedPath` **už rozsekaný podle šířky**: 4znakové položky =
2 B, 2znakové = 1 B. Nespojuj si to sám. Dál nese `observer_name`, `observer_iata`
a `observation_count` (kolik pozorování na ten paket — desítky znamenají, že se
paket rozešel po republice). Okno bývá ~5 h dozadu, na starší data nesahej.

U uzlu je `relay_count_1h`/`relay_count_24h`/`last_relayed` — **ty dokazují, že uzel
žije, i když jeho vlastní advert je den starý**. A `last_relayed` se nafukuje
shodami na 1bajtový hash, takže se z něj nesmí soudit u 1B uzlů.

### MQTT — raw pakety z našich mostů i observerů
```bash
mosquitto_sub -h stor.grg -p 1883 -t 'meshcore/#' -v      # anonymně, bez auth
```
Nese `raw` hex, `RSSI`, `SNR`, `origin`. Dekódování: `path_len = raw[1]`, hopů
`& 0x3F`, šířka `((>>6) & 3) + 1`. Publikují sem mosty na dopey i openHop observery.

### Prometheus/Grafana — pro stav uzlu, ne pro trasy
`meshcore_uptime_seconds{node="…"}` je nejlepší důkaz, že flash dosedl (spadne
k nule). Přes MCP: datasource `mimir` uid `a36e61cb-f1ca-4070-940f-3d15c71fdab4`.

### CoreScope na stor.grg
Vlastní analyzer, `:8081`. **SSH tam chce Yubikey**, takže když není po ruce, jdi
přes komunitní analyzer nebo MQTT — mají totožné API resp. tatáž data.

## Interpretace

- **Naše uzly na chatě (hrebecná, probe, ob2) jsou RF ostrov** — v komunitních
  datech nebudou, a to není porucha. `tth-ltm` u dopey je náš jediný uzel, který
  komunita normálně vidí.
- **Companion (L1) se v analyzeru neobjeví jako uzel**, protože neposílá adverty.
  Objeví se ale jako **hop v cestě**, když něco přeposlal — a to je zároveň důkaz,
  že mu jede `repeat`.
- **Šířka hashe v naší flotile je 2 B.** 1 B v cestě u cizího uzlu je jeho volba,
  ne chyba; naše pakety musí být 2 B (od opravy `3cb16750` včetně zerohopu a DIRECT
  odpovědí).
- **Časové zóny:** RemoteTerm a `journalctl` jedou v CEST, MQTT a analyzer v UTC.
  Dvě hodiny rozdílu udělaly z „to bylo včera" „to bylo právě v letovém okně".

## Postup

1. Napiš si, kudy by paket letěl, a urči **koncový** uzel.
2. Zkontroluj zdroj u toho konce (doma = RemoteTerm, komunita = analyzer).
3. Korelovat časy — **v obou zónách**.
4. Porovnej s kontrolní skupinou: stejné zprávy z jiné doby/místa. („0 z 8 ze
   země vs 4 z 5 ze vzduchu" je závěr, „4 zprávy prolezly" není.)
5. Prefix hopu přelož na jméno (`/api/contacts`, `/api/nodes?limit=2000`) a teprve
   pak tvrď, kdo to byl.
