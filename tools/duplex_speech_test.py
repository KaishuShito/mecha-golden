"""続く声 (say で作った 5 秒ほどの日本語) を本体で鳴らし、マイク (エコー除去後) の大きさを 0.1 秒ごとに見る。"""
import serial, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stackchan_port import find_port, time, numpy as np, subprocess, tempfile, wave
with tempfile.TemporaryDirectory() as td:
    subprocess.run(["say", "-v", "Kyoko", "-o", f"{td}/a.aiff", "こんにちは。今日はいい天気ですね。メカゴールデンはお散歩が大好きです。"], check=True)
    subprocess.run(["afconvert", "-f", "WAVE", "-d", "LEI16@16000", "-c", "1", f"{td}/a.aiff", f"{td}/a.wav"], check=True)
    speech = wave.open(f"{td}/a.wav").readframes(10**8)
s = serial.Serial(find_port(), 115200, timeout=0.02)
F = lambda t, p=b'': b'\xa5\x5a' + t + len(p).to_bytes(2, 'little') + p
buf = bytearray(); mic = bytearray(); logs = []
def pump(sec, feed=None):
    global buf
    t0 = time.time(); off = 0
    while time.time() - t0 < sec:
        s.write(F(b'S', b't'))
        if feed is not None and off < len(feed):  # 実時間より少し速く送る (GPT-Live の返事と同じように)
            target = int((time.time() - t0 + 0.3) * 32000)
            while off < min(target, len(feed)):
                s.write(F(b'A', feed[off:off + 2048])); off += 2048
        buf += s.read(65536)
        while True:
            i = buf.find(b'\xa5\x5a')
            if i < 0 or len(buf) < i + 5: break
            n = int.from_bytes(buf[i+3:i+5], 'little')
            if len(buf) < i + 5 + n: break
            t = buf[i+2:i+3]; p = bytes(buf[i+5:i+5+n]); del buf[:i+5+n]
            if t == b'M': mic.extend(p)
            elif t == b'L': logs.append(p.decode(errors='replace').strip())
        time.sleep(0.02)
pump(1.5); m0 = len(mic)
pump(len(speech) / 32000 + 1.5, speech)
a = np.frombuffer(bytes(mic[m0:]), dtype='<i2').astype(float)
q = np.frombuffer(bytes(mic[:m0]), dtype='<i2').astype(float)
print('静かな時 rms %.0f' % np.sqrt((q ** 2).mean()))
w = 1600
series = [np.sqrt((a[i:i+w] ** 2).mean()) for i in range(0, len(a) - w, w)]
print('声を鳴らしている間 (0.1 秒ごとの rms):', ' '.join('%d' % x for x in series))
print('中央値 %.0f / 90%% %.0f' % (np.median(series), np.percentile(series, 90)))
for l in logs:
    if 'barge' in l or 'near ref' in l: print(l[l.find('res='):] if 'res=' in l else l)
s.write(F(b'S', b'o'))
