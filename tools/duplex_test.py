"""しゃべりながら聞く方式の確認: 静かな時のマイク、鳴き声を鳴らしている時のマイク (エコーがどれだけ残るか) を測る。"""
import serial, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stackchan_port import find_port, time, numpy as np, sys
s = serial.Serial(find_port(), 115200, timeout=0.05)
F = lambda t, p=b'': b'\xa5\x5a' + t + len(p).to_bytes(2, 'little') + p
buf = bytearray(); mic = bytearray(); logs = []
def pump(sec, play_at=None, sid=1):
    global buf
    t0 = time.time(); done = False
    while time.time() - t0 < sec:
        s.write(F(b'S', b't'))
        if play_at is not None and not done and time.time() - t0 > play_at:
            s.write(F(b'P', bytes([sid]))); done = True
        buf += s.read(65536)
        while True:
            i = buf.find(b'\xa5\x5a')
            if i < 0 or len(buf) < i + 5: break
            n = int.from_bytes(buf[i+3:i+5], 'little')
            if len(buf) < i + 5 + n: break
            t = buf[i+2:i+3]; p = bytes(buf[i+5:i+5+n]); del buf[:i+5+n]
            if t == b'M': mic.extend(p)
            elif t == b'L': logs.append(p.decode(errors='replace').strip())
        time.sleep(0.04)
def rms(b):
    a = np.frombuffer(bytes(b), dtype='<i2').astype(float); return float(np.sqrt((a ** 2).mean())) if len(a) else 0
pump(1.0); m0 = len(mic); pump(2.5); print('静か: %.1f 秒, rms %.0f' % ((len(mic) - m0) / 32000, rms(mic[m0:])))
n = int(sys.argv[1]) if len(sys.argv) > 1 else 3
for k in range(n):
    m0 = len(mic); pump(2.0, 0.2, 1)
    print('鳴き声 %d: マイク rms %.0f' % (k, rms(mic[m0:])))
for l in logs:
    if 'duplex' in l and 'rx sample' not in l: print(l)
s.write(F(b'S', b'o'))
open('/tmp/duplex_mic.raw', 'wb').write(bytes(mic))
