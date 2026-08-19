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
