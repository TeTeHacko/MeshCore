#!/usr/bin/env python3
"""Legacy Nordic DFU (SDK ~11, dfu_version 0.5) over BLE pro Adafruit nRF52 bootloader.
Generický BlueZ adaptér přes bleak (bez Nordic donglu).

Fáze 1: připoj běžící APP, enable notify na 1531, zapiš 0x01 (buttonless) → reboot do bootloaderu.
Fáze 2: reconnect na bootloader (advertuje 1530 DFU svc), proveď START/INIT/IMAGE(PRN)/VALIDATE/ACTIVATE.
        BLE DFU je flaky (dropnutá receipt notif → přenos spadne). Fáze 2 se proto
        auto-retryuje: po pádu pošle RESET (bootloader → čistý IDLE) a zkusí znovu.

Použití: ble_dfu.py <zip> <ble-mac> [phase2]
  phase2  přeskočí fázi 1 (buttonless) — použij když uzel UŽ visí v bootloaderu
          (advertuje SCAP_DFU), např. po přerušeném DFU. Zotavení bez power-cycle.
"""
import asyncio, struct, sys, zipfile, io
from bleak import BleakClient, BleakScanner

DFU_SVC = "00001530-1212-efde-1523-785feabcd123"
CP_UUID = "00001531-1212-efde-1523-785feabcd123"   # control point (write+notify)
PKT_UUID = "00001532-1212-efde-1523-785feabcd123"  # packet (write-no-resp)

# opcody
START_DFU, INIT_DFU, RECEIVE_FW, VALIDATE, ACTIVATE_RESET, RESET, REPORT, PKT_RCPT_REQ = 1,2,3,4,5,6,7,8
RESP, PKT_RCPT = 0x10, 0x11

class Notif:
    def __init__(self): self.q = asyncio.Queue()
    def cb(self, _h, data): self.q.put_nowait(bytes(data))
    async def wait(self, timeout=30):
        return await asyncio.wait_for(self.q.get(), timeout)

def log(*a): print(*a, flush=True)

async def expect_resp(nf, opcode, timeout=30):
    while True:
        d = await nf.wait(timeout)
        if d and d[0] == RESP and d[1] == opcode:
            if d[2] != 1:
                raise RuntimeError(f"DFU resp opcode {opcode} status {d[2]} (fail)")
            return d
        if d and d[0] == PKT_RCPT:
            continue  # receipt, ignoruj tady
        log("   (neočekávaná notif:", d.hex(), ")")

def mac_plus_one(mac):
    p = mac.upper().split(":")
    p[-1] = "%02X" % ((int(p[-1], 16) + 1) & 0xFF)
    return ":".join(p)

async def find(mac, want_dfu=False, timeout=30):
    """Najdi zařízení. POZOR: běžící APP inzeruje DFU službu taky (kvůli
    buttonless), takže 'want_dfu' NESMÍ matchovat jen podle DFU svc -> spletl by
    si app s bootloaderem. Bootloader advertuje SCAP_DFU na MAC+1 (SenseCap na
    stožáru) NEBO AdaDFU na TÉŽE MAC (XIAO, novější SenseCap Solar).

    ADRESA JE POVINNÁ. Dřív stačil název obsahující "DFU" bez ohledu na adresu a
    bralo se prostě první zařízení ze skenu -- s víc deskami v bootloaderu
    najednou to sáhne na cizí. Stalo se: cíl CE:72:…:02, připojilo se
    FE:11:…:C4. Zachytilo se to jen proto, že se log četl ručně; o pár sekund
    později by START_DFU přepsal jinou desku."""
    boot_mac = mac_plus_one(mac)
    log(f"   scan (want_dfu={want_dfu}, mac={mac}, boot={boot_mac})...")
    devs = await BleakScanner.discover(timeout=timeout, return_adv=True)
    cand, rejected = [], []
    for d, adv in devs.values():
        addr = d.address.upper()
        has_dfu = DFU_SVC in [u.lower() for u in (adv.service_uuids or [])]
        name = (d.name or adv.local_name or "")
        if want_dfu:
            if not has_dfu:
                continue
            looks_like_dfu = "DFU" in name.upper()
            right_addr = addr in (boot_mac, mac.upper())
            if looks_like_dfu and right_addr:
                cand.append((d, adv, 0))
            elif looks_like_dfu:
                rejected.append(f"{addr} {name!r}")
        elif addr == mac.upper():
            cand.append((d, adv, 1))
    cand.sort(key=lambda x: x[2])
    for d, adv, _ in cand:
        log(f"   kandidát: {d.address} {d.name!r} dfu={DFU_SVC in [u.lower() for u in (adv.service_uuids or [])]}")
    if rejected:
        log(f"   (v bootloaderu i CIZÍ desky, ignoruji: {', '.join(rejected)})")
    return cand[0][0] if cand else None

async def phase1_buttonless(mac):
    log("== FÁZE 1: buttonless reboot do bootloaderu ==")
    dev = await BleakScanner.find_device_by_address(mac, timeout=25)
    if not dev: raise RuntimeError("APP nenalezen ve scanu")
    async with BleakClient(dev, timeout=30) as c:
        log("   připojen k APP, is_connected:", c.is_connected)
        svcs = [s.uuid.lower() for s in c.services]
        if DFU_SVC not in svcs:
            raise RuntimeError("APP nemá DFU službu 1530")
        nf = Notif()
        await c.start_notify(CP_UUID, nf.cb)   # write-authorize vyžaduje notify enabled
        try:
            await c.write_gatt_char(CP_UUID, bytes([START_DFU]), response=True)
        except Exception as e:
            log("   (write 0x01 → disconnect, očekávané):", type(e).__name__)
    log("   APP odpojen, čekám na reboot bootloaderu...")
    await asyncio.sleep(4)

class ImageDone(Exception):
    """Bootloader potvrdil celý image dřív, než smyčka došla na PRN práh."""

class ChunkTooBig(Exception):
    """Bootloader neodpověděl na první receipt -> nezvládá velké pakety."""

async def send_image(c, nf, pkt_char, data, prn=8, chunk=None):
    if chunk is None:
        mtu = getattr(c, "mtu_size", 23) or 23
        chunk = max(20, mtu - 3)
    log(f"   posílám image {len(data)} B, chunk {chunk} B, PRN {prn}")
    sent = 0; since_rcpt = 0
    total = len(data)
    for off in range(0, total, chunk):
        piece = data[off:off+chunk]
        await c.write_gatt_char(pkt_char, piece, response=False)
        sent += len(piece); since_rcpt += 1
        if prn and since_rcpt >= prn:
            since_rcpt = 0
            # počkej na receipt [0x11, <4B received>]
            while True:
                try:
                    d = await nf.wait(60)
                except asyncio.TimeoutError:
                    # Ticho po PRVNÍ skupině = bootloader ty pakety vůbec
                    # nepobral. Není to congestion, ale velikost: legacy DFU
                    # (SDK 11) na některých bootloaderech bere jen 20 B, a když
                    # BlueZ vyjedná MTU 247, posíláme 244 B a přenos umře pod
                    # 48 kB. Pozorováno: XIAO nRF52840 (AdaDFU, MTU 247) padá,
                    # SenseCap Solar (AdaDFU, MTU zůstane 23) i tth-ltm
                    # (SCAP_DFU na MAC+1, MTU 247) jedou. Volající to zkusí
                    # znovu s 20 B.
                    if sent <= prn * chunk and chunk > 20:
                        raise ChunkTooBig()
                    raise
                if d and d[0] == PKT_RCPT:
                    got = struct.unpack("<I", d[1:5])[0]
                    if got != sent:
                        raise RuntimeError(f"PRN mismatch: bootloader {got} vs poslal {sent}")
                    break
                # Poslední skupina nemusí na PRN práh dosáhnout: bootloader pak
                # místo receiptu pošle rovnou úspěšnou RESP(RECEIVE_FW). To je
                # konec přenosu, ne chyba.
                if d and d[0] == RESP and d[1] == RECEIVE_FW and d[2] == 1:
                    log(f"   image potvrzen bootloaderem na {sent}/{total} B")
                    raise ImageDone()
                if d and d[0] == RESP:
                    raise RuntimeError(f"nečekaná RESP během image: {d.hex()}")
            if (sent // chunk) % 200 == 0:
                log(f"      {sent}/{total} B ({100*sent//total} %)")
    log(f"   image odesláno {sent}/{total} B")

async def phase2_dfu(mac, fw_bin, fw_dat, chunk=None):
    log("== FÁZE 2: DFU na bootloaderu ==")
    dev = None
    for attempt in range(6):
        dev = await find(mac, want_dfu=True, timeout=12)
        if dev: break
        log(f"   bootloader zatím nenalezen (pokus {attempt+1})...")
    if not dev: raise RuntimeError("bootloader (DFU svc) nenalezen ve scanu")
    async with BleakClient(dev, timeout=30) as c:
        log("   připojen k bootloaderu:", dev.address, "mtu:", getattr(c, "mtu_size", "?"))
        try:
            await c._backend._acquire_mtu()   # BlueZ: vyjednej větší MTU
            log("   MTU po acquire:", getattr(c, "mtu_size", "?"))
        except Exception as e:
            log("   (_acquire_mtu:", type(e).__name__, e, ")")
        nf = Notif()
        await c.start_notify(CP_UUID, nf.cb)
        # START_DFU (application)
        await c.write_gatt_char(CP_UUID, bytes([START_DFU, 0x04]), response=True)
        await c.write_gatt_char(PKT_UUID, struct.pack("<III", 0, 0, len(fw_bin)), response=False)
        await expect_resp(nf, START_DFU); log("   START ok")
        # INIT packet
        await c.write_gatt_char(CP_UUID, bytes([INIT_DFU, 0x00]), response=True)
        await c.write_gatt_char(PKT_UUID, fw_dat, response=False)
        await c.write_gatt_char(CP_UUID, bytes([INIT_DFU, 0x01]), response=True)
        await expect_resp(nf, INIT_DFU); log("   INIT ok")
        # PRN
        prn = 8
        await c.write_gatt_char(CP_UUID, bytes([PKT_RCPT_REQ]) + struct.pack("<H", prn), response=True)
        # RECEIVE image
        await c.write_gatt_char(CP_UUID, bytes([RECEIVE_FW]), response=True)
        try:
            await send_image(c, nf, PKT_UUID, fw_bin, prn=prn, chunk=chunk)
            await expect_resp(nf, RECEIVE_FW, timeout=90)
        except ImageDone:
            pass          # RESP(RECEIVE_FW) už dorazila uvnitř send_image
        # ChunkTooBig se TADY neresi. Restart od START_DFU uvnitr tehoz spojeni
        # bootloader odmitne se status 2 = INVALID_STATE -- stavovy automat uz je
        # v "receiving" a dostat ho zpatky do IDLE umi jen RESET (opcode 6) plus
        # nove spojeni. Vyhazuje se ven, main() to zopakuje s vynucenymi 20 B.
        log("   IMAGE ok")
        # VALIDATE
        await c.write_gatt_char(CP_UUID, bytes([VALIDATE]), response=True)
        await expect_resp(nf, VALIDATE); log("   VALIDATE ok")
        # ACTIVATE & reset
        try:
            await c.write_gatt_char(CP_UUID, bytes([ACTIVATE_RESET]), response=True)
        except Exception as e:
            log("   (ACTIVATE → disconnect, očekávané):", type(e).__name__)
    log("== HOTOVO: bootloader aktivoval nový firmware a rebootuje ==")

async def reset_bootloader(mac):
    """Poke a stuck bootloader with RESET (opcode 6) so it reboots into a clean
    DFU IDLE. After an interrupted image transfer the Adafruit bootloader keeps
    its 'receiving image' state, so a fresh START_DFU returns INVALID_STATE;
    this soft-reset clears the state machine (the GPREGRET flag survives, so it
    re-enters DFU) and lets the next attempt start clean."""
    log("   RESET bootloaderu (opcode 6) → čistý DFU IDLE...")
    for att in range(4):
        try:
            dev = await find(mac, want_dfu=True, timeout=12)
            if not dev:
                await asyncio.sleep(2); continue
            async with BleakClient(dev, timeout=25) as c:
                try: await c.start_notify(CP_UUID, lambda _h, _d: None)
                except Exception: pass
                try:
                    await c.write_gatt_char(CP_UUID, bytes([RESET]), response=True)
                    log("   RESET zapsán")
                except Exception as e:
                    log(f"   RESET write → {type(e).__name__} (reboot, očekávané)")
            return True
        except Exception as e:
            log(f"   RESET pokus {att+1}: {type(e).__name__} {str(e)[:50]}")
            await asyncio.sleep(2)
    return False

async def recover_to_bootloader(mac):
    """Get the node back into a clean DFU state, whichever way RESET left it.

    reset_bootloader() on its own is not enough, and the two board families
    differ: after RESET the SenseCap bootloader stays in DFU (the GPREGRET flag
    survives), while the XIAO boots the APPLICATION instead. Assuming the
    SenseCap behaviour made every XIAO retry scan forever for a bootloader that
    was no longer there -- observed on tth-x4-rpt, where the 20 B fallback could
    never get started because the RESET that arms it also ended DFU mode.

    So: reset, and if the bootloader is gone, ask the application to go back."""
    await reset_bootloader(mac)
    await asyncio.sleep(6)
    dev = await find(mac, want_dfu=True, timeout=12)
    if dev:
        return
    log("   po RESETu bootloader pryč (naběhla APP) → buttonless znovu")
    await phase1_buttonless(mac)

async def main():
    zippath, mac = sys.argv[1], sys.argv[2]
    skip_phase1 = len(sys.argv) > 3 and sys.argv[3] == "phase2"
    z = zipfile.ZipFile(zippath)
    fw_bin = z.read("firmware.bin"); fw_dat = z.read("firmware.dat")
    log(f"firmware.bin {len(fw_bin)} B, firmware.dat {len(fw_dat)} B, cíl {mac}")
    if skip_phase1:
        log("== FÁZE 1 přeskočena (uzel už v bootloaderu) ==")
    else:
        await phase1_buttonless(mac)
    # BLE DFU je flaky — přenos občas spadne uprostřed (dropnutá receipt notif).
    # Auto-retry: po pádu vyresetuj bootloader do IDLE a zkus phase2 znovu.
    ATTEMPTS = 6
    chunk = None          # None = mtu-3; prepne se na 20 po ChunkTooBig
    for i in range(ATTEMPTS):
        try:
            await phase2_dfu(mac, fw_bin, fw_dat, chunk=chunk)
            return   # hotovo
        except ChunkTooBig:
            # Tenhle bootloader nebere mtu-3 velke pakety. Nejde to zachranit
            # v behu -- START_DFU podruhe vrati status 2 (INVALID_STATE) --
            # takze RESET, nove spojeni a znovu, natvrdo po 20 B.
            log("!! bootloader nezvlada velke pakety -> opakuji s 20 B")
            chunk = 20
            await recover_to_bootloader(mac)
        except Exception as e:
            log(f"!! DFU pokus {i+1}/{ATTEMPTS} selhal: {type(e).__name__}: {str(e)[:80]}")
            if i == ATTEMPTS - 1:
                raise
            await recover_to_bootloader(mac)

asyncio.run(main())
