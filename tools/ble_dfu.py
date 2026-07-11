#!/usr/bin/env python3
"""Legacy Nordic DFU (SDK ~11, dfu_version 0.5) over BLE pro Adafruit nRF52 bootloader.
Generický BlueZ adaptér přes bleak (bez Nordic donglu).

Fáze 1: připoj běžící APP, enable notify na 1531, zapiš 0x01 (buttonless) → reboot do bootloaderu.
Fáze 2: reconnect na bootloader (advertuje 1530 DFU svc), proveď START/INIT/IMAGE(PRN)/VALIDATE/ACTIVATE.

Použití: ble_dfu.py <zip> <ble-mac>
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

async def find(mac, want_dfu=False, timeout=30):
    """Najdi zařízení — v bootloaderu preferuj to co advertuje DFU svc."""
    log(f"   scan (want_dfu={want_dfu}, mac={mac})...")
    devs = await BleakScanner.discover(timeout=timeout, return_adv=True)
    cand = []
    for d, adv in devs.values():
        has_dfu = DFU_SVC in [u.lower() for u in (adv.service_uuids or [])]
        if want_dfu and has_dfu: cand.append((d, adv, 0))
        elif d.address.upper() == mac.upper(): cand.append((d, adv, 1))
    cand.sort(key=lambda x: x[2])
    for d, adv, _ in cand:
        log(f"   kandidát: {d.address} {d.name!r} dfu={DFU_SVC in [u.lower() for u in (adv.service_uuids or [])]}")
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

async def send_image(c, nf, pkt_char, data, prn=8):
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
                d = await nf.wait(60)
                if d and d[0] == PKT_RCPT:
                    got = struct.unpack("<I", d[1:5])[0]
                    if got != sent:
                        raise RuntimeError(f"PRN mismatch: bootloader {got} vs poslal {sent}")
                    break
                if d and d[0] == RESP:
                    raise RuntimeError(f"nečekaná RESP během image: {d.hex()}")
            if (sent // chunk) % 200 == 0:
                log(f"      {sent}/{total} B ({100*sent//total} %)")
    log(f"   image odesláno {sent}/{total} B")

async def phase2_dfu(mac, fw_bin, fw_dat):
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
        await send_image(c, nf, PKT_UUID, fw_bin, prn=prn)
        await expect_resp(nf, RECEIVE_FW, timeout=90); log("   IMAGE ok")
        # VALIDATE
        await c.write_gatt_char(CP_UUID, bytes([VALIDATE]), response=True)
        await expect_resp(nf, VALIDATE); log("   VALIDATE ok")
        # ACTIVATE & reset
        try:
            await c.write_gatt_char(CP_UUID, bytes([ACTIVATE_RESET]), response=True)
        except Exception as e:
            log("   (ACTIVATE → disconnect, očekávané):", type(e).__name__)
    log("== HOTOVO: bootloader aktivoval nový firmware a rebootuje ==")

async def main():
    zippath, mac = sys.argv[1], sys.argv[2]
    z = zipfile.ZipFile(zippath)
    fw_bin = z.read("firmware.bin"); fw_dat = z.read("firmware.dat")
    log(f"firmware.bin {len(fw_bin)} B, firmware.dat {len(fw_dat)} B, cíl {mac}")
    await phase1_buttonless(mac)
    await phase2_dfu(mac, fw_bin, fw_dat)

asyncio.run(main())
