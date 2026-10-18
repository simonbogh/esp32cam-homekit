#!/usr/bin/env python3
"""Benchmark camera XCLK settings: capture FPS, MJPEG FPS delivered over Wi-Fi, RSSI.

Usage: python3 tools/xclk_bench.py [host] [seconds-per-setting] [mhz-list]
Defaults: esp32cam.local, 30 s, 8,20. Uses only the Python standard library.
"""
import json
import sys
import threading
import time
import urllib.request

HOST = sys.argv[1] if len(sys.argv) > 1 else "esp32cam.local"
SECS = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0
MHZ = [int(m) for m in (sys.argv[3] if len(sys.argv) > 3 else "8,20").split(",")]
WEB = f"http://{HOST}:81"
STREAM = f"http://{HOST}:82/stream"


def get(url, timeout=5):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return r.read()


def stream_frames(seconds, result):
    """Read the MJPEG stream and count complete JPEG frames and bytes."""
    frames, nbytes, t0 = 0, 0, time.time()
    try:
        with urllib.request.urlopen(STREAM, timeout=10) as r:
            buf = b""
            while time.time() - t0 < seconds:
                chunk = r.read(4096)
                if not chunk:
                    break
                nbytes += len(chunk)
                buf += chunk
                while True:
                    soi = buf.find(b"\xff\xd8")
                    eoi = buf.find(b"\xff\xd9", soi + 2) if soi >= 0 else -1
                    if soi < 0 or eoi < 0:
                        break
                    frames += 1
                    buf = buf[eoi + 2:]
    except Exception as e:  # noqa: BLE001 - report and keep the partial result
        result["error"] = str(e)
    elapsed = time.time() - t0
    result.update(frames=frames, fps=frames / elapsed, kbps=nbytes * 8 / 1000 / elapsed)


def bench(mhz):
    get(f"{WEB}/control?xclk={mhz}")
    time.sleep(5)  # let auto-exposure settle
    result, samples = {}, []
    t = threading.Thread(target=stream_frames, args=(SECS, result))
    t.start()
    while t.is_alive():
        try:
            samples.append(json.loads(get(f"{WEB}/stats")))
        except Exception:  # noqa: BLE001 - a missed sample is fine
            pass
        time.sleep(2)
    t.join()
    avg = lambda k: sum(s[k] for s in samples) / max(1, len(samples))  # noqa: E731
    return {
        "xclk_mhz": mhz,
        "cap_fps": round(avg("cap_fps"), 1),
        "mjpeg_fps": round(result.get("fps", 0), 1),
        "mjpeg_kbps": round(result.get("kbps", 0)),
        "frame_kb": round(avg("frame_bytes") / 1024, 1),
        "rssi_avg": round(avg("rssi"), 1),
        "rssi_min": min((s["rssi"] for s in samples), default=0),
        "cap_errors": samples[-1]["cap_errors"] if samples else None,
        "error": result.get("error"),
    }


if __name__ == "__main__":
    rows = [bench(m) for m in MHZ]
    get(f"{WEB}/control?xclk=8")
    print(f"{'XCLK':>5} {'cap fps':>8} {'MJPEG fps':>10} {'kbit/s':>7} {'frame KB':>9} {'RSSI avg':>9} {'RSSI min':>9} {'errors':>7}")
    for r in rows:
        print(f"{r['xclk_mhz']:>4}M {r['cap_fps']:>8} {r['mjpeg_fps']:>10} {r['mjpeg_kbps']:>7} {r['frame_kb']:>9} "
              f"{r['rssi_avg']:>9} {r['rssi_min']:>9} {r['cap_errors']:>7}" + (f"  ({r['error']})" if r["error"] else ""))
