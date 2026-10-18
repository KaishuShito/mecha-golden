"""会話の速さを測る (本体なし・音は鳴らない)。
合成音声 (say) の質問を GPT-Live に流し、「話し終わり」から「返事の声を本体へ送り始める」までの時間を測る。
使い方: .venv/bin/python -I bridge/latency_test.py [--no-addressed] [--runs N]"""
import argparse, asyncio, os, subprocess, sys, tempfile, time, wave
sys.path.insert(0, os.path.dirname(__file__))
import live_bridge as lb
from decide import Decider

QUESTIONS = ["メカゴールデン、こんにちは。元気？", "メカゴールデン、好きな食べ物はなに？", "ねえメカゴールデン、今日は何曜日だと思う？", "メカゴールデン、しりとりしよう。りんご。"]

def tts(text, path):
    with tempfile.TemporaryDirectory() as td:
        subprocess.run(["say", "-v", "Kyoko", "-o", f"{td}/a.aiff", text], check=True)
        subprocess.run(["afconvert", "-f", "WAVE", "-d", "LEI16@16000", "-c", "1", f"{td}/a.aiff", path], check=True)
    return wave.open(path).readframes(10**8)

class FakeDev:
    def __init__(self, pcm):
        self.pcm, self.pos, self.t0 = pcm, 0, None
        self.events, self.frames, self.touches = asyncio.Queue(), asyncio.Queue(), asyncio.Queue()
        self.speaking, self.connected, self.sent = False, True, []
    def take_mic(self, n):
        if self.t0 is None: self.t0 = time.time()
        b = self.pcm[self.pos:self.pos + n]; self.pos += n
        return b if len(b) == n else None
    def send(self, t, p=b""): pass
    def send_audio(self, pcm):
        if self.t0 is None: self.t0 = time.time()
        self.sent.append(time.time() - self.t0)
    def caption(self, w, t): pass
    def mood(self, i, t): pass

async def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--no-addressed", action="store_true"); ap.add_argument("--voice", default="harema"); ap.add_argument("--rate", type=int, default=24000); ap.add_argument("--slow-backend", action="store_true"); a = ap.parse_args()
    sil = lambda s: b"\0" * int(32000 * s)
    pcm, ends = sil(6), []
    for i, q in enumerate(QUESTIONS):
        x = tts(q, f"/tmp/lat_{os.getpid()}_{i}.wav")
        pcm += x; ends.append(len(pcm) / 32000); pcm += sil(9)
    dev = FakeDev(pcm)
    args = argparse.Namespace(voice=a.voice, rate=a.rate, fast_backend=not a.slow_backend, no_cached_greeting=False, backend="gpt-6-luna", gate=120, idle_min=0,
                              no_addressed=a.no_addressed, addressed_th=0.5, facing_th=0.7, cam_leave=0, verbose=False, pet_thanks=False)
    async def stopper():
        await asyncio.sleep(len(pcm) / 32000 + 3); lb.STOP.set()
    asyncio.create_task(stopper())
    await lb.run_session(dev, args, Decider(lb.load_key()), lb.World(), "start")
    print("\n話し終わり → 返事の声を送り始めるまで:")
    lats = []
    for q, e in zip(QUESTIONS, ends):
        nxt = [t for t in dev.sent if t > e]
        lat = (nxt[0] - e) if nxt and nxt[0] - e < 8 else None
        lats.append(lat)
        print(f"  {q[:20]:<22} {('%.2f s' % lat) if lat is not None else '返事なし'}")
    ok = [l for l in lats if l is not None]
    if ok: print(f"  平均 {sum(ok)/len(ok):.2f} s / 最短 {min(ok):.2f} s / 最長 {max(ok):.2f} s")

asyncio.run(main())
