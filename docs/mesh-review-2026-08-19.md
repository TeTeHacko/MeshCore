# Revize meshe: šířka path hashe a čas (19. 8. 2026)

Kontrola všech uzlů, které CoreScope slyšel za posledních 24 h — našich
i komunitních. Zadání: **minimálně 2 bajty path hashe (3 B je záměrný
experiment, 1 B nepřípustný)** a srovnaný čas.

Zdroj: `http://stor.grg:8081/api/nodes?limit=5000` (734 uzlů historicky,
**643 slyšených za 24 h**) a `/api/packets?limit=3000` pro čas.

## PAST: `hash_size` z CoreScope je DOLNÍ HRANICE, ne konfigurace uzlu

Než z těch čísel začneš cokoli vyvozovat: CoreScope šířku **odvozuje
z pozorovaných paketů**, a to je něco jiného než nastavení uzlu. Dokázáno na
vlastním uzlu:

| | CoreScope | uzel sám |
|---|---|---|
| `TTH` (T1000-E, pubkey `3080796b…`) | `hash_size 2`, status **confirmed**, evidence `advert` | `path_hash_mode: 2` = **3 bajty** |

Takže i „confirmed" znamená jen *„viděli jsme aspoň tolik"*. Vysvětlení je
v AGENTS.md: **šířku path hashe v paketu volí ODESÍLATEL**, ne uzel, jehož hash
se v cestě objeví. Z pozorování cizí cesty se tedy o konfiguraci uzlu nedá
spolehlivě soudit vůbec nic.

Sémantika polí, jak vyšla z dat (643 živých uzlů):

| evidence | status | hash_size | počet | jak to čtu |
|---|---|---|---|---|
| `advert` | confirmed | 2 | 462 | **aspoň** 2 B, tvrdý důkaz z vlastního advertu |
| `advert` | confirmed | 3 | 6 | aspoň 3 B |
| `path` | suspected | 1 | 115 | jen z cizí cesty ⇒ **není to důkaz o tomto uzlu** |
| (žádná) | unknown | 1 | 59 | žádný důkaz; `1` je výchozí hodnota, ne měření |

`hash_size_inconsistent` je `False` u všech 643 — nikde se šířka nerozchází.

## DOPLNĚK 22. 8. 2026 večer: u nás OPRAVENO

Mechanismus popsaný níž je od `3cb16750` (a `f1728168` v Solo) v naší flotile
opravený — a bylo to širší než zerohop: i **`sendDirect()` s prázdnou cestou**
deklaroval 1 B, což je běžná odpověď sousedovi (přesně ty RESPONSE pakety, co
byly vidět v appce). Změřeno na vzduchu: hrebecná `0x00` → `0x40`, 18/18 DIRECT
paketů. Pro čtení CIZÍCH uzlů ale popis níž platí dál — oni tu chybu mají.

## DOPLNĚK 22. 8. 2026: proč „suspected 1 B" nejspíš nikoho neusvědčuje

Měření na 3000 paketech ukázalo mechanismus, který tu chyběl. **Každý zerohop
paket deklaruje šířku 1 bajt, ať má uzel nastaveno cokoliv** — `Mesh::sendZeroHop()`
dělá `packet->path_len = 0`, což vynuluje celý bajt včetně horních dvou bitů,
kde šířka bydlí (`Packet.h:85`). Floodová větev naproti tomu šířku dostává:
`sendFloodScoped(..., _prefs.path_hash_mode + 1)` (`simple_repeater/MyMesh.cpp:1386`).

**Na routing to nemá vliv**: zerohop paket nemá cestu a nikdy se nepřeposílá —
forwardovací blok se spustí až při `getPathHashCount() > 0` (`Mesh.cpp:83`).
Nikdo do něj tedy hop nepřipisuje a žádná dvojznačnost nevzniká.

Co to ale dělá: **kazí to statistiku každému analyzeru**, který šířku odvozuje
z pozorování. Uzel s `path.hash.mode 1` a zapnutým `advert.interval` vypadá
podle svých zerohop advertů jako jednobajtový. Naše `tth-hrebecna` to dělala
každé 2 h a v datech je to vidět jako `flood: {2 B}` vs `route2: {1 B}` od téhož
původce. **Těch 115 komunitních „suspected 1 B" uzlů je proto potřeba číst ještě
opatrněji, než říká pravidlo výš** — část z nich bude jen tohle.

Změřený stav naší flotily (3000 paketů, 22. 8. 2026): `tth-ltm` se v cestách
objevuje **224× jako 2bajtový hop**, floodové adverty `tth-hrebecna`,
`tth-plesivec-abertamy` i L1 Pro jsou 2 B. Konfigurace ověřena na všech:
`path.hash.mode 1` na třech SenseCapech, `mesh.path_hash_mode: 1` u obou openHop
démonů (dědí ji i room server přes `dispatcher.set_default_path_hash_mode`,
`config_manager.py:352`). **Výjimka jsou companion identity** — ty mají vlastní
`CompanionPrefs.path_hash_mode` s defaultem 0 a přebijí démona, takže se musí
nastavovat zvlášť (bot i exportér to dělají při každém připojení).

## Naše uzly

CoreScope zná čtyři, všechny **confirmed ≥2 B**:

| uzel | role | CoreScope | čas |
|---|---|---|---|
| `mc-tth-ltm.rfa.cz☀️` | repeater | 2 B confirmed | skew **2–3 s** (z mostu) |
| `tth-ob1` | repeater | 2 B confirmed | **2 s** |
| `tth-room` | room | 2 B confirmed | **2 s** |
| `TTH` (T1000-E) | companion | 2 B confirmed — ale reálně **3 B** | — |

Zbytek našich CoreScope znát nemůže, tak jsem je přečetl přímo přes BLE:

| uzel | `path.hash.mode` | = bajty | hodiny |
|---|---|---|---|
| `tth-hrebecna` | 1 | **2 B** | 19:08 UTC ✔ |
| `tth-plesivec-abertamy` | 1 | **2 B** | 19:10 UTC ✔ |

(`mode` je o jeden menší než počet bajtů: `path_hash_size = mode + 1`.)
`tth-ob2` je momentálně dole (deska čeká na přesun na chatu), `tth-x*` jsou
lavicové a na 866.5, mimo ostrý mesh.

**Závěr pro nás: nikde není 1 B.** Jediná odchylka je záměrná — T1000-E na 3 B.

## Komunita

Z 639 živých komunitních uzlů:

- **468 potvrzeně ≥2 B** (462× 2 B, 6× 3 B)
- **115 „suspected 1 B"** — ale jen z pozorované cesty, což podle pravidla výš
  vypovídá o odesílateli, ne o nich. Nelze z toho tvrdit, že jsou na 1 B.
- **59 „unknown"** — žádný důkaz.

Šest uzlů na 3 B: MNICHOVICE, Strojovna🔩, ☠️ DEAD 🥩 BEEF R, Pičhora,
Roztoky1📬, olomouc.meshcore.cz.

Mezi „suspected 1 B" je 39 uzlů, které **aktivně přeposílají** (>100 relayů /
24 h), nejvíc rip5.meshcore.cz (2446), roudnice.meshcore.cz (1776),
lmkz.meshcore.cz (1324). Kdyby na 1 B doopravdy byly, jsou to přesně ty, které
routing bolí nejvíc — ale bez tvrdého důkazu je to podezření, ne obvinění.

## Čas

Z 634 advertů s časovou značkou od 384 uzlů (skew = pozorování − čas v advertu,
plus znamená „uzel je pozadu"):

| odchylka | uzlů |
|---|---|
| OK (< 60 s) | **173** |
| 60 s – 1 h | 35 |
| 1 – 24 h | 17 |
| **> 1 den** | **159** |

Těch 159 není rozptýlených: shlukují se na **820–826 dnech pozadu**, což je
tentýž podpis, jaký měla naše flotila (811–812 dní) — uzel bez GPS nabootuje na
zadrátovanou epochu a nikdo mu čas nenastaví. Nejhorší: VISNOVA.SOLAR 825,9 d,
UJEZDEC.SOLAR 825,9 d, OK1MDX Repeater 825,2 d.

Naše uzly jsou všechny v jednotkách sekund. To u tth-ltm ukázalo 270 s jen
proto, že jsem ho 20 minut před měřením přeflashoval a vzorek (n=2) padl do
okna po rebootu; aktuální `clock_skew_secs` z mostu je **2–3 s** a
`clock_set_count: 0`, tedy most ho ani nemusel dorovnávat.

## Co z toho plyne

1. **U nás je hotovo** — nikde 1 B, čas srovnaný, jediná odchylka (3 B na
   T1000-E) je záměrná.
2. **Čísla o komunitě neber jako diagnózu jejich uzlů.** 174 „jednobajtových" je
   z 90 % artefakt toho, že šířku v cestě volí odesílatel. Kdo to chce vědět
   jistě, musí se zeptat uzlu (jeho `path.hash.mode`), ne odvozovat z provozu.
3. **Čas je v komunitě reálný problém**, a ten se odvodit dá: 159 uzlů 820+ dní
   pozadu je tvrdé číslo z jejich vlastních advertů. Klienti pak zahazují jejich
   odpovědi na timestampu.
