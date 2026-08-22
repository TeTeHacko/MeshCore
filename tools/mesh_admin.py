#!/usr/bin/env python3
"""Admin CLI na uzel PŘES RF, skrz companion identitu openHop démona.

    echo -n '<heslo>' | mesh_admin.py <pubkey|prefix> "get repeat" "ble off"
    mesh_admin.py                     # jen vypíše kontakty, nic nepošle

Existuje kvůli uzlům, ke kterým se nedá přijít s kabelem ani s Bluetooth:
stožár, kopec, chata 130 km daleko. Companion openHopu (`oh_scrape_enabled`)
je rádio, tohle je konzole k němu.

Heslo jde VÝHRADNĚ přes stdin — v argv by ho viděl každý `ps`, a konzole
uzlu příkaz echuje zpátky, takže by skončilo i ve výpisu.

CO PŘES MESH NEJDE: `stats-packets`, `stats-radio`, `stats-core`, `log`,
`erase`, `get acl`, `set freq`, `set prv.key` — ty jsou v CommonCLI.cpp
gatované na `sender_timestamp == 0`, tedy na lokální konzoli (USB/BLE).
Všechno ostatní ano, včetně `advert`, `nodes`, `neighbors` a `ble off`.

POZOR, `ble off` je persistentní: než ho pošleš, ověř TOUHLE cestou, že uzel
po meshi odpovídá — jinak si zavřeš dveře, které už nemáš čím otevřít.
"""
import asyncio
import os
import sys

from meshcore import EventType, MeshCore

PORT = 5012
NAME = os.environ.get("MC_NAME", "node")


async def main():
    args = sys.argv[1:]
    prefix = args[0].lower() if args else None
    cmds = args[1:]

    pwd = ""
    if not sys.stdin.isatty():
        pwd = sys.stdin.readline().strip()

    mc = await MeshCore.create_tcp(host="127.0.0.1", port=PORT, auto_reconnect=False)

    # bufferuj odpovědi OD ZAČÁTKU: wait_for_event vidí jen události vzniklé
    # po svém zavolání, takže odpověď doručená během čekání jinde by zmizela
    inbox = []
    mc.subscribe(EventType.CONTACT_MSG_RECV, lambda ev: inbox.append(ev.payload))
    # Šířku path hashe u paketů, které originuje COMPANION, drží jeho vlastní
    # pref — ne `mesh.path_hash_mode` démona. Default je 0 = 1 bajt, a config
    # pro něj knob nemá; nastavuje se při každém připojení, protože restart
    # démona ho vrací zpátky. Zbytek CZ meshe jede na 2 B.
    if (await mc.commands.get_path_hash_mode()) != 1:
        await mc.commands.set_path_hash_mode(1)
    await mc.ensure_contacts()
    contacts = mc.contacts or {}

    print(f"kontaktů: {len(contacts)}")
    target = None
    for name, c in contacts.items():
        pk = (c.get("public_key") or "").lower()
        print(f"  {name!r:28} {pk[:12]} type={c.get('type')} path_len={c.get('out_path_len')}")
        if prefix and pk.startswith(prefix):
            target = c
    if not prefix:
        return
    if target is None:
        # Bez kontaktu to jde taky: `_validate_destination(dst, prefix_length=32)`
        # bere celý 32bajtový pubkey v hexu. Uzel, který ještě neadvertoval,
        # se tak dá oslovit z jeho zálohované identity — bez cesty jde dotaz
        # floodem, což na 0 hopů nevadí.
        if len(prefix) == 64:
            # Login se šifruje sdíleným tajemstvím odvozeným z kontaktu, takže
            # holá adresa nestačí (`send_login` vrátí ERROR). Kontakt ale jde
            # vyrobit ze samotného pubkey — uzel, který nikdy neadvertoval, se
            # tak dá oslovit ze zálohované identity. `out_path_len -1` = cesta
            # neznámá ⇒ dotaz jde floodem, což na 0 hopů nevadí.
            name = (sys.argv[0] and NAME) or "node"
            print(f"kontakt {prefix[:8]}… neexistuje, zakládám ho z pubkey")
            await mc.commands.add_contact({
                "public_key": prefix,
                "type": 2,              # 2 = repeater (companion je 1)
                "flags": 0,
                "out_path": "",
                "out_path_len": -1,
                "out_path_hash_mode": 0,
                "adv_name": name,
                "last_advert": 0,
                "adv_lat": 0.0,
                "adv_lon": 0.0,
            })
            await asyncio.sleep(2)
            fresh = await mc.commands.get_contacts()
            for _n, c in ((fresh.payload if fresh else None) or {}).items():
                if (c.get("public_key") or "").lower().startswith(prefix):
                    target = c
            if target is None:
                print("kontakt se nepodařilo založit")
                return
            print("kontakt založen")
        else:
            print(f"KONTAKT {prefix} NENÍ — uzel musí advertnout, nebo zadej celý pubkey")
            return
    if not cmds:
        return

    await mc.start_auto_message_fetching()

    ev = await mc.commands.send_login(target, pwd)
    print("login:", getattr(ev, "type", ev))
    ok = await mc.wait_for_event(EventType.LOGIN_SUCCESS, timeout=20)
    if ok is None:
        fail = await mc.wait_for_event(EventType.LOGIN_FAILED, timeout=1)
        print("LOGIN SELHAL", "(LOGIN_FAILED)" if fail else "(timeout)")
        return
    print("login OK")

    for cmd in cmds:
        inbox.clear()
        await mc.commands.send_cmd(target, cmd)
        for _ in range(40):            # ~20 s
            if inbox:
                break
            await asyncio.sleep(0.5)
        out = [m.get("text", "") for m in inbox] or ["(bez odpovědi)"]
        print(f"$ {cmd}\n  -> " + " | ".join(out))
        await asyncio.sleep(1)

    await mc.disconnect()


asyncio.run(main())
