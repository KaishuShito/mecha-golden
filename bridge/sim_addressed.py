"""本体なしで話しかけ判定を試す: 合成音声 (人どうし → ロボットへ → 独り言) を GPT-Live に流し、返事が本体へ送られたかを数える。"""
import asyncio, sys, time, wave, argparse
sys.path.insert(0, __import__("os").path.dirname(__file__))
import live_bridge as lb
from decide import Decider

class FakeDev:
    def __init__(self, pcm):
        self.pcm = pcm; self.pos = 0; self.t0 = None
        self.events = asyncio.Queue(); self.frames = asyncio.Queue(); self.speaking = False
        self.sent = []
    def take_mic(self, n):
        if self.t0 is None: self.t0 = time.time()
        b = self.pcm[self.pos:self.pos+n]; self.pos += n
        return b if len(b) == n else None
    def send(self, t, p=b""): pass
    def send_audio(self, pcm): self.sent.append((round(time.time() - (self.t0 or time.time()), 1), len(pcm)))
    def caption(self, w, t): pass
    def mood(self, i, t): pass

async def main():
    rd = lambda f: wave.open(f).readframes(10**8)
    sil = lambda s: b"\0" * int(32000 * s)
    pcm = sil(4) + rd("/tmp/sim/a.wav") + sil(7) + rd("/tmp/sim/b.wav") + sil(9) + rd("/tmp/sim/c.wav") + sil(6) + rd("/tmp/sim/d.wav") + sil(12)
    dev = FakeDev(pcm)
    args = argparse.Namespace(voice="marin", backend="none", gate=120, idle_min=0, no_addressed=False, addressed_th=0.5,
                              facing_th=0.7, cam_leave=0, verbose=True)
    world = lb.World()
    async def stopper():
        await asyncio.sleep(len(pcm) / 32000 + 4); lb.STOP.set()
    asyncio.create_task(stopper())
    dec = Decider(lb.load_key())
    await lb.run_session(dev, args, dec, world, "start")
    secs = sorted(set(int(t) for t, _ in dev.sent))
    print("本体へ送った返事の時刻(秒):", secs)
    print("Decisions:", dec.calls, dec.by_kind, dec.tokens, "tokens")

asyncio.run(main())
