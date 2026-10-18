"""スタックチャンの画面 (キャンバス) をシリアル経由で PNG に保存する。
使い方: python tools/snap.py [状態番号 0-7 ...]  (状態番号を省くと今の画面)"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stackchan_port import find_port
import sys, time, serial
from PIL import Image

PORT = find_port()

def snap(s, name):
    s.reset_input_buffer()
    s.write(b"S")
    deadline = time.time() + 5
    line = b""
    while not line.startswith(b"SNAP") and time.time() < deadline:
        line = s.readline()
    if not line.startswith(b"SNAP"):
        raise SystemExit("no SNAP header")
    _, w, h, st = line.decode().split()
    w, h = int(w), int(h)
    n = w * h * 2
    data = b""
    while len(data) < n and time.time() < deadline + 10:
        data += s.read(n - len(data))
    if len(data) < n: raise SystemExit(f"short {len(data)}/{n} line={line[:40]!r}")
    px = []
    for i in range(0, n, 2):
        v = (data[i] << 8) | data[i + 1]  # バッファは上位バイトが先
        px.append((((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31))
    img = Image.new("RGB", (w, h))
    img.putdata(px)
    out = f"shots/{name or st}.png"
    img.resize((w * 2, h * 2), Image.NEAREST).save(out)
    print(out)

with serial.Serial(PORT, 115200, timeout=1) as s:
    time.sleep(0.3)
    args = sys.argv[1:]
    if not args:
        snap(s, None)
    for a in args:
        s.write(a.encode())
        time.sleep(0.9)
        snap(s, None)
    s.write(b"p")
    time.sleep(0.3)
    print(s.read(s.in_waiting or 1).decode(errors="replace"))
