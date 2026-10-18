"""2 つの設定を「同じ時間帯に、交互に」走らせて、話し終わり → 返事の最初の音 の遅れを比べる (本体なし・音は鳴らない)。
各回: 合成音声の質問を 6 つ流し、イベントの生の時刻を JSONL に残す。結果は 50%/90% とブートストラップの信頼区間。
使い方: .venv/bin/python -I bridge/ab_test.py --a "--voice marin --rate 16000" --b "--voice harema --rate 24000" --rounds 6"""
import argparse, asyncio, json, os, random, shlex, subprocess, sys, tempfile, time, wave
import numpy as np
sys.path.insert(0, os.path.dirname(__file__))
import live_bridge as lb
from decide import Decider

QUESTIONS = ["メカゴールデン、こんにちは。元気？", "メカゴールデン、好きな食べ物はなに？", "メカゴールデン、しりとりしよう。りんご。",
             "ねえメカゴールデン、犬って何歳まで生きるの？", "メカゴールデン、なにか面白い話して。", "メカゴールデン、明日は晴れるかな？"]
VOICES_TTS = ["Kyoko", "Eddy (Japanese (Japan))", "Flo (Japanese (Japan))", "Reed (Japanese (Japan))", "Sandy (Japanese (Japan))"]

def tts(text, voice, path):
    with tempfile.TemporaryDirectory() as td:
        subprocess.run(["say", "-v", voice, "-o", f"{td}/a.aiff", text], check=True)
        subprocess.run(["afconvert", "-f", "WAVE", "-d", "LEI16@16000", "-c", "1", f"{td}/a.aiff", path], check=True)
    return wave.open(path).readframes(10**8)

class FakeDev:
    def __init__(self, pcm, trace):
        self.pcm, self.pos, self.t0 = pcm, 0, None
        self.events, self.frames, self.touches = asyncio.Queue(), asyncio.Queue(), asyncio.Queue()
        self.speaking, self.connected, self.sent, self.trace = False, True, [], trace
    def take_mic(self, n):
        if self.t0 is None: self.t0 = time.monotonic()
        b = self.pcm[self.pos:self.pos + n]; self.pos += n
        return b if len(b) == n else None
    def send(self, t, p=b""): pass
    def send_audio(self, pcm):
        if self.t0 is None: self.t0 = time.monotonic()
        self.sent.append(time.monotonic() - self.t0)
    def caption(self, w, t): pass
    def mood(self, i, t): pass

def parse(extra):
    ap = argparse.ArgumentParser()
    ap.add_argument("--voice", default="harema"); ap.add_argument("--rate", type=int, default=24000)
    ap.add_argument("--slow-backend", action="store_true"); ap.add_argument("--persona", default="persona.md")
    a = ap.parse_args(shlex.split(extra))
    return argparse.Namespace(voice=a.voice, rate=a.rate, fast_backend=not a.slow_backend, no_cached_greeting=True, backend="gpt-6-luna",
                              gate=120, idle_min=0, no_addressed=True, addressed_th=0.5, facing_th=0.7, cam_leave=0, verbose=False,
                              pet_thanks=False), a.persona

async def one(label, extra, pcm, ends, outdir, idx):
    args, persona = parse(extra)
    os.environ["PERSONA_FILE"] = persona
    dev = FakeDev(pcm, None)
    stop = asyncio.Event()
    async def stopper():
        await asyncio.sleep(len(pcm) / 32000 + 3); stop.set()
    lb_STOP = lb.STOP
    t = asyncio.create_task(stopper())
    # 会話を 1 回ぶん回す。STOP は全体で共有なので、ここでは時間で止める
    task = asyncio.create_task(lb.run_session(dev, args, Decider(lb.load_key()), lb.World(), "start"))
    await stop.wait()
    lb_STOP.set()
    try:
        await asyncio.wait_for(task, 20)
    except Exception:
        pass
    lb_STOP.clear()
    lats = []
    for e in ends:
        nxt = [x for x in dev.sent if x > e]
        lats.append((nxt[0] - e) if nxt and nxt[0] - e < 8 else None)
    t = dev.sent
    bursts, durs = [], []
    if t:
        s0 = prev = t[0]
        for x in t[1:]:
            if x - prev > 0.6: durs.append(prev - s0); s0 = x
            prev = x
        durs.append(prev - s0)
    REPLY_DURS.setdefault(label, []).extend(durs[1:])  # 最初 (挨拶) は除く
    rec = {"label": label, "reply_durations": durs, "extra": extra, "round": idx, "latencies": lats, "sent_times": dev.sent[:2000], "ends": ends, "at": time.time()}
    with open(outdir / "results.jsonl", "a") as f:
        f.write(json.dumps(rec, ensure_ascii=False) + "\n")
    return lats

REPLY_DURS = {}

def boot_ci(x, f, n=2000):
    x = np.array(x); rs = [f(np.random.choice(x, len(x))) for _ in range(n)]
    return np.percentile(rs, 2.5), np.percentile(rs, 97.5)

async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", required=True); ap.add_argument("--b", required=True); ap.add_argument("--rounds", type=int, default=6)
    ap.add_argument("--out", default=str(lb.HERE / "eval" / time.strftime("ab-%Y%m%d-%H%M%S")))
    a = ap.parse_args()
    from pathlib import Path
    outdir = Path(a.out); outdir.mkdir(parents=True, exist_ok=True)
    res = {"A": [], "B": []}
    miss = {"A": 0, "B": 0}
    ov = {"A": 0, "B": 0}
    for r in range(a.rounds):
        voice = VOICES_TTS[r % len(VOICES_TTS)]
        sil = lambda s: b"\0" * int(32000 * s)
        qs = random.sample(QUESTIONS, len(QUESTIONS))
        pcm, ends = sil(5), []
        for i, q in enumerate(qs):
            x = tts(q, voice, f"/tmp/ab_{os.getpid()}_{i}.wav")
            # 話し終わり = 音の大きさが最大の 5% を最後に超えた所 (ファイルの終わりではない。合成音声は後ろに無音が付くため)
            arr = np.frombuffer(x, dtype="<i2").astype(float)
            env = np.array([np.sqrt((arr[j:j+160] ** 2).mean()) for j in range(0, len(arr) - 160, 160)])
            last = int(np.where(env > env.max() * 0.05)[0][-1]) * 160 + 160
            ends.append((len(pcm) + last * 2) / 32000)
            pcm += x + sil(8)
        # A と B を同時に走らせる (同じ時間帯・同じ混み具合)
        la, lbb = await asyncio.gather(one("A", a.a, pcm, ends, outdir, r), one("B", a.b, pcm, ends, outdir, r))
        # 0.25 秒未満は「前の返事が続いていて、質問にかぶった」とみなして別に数える
        ov["A"] += sum(x is not None and x < 0.25 for x in la); ov["B"] += sum(x is not None and x < 0.25 for x in lbb)
        la = [x if (x is None or x >= 0.25) else None for x in la]; lbb = [x if (x is None or x >= 0.25) else None for x in lbb]
        res["A"] += [x for x in la if x is not None]; res["B"] += [x for x in lbb if x is not None]
        miss["A"] += sum(x is None for x in la); miss["B"] += sum(x is None for x in lbb)
        print(f"round {r+1}/{a.rounds} ({voice}): A {np.median([x for x in la if x]) if any(la) else float('nan'):.2f}s  B {np.median([x for x in lbb if x]) if any(lbb) else float('nan'):.2f}s", flush=True)
    for k, extra in (("A", a.a), ("B", a.b)):
        x = res[k]
        if not x: continue
        lo, hi = boot_ci(x, np.median)
        print(f"{k} [{extra}] 返事なし {miss[k] - ov[k]} 回 / かぶり {ov[k]} 回 / n={len(x)} 中央値 {np.median(x):.2f}s (95%CI {lo:.2f}-{hi:.2f}) / 90% {np.percentile(x,90):.2f}s")
    for k in ("A", "B"):
        d = REPLY_DURS.get(k, [])
        if d: print(f"{k} 返事の長さ: 中央値 {np.median(d):.1f}s / 90% {np.percentile(d, 90):.1f}s (n={len(d)})")
    if res["A"] and res["B"]:
        d = [np.median(np.random.choice(res["B"], len(res["B"]))) - np.median(np.random.choice(res["A"], len(res["A"]))) for _ in range(4000)]
        print(f"B - A の中央値の差: {np.median(res['B']) - np.median(res['A']):+.2f}s (95%CI {np.percentile(d,2.5):+.2f} 〜 {np.percentile(d,97.5):+.2f})")
    print("生データ:", outdir)

asyncio.run(main())
