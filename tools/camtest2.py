"""会話モードで、スピーカー再生 (小さな音) と撮影を交互に行い、カメラが壊れるかを見る。"""
import serial, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stackchan_port import find_port, time, math, array, re
s = serial.Serial(find_port(), 115200, timeout=0.1)
F = lambda t, p=b'': b'\xa5\x5a' + t + len(p).to_bytes(2, 'little') + p
tone = array.array('h', [int(600 * math.sin(2 * math.pi * 440 * i / 16000)) for i in range(16000 // 2)]).tobytes()
ok = 0
for i in range(15):
    s.write(F(b'S', b't'))
    for j in range(0, len(tone), 2048): s.write(F(b'A', tone[j:j+2048]))
    t0 = time.time(); buf = bytearray()
    while time.time() - t0 < 1.6: s.write(F(b'S', b't')); buf += s.read(65536); time.sleep(0.2)
    s.write(F(b'S', b'v')); t0 = time.time(); got = False
    while time.time() - t0 < 4:
        buf += s.read(8192); j = buf.find(b'\xa5\x5aV')
        if j >= 0 and len(buf) >= j + 5 and len(buf) >= j + 5 + int.from_bytes(buf[j+3:j+5], 'little'): got = True; break
    ok += got
    print(i, 'cam', got, 'speak' in buf.decode(errors='replace'), flush=True)
s.write(F(b'S', b'o'))
print('ok', ok, '/ 15')
