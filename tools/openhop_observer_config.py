#!/usr/bin/env python3
"""Vygeneruj config.yaml pro openHop Repeater v roli PASIVNÍHO observeru.

  tools/openhop_observer_config.py --name tth-ob1 --iata ULK \
      --key out/identities/tth-ob1.key --serial 67901109B61E604A \
      --out out/openhop/ob1/config.yaml

Proč skript a ne ručně psaný YAML: démon si config.yaml **přepisuje sám** (ukládá
do něj vygenerovaný JWT secret a znormalizuje celý soubor), takže komentáře v něm
nepřežijí ani první start. Zdroj pravdy je tedy tenhle generátor, ne ten YAML.

Co je tu zadrátované a proč:

  mode: no_tx          Observer nesmí do meshe vysílat. `engine.py` z toho dělá
                       allow_local_tx=False, `main.py` vypne adverty. Pásovka
                       a šle: adverty 0, discovery off, LBT off, tx_power 2 dBm.
                       Orákulum, že to opravdu mlčí, není tenhle config, ale
                       sent_flood_count/sent_direct_count/forwarded_count = 0
                       v /api/stats a `drop_reason: "Repeat disabled"` u paketů.
  identity_key         64bajtový MeshCore klíč (scalar‖nonce) přímo v configu,
                       stejná cesta jako `convert_firmware_key.sh` — narozdíl od
                       identity_file (base64 32 B), který si démon generuje sám.
                       Klíče se melou `tools/gen_node_id.py` na nekolizní 2B
                       prefix; observer sice nevysílá, ale analyzer atribuuje
                       hopy podle prefixu pubkey, tak ať nepřidáváme kolizi.
  radio_type: pymc_usb Deska běží openhop_modem firmware = hloupé SX1262 PHY.
                       POZOR: ten firmware přejmenuje USB port ze
                       usb-Seeed_Studio_XIAO_nRF52840_<sn> na
                       usb-Seeed_XIAO-Wio-SX1262_<sn>, proto se sem dává jen
                       sériové číslo a cestu skládá skript.
  http.host 127.0.0.1  Web UI je za loginem, ale statika a /auth/login jsou
                       veřejné a heslo je jen v configu. Do LAN to pusť teprve
                       s rozmyslem (viz README).
  mqtt_brokers         Lokální CoreScope zapnutý, komunitní cluster CZ1/CZ2
                       (mqtt1.meshcore.cz, mqtt2.meshcore.website — WSS:443,
                       TLS, JWT s audience = hostname brokeru) zapsaný, ale
                       `enabled: false`. Přepnutí je jeden flag. Komunita si
                       výslovně přeje POUZE uzly na CZ presetu.
"""

from __future__ import annotations

import argparse
import pathlib
import secrets
import sys

try:
    import yaml
except ImportError:
    sys.exit("chybi pyyaml: .venv/bin/pip install pyyaml")

# CZ preset ([cz_radio] v platformio.local.ini). Observer, který sedí na jiném
# presetu, neslyší nic zajímavého a do komunitního clusteru nesmí vůbec.
CZ_RADIO = {
    "frequency": 869_432_000,
    "bandwidth": 62500,
    "spreading_factor": 7,
    "coding_rate": 5,
    "tx_power": 2,
    "preamble_length": 16,
    "implicit_header": False,
}

COMMUNITY_BROKERS = [
    ("CZ 1", "mqtt1.meshcore.cz"),
    ("CZ 2", "mqtt2.meshcore.website"),
]


def build(name: str, iata: str, key_hex: str, serial: str, storage: str,
          local_host: str, local_port: int, http_port: int, owner: str,
          email: str, community: bool) -> dict:
    key = bytes.fromhex(key_hex)
    if len(key) != 64:
        raise SystemExit(f"privatni klic musi mit 64 B (128 hex znaku), ma {len(key)}")

    brokers = [{
        "name": "corescope-local",
        "enabled": True,
        "host": local_host,
        "port": local_port,
        "transport": "tcp",
        "format": "meshcoretomqtt",   # topic meshcore/{IATA}/{PUBKEY}/{packets,status}
        "retain_status": True,
        "disallowed_packet_types": [],
    }]
    for broker_name, host in COMMUNITY_BROKERS:
        brokers.append({
            "name": broker_name,
            "enabled": community,
            "host": host,
            "port": 443,
            "transport": "websockets",
            "format": "meshcoretomqtt",
            "retain_status": True,
            "disallowed_packet_types": [],
            "use_jwt_auth": True,
            "audience": host,          # NE analyzer.meshcore.cz — audience je broker
            "tls": {"enabled": True, "insecure": False},
        })

    return {
        "repeater": {
            "node_name": name,
            "mode": "no_tx",
            "identity_key": key,
            "latitude": 0.0,
            "longitude": 0.0,
            "owner_info": owner,
            "send_advert_interval_hours": 0,
            "allow_discovery": False,
            "cache_ttl": 3600,
            "max_flood_hops": 64,
            "security": {
                "max_clients": 1,
                "admin_password": secrets.token_urlsafe(12),
                "guest_password": "",
                "allow_read_only": False,
                "jwt_secret": secrets.token_hex(32),
                "jwt_expiry_minutes": 60,
            },
        },
        "radio_type": "pymc_usb",
        "pymc_usb": {
            "port": f"/dev/serial/by-id/usb-Seeed_XIAO-Wio-SX1262_{serial}-if00",
            "baudrate": 921600,
            "lbt_enabled": False,
        },
        "radio": dict(CZ_RADIO),
        "mesh": {"path_hash_mode": 1},   # 2 B, jako zbytek naší flotily
        "gps": {"enabled": False},
        "sensors": {"enabled": False},
        "glass": {"enabled": False},
        "storage": {"storage_dir": storage},
        "http": {"host": "127.0.0.1", "port": http_port},
        "logging": {"level": "INFO"},
        "mqtt_brokers": {
            "iata_code": iata,
            "owner": owner,
            "email": email,
            "status_interval": 300,
            "brokers": brokers,
        },
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=(__doc__ or "").split("\n")[0])
    ap.add_argument("--name", required=True, help="node_name, napr. tth-ob1")
    ap.add_argument("--iata", required=True, help="trojznak regionu, napr. ULK")
    ap.add_argument("--key", required=True, help="soubor s 64B prv.key v hexu (gen_node_id.py)")
    ap.add_argument("--serial", required=True, help="seriove cislo desky s modem firmwarem")
    ap.add_argument("--out", required=True, help="kam zapsat config.yaml (0600)")
    ap.add_argument("--storage", help="storage_dir na cilovem hostu (default vedle configu)")
    ap.add_argument("--local-broker", default="stor.grg", help="host lokalniho CoreScope")
    ap.add_argument("--local-port", type=int, default=1883)
    ap.add_argument("--http-port", type=int, default=8000)
    ap.add_argument("--owner", default="tth")
    ap.add_argument("--email", default="")
    ap.add_argument("--community", action="store_true",
                    help="zapnout komunitni brokery CZ1/CZ2 (JEN pro uzel na CZ presetu!)")
    args = ap.parse_args()

    key_hex = pathlib.Path(args.key).read_text().strip()
    out = pathlib.Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    storage = args.storage or str(out.parent / "data")

    cfg = build(args.name, args.iata, key_hex, args.serial, storage,
                args.local_broker, args.local_port, args.http_port,
                args.owner, args.email, args.community)

    # 0600 od začátku: v configu je privátní klíč uzlu i admin heslo.
    out.touch(mode=0o600, exist_ok=True)
    out.chmod(0o600)
    out.write_text(yaml.safe_dump(cfg, sort_keys=True, allow_unicode=True))

    pub_hint = "pubkey si přečti z /api/stats nebo z logu při startu"
    print(f"zapsano {out} (0600)")
    print(f"  node_name   {args.name}")
    print(f"  IATA        {args.iata}")
    print(f"  port        {cfg['pymc_usb']['port']}")
    print(f"  radio       {CZ_RADIO['frequency']/1e6:.3f} MHz SF{CZ_RADIO['spreading_factor']}"
          f" BW{CZ_RADIO['bandwidth']//1000} CR{CZ_RADIO['coding_rate']} @ {CZ_RADIO['tx_power']} dBm")
    print(f"  brokery     " + ", ".join(
        f"{b['name']}{'' if b['enabled'] else ' (vypnuty)'}" for b in cfg["mqtt_brokers"]["brokers"]))
    print(f"  admin heslo v configu, {pub_hint}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
