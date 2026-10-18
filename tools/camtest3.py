"""会話モード (マイク録音) のオン→オフ の後に撮れるかを見る。"""
import serial, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stackchan_port import find_port, time
s = serial.Serial(find_port(), 115200, timeout=0.1)
F = lambda t, p=b'': b'\xa5\x5a' + t + len(p).to_bytes(2, 'little') + p
def cap():
    s.write(F(b'S', b'v')); t0 = time.time(); buf = bytearray()
    while time.time() - t0 < 3:
        buf += s.read(65536); j = buf.find(b'\xa5\x5aV')
        if j >= 0 and len(buf) >= j + 5 and len(buf) >= j + 5 + int.from_bytes(buf[j+3:j+5], 'little'): return True, buf
    return False, buf
time.sleep(0.5); s.read(10**6)
print('standby', [cap()[0] for _ in range(3)])
for _ in range(10): s.write(F(b'S', b't')); time.sleep(0.2)
print('talk(mic on)', [cap()[0] for _ in range(3)])
s.write(F(b'S', b'o')); time.sleep(1)
r = [cap() for _ in range(3)]
print('after talk off', [x[0] for x in r], b'FB-SIZE' in b''.join(x[1] for x in r))
