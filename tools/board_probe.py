#!/usr/bin/env python3
"""Otisk XIAO desek na USB: co na které z nich BĚŽÍ, a jestli se to změnilo.

  tools/board_probe.py --all
  tools/board_probe.py --all --compare <soubor s predchozim otiskem>
  tools/board_probe.py --all --compare <soubor> --expect-changed <serial>

Vzniklo 1. 9. 2026 poté, co `pio run -t upload --upload-port <x4>` ohlásil
FAILED a obraz přistál na **x2**, desce, která na té příkazové řádce nebyla.
Poznat to šlo až po hodině: x2 přestal odpovídat konzoli i companion protokolu
a vypadal jako zaseklý, přitom na něm normálně běžel KISS modem, který ani
jedno z toho nemá. Tři replugy nepomohly a pomoct nemohly.

Otisk je proto přes VŠECHNY protokoly, které na těch deskách jezdí, ne jen přes
ten, který zrovna čekáme:

  text        MeshCore repeater/analyzer   `ver` -> v1.17.1-tth...
  companion   MeshCore companion_radio     CMD_DEVICE_QUERY (bez textové konzole)
  kiss        examples/kiss_modem          SetHardware GetVersion (0x11 -> 0x91)
  modem       openhop_modem                port `XIAO-Wio-SX1262`, mlčí na vše
  bootloader  UF2                          port bez "Studio" / se "Sense"

`modem` a `bootloader` se poznají podle JMÉNA portu, ne podle idProduct: obojí
drží 0044 a AGENTS.md čte 0044 jako bootloader, což pro openhop_modem neplatí.
"""
import argparse, glob, os, re, struct, subprocess, sys, threading, time

try:
    import serial
except ImportError:
    sys.exit("chybi pyserial")

# Kolik sekund nejvys smi trvat otisk CELE sbernice.
# 8 s staci: probe zive desky trva ~2,5 s (zmereno). Deska, ktera se do te doby
# neozve, se oznaci jako "visi" -- to je poctivejsi vysledek nez cekat dele.
# ZNAMY NEDOSTATEK: T1000-E karty blokuji uvnitr serial.Serial() open(2) a drzi
# beh i pres deadline, takze cely otisk trva ~30 s navic. Nebrani to praci, jen
# to zdrzuje; skutecna oprava chce otevirat port jinak nez pres pyserial.
PROBE_DEADLINE = float(os.environ.get("BOARD_PROBE_DEADLINE", "8"))


def usb_boards():
    """[(serial, port, board, state_hint)] pro kazdou desku flotily na USB.

    Typ desky i rezim se poznaji z JMENA portu, protoze idProduct lze -- 0044
    drzi jak UF2 bootloader XIAO, tak openhop_modem, coz je aplikace. A na
    T1000-E je to jmeno primo obracene, nez by clovek cekal:

      Seeed_Studio_XIAO_nRF52840_<sn>   XIAO, aplikace (MeshCore build)
      Seeed_XIAO_nRF52840_Sense_<sn>    XIAO, bootloader
      Seeed_XIAO-Wio-SX1262_<sn>        XIAO, openhop_modem (APLIKACE, ne boot)
      Seeed_Studio_T1000-E-BOOT_<sn>    T1000-E, APLIKACE  <- "-BOOT" ma aplikace!
      Seeed_Studio_T1000-E_<sn>         T1000-E, bootloader (+ UF2 disk T1000-E)
    """
    pats = [
        # (regex na jmeno portu, deska, stav)
        (r"Seeed_Studio_T1000-E-BOOT_", "t1000e", "app"),
        (r"Seeed_Studio_T1000-E_",      "t1000e", "boot"),
        (r"Seeed_XIAO-Wio-SX1262_",     "xiao",   "modem"),
        (r"Seeed_Studio_XIAO_nRF52840_", "xiao",  "app"),
        (r"Seeed_XIAO_nRF52840",        "xiao",   "boot"),
        (r"Wio|TRACKER",                "wio-l1", "app"),
    ]
    out = []
    for port in sorted(glob.glob("/dev/serial/by-id/*")):
        name = os.path.basename(port)
        m = re.search(r"_([0-9A-F]{16})-if00", name)
        if not m:
            continue                      # ESP32 a spol. -- neni to nase nRF52 flotila
        for rx, board, state in pats:
            if re.search(rx, name):
                out.append((m.group(1), port, board, state))
                break
    return out


def _open(port):
    s = serial.Serial(port, 115200, timeout=0)
    s.dtr = True
    s.rts = True
    time.sleep(0.9)
    s.reset_input_buffer()
    return s


def _drain(s, w):
    buf, t = b"", time.time()
    while time.time() - t < w:
        d = s.read(8192)
        if d:
            buf += d
        else:
            time.sleep(0.02)
    return buf


def probe(port):
    """('text'|'companion'|'kiss'|'nic', verze) -- zkusí protokoly po řadě."""
    try:
        s = _open(port)
    except Exception as e:
        return ("nedostupny", type(e).__name__)
    try:
        # Prvni prikaz po otevreni portu se casto ztrati, proto to prazdne CRLF.
        s.write(b"\r\n"); time.sleep(0.4); s.reset_input_buffer()

        s.write(b"ver\r\n")
        txt = _drain(s, 1.2).decode(errors="replace")
        m = re.search(r"(v\d+\.\d+\.\d+-tth[0-9a-f]+\+?)", txt)
        if m:
            return ("text", m.group(1))

        s.reset_input_buffer()
        s.write(b"<" + struct.pack("<H", 2) + bytes([22, 10]))   # CMD_DEVICE_QUERY
        s.flush()
        raw = _drain(s, 1.5)
        i = raw.find(b">")
        if i >= 0 and len(raw) > i + 3:
            ln = struct.unpack("<H", raw[i + 1:i + 3])[0]
            fr = raw[i + 3:i + 3 + ln]
            if fr and fr[0] == 13 and len(fr) >= 80:             # RESP_DEVICE_INFO
                return ("companion", fr[60:80].split(b"\0")[0].decode(errors="replace"))
            if fr:
                return ("companion", "?")

        s.reset_input_buffer()
        s.write(bytes([0xC0, 0x06, 0x11, 0xC0]))                 # KISS GetVersion
        raw = _drain(s, 1.2)
        if len(raw) > 4 and raw[1] == 0x06 and raw[2] == 0x91:
            # GetVersion (0x11) vrací verzi KISS PROTOKOLU, ne našeho buildu --
            # ta je k nerozeznání mezi dvěma různými firmwary. Stampovaná verze
            # jede v GetDeviceName (0x16), viz KissModem::handleGetDeviceName.
            s.reset_input_buffer()
            s.write(bytes([0xC0, 0x06, 0x16, 0xC0]))             # KISS GetDeviceName
            nm = _drain(s, 1.2)
            if len(nm) > 4 and nm[1] == 0x06 and nm[2] == 0x96:
                txt = nm[3:-1].decode(errors="replace")
                m = re.search(r"(v\d+\.\d+\.\d+-tth[0-9a-f]+\+?)", txt)
                if m:
                    return ("kiss", m.group(1))
                return ("kiss", txt.strip() or "?")
            # Starší KISS build bez stampované verze v GetDeviceName.
            return ("kiss", "proto" + ".".join(str(b) for b in raw[3:-1]))

        return ("nic", "-")
    finally:
        try:
            s.close()
        except Exception:
            pass


def fingerprint():
    """Otisk cele sbernice. Paralelne -- porty jsou nezavisle a serazovat je za
    sebe se necetlo: pet desek po ~9 s (settle + tri protokoly, u mlcicich desek
    plny timeout) delalo pres dve minuty, coz je na kontrolu spoustenou PRED a PO
    kazdym flashem moc. Poradi vysledku je dane vstupem, ne dobehem."""
    boards = usb_boards()
    rows = [None] * len(boards)

    def one(i, sn, port, board, state):
        if state in ("boot", "modem"):
            # Bootloader nemluvi nasim protokolem a openhop_modem nemluvi vubec;
            # ptat se jich je jen ztrata casu (a u boot i riziko, ze to vypada
            # jako zaseknuta deska).
            rows[i] = (sn, f"{board}/{state}", "-")
            return
        kind, ver = probe(port)
        rows[i] = (sn, f"{board}/{kind}", ver)

    # TVRDY DEADLINE NA DESKU, a DAEMON vlakna. `serial.Serial()` umi na desce,
    # ktera neodpovida, viset v open() a nevratit se -- 1. 9. 2026 kvuli tomu cely
    # otisk nevypsal NIC ani za dve minuty. Nastroj spousteny pred a po kazdem
    # flashi se nesmi dat zablokovat jednou deskou.
    #
    # ThreadPoolExecutor tu nestaci: jeho vlakna nejsou daemon, takze i kdyz se
    # na vysledek necekalo, Python na ne pri exitu pockal a proces nedobehl
    # (`timeout` ho pak zabil, exit 124). Vlastni daemon vlakna proces nedrzi.
    threads = []
    for i, (sn, port, b, st) in enumerate(boards):
        t = threading.Thread(target=one, args=(i, sn, port, b, st), daemon=True)
        t.start()
        threads.append(t)
    deadline = time.time() + PROBE_DEADLINE
    for t in threads:
        t.join(timeout=max(0.2, deadline - time.time()))
    for i, (sn, _, board, _) in enumerate(boards):
        if rows[i] is None:
            rows[i] = (sn, f"{board}/visi", "neodpovida v case")
    return [r for r in rows if r]


def fmt(rows):
    return "\n".join(f"{sn} {kind} {str(ver).strip()}" for sn, kind, ver in rows)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--compare", help="soubor s predchozim otiskem")
    ap.add_argument("--expect-changed", help="seriak, ktery se zmenit MA")
    a = ap.parse_args()

    rows = fingerprint()
    now = {sn: (k, v) for sn, k, v in rows}

    if not a.compare:
        print(fmt(rows))
        return 0

    prev = {}
    for line in open(a.compare):
        # Verze muze obsahovat mezery (starsi KISS build vraci "Seeed Xiao-nrf52"),
        # takze split() na tri pole utne zbytek a porovnani pak hlasi ZMENU tam,
        # kde se nic nezmenilo. Falesny poplach je horsi nez zadna kontrola.
        p = line.split(None, 2)
        if len(p) >= 3:
            prev[p[0]] = (p[1], p[2].strip())

    bad = False
    for sn in sorted(set(prev) | set(now)):
        was, is_ = prev.get(sn), now.get(sn)
        tag = ""
        if was != is_:
            # Poradi je zamerne: deska, ktera ZMIZELA, neni uspech ani kdyz je to
            # cil. Po flashi se to na tehle lavici stava bezne a hlaska "zmeneno
            # (cil)" by to zakryla -- pritom se z desky nedá precist, co na ni je,
            # takze flash NENI overeny. Chce to replug a spustit znovu.
            if is_ is None:
                tag = "  <- ZMIZELA z USB (po flashi bezne) -- replug a spust znovu"
                bad = True
            elif was is None:
                tag = "  <- NOVA deska na sbernici"
            elif a.expect_changed and sn.endswith(a.expect_changed.upper()):
                tag = "  <- zmeneno (cil)"
            else:
                tag = "  <- !!! ZMENENO, PRESTOZE TO NEBYL CIL !!!"
                bad = True
        print(f"  {sn}  {was[0] if was else '-':10s} {was[1] if was else '-':22s}"
              f" -> {is_[0] if is_ else '-':10s} {is_[1] if is_ else '-':22s}{tag}")

    if a.expect_changed:
        sn = next((s for s in now if s.endswith(a.expect_changed.upper())), None)
        if sn and prev.get(sn) == now.get(sn):
            print(f"\nCIL {a.expect_changed} SE NEZMENIL -- flash se neaplikoval,"
                  f" i kdyby nastroj hlasil uspech.")
            bad = True

    print("\nVERDIKT:", "PROBLEM (viz vys)" if bad else "ok, zmenil se jen cil")
    return 1 if bad else 0


if __name__ == "__main__":
    rc = main()
    # os._exit misto sys.exit: vlakno, ktere visi v serial.Serial() open(2),
    # drzi otevreny file descriptor a interpret na nej pri uklidu ceka -- otisk
    # se vypsal za par sekund, ale proces dobihal jeste ~30 s (a pod `timeout`
    # se tvaril jako zaseknuty). Vysledek uz je venku, uklizet nemame co.
    sys.stdout.flush()
    sys.stderr.flush()
    os._exit(rc)
