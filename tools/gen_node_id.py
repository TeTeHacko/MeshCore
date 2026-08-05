#!/usr/bin/env python3
"""Vygeneruje identitu uzlu s NEKOLIZNIM path hashem.

Proc to nejde nechat na nahode
-------------------------------
Path hash je prosty prefix verejneho klice (`Identity.h:20`) a jeho sirku urcuje
ODESILATEL paketu, ne forwarder -- `Mesh.cpp:346` pripoji `packet->getPathHashSize()`
bajtu, takze do jednobajtoveho paketu pripise jeden bajt i repeater nastaveny na dva.
Kandidatem na hop je proto VZDY cela populace repeateru, ne jen ti nastaveni na tutez
sirku. Zmereno na CZ meshi 5. 8. 2026 (669 repeateru):

    1 bajt  -> 620 repeateru sedi na sdilenem prefixu (253 z 256 hodnot obsazeno)
    2 bajty -> 6                                      (obsazenost 1,25 %)
    3 bajty -> 0

Jednobajtova unikatnost je tedy neziskatelna (pigeonhole: 669 > 256), zato dvoubajtova
je skoro zadarmo. Kolize neni kosmetika: `isHashMatch` porovna prave tyhle bajty, takze
na kolidujici hop reaguje VIC repeateru -- rozbiji to routovani i atribuci v analyzeru.

Pouziti
-------
    tools/gen_node_id.py                        # 2B prefix volny proti CoreScope
    tools/gen_node_id.py --bytes 3              # prisnejsi
    tools/gen_node_id.py --first-byte 5f        # k tomu konkretni prvni bajt
    tools/gen_node_id.py --known klice.txt      # offline, misto CoreScope

Nahrani do uzlu (klic je 64 B = seed32 || pubkey32, viz PRV_KEY_SIZE v MeshCore.h):
    meshcore-cli -a <adresa> set private_key <128 hex znaku>
"""
import argparse, hashlib, json, os, secrets, sys, urllib.request
from cryptography.hazmat.primitives import serialization as ser
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey


def keypair(seed: bytes):
    """Vrati (pub32, prv64) ve formatu, ktery ceka MeshCore.

    POZOR: prv NENI seed||pub (konvence libsodium), ale **clampnuty SHA-512(seed)**
    -- viz lib/ed25519/keypair.c:

        sha512(seed, 32, private_key);
        private_key[0] &= 248; private_key[31] &= 63; private_key[31] |= 64;
        ge_scalarmult_base(&A, private_key);

    `set prv.key` jinak odpovi "Error, bad key", protoze validatePrivateKey()
    si pubkey odvodi pres ed25519_derive_pub() z prvnich 32 bajtu jako ze skalaru.
    Pub pocitame pres standardni Ed25519 ze seedu -- RFC 8032 dela presne tohle.
    """
    h = bytearray(hashlib.sha512(seed).digest())
    h[0] &= 248
    h[31] &= 63
    h[31] |= 64
    pub = Ed25519PrivateKey.from_private_bytes(seed).public_key().public_bytes(
        ser.Encoding.Raw, ser.PublicFormat.Raw)
    return pub, bytes(h)

# Verejny komunitni analyzer: vidi cely mesh (50 observeru), ne jen to, co slysi
# nase dve. Kolize se musi hledat proti nejuplnejsi populaci, jakou sezeneme.
DEFAULT_SRC = "https://analyzer.meshcore.cz/api/analytics/hash-sizes"


def load_known(src):
    """Vrati (vsechny_pubkey, pubkey_repeateru) jako lowercase hex."""
    if src.startswith("http"):
        with urllib.request.urlopen(src, timeout=20) as r:
            d = json.load(r)
        nodes = d["multiByteCapability"]
        return ([n["pubkey"].lower() for n in nodes],
                [n["pubkey"].lower() for n in nodes if n.get("role") == "repeater"])
    keys = [l.strip().lower() for l in open(src) if l.strip()]
    return keys, keys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bytes", type=int, default=2,
                    help="kolik bajtu prefixu musi byt volnych (default 2)")
    ap.add_argument("--first-byte", metavar="HEX",
                    help="vynutit konkretni prvni bajt, napr. 5f")
    ap.add_argument("--known", default=DEFAULT_SRC,
                    help="zdroj znamych klicu: URL CoreScope nebo soubor")
    ap.add_argument("--max-tries", type=int, default=5_000_000)
    ap.add_argument("--derive", metavar="HEX128",
                    help="jen odvodit pubkey z existujiciho 64B prv.key a skoncit. "
                         "PAST: prv[32:64] NENI pubkey (je to druha pulka SHA-512) "
                         "-- pub = ge_scalarmult_base(prv[0:32]). Chce PyNaCl.")
    ap.add_argument("--out", metavar="SOUBOR",
                    help="kam zapsat privatni klic (0600). Bez toho se NEVYPISUJE "
                         "-- klic nema co delat v scrollbacku terminalu.")
    a = ap.parse_args()

    if a.derive:
        import nacl.bindings as nb          # jen tady, jinak staci `cryptography`
        prv = bytes.fromhex(a.derive.strip())
        if len(prv) != 64:
            sys.exit(f"prv.key ma mit 64 bajtu (128 hex), ma {len(prv)}")
        print(nb.crypto_scalarmult_ed25519_base_noclamp(prv[:32]).hex())
        return

    try:
        allk, repk = load_known(a.known)
    except Exception as e:
        sys.exit(f"nelze nacist zname klice z {a.known}: {e}")

    n = a.bytes * 2
    taken_all = {k[:n] for k in allk}
    bucket1 = {}
    for k in repk:
        bucket1[k[:2]] = bucket1.get(k[:2], 0) + 1
    print(f"znamych uzlu {len(allk)} (z toho repeateru {len(repk)}); "
          f"obsazenych {a.bytes}B prefixu: {len(taken_all)}", file=sys.stderr)

    want = a.first_byte.lower() if a.first_byte else None
    if want and len(want) != 2:
        sys.exit("--first-byte chce prave dva hex znaky")

    if want in ("00", "ff"):
        sys.exit("00 a ff nejsou volne, ale ZAKAZANE -- Identity.cpp:56 odmita "
                 "klic s pub[0] == 0x00 nebo 0xFF jako neplatny")

    for i in range(a.max_tries):
        seed = secrets.token_bytes(32)
        pub, prv64 = keypair(seed)
        h = pub.hex()
        if h[:2] in ("00", "ff"):     # firmware by takovy klic odmitl
            continue
        if want and h[:2] != want:
            continue
        if h[:n] in taken_all:
            continue
        crowd = bucket1.get(h[:2], 0)
        print(f"nalezeno po {i + 1} pokusech", file=sys.stderr)
        print(f"pub_key    {h}")
        print(f"path hash  1B={h[:2]}  2B={h[:4]}  3B={h[:6]}")
        print(f"prvni bajt {h[:2]} sdili {crowd} repeateru"
              + ("  <-- unikatni i na 1 bajt!" if crowd == 0 else ""))
        if a.out:
            fd = os.open(a.out, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            with os.fdopen(fd, "w") as f:
                f.write(prv64.hex() + "\n")
            print(f"\nprivatni klic zapsan do {a.out} (0600). Nahrani do uzlu:")
            print(f"  repeater po seriove konzoli:  set prv.key $(cat {a.out})")
            print(f"  companion pres meshcore-cli:  set private_key $(cat {a.out})")
        else:
            print("\n(privatni klic se nevypisuje; pouzij --out SOUBOR)")
        return
    sys.exit("nenalezeno, zvys --max-tries nebo uvolni podminky")


if __name__ == "__main__":
    main()
