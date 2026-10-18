"""カメラを N 回撮って、成功数・かかった時間・空きメモリを見る (画像は保存しない)。"""
import serial, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stackchan_port import find_port, time, sys, re
N = int(sys.argv[1]) if len(sys.argv) > 1 else 20
s = serial.Serial(find_port(), 115200, timeout=0.1)
time.sleep(0.3); s.read(10**6)
F = lambda t, p=b'': b'\xa5\x5a' + t + len(p).to_bytes(2, 'little') + p
ok = 0; times = []
TALK = '--talk' in sys.argv
for i in range(N):
    if TALK: s.write(F(b'S', b't'))
    buf = bytearray(); t0 = time.time(); s.write(F(b'S', b'v')); got = False
    while time.time() - t0 < 4:
        buf += s.read(8192)
        j = buf.find(b'\xa5\x5aV')
        if j >= 0 and len(buf) >= j + 5:
            n = int.from_bytes(buf[j+3:j+5], 'little')
            if len(buf) >= j + 5 + n: got = True; break
    if got: ok += 1; times.append(time.time() - t0)
    if i % 10 == 9:
        s.write(b'p'); time.sleep(0.4); txt = s.read(200000).decode(errors='replace')
        m = re.findall(r'free_heap=\d+ psram=\d+', txt); errs = txt.count('FB-SIZE') + txt.count('grab failed')
        print(f'{i+1}: ok={ok} {m[-1] if m else ""} errs_in_log={errs}', flush=True)
    time.sleep(0.5)
print('ok', ok, '/', N, 'avg', round(sum(times)/max(1,len(times)), 2), 's')
