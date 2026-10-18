#!/usr/bin/env python3
"""HomeKit (HAP) test client for the ESP32-CAM accessory, without an iPhone.

Needs a venv with aiohomekit (>= 4.0):  python3 -m venv .venv && .venv/bin/pip install aiohomekit
Usage:
  hk_test.py -f pairing.json pair  <device-id> <setup-code>   # device id from `dns-sd -L "ESP32 Cam" _hap._tcp` (id=..)
  hk_test.py -f pairing.json check                            # dump camera TLVs; minimal SetupEndpoints
                                                              # (expect status 2 = rejected incomplete request,
                                                              #  or 1 = busy when built with LIVE_STREAM=0)
  hk_test.py -f pairing.json snapshot out.jpg [w h]           # HAP POST /resource
  hk_test.py -f pairing.json watch [seconds]                  # print MotionDetected changes
  hk_test.py -f pairing.json stream out.h264 [seconds]        # act like iOS: negotiate live video, receive
                                                              # SRTP (needs pylibsrtp), save H.264, ffprobe it
  hk_test.py -f pairing.json unpair                           # remove pairing so an iPhone can pair
"""
import argparse
import asyncio
import base64
import contextlib
import os
import pathlib
import socket
import struct
import subprocess
import time

from aiohomekit.characteristic_cache import CharacteristicCacheFile
from aiohomekit.controller import Controller
from aiohomekit.zeroconf import ZeroconfServiceListener
from zeroconf.asyncio import AsyncServiceBrowser, AsyncZeroconf

ALIAS = "esp32cam"
CAMERA_SERVICE = "110"
CHAR_NAMES = {
    "114": "SupportedVideoStreamConfiguration",
    "115": "SupportedAudioStreamConfiguration",
    "116": "SupportedRTPConfiguration",
    "117": "SelectedRTPStreamConfiguration",
    "118": "SetupEndpoints",
    "120": "StreamingStatus",
    "22": "MotionDetected",
}


def short_uuid(t: str) -> str:
    t = t.upper()
    if t.endswith("-0000-1000-8000-0026BB765291"):
        t = t.split("-")[0]
    return t.lstrip("0")


def tlv_decode(data: bytes) -> list[tuple[int, bytes]]:
    items, i = [], 0
    while i + 2 <= len(data):
        tag, ln = data[i], data[i + 1]
        val = data[i + 2:i + 2 + ln]
        i += 2 + ln
        if items and items[-1][0] == tag and len(items[-1][1]) % 255 == 0 and len(items[-1][1]) > 0:
            items[-1] = (tag, items[-1][1] + val)  # fragment continuation
        else:
            items.append((tag, val))
    return items


def tlv_print(data: bytes, indent: int = 2, depth: int = 0) -> None:
    for tag, val in tlv_decode(data):
        pad = " " * (indent * (depth + 1))
        nested = None
        if len(val) >= 3 and depth < 4:
            try:
                parsed = tlv_decode(val)
                if parsed and sum(2 + len(v) for _, v in parsed) == len(val):
                    nested = val
            except Exception:  # noqa: BLE001
                nested = None
        if nested is not None and len(val) > 2:
            print(f"{pad}[{tag}]")
            tlv_print(val, indent, depth + 1)
        else:
            num = int.from_bytes(val, "little") if 0 < len(val) <= 8 else None
            print(f"{pad}[{tag}] len={len(val)} {val.hex()}" + (f" (={num})" if num is not None else ""))


@contextlib.asynccontextmanager
async def controller_for(path: str):
    zc = AsyncZeroconf()
    controller = Controller(async_zeroconf_instance=zc,
                            char_cache=CharacteristicCacheFile(pathlib.Path(path).with_suffix(".charmap.json")))
    async with zc:
        browser = AsyncServiceBrowser(zc.zeroconf, ["_hap._tcp.local.", "_hap._udp.local."],
                                      listener=ZeroconfServiceListener())
        async with controller:
            if os.path.exists(path):
                controller.load_data(path)
            yield controller
            controller.save_data(path)
        await browser.async_cancel()


async def find_chars(pairing) -> dict[str, tuple[int, int]]:
    accs = await pairing.list_accessories_and_characteristics()
    found = {}
    for acc in accs:
        for svc in acc["services"]:
            for ch in svc["characteristics"]:
                u = short_uuid(ch["type"])
                if u in CHAR_NAMES:
                    found[CHAR_NAMES[u]] = (acc["aid"], ch["iid"])
            if short_uuid(svc["type"]) == CAMERA_SERVICE:
                found["_camera_service"] = (acc["aid"], svc["iid"])
    return found


async def cmd_pair(c, a):
    if ALIAS in c.aliases:
        print("already paired")
        return
    discovery = await c.async_find(a.device_id)
    finish = await discovery.async_start_pairing(ALIAS)
    pairing = await finish(a.code)
    c.load_pairing(ALIAS, pairing.pairing_data)  # aiohomekit 4.0.1 does not register the alias itself
    print("paired")


async def cmd_check(c, a):
    p = c.aliases[ALIAS]
    chars = await find_chars(p)
    print("camera service:", chars.get("_camera_service"))
    for name in ("SupportedVideoStreamConfiguration", "SupportedAudioStreamConfiguration",
                 "SupportedRTPConfiguration", "StreamingStatus"):
        aid, iid = chars[name]
        v = (await p.get_characteristics([(aid, iid)]))[(aid, iid)]["value"]
        print(f"{name} ({aid}.{iid}):")
        tlv_print(base64.b64decode(v))

    # SetupEndpoints write + read back, like iOS does when live view opens
    aid, iid = chars["SetupEndpoints"]
    session = os.urandom(16)
    req = bytes([1, 16]) + session
    await p.put_characteristics([(aid, iid, base64.b64encode(req).decode())])
    v = (await p.get_characteristics([(aid, iid)]))[(aid, iid)]["value"]
    resp = base64.b64decode(v)
    print("SetupEndpoints response:")
    tlv_print(resp)
    items = dict(tlv_decode(resp))
    print("session echoed:", items.get(1) == session, "| status:", items.get(2, b"?").hex())


async def cmd_snapshot(c, a):
    p = c.aliases[ALIAS]
    t0 = time.time()
    img = await p.image(1, a.width, a.height)
    dt = time.time() - t0
    if not img:
        print("no image returned")
        return
    pathlib.Path(a.out).write_bytes(img)
    is_jpeg = img[:2] == b"\xff\xd8"
    print(f"{len(img)} bytes in {dt * 1000:.0f} ms -> {a.out} (JPEG: {is_jpeg})")


async def cmd_watch(c, a):
    p = c.aliases[ALIAS]
    chars = await find_chars(p)
    key = chars["MotionDetected"]
    last, t_end = None, time.time() + a.seconds
    while time.time() < t_end:
        v = (await p.get_characteristics([key]))[key]["value"]
        if v != last:
            print(time.strftime("%H:%M:%S"), "MotionDetected =", v)
            last = v
        await asyncio.sleep(0.5)


def tlv(tag: int, val: bytes) -> bytes:
    out = b""
    while True:
        chunk, val = val[:255], val[255:]
        out += bytes([tag, len(chunk)]) + chunk
        if not val:
            return out


def local_ip_for(remote: str) -> str:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.connect((remote, 9))
        return s.getsockname()[0]


async def cmd_stream(c, a):
    from pylibsrtp import Policy, Session  # pip install pylibsrtp

    p = c.aliases[ALIAS]
    chars = await find_chars(p)
    se, sel = chars["SetupEndpoints"], chars["SelectedRTPStreamConfiguration"]
    accessory_ip = p.pairing_data["AccessoryIP"]
    my_ip = local_ip_for(accessory_ip)

    vsock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    vsock.bind(("0.0.0.0", 0))
    vsock.settimeout(0.2)
    asock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    asock.bind(("0.0.0.0", 0))
    vport, aport = vsock.getsockname()[1], asock.getsockname()[1]

    session_id = os.urandom(16)
    vkey, vsalt, akey, asalt = os.urandom(16), os.urandom(14), os.urandom(16), os.urandom(14)
    address = tlv(1, b"\x00") + tlv(2, my_ip.encode()) + tlv(3, struct.pack("<H", vport)) + tlv(4, struct.pack("<H", aport))
    req = (tlv(1, session_id) + tlv(3, address) + tlv(4, tlv(1, b"\x00") + tlv(2, vkey) + tlv(3, vsalt))
           + tlv(5, tlv(1, b"\x00") + tlv(2, akey) + tlv(3, asalt)))
    await p.put_characteristics([(se[0], se[1], base64.b64encode(req).decode())])
    resp = dict(tlv_decode(base64.b64decode((await p.get_characteristics([se]))[se]["value"])))
    status = resp.get(2, b"\xff")[0]
    print("SetupEndpoints status:", status)
    if status != 0:
        return
    acc = dict(tlv_decode(resp[3]))
    acc_ip, acc_vport = acc[2].decode(), struct.unpack("<H", acc[3])[0]
    ssrc = struct.unpack("<I", resp[6])[0]
    print(f"accessory {acc_ip}:{acc_vport} ssrc {ssrc:#010x} -> receiving on {my_ip}:{vport}")

    w, h, fps, kbps = a.width, a.height, a.fps, a.kbps
    video = (tlv(1, b"\x00") + tlv(2, tlv(1, b"\x00") + tlv(2, b"\x00") + tlv(3, b"\x00"))
             + tlv(3, tlv(1, struct.pack("<H", w)) + tlv(2, struct.pack("<H", h)) + tlv(3, bytes([fps])))
             + tlv(4, tlv(1, bytes([99])) + tlv(2, struct.pack("<I", 0x1234)) + tlv(3, struct.pack("<H", kbps))
                   + tlv(4, struct.pack("<f", 0.5)) + tlv(5, struct.pack("<H", 1378))))
    audio = (tlv(1, b"\x03") + tlv(2, tlv(1, b"\x01") + tlv(2, b"\x00") + tlv(3, b"\x02") + tlv(4, b"\x14"))
             + tlv(3, tlv(1, bytes([110])) + tlv(2, struct.pack("<I", 0x5678)) + tlv(3, struct.pack("<H", 24))
                   + tlv(4, struct.pack("<f", 5.0)) + tlv(6, bytes([13]))) + tlv(4, b"\x00"))
    start = tlv(1, tlv(1, session_id) + tlv(2, b"\x01")) + tlv(2, video) + tlv(3, audio)
    await p.put_characteristics([(sel[0], sel[1], base64.b64encode(start).decode())])
    t_start = time.time()

    srtp = Session(policy=Policy(key=vkey + vsalt, ssrc_type=Policy.SSRC_ANY_INBOUND,
                                 srtp_profile=Policy.SRTP_PROFILE_AES128_CM_SHA1_80))
    out, nal = bytearray(), bytearray()
    n_rtp = n_rtcp = n_bad = frames = 0
    first_rx = None
    timestamps, rx_bytes = [], 0
    last_keepalive = 0.0
    while time.time() - t_start < a.seconds:
        if not a.abandon and time.time() - last_keepalive > 2:  # stand-in for iOS RTCP receiver reports
            vsock.sendto(b"\x80\xc9\x00\x01" + struct.pack(">I", 0x1234), (acc_ip, acc_vport))
            last_keepalive = time.time()
        try:
            pkt, _ = vsock.recvfrom(2048)
        except socket.timeout:
            continue
        first_rx = first_rx or time.time()
        rx_bytes += len(pkt)
        try:
            if 200 <= pkt[1] <= 204:
                srtp.unprotect_rtcp(pkt)
                n_rtcp += 1
                continue
            rtp = srtp.unprotect(pkt)
        except Exception:  # noqa: BLE001 - count auth/decrypt failures
            n_bad += 1
            continue
        n_rtp += 1
        hdr = 12 + 4 * (rtp[0] & 0x0F)
        payload = rtp[hdr:]
        nal_type = payload[0] & 0x1F
        if 1 <= nal_type <= 23:
            out += b"\x00\x00\x00\x01" + payload
        elif nal_type == 28:
            if payload[1] & 0x80:
                nal = bytearray([(payload[0] & 0xE0) | (payload[1] & 0x1F)])
            nal += payload[2:]
            if payload[1] & 0x40:
                out += b"\x00\x00\x00\x01" + nal
        if rtp[1] & 0x80:  # marker: end of access unit
            frames += 1
            timestamps.append(struct.unpack(">I", rtp[4:8])[0])

    if a.abandon:
        print("abandoned session (no keepalive, no END): the accessory should time it out")
        return
    end = tlv(1, tlv(1, session_id) + tlv(2, b"\x00"))
    await p.put_characteristics([(sel[0], sel[1], base64.b64encode(end).decode())])

    dur = (timestamps[-1] - timestamps[0]) / 90000 if len(timestamps) > 1 else 0
    print(f"first packet after {((first_rx or t_start) - t_start) * 1000:.0f} ms; RTP {n_rtp}, SRTCP {n_rtcp}, "
          f"auth/decrypt failures {n_bad}")
    print(f"frames {frames}, {(frames - 1) / dur if dur else 0:.1f} fps (RTP clock), "
          f"{rx_bytes * 8 / 1000 / a.seconds:.0f} kbit/s on the wire")
    pathlib.Path(a.out).write_bytes(out)
    probe = subprocess.run(["ffprobe", "-v", "error", "-count_frames", "-select_streams", "v:0", "-show_entries",
                            "stream=codec_name,profile,width,height,nb_read_frames", "-of", "default=nw=1", a.out],
                           capture_output=True, text=True)
    print("ffprobe:", " ".join(probe.stdout.split()), probe.stderr.strip()[:300])
    png = str(pathlib.Path(a.out).with_suffix(".png"))
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", a.out, "-vf", "select=eq(n\\,%d)" % max(0, frames // 2),
                    "-frames:v", "1", png], capture_output=True)
    print("middle frame ->", png)


async def cmd_unpair(c, a):
    if ALIAS in c.aliases:
        await c.remove_pairing(ALIAS)
        print("unpaired")
    else:
        print("not paired")


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-f", dest="file", required=True)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("pair"); s.add_argument("device_id"); s.add_argument("code"); s.set_defaults(fn=cmd_pair)
    s = sub.add_parser("check"); s.set_defaults(fn=cmd_check)
    s = sub.add_parser("snapshot"); s.add_argument("out"); s.add_argument("width", nargs="?", type=int, default=640)
    s.add_argument("height", nargs="?", type=int, default=480); s.set_defaults(fn=cmd_snapshot)
    s = sub.add_parser("watch"); s.add_argument("seconds", nargs="?", type=float, default=30); s.set_defaults(fn=cmd_watch)
    s = sub.add_parser("stream"); s.add_argument("out"); s.add_argument("seconds", nargs="?", type=float, default=15)
    s.add_argument("--width", type=int, default=320); s.add_argument("--height", type=int, default=240)
    s.add_argument("--fps", type=int, default=15); s.add_argument("--kbps", type=int, default=299)
    s.add_argument("--abandon", action="store_true", help="send no keepalives/END (tests the timeout)")
    s.set_defaults(fn=cmd_stream)
    s = sub.add_parser("unpair"); s.set_defaults(fn=cmd_unpair)
    a = ap.parse_args()
    async with controller_for(a.file) as c:
        await a.fn(c, a)


if __name__ == "__main__":
    asyncio.run(main())
