# Co z forku posíláme do upstreamu (19. 8. 2026)

Triáž všech 87 vlastních commitů nad `origin/main` (v1.17.1) na tři hromádky:
**jde nahoru**, **možná nahoru po diskuzi**, **zůstává nám**. Cílem je, aby se
příště nemuselo znovu rozhodovat, co je obecná oprava a co naše specialita.

## Pravidla upstreamu, která platí (`CONTRIBUTING.md`)

- **PR se dělá proti `dev`, ne proti `main`.** Větve `fix/xxx`, `feature/yyy`,
  `docs/whatever`.
- Jedna oprava = jeden PR. Malé opravy bez předchozího issue, **větší změny
  a nové featury nejdřív jako issue** a až po palci od maintainera PR.
- Styl: 2 mezery, `camelCase`, `#define` velkými, řádky do ~100 znaků,
  `.clang-format` je v repu.
- **Agentní PR mají do titulku přidat `🤖🤖`** — upstream to má jako oficiální
  opt-in do zrychleného review. Není to volitelná zdvořilost, je to jejich
  proces.

Pozor při portování: `dev` je proti `main` o 17 commitů dál a **část našich
oprav už tam v jiné podobě je** (viz PR2 níž). Cherry-pick naslepo tedy buď
konfliktuje, nebo vrací zpátky něco, co upstream vyřešil jinak — každý kus se
musí proti `dev` znovu ověřit.

## Hromádka 1: připravené větve (na forku, nad `origin/dev`)

| větev | commit | co to je | build |
|---|---|---|---|
| `fix/contact-info-zero-init` | `3d688c7` | `ContactInfo` se plní po polích a zbytek zůstane smetí ze stacku — `gps_lat/gps_lon` u kontaktu přidaného z appky, a `shared_secret_valid` u **každého** kontaktu při každém bootu (to je ta nebezpečná polovina: nenulový bajt ⇒ DM se zašifruje neinicializovaným klíčem) | `t1000e_companion_radio_ble`, `RAK_4631_terminal_chat` |
| `fix/trace-path-bounds-check` | `434c54e` | dvě čtení za koncem bufferu v `Mesh::onRecvPacket()` — TRACE s `payload_len < 9` podteče `len` a `isHashMatch()` čte až ~500 B do 184B pole; u PATH `hash_size*hash_count` může přelézt dešifrovanou délku | `t1000e_repeater`, `t1000e_companion_radio_ble` |
| `fix/tx-requeue-not-drop` | `92321a8` | neúspěšný TX se zahodí bez retry a bez counteru; naměřeno **~40 % ztracených DIRECT forwardů** při nečinném BLE spojení | `t1000e_repeater`, `t1000e_room_server`, `Heltec_ct62_sensor`, `t1000e_companion_radio_ble` |

### Kandidát 22. 8. 2026: `sendZeroHop()` maže šířku path hashe

`Mesh::sendZeroHop()` dělá `packet->path_len = 0`, čímž vynuluje **celý bajt**
včetně horních dvou bitů, kde je šířka hashe (`Packet.h:85`). Každý zerohop
paket proto hlásí 1 bajt bez ohledu na `path_hash_mode` uzlu, kdežto floodová
větev šířku dostává (`sendFloodScoped(..., _prefs.path_hash_mode + 1)`).

**Není to bug v routingu** — zerohop paket nemá cestu a nikdy se nepřeposílá
(`Mesh.cpp:83` chce `getPathHashCount() > 0`). Je to bug v tom, co uzel o sobě
hlásí: analyzery odvozují šířku z pozorovaných paketů a dvoubajtový uzel jim
podle svých zerohop advertů vyjde jako jednobajtový. Na CZ meshi je to
pravděpodobné vysvětlení velké části uzlů vedených jako „suspected 1 B".

Oprava: `sendZeroHop()` dostane `path_hash_size=1` (stejně jako `sendFlood()`)
a volající mu předají `_prefs.path_hash_mode + 1`. Default zachovává dnešní
chování, takže ostatní příklady se nemusí měnit. **Pozor na přetížení
s `transport_codes`** — holá `0` jako delay je zároveň null pointer, takže
volání s literálem chce `(uint32_t)0`, jinak je ambiguous.

Změřeno: `tth-hrebecna` posílala `flood: 2 B` a `route2 (zerohop): 1 B` od
téhož původce, dvě hodiny od sebe. Build ověřen na `SenseCap_hreb_rpt`,
`SenseCap_ples_rpt`, `SenseCap_Solar_repeater_ble`.


Texty PR: `../../scratchpad/pr/*.md` v session, obsah je i v commit messages.

Poznámky k jednotlivým:

- **PR2 se nedal cherry-picknout.** Upstream na `dev` mezitím doplnil
  `Packet::isValidPathLen()` u PATH (naše polovina) a opravil wrap `offset` na
  `uint16_t` u TRACE. Větev je proto napsaná znovu a obsahuje **jen to, co na
  `dev` pořád chybí**: `payload_len < 9`, kontrola celého hashe a `k >= len`.
- **PR3 mění chování jádra**, takže podle CONTRIBUTING patří spíš do issue.
  V popisu jsou vypsané věci, na které se má maintainer ptát (airtime, počet
  retry, řazení ve frontě) a menší varianta „nech drop, přidej jen countery".
- Ve všech třech jsou odstraněné naše značky `// CUSTOM (TeTeHacko)` a odkazy na
  náš analyzer. Do upstreamu nesmí jít fork-specifické komentáře.

## Hromádka 2: obecné, ale chce to diskuzi (issue first)

| commit | co to je | proč to není hotové |
|---|---|---|
| `89eb71ab` | gate na export klíče / PIN přes neautentizovaný USB companion | bezpečnostní, ale sedí na našem dual-serial rozhraní, které upstream nahradil `MultiSerialInterface` — musí se přepsat |
| `8e3b72c4` (zbytek) + `fda7dbbb` | bounds-check kopií v CLI | smíchané s naší featurou `sensor read`, chce rozdělit |
| `0ef8f7f4` | provoz bez DIO1 (`LORA_POLL_IRQ`) + příkaz `dio1` | zachránilo nám desku s odpadlým drátkem; obecně užitečné, ale je to nová featura → issue |
| `5462b8d8` | duty cycle GPS přijímače | ušetří 42–45 mA na uzlu, co GPS chce jen na hodiny; nová featura |
| `b9825031` | uzel bez GPS startuje na build epoch, ne v roce 2024 | opravuje reálný problém (všechny naše desky byly 811 dní pozadu), ale mění default |
| `563fd83c` + `568a351d` | `i2c` scan z konzole + oprava ořezávání odpovědi | obecné, malé; půjde nahoru po prvních třech |
| `b4f943df` | per-paket RSSI v `Packet` | 3 řádky, ale bez konzumenta se maintainer zeptá „kdo to používá" — má smysl poslat spolu s něčím, co RSSI čte |
| `0b18490a`, `62627080`, `5c96b817`, `7a964c33` | `blink`, `txpwr`, `dfu [uf2\|serial\|ota]` | obecné příkazy, ale `dfu` je nRF52-specifický (GPREGRET) |
| `1333aeb9` … | viz PR3 | |

## Hromádka 3: zůstává nám

- **Celé `tools/`** (34 commitů). Upstream `tools/` vůbec nemá — má `bin/`,
  `create-uf2.py`. Naše skripty navíc předpokládají naši flotilu a naše cesty.
- **Analyzer** (`nodes`, `rxlog`, `pktlog`, pktfeed ring) a **kanálový bot** —
  naše featury pro CoreScope, ne obecný firmware.
- **Textová konzole repeateru přes BLE** a všechno ladění spojení nad ní
  (`blepwr`, `bleconn`, conn interval, supervision timeout, non-blocking TX
  pump). Stojí to na naší úpravě, kterou upstream nemá.
- **`SerialDualInterface`** — upstream to vyřešil `MultiSerialInterface`,
  naše verze je mrtvá větev.
- **2bajtový path hash jako default** (`34e2f2a0`) — to není oprava, to je
  politika sítě. Upstream má režimy, default si drží sám.
- **Úspory zapnuté defaultně** (`aef2c97b`, `7489a5ee`) — totéž, mění chování
  všem.
- **Docs, AGENTS.md, CLAUDE.md, INFRA symlink, `variants/sensecap_solar`,
  `tools/provision/*`** — popisují naši infru a naše uzly.

## Postup, až se bude pokračovat

1. Poslat PR1 a PR2 (čisté opravy, malé, bez diskuze).
2. PR3 podle toho, co řekne maintainer — buď PR, nebo nejdřív issue s těmi
   naměřenými čísly.
3. Teprve pak hromádku 2, po jednom, každý zvlášť ověřený proti `dev`
   (znovu: `dev` se hýbe, půlka práce je zjistit, co už tam je).


---

# openHop a RemoteTerm (19. 8. 2026)

Do MeshCore upstreamu **neposíláme nic** (rozhodnuto 19. 8. 2026). Tři větve
z první části dokumentu zůstávají připravené na forku, ale PR se neotevírají.
Co se posílá, jde do openHopu a RemoteTermu.

## Stav větví

| projekt | fork | větev | proti | stav |
|---|---|---|---|---|
| openHop | `TeTeHacko/openhop_repeater` | `fix/room-advert-node-name` | `dev` | pushnuto, 23 testů zelených, PR **neotevřen** |
| RemoteTerm | `TeTeHacko/Remote-Terminal-for-MeshCore` | `feature/configurable-map-tiles` | `main` | pushnuto (commit `76f81c8`), `all_quality.sh` **nedojel** — před PR znovu pustit |

RemoteTerm chce podle CONTRIBUTING **nejdřív issue**, teprve pak PR
(„a brand new feature appearing first in a PR is an antipattern"). Text issue je níž.

## openHop PR: room advert name

### Problem

A room server adverts under two different names depending on what triggered the advert.

`repeater/handler_helpers/room_server.py:149` reads

```python
node_name = room_settings.get("room_name", room_name)
```

but `room_name` is not a documented setting — it appears nowhere in `config.yaml.example`
or `docs/`. Every other advert path reads `node_name`:

- `repeater/web/api_endpoints.py:6517` — `POST /api/send_room_server_advert`
- `repeater/main.py:854`, `:1126`, `:1540` — the periodic advert scheduler

`config.yaml.example` documents `settings.node_name`, and so does the existing test
fixture for this very function (`tests/test_handler_helpers_room_server.py`:
`settings: {"node_name": "Room Alpha", ...}`). So the CLI `advert` command always falls
through to the identity's `name` and ignores the configured one.

### How it shows up

```yaml
identities:
  room_servers:
    - name: RoomServer
      settings:
        node_name: "tth-room"
```

The scheduler and the HTTP endpoint advert as `tth-room`. An admin typing `advert` over
RF adverts as `RoomServer` — and because clients take the newest advert for a public key,
the room renames itself for everyone on the mesh.

That is not hypothetical: this happened to our node today. The room had been `tth-room`
for a day; one CLI advert renamed it to `RoomServer` on every client in range, and it took
a second advert (after working around the bug in config) to put the name back.

### Fix

One line, so the CLI path reads the same key as every other path.

Plus a regression test asserting `create_advert()` receives the configured name and
coordinates. It fails on the old behaviour with `+ room-alpha`, and the full file passes
with the fix:

```
tests/test_handler_helpers_room_server.py .......................  23 passed
```

(Run in a container built from this repo's image with `pytest`/`pytest-asyncio` added.)

### Compatibility

Anyone who worked around this by setting `room_name` would have to switch to `node_name`.
The key is undocumented, so that looks safe — but if you would rather not break it, I am
happy to push `room_settings.get("node_name", room_settings.get("room_name", room_name))`
instead. Your call.


## RemoteTerm issue: konfigurovatelné dlaždice

**Title:** Map tile URL is hardcoded to osm.org in four components — no way to use another tile server

### What I ran into

osm.org answers my browser with `403` for RemoteTerm's map tiles, so every map view is
blank. I already run a caching tile proxy for other tools, but there is no way to point
RemoteTerm at it: the URL is hardcoded in four separate places.

```
frontend/src/components/MapView.tsx        (the 'light' base layer preset)
frontend/src/components/PathRouteMap.tsx
frontend/src/components/NeighborsMiniMap.tsx
frontend/src/components/ContactInfoPane.tsx
```

The fourth one is easy to miss — I patched three, rebuilt, and the contact-detail mini map
still went to osm.org.

Two reasons this is worth a knob, beyond the 403:

- **Caching.** A shared proxy serves tiles from disk, which is faster and much politer to
  osm.org than every client fetching them directly.
- **Deployments that should not reach osm.org at all.** Mine sits on a segment where the
  browser can talk to my own hosts and nothing else. Right now that means no maps, even
  though everything else works offline.

### What I would like

Two env vars, in the same shape as the existing server-side settings:

- `MESHCORE_MAP_TILE_URL` — tile template (`{z}/{x}/{y}`), empty keeps OpenStreetMap
- `MESHCORE_MAP_TILE_ATTRIBUTION` — optional, defaults to the OSM credit

Only the **default** base layer would change. The other layer choices are named after
their providers (CARTO, OpenTopoMap, Esri), so overriding those would make the labels lie.

### Note on scope, because of the project principles

This is only about where the *browser* fetches map imagery. It adds no radio traffic and
no path from the internet onto the mesh — if anything it removes a network dependency,
since the instance can then run with no route to osm.org at all.

### Offer

I have this implemented and passing `./scripts/quality/all_quality.sh` (config validation
+ `/api/health` plumbing + a small context, with backend and frontend tests). Happy to
open the PR if you want it this way, or to adjust the shape first — filing the issue first
per CONTRIBUTING.


## RemoteTerm PR (referuje to issue)

Closes #<issue>

### What

Adds `MESHCORE_MAP_TILE_URL` and `MESHCORE_MAP_TILE_ATTRIBUTION` so the default base map
can point at another tile server, and replaces the four hardcoded osm.org URLs with a
single resolved source.

Default behaviour is unchanged: unset means the built-in OpenStreetMap layer, byte for
byte the same URL and attribution as before.

### Why

osm.org answers some clients with `403`, which leaves every map blank; a caching proxy is
both a fix and politer to osm.org; and some deployments should not reach osm.org at all.
Details and discussion in the issue.

### How

- `app/config.py` — the two settings, plus validation: the URL must contain `{z}`, `{x}`
  and `{y}` (a plain URL would render an empty map, so fail at startup instead), and an
  attribution without a URL is rejected rather than silently mis-crediting tiles that
  still come from OSM.
- `app/routers/health.py` — surfaced on `/api/health`, the same way `bots_disabled` and
  `basic_auth_enabled` already reach the frontend. No new endpoint.
- `frontend/src/contexts/MapTileContext.tsx` — a small context in the shape of the
  existing ones, with `resolveTileLayer()` holding the fallback rules.
- The four map components consume `useMapTileLayer()`.
- `MapView` only substitutes the `light` preset; CARTO / OpenTopoMap / Esri keep their own
  URLs, since those labels name their source.
- Docs: README env table and `docker-compose.example.yml`.

A custom URL keeps the OpenStreetMap attribution unless an attribution is given too —
self-hosted tile servers usually proxy OSM data, so dropping the credit would be wrong,
while a genuinely different source can say so.

### Tests

- `tests/test_config.py::TestMapTileOverride` — defaults, a valid template, a URL without
  placeholders, and attribution without a URL.
- `frontend/src/test/mapTiles.test.ts` — fallback for null/undefined/empty/whitespace,
  override, attribution defaulting, trimming.
- `./scripts/quality/all_quality.sh` passes (ruff, pyright, pytest, eslint, prettier,
  vitest, frontend build).

Verified against a real proxy at zoom 13–19 before proposing this: the map renders, and
with the vars unset the OSM layer is unchanged.
