# Plešivec na stožár — checklist

Stav uzlu ověřen 22. 8. 2026 (verify-cz-silent 100 %): v1.17.1-tthe5b4572,
tichý, path 2B, af 9, rxgain on, powersaving on, gps duty, BLE ON, hodiny OK,
advertovaná poloha záměrně hrubá. Identita zálohovaná v keys.yml.

## 1. Nahoře (POŘADÍ JE DŮLEŽITÉ)

1. **NEJDŘÍV našroubovat anténu** — každý krok aktivace vysílá a vysílání do
   prázdného konektoru ničí PA.
2. Zapnout uzel, počkat ~1 min.
3. Aktivace z notebooku (bond má e7470 i sneezy; BLE dosah metry):

       ~/mc/venv/bin/python ~/mc/tools/ble_cli.py DC:28:1D:8A:04:1A \
           -f ~/mc/tools/activate-cz-mast.txt

   Očekávaný konec výpisu: `repeat on / 120 / 25` a „Advert sent".
4. Fallback telefonem: MeshCore appka → tth-plesivec-abertamy → Repeater admin
   (heslo = `admin_pw` z platformio.local.ini, NIKAM neopisovat) →
   `set repeat on` → `set advert.interval 120` → `set flood.advert.interval 25`
   → Send advert. POZOR: advert.interval je v MINUTÁCH, flood v HODINÁCH —
   špatná jednotka se TIŠE odmítne a nechá starou hodnotu.

## 2. Ověření

- na místě: advert uzlu v appce do pár minut
- z domova: `meshcore_uptime_seconds{node="tth-plesivec-abertamy"}` v Grafaně
  (přes sneezy exportér — proto krok 0!), `analyzer.meshcore.cz` do 25 h,
  a `neighbors` na hrebecné musí ukázat 5F14D8C9 s čerstvým časem

## Kdyby něco

- uzel po aktivaci nemluví → `get repeat` (on?) a OBĚ intervaly (120/25, ne 0)
- BLE nikde → dlouhý stisk = vypnout a zapnout
- ostatní jde z domova přes RF (skill remote-admin) — KROMĚ vypnutého uzlu;
  radši 2 min kontroly navíc než sundavat stožár

## Po aktivaci: ostrov je na meshi → most a room (23. 8. 2026)

Jakmile Plešivec slyší Klínovec (ověř: `!link` v #tth-test od tth-bot-hrebecna
ukáže cestu bez ob1/ob2 hashů, nebo CoreScope RX na chatě s cizím originem):

1. **Vypnout internetový most** — na dopey `sudo systemctl disable --now
   openhop-ether` a v `home-scripts/openhop-observer/host_vars/*` vrátit
   `oh_mode` (dopey→monitor, sneezy→monitor) + odebrat `oh_bridge_link_host`;
   `ansible-playbook deploy.yml`. Most s RF překryvem = dvojité floody
   a fantomové trasy, byl vždycky dočasný.
2. **Room `abertamy-hrebecna` udělat public** — na sneezy (v kontejneru):
   `POST /api/send_room_server_advert {"name":"RoomServer",
   "node_name":"abertamy-hrebecna"}` (login admin + heslo z configu).
   Do té doby room NEADVERTUJE a zná ho jen ten, kdo má pubkey `6284ded4…`.
