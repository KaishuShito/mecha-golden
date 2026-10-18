"""スタックチャン (メカゴールデン) と GPT-Live をつなぐ中継。

スタックチャンは USB でつなぐ。マイクの音を GPT-Live に送り、返事の声と字幕をスタックチャンに返す。
API キーはリポジトリ直下の .env の OPENAI_API_KEY を読む (環境変数があればそちらを優先)。

使い方:
  .venv/bin/python -I bridge/live_bridge.py             # 会話する (Ctrl-C で終わる)
  .venv/bin/python -I bridge/live_bridge.py --selftest  # API を使わず、マイク録音とスピーカーだけ試す
"""
import argparse
import array
import asyncio
import base64
import collections
import datetime as dt
import json
import math
import os
import re
import subprocess
import sys
import tempfile
import threading
import time
import wave
from pathlib import Path

import serial
import numpy as np
import soxr

HERE = Path(__file__).resolve().parent
STOP = asyncio.Event()
RATE = 16000
CHUNK = 640  # 20ms
MAGIC = b"\xa5\x5a"


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, file=sys.stderr, flush=True)


def load_key():
    sys.path.insert(0, str(HERE.parent / "tools"))
    from stackchan_port import load_env
    key = os.environ.get("OPENAI_API_KEY") or load_env().get("OPENAI_API_KEY")
    if not key:
        raise SystemExit("OPENAI_API_KEY が見つかりません。リポジトリ直下の .env に書いてください (.env.example を参照)")
    return key


def rms(b: bytes) -> float:
    a = array.array("h", b[: len(b) - len(b) % 2])
    return math.sqrt(sum(x * x for x in a) / len(a)) if a else 0.0


class Device:
    """USB シリアルのフレーム (A5 5A type lenLE payload) をやり取りする。"""

    def __init__(self, port, loop, verbose=False):
        self.port = port
        self.ser = serial.Serial(port, 115200, timeout=0.05)
        self.loop = loop
        self.verbose = verbose
        self.lock = threading.Lock()
        self.mic = bytearray()
        self.mic_lock = threading.Lock()
        self.events: asyncio.Queue = asyncio.Queue()
        self.frames: asyncio.Queue = asyncio.Queue(maxsize=2)
        self.touches: asyncio.Queue = asyncio.Queue()
        self.speaking = False
        self.connected = True
        self.running = True
        self.th = threading.Thread(target=self._reader, daemon=True)
        self.th.start()

    def send(self, t: bytes, payload: bytes = b""):
        with self.lock:
            try:
                self.ser.write(MAGIC + t + len(payload).to_bytes(2, "little") + payload)
            except (serial.SerialException, OSError):
                pass  # 再接続中

    def send_audio(self, pcm: bytes):
        TRACE.audio("to_device", pcm)
        for i in range(0, len(pcm), 2048):
            self.send(b"A", pcm[i : i + 2048])

    def caption(self, who: str, text: str):
        text = re.sub(r"\[[^\]]*\]", "", text).strip()
        data = text[-60:].encode("utf-8")
        self.send(b"C", who.encode() + data[-230:])

    def take_mic(self, n: int) -> bytes | None:
        with self.mic_lock:
            if len(self.mic) < n:
                return None
            b = bytes(self.mic[:n])
            del self.mic[:n]
            return b

    def _reader(self):
        buf = bytearray()
        while self.running:
            try:
                buf += self.ser.read(4096)
            except (serial.SerialException, OSError) as e:
                log("serial error, つなぎ直します:", str(e)[:80])
                self.connected = False
                buf.clear()
                while self.running:
                    time.sleep(1.0)
                    try:
                        with self.lock:
                            try:
                                self.ser.close()
                            except Exception:
                                pass
                            self.ser = serial.Serial(self.port, 115200, timeout=0.05)
                        self.connected = True
                        log("serial reconnected")
                        break
                    except (serial.SerialException, OSError):
                        continue
                continue
            while True:
                i = buf.find(MAGIC)
                if i < 0:
                    if len(buf) > 1:
                        self._text(bytes(buf[:-1]))
                        del buf[:-1]
                    break
                if i > 0:
                    self._text(bytes(buf[:i]))
                    del buf[:i]
                if len(buf) < 5:
                    break
                t = buf[2:3]
                n = int.from_bytes(buf[3:5], "little")
                if len(buf) < 5 + n:
                    break
                p = bytes(buf[5 : 5 + n])
                del buf[: 5 + n]
                if t == b"M":
                    TRACE.audio("mic_in", p)
                    with self.mic_lock:
                        self.mic += p
                        if len(self.mic) > RATE * 2 * 8:  # 8 秒より古い分は捨てる
                            del self.mic[: len(self.mic) - RATE * 2 * 8]
                elif t == b"L":
                    if self.verbose or not p.startswith(b"[mic]"):
                        log("[dev]", p.decode(errors="replace").strip())
                elif t == b"E":
                    ev = p.decode(errors="replace")
                    TRACE.event("device", ev=ev)
                    if self.verbose:
                        log("[dev-event]", ev)
                    if ev in ("speak", "listen"):
                        self.speaking = ev == "speak"
                    q = self.touches if ev.startswith("touch:") else self.events
                    self.loop.call_soon_threadsafe(q.put_nowait, ev)
                elif t == b"V":
                    def put(jpg=p):
                        if self.frames.full():
                            self.frames.get_nowait()
                        self.frames.put_nowait(jpg)
                    self.loop.call_soon_threadsafe(put)

    def _text(self, b: bytes):
        s = b.decode(errors="replace").strip()
        if s and self.verbose:
            log("[dev-raw]", s[:200])

    def mood(self, idx: int, text: str):
        self.send(b"D", bytes([idx]) + text.encode("utf-8")[:110])

    def close(self):
        try:
            self.send(b"S", b"o")
        except Exception:
            pass
        self.running = False
        time.sleep(0.1)
        self.ser.close()


async def heartbeat(dev: Device):
    while True:
        dev.send(b"S", b"t")
        await asyncio.sleep(1.0)


async def selftest(dev: Device):
    """API を使わずに、マイク (4 秒録音) とスピーカー (say の声) を試す。"""
    hb = asyncio.create_task(heartbeat(dev))
    await asyncio.sleep(1.5)
    with dev.mic_lock:
        dev.mic.clear()
    log("4 秒録音します。スタックチャンに話しかけてください")
    await asyncio.sleep(4)
    with dev.mic_lock:
        rec = bytes(dev.mic)
    out = HERE / "selftest_mic.wav"
    with wave.open(str(out), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(rec)
    log(f"録音 {len(rec) / 32000:.1f} 秒, rms={rms(rec):.0f} → {out}")
    with tempfile.TemporaryDirectory() as td:
        aiff, wav16 = f"{td}/s.aiff", f"{td}/s.wav"
        subprocess.run(["say", "-v", "Kyoko", "-o", aiff, "こんにちは。メカゴールデンだワン。スピーカーのテスト中です。"], check=True)
        subprocess.run(["afconvert", "-f", "WAVE", "-d", "LEI16@16000", "-c", "1", aiff, wav16], check=True)
        pcm = wave.open(wav16).readframes(10**8)
    log(f"スピーカーで {len(pcm) / 32000:.1f} 秒の声を流します")
    dev.caption("a", "こんにちは。メカゴールデンだワン。スピーカーのテスト中です。")
    for i in range(0, len(pcm), 3200):
        dev.send_audio(pcm[i : i + 3200])
        await asyncio.sleep(0.095)
    await asyncio.sleep(2.0)
    hb.cancel()
    while not dev.events.empty():
        log("event:", dev.events.get_nowait())




class Trace:
    """全区間の時刻と音を残す (--trace の時だけ)。時刻は time.monotonic() に統一。
    events.jsonl: 出来事 / mic_in.raw: 本体から届いたマイク (16kHz) / to_openai.raw: OpenAI に送った音 / from_openai.raw: 返ってきた音 (変換後 16kHz) / to_device.raw: 本体に送った音"""

    def __init__(self, on: bool):
        self.on = on
        if not on:
            return
        self.dir = HERE / "eval" / "sessions" / time.strftime("%Y%m%d-%H%M%S")
        self.dir.mkdir(parents=True, exist_ok=True)
        self.ev = open(self.dir / "events.jsonl", "a")
        self.files = {k: open(self.dir / f"{k}.raw", "ab") for k in ("mic_in", "to_openai", "from_openai", "to_device")}
        self.t0 = time.monotonic()
        self.counts = {k: 0 for k in self.files}

    def event(self, kind, **kw):
        if self.on and not self.ev.closed:
            self.ev.write(json.dumps({"t": round(time.monotonic() - self.t0, 4), "kind": kind, **kw}, ensure_ascii=False) + "\n")

    def audio(self, stream, pcm: bytes):
        if self.on and pcm and not self.files[stream].closed:
            self.files[stream].write(pcm)
            self.counts[stream] += len(pcm) // 2
            self.event("audio", stream=stream, samples=len(pcm) // 2, total=self.counts[stream])

    def close(self):
        if self.on:
            self.on = False
            self.ev.close()
            for f in self.files.values():
                f.close()


TRACE = Trace(False)


class World:
    """カメラ・見張りの判定結果を、会話と待機の両方から見られるように置いておく。"""

    def __init__(self):
        self.present = False
        self.facing = 0.0
        self.facing_at = 0.0
        self.present_streak = 0
        self.absent_streak = 0
        self.facing_streak = 0
        self.absent_since = time.time()
        self.in_session = False
        self.suppress_auto_wake = False  # 会話が自然に終わったら、人が一度いなくなるまで自分からは起きない
        self.cam_debug_left = 0
        self.mood = ""


async def camera_loop(dev: Device, dec, world: World, args):
    """数秒おきにカメラ画像をもらい、人がいるか・こちらを向いているかを判定する。画像は保存しない。"""
    while not STOP.is_set():
        await asyncio.sleep((1.0 if world.present else args.cam_interval) if not world.in_session else 2.0)  # 人がいる間は 1 秒おき (身ぶりにすぐ反応)
        if dev.speaking:
            continue
        while not dev.frames.empty():
            dev.frames.get_nowait()
        dev.send(b"S", b"v")
        try:
            jpg = await asyncio.wait_for(dev.frames.get(), 4)
        except TimeoutError:
            continue
        if world.cam_debug_left > 0:
            world.cam_debug_left -= 1
            (HERE / "cam_debug").mkdir(exist_ok=True)
            (HERE / "cam_debug" / f"{int(time.time())}.jpg").write_bytes(jpg)
        world.last_frame = time.time()
        try:
            p, f, sec = await dec.presence(jpg)
        except Exception as e:
            log("presence error", str(e)[:200])
            continue
        now = time.time()
        wv, pw = getattr(dec, "last_gesture", (0.0, 0.0))
        if args.verbose:
            log(f"[cam] person={p:.2f} facing={f:.2f} wave={wv:.2f} paw={pw:.2f} ({sec:.2f}s)")
        if p >= 0.5 and now - getattr(world, "last_gesture", 0) > 4:
            if pw >= 0.6 and pw >= wv:
                dev.send(b"S", b"h"); world.last_gesture = now; log(f"(お手! {pw:.2f})")  # 音は出さない
            elif wv >= 0.6:
                dev.send(b"S", b"w"); world.last_gesture = now; log(f"(手を振り返した {wv:.2f})")
        if p >= args.present_th:
            world.present_streak += 1
            world.absent_streak = 0
        elif p < 0.3:
            world.absent_streak += 1
            world.present_streak = 0
        world.facing = f if p >= args.present_th else 0.0
        world.facing_at = now
        world.facing_streak = world.facing_streak + 1 if world.facing >= args.facing_th else 0

        if not world.present and world.present_streak >= 2:
            world.present = True
            log(f"(人が来た person={p:.2f} facing={f:.2f})")
            if not world.in_session:
                dev.send(b"S", b"w")
            dev.send(b"S", b"U")  # 首を上げて顔を見る
        if world.present and world.absent_streak >= 3:
            world.present = False
            world.absent_since = now
            log("(人がいなくなった)")
        if not world.present and now - world.absent_since > 30 and not world.in_session and not getattr(world, "head_down", False):
            dev.send(b"S", b"D")  # いなくなって 30 秒で首を戻す
            world.head_down = True
        if world.present:
            world.head_down = False
        if not world.present and now - world.absent_since > 60:
            world.suppress_auto_wake = False
        if (args.cam_wake and not world.in_session and not world.suppress_auto_wake
                and world.present and world.facing_streak >= 2):
            log("(こっちを見ているので話しかける)")
            dev.events.put_nowait("wake_cam")


async def mood_loop(dev: Device, dec, world: World, args):
    """1 分おきに Cockpit の子タスク一覧と Airi の見回り時刻を読み、普段の気分を決める (読むだけ)。"""
    last = None
    while not STOP.is_set():
        try:
            brief = subprocess.run(args.mood_cmd, shell=True, capture_output=True, text=True, timeout=30).stdout
            age = 0.0
            if args.heartbeat_file:
                hb = json.loads(Path(args.heartbeat_file).read_text())["lastBeatAt"]
                age = (dt.datetime.now(dt.timezone.utc) - dt.datetime.fromisoformat(hb)).total_seconds() / 60
            mood, conf, sec = await dec.mood(brief, age, dt.datetime.now().strftime("%Y-%m-%d %H:%M JST"))
            if conf < 0.45:
                mood = "idle"
            lines = [l for l in brief.splitlines() if l.strip()]
            running = sum(1 for l in lines if " running " in l)
            asks = sum(1 for l in lines if " ASK " in l)
            errors = sum(1 for l in lines if " error" in l or " failed" in l)
            reason = {
                "idle": "静かだワン",
                "working": f"子タスク {running} 件が作業中",
                "review": "終わった作業の確認待ち",
                "waiting": f"Ask の返事待ち {asks} 件",
                "failed": (f"失敗したタスク {errors} 件" if errors else f"Airi の見回りが {age:.0f} 分止まってる"),
            }[mood]
            dev.mood(MOODS_IDX[mood], reason)
            world.mood = mood
            if (mood, reason) != last or args.verbose:
                log(f"[見張り] {mood} ({conf:.2f}) {reason}")
                last = (mood, reason)
        except Exception as e:
            log("mood error", str(e)[:200])
        for _ in range(int(args.mood_interval * 2)):
            if STOP.is_set():
                return
            await asyncio.sleep(0.5)


async def touch_loop(dev: Device, dec, world: World, args):
    """触られたら、状況に合う鳴き声を Decisions で選んで本体に鳴らさせる (返事が遅ければ本体が自分で鳴く)。"""
    history = collections.deque(maxlen=6)
    names = [n for n, _ in dec.SOUNDS]
    while not STOP.is_set():
        try:
            ev = await asyncio.wait_for(dev.touches.get(), 0.5)
        except TimeoutError:
            continue
        now = time.time()
        recent = [f"{now - t:.0f} 秒前 {e} → {names[s]}" for t, e, s in history if now - t < 120]
        last_any = history[-1][0] if history else 0
        ctx = (
            f"いまの触られ方: {ev} (touch:種類:場所。tap=軽く1回, multi_tap=連打, stroke:forward=頭を前から後ろへなでた, stroke:against=逆なで, screen_face=画面の顔をタッチ)\n"
            f"前に触られたのは: {f'{now - last_any:.0f} 秒前' if last_any else 'しばらく誰も触っていない'}\n"
            f"直近 2 分の触られ方と鳴いた声: {'; '.join(recent) or 'なし'}\n"
            f"会話中: {'はい' if world.in_session else 'いいえ'} / 前に人: {'いる' if world.present else 'いない'}\n"
            f"いまの気分: {world.mood or '不明'} / 時刻: {dt.datetime.now():%H:%M}"
        )
        try:
            last2 = [s_ for _, _, s_ in list(history)[-2:]]
            avoid = names[last2[0]] if len(last2) == 2 and last2[0] == last2[1] else ""
            sid, conf, sec = await dec.touch_sound(ctx, avoid)
        except Exception as e:
            log("touch error", str(e)[:200])
            continue
        dev.send(b"P", bytes([sid]))
        history.append((now, ev, sid))
        log(f"(さわられた {ev} → {names[sid]} {conf:.2f} {sec:.2f}s)")


MOODS_IDX = {"idle": 0, "working": 1, "review": 2, "waiting": 3, "failed": 4}


async def run_session(dev: Device, args, dec, world: World, wake: str) -> str:
    from openai import AsyncOpenAI

    instructions = (HERE / os.environ.get("PERSONA_FILE", "persona.md")).read_text()
    session = {
        "model": "gpt-live-1",
        "instructions": instructions,
        "audio": {"format": {"type": "audio/pcm", "rate": args.rate}, "output": {"voice": args.voice}},
    }
    if args.backend != "none":
        session["delegation"] = {
            "type": "responses",
            "responses": {
                "model": args.backend,
                "instructions": "あなたはロボット犬メカゴールデンの頭脳です。調べた結果や考えた答えを、短い日本語で返してください。",
                "tools": [{"type": "web_search"}],
                "tool_choice": "auto",
            },
        }
        if args.fast_backend:  # 公式「Reduce backend latency」: 優先処理と、考える量を少なめに
            session["delegation"]["responses"]["service_tier"] = "priority"
            session["delegation"]["responses"]["reasoning"] = {"effort": "low"}

    # 本体のマイクは 16kHz。GPT-Live の標準 (24kHz) で話す時は、ここで変換する (途切れないように状態を持つ変換器)
    up = soxr.ResampleStream(RATE, args.rate, 1, dtype="int16") if args.rate != RATE else None
    down = soxr.ResampleStream(args.rate, RATE, 1, dtype="int16") if args.rate != RATE else None
    greet_file = HERE / f"greeting_{args.voice}.pcm"
    greet_text = "やっほー！メカゴールデンだワン。なにかお話しする？"

    global TRACE
    TRACE = Trace(getattr(args, "trace", False))
    if TRACE.on:
        log("記録中:", TRACE.dir)
        TRACE.event("config", voice=args.voice, rate=args.rate, fast_backend=args.fast_backend, persona=os.environ.get("PERSONA_FILE", "persona.md"))
    logdir = HERE / "logs"
    logdir.mkdir(exist_ok=True)
    convo = open(logdir / f"{dt.datetime.now():%Y%m%d-%H%M%S}.txt", "a", encoding="utf-8")

    started = asyncio.Event()
    st = {
        "speaking": False, "last_voice": 0.0, "drop": False,
        "ai": "", "ai_t": 0.0, "last_ai": "", "last_ai_t": 0.0,
        "user": "", "user_t": 0.0, "turn_user": "",
        "last_engaged": time.time(), "think_until": 0.0, "allow_until": 0.0,
        "gate": None, "hold": [], "cache": {}, "pre_task": None,
        "verdict": None, "last_verdict": True, "turn_closed": False,
        "end_at": 0.0, "user_lines": [], "user_end_ms": 0,
        "allowed": 0, "denied": 0,
    }
    preroll = collections.deque(maxlen=5)

    def flush_line(who):
        key = "ai" if who == "a" else "user"
        if st[key].strip():
            line = st[key].strip()
            if who == "u" and not args.no_addressed and st["verdict"] is not True:
                st["user_lines"].append(line)  # 話しかけと分かるまで書かない (人どうしの会話は残さない)
            else:
                write_line(who, line)
            if who == "a":
                st["last_ai"] = line
                st["last_ai_t"] = time.time()
        st[key] = ""

    def write_line(who, line):
        convo.write(f"{'メカゴールデン' if who == 'a' else 'あなた'}: {line}\n")
        convo.flush()
        log(("🐶 " if who == "a" else "🗣  ") + line)

    def settle_user_lines(allowed: bool):
        if allowed:
            for l in st["user_lines"]:
                write_line("u", l)
        elif st["user_lines"]:
            convo.write("# (人どうしの会話・独り言: 記録しない)\n")
            convo.flush()
        st["user_lines"] = []

    async def decide(text: str):
        if text in st["cache"]:
            return st["cache"][text]
        looking = world.facing if time.time() - world.facing_at < 15 else 0.0
        try:
            p, k, e, sec = await dec.addressed(text, st["last_ai"], looking, time.time() - st["last_ai_t"] if st["last_ai_t"] else 999)
        except Exception as ex:
            log("addressed error", str(ex)[:200])
            p, k, e, sec = 1.0, "error", 0.0, 0
        ok = p >= args.addressed_th or (p >= 0.3 and looking >= args.facing_th) or (e >= 0.75 and p >= 0.2)
        if ok and e >= 0.75 and not st["end_at"]:
            log(f"(会話を終えたがっている {e:.2f}「{text.strip()[:30]}」)")
            st["end_at"] = time.time() + 8
        st["cache"][text] = (ok, p, k)
        return ok, p, k

    async with AsyncOpenAI(api_key=load_key()) as client:
        async with client.live.connect() as conn:
            await conn.session.start(session=session, event_id="start")

            async def say(content: str):
                st["allow_until"] = time.time() + 10
                await conn.session.commentary.append(delegation_id=None, content=content)

            async def pacer():
                await started.wait()
                nxt = time.monotonic()
                while True:
                    b = dev.take_mic(CHUNK) or b"\0" * CHUNK
                    if up is not None:
                        b = up.resample_chunk(np.frombuffer(b, dtype="<i2")).astype("<i2").tobytes()
                    if b:
                        TRACE.audio("to_openai", b)
                        await conn.session.input_audio.append(audio=base64.b64encode(b).decode())
                    nxt += 0.02
                    await asyncio.sleep(max(0, nxt - time.monotonic()))

            async def resolve_gate():
                for _ in range(6):  # 文字起こしが遅れて届くことがあるので、最大 0.6 秒待つ
                    if st["turn_user"].strip():
                        break
                    await asyncio.sleep(0.1)
                text = st["turn_user"]
                if not text.strip():
                    ok, p, k = st["last_verdict"], 1.0, "no_text"
                else:
                    ok, p, k = await decide(text)
                    st["verdict"] = ok
                    st["last_verdict"] = ok
                if st["gate"] != "pending":
                    return
                if ok:
                    st["gate"] = "allow"
                    st["allowed"] += 1
                    st["last_engaged"] = time.time()
                    settle_user_lines(True)
                    flush_line("u")
                    for h in st["hold"]:
                        dev.send_audio(h)
                    if args.verbose:
                        log(f"(話しかけ: {p:.2f} {k}「{text.strip()[:30]}」)")
                else:
                    st["gate"] = "deny"
                    st["denied"] += 1
                    log(f"(返事しない: 話しかけではない {p:.2f} {k})")
                    st["user"] = ""
                    settle_user_lines(False)
                    convo.write(f"# 返事しなかった ({p:.2f} {k})\n")
                    # GPT-Live には何も伝えない (伝えると「人どうしで話してるみたい」と次の返事で言い出してしまう)
                st["hold"] = []

            async def watch():
                try:
                    await asyncio.wait_for(started.wait(), 8)
                except TimeoutError:
                    log("session.started が来ないので、つなぎ直します")
                    await conn.close()
                    return
                while True:
                    await asyncio.sleep(0.25)
                    now = time.time()
                    while not dev.events.empty():
                        ev = dev.events.get_nowait()
                        if ev in ("interrupt", "barge"):
                            log("(声でさえぎられた)" if ev == "barge" else "(さえぎられた)")
                            st["drop"] = True
                            heard = st["ai"].strip()
                            await conn.session.instructions.append(  # 公式: 割り込まれたら、言えたところまでを伝えて止める
                                delegation_id=None,
                                content=f"（相手があなたの話をさえぎった。あなたが言えたのは「{heard[-60:]}」まで。話すのをやめて、相手の話を聞くこと）",
                            )
                        elif ev == "pet" and args.pet_thanks and now - st.get("pet_t", 0) > 6:
                            st["pet_t"] = now  # 10/16 13:03 の段階の動き: なでられたら一言お礼
                            st["last_engaged"] = now
                            log("(頭をなでられた)")
                            await say("（いま頭をなでてもらった）えへへ、なでてくれてありがとうワン！")
                        elif ev == "end":
                            log("会話をやめる (長押し・おわるボタン)")
                            world.suppress_auto_wake = True
                            await conn.session.close()
                            return
                    if st["think_until"] and now > st["think_until"]:
                        st["think_until"] = 0
                        dev.send(b"S", b"n")
                    # 話し終わったらすぐ「考え中」の顔にする (返事の声が出るまでの間を埋める)
                    if (st["turn_user"].strip() and not st["speaking"] and st["verdict"] is not False and not st.get("cue_on")
                            and 0.35 < now - st["user_t"] < 3):
                        dev.send(b"S", b"k")
                        st["cue_on"] = now
                    if st.get("cue_on") and (st["speaking"] or now - st["cue_on"] > 4 or st["verdict"] is False):
                        if not st["think_until"]:
                            dev.send(b"S", b"n")
                        st["cue_on"] = 0
                    if st["ai"] and now - st["ai_t"] > 1.5:
                        flush_line("a")
                    if st["user"] and now - st["user_t"] > 1.5:
                        flush_line("u")
                    if STOP.is_set():
                        await conn.session.close()
                        return
                    if not dev.connected:
                        log("USB が抜けたので会話を閉じます")
                        await conn.session.close()
                        return
                    replied = st["last_voice"] > st["end_at"] - 8 and now - st["last_voice"] > 1.2
                    if st["end_at"] and not dev.speaking and not st["speaking"] and (now > st["end_at"] or replied):
                        log("会話をやめる (バイバイと言われた)")
                        world.suppress_auto_wake = True
                        await conn.session.close()
                        return
                    if args.idle_min and now - st["last_engaged"] > args.idle_min * 60:
                        log(f"{args.idle_min} 分話しかけられなかったので終わります")
                        world.suppress_auto_wake = True
                        await conn.session.close()
                        return
                    cam_ok = now - getattr(world, "last_frame", 0) < 30  # カメラが動いていない時は「いなくなった」と判断しない
                    if args.cam_leave and cam_ok and not world.present and now - world.absent_since > args.cam_leave and now - st["last_engaged"] > 30:
                        log("前に人がいなくなったので終わります")
                        await conn.session.close()
                        return

            async def predecide(text: str):
                await asyncio.sleep(0.3)
                await decide(text)

            tasks = [asyncio.create_task(pacer()), asyncio.create_task(watch()), asyncio.create_task(heartbeat(dev))]
            reason = "closed"
            try:
                async for ev in conn:
                    t = ev.type
                    if t not in ("session.output_audio.delta", "session.output_transcript.delta", "session.input_transcript.delta", "session.usage.updated"):
                        TRACE.event("server", type=t)
                    if t == "session.started":
                        started.set()
                        log("会話を始めました", ev.session.id, f"(きっかけ: {wake})")
                        if greet_file.exists() and not args.no_cached_greeting:
                            # 録っておいた挨拶を手元ですぐ鳴らす (GPT-Live の返事を待たない)。GPT-Live には「もう挨拶した」と伝える
                            dev.send_audio(greet_file.read_bytes())
                            st["allow_until"] = 0
                            await conn.session.instructions.append(
                                delegation_id=None,
                                content=f"あなたはいま「{greet_text}」と挨拶したところ。同じ挨拶は繰り返さず、相手が話すのを待つこと。",
                            )
                            log("(録っておいた挨拶を鳴らした)")
                        else:
                            st["greet_rec"] = bytearray()  # 最初の挨拶を録っておき、次からはそれを使う
                            await say(greet_text)
                    elif t == "session.output_audio.delta":
                        pcm = base64.b64decode(ev.delta)
                        if down is not None:
                            pcm = down.resample_chunk(np.frombuffer(pcm, dtype="<i2")).astype("<i2").tobytes()
                            if not pcm:
                                continue
                        TRACE.audio("from_openai", pcm)
                        if st.get("greet_rec") is not None:
                            st["greet_rec"] += pcm
                        now = time.time()
                        loud = rms(pcm) > args.gate
                        if loud:
                            st["last_voice"] = now
                        if not st["speaking"]:
                            if not loud:
                                preroll.append(pcm)
                                continue
                            st["speaking"] = True
                            TRACE.event("voice_onset", turn=st["turn_user"][-40:])
                            text = st["turn_user"].strip()
                            if st["verdict"] is not None and not (args.no_addressed or now < st["allow_until"] or st["drop"]):
                                st["gate"] = "allow" if st["verdict"] else "deny"
                                if st["verdict"]:
                                    st["last_engaged"] = now
                            elif args.no_addressed or now < st["allow_until"] or st["drop"]:
                                st["gate"] = "allow"
                                st["last_engaged"] = now
                                if args.verbose:
                                    log(f"(返事を流す: {'判定なし' if args.no_addressed else '自分から話す番' if now < st['allow_until'] else 'さえぎり後'})")
                                if now < st["allow_until"]:
                                    st["allow_until"] = 0
                                    st["verdict"] = True
                                    st["last_verdict"] = True
                            else:
                                st["gate"] = "pending"
                                st["hold"] = list(preroll)
                                asyncio.create_task(resolve_gate())
                            if st["gate"] == "allow" and not st["drop"]:
                                for p in preroll:
                                    dev.send_audio(p)
                            preroll.clear()
                        if st["gate"] == "allow" and not st["drop"]:
                            dev.send_audio(pcm)
                        elif st["gate"] == "pending":
                            st["hold"].append(pcm)
                        if now - st["last_voice"] > 0.5:
                            if st.get("greet_rec") is not None and len(st["greet_rec"]) > RATE * 2:
                                rec = bytes(st["greet_rec"])
                                a = np.frombuffer(rec, dtype="<i2")
                                loud = np.where(np.abs(a) > 300)[0]
                                if len(loud):
                                    greet_file.write_bytes(a[max(0, loud[0] - 1600): loud[-1] + 3200].tobytes())
                                    log(f"(挨拶を録った: {greet_file.name})")
                                st["greet_rec"] = None
                            st["speaking"] = False
                            st["drop"] = False
                            st["turn_closed"] = True
                            st["gate"] = None
                            st["hold"] = []
                    elif t == "session.output_transcript.delta":
                        TRACE.event("out_text", text=ev.delta, start_ms=getattr(ev, "start_ms", None), end_ms=getattr(ev, "end_ms", None))
                        now = time.time()
                        if st["gate"] in ("deny", "pending") or st["verdict"] is False:
                            continue
                        if st["ai"] and now - st["ai_t"] > 1.5:
                            flush_line("a")
                        st["ai"] += ev.delta
                        st["ai_t"] = now
                        dev.caption("a", st["ai"])
                    elif t == "session.input_transcript.delta":
                        TRACE.event("in_text", text=ev.delta, start_ms=getattr(ev, "start_ms", None), end_ms=getattr(ev, "end_ms", None))
                        now = time.time()
                        if st["user"] and now - st["user_t"] > 1.5:
                            flush_line("u")
                        st["user"] += ev.delta
                        sms = getattr(ev, "start_ms", None)
                        gap_ms = (sms - st["user_end_ms"]) if (sms is not None and st["user_end_ms"]) else 0
                        if st["turn_closed"] or (gap_ms > 2500 and not st["speaking"]):
                            if st["verdict"] is not True:
                                settle_user_lines(False)
                            st["turn_user"] = ""
                            st["verdict"] = None
                            st["turn_closed"] = False
                        st["turn_user"] += ev.delta
                        st["user_end_ms"] = getattr(ev, "end_ms", None) or st["user_end_ms"]
                        st["user_t"] = now
                        dev.caption("u", st["user"])
                        if not args.no_addressed:
                            if st["pre_task"]:
                                st["pre_task"].cancel()
                            st["pre_task"] = asyncio.create_task(predecide(st["turn_user"]))
                    elif t == "response.event":
                        inner = getattr(ev, "event", None) or {}
                        it = inner.get("type", "") if isinstance(inner, dict) else getattr(inner, "type", "")
                        if it in ("response.completed", "response.done", "response.failed", "response.incomplete"):
                            st["think_until"] = 0
                            dev.send(b"S", b"n")
                            st["allow_until"] = time.time() + 15
                    elif t == "session.delegation.created":
                        if not st["think_until"]:
                            dev.send(b"S", b"k")
                            log("(考え中…)")
                        st["think_until"] = time.time() + 20
                    elif t == "session.closed":
                        reason = getattr(ev, "reason", "closed")
                        log("会話を閉じました:", reason, ev.usage, f"返事した {st['allowed']} / しなかった {st['denied']}")
                        convo.write(f"# closed: {reason} {ev.usage}\n")
                        break
                    elif t == "error":
                        log("ERROR", ev.model_dump_json()[:500])
                    elif args.verbose and t not in ("session.usage.updated",):
                        log("event", t)
            finally:
                for tk in tasks:
                    tk.cancel()
                flush_line("a")
                flush_line("u")
                convo.close()
                TRACE.close()
    return reason


async def main():
    import signal
    from decide import Decider

    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None, help="USB ポート (省略すると自動で探す。環境変数 STACKCHAN_PORT でも指定できる)")
    ap.add_argument("--voice", default="harema", help="harema = 日本語の女性の声 / shida = 日本語の男性の声 / marin = 英語の声")
    ap.add_argument("--rate", type=int, default=24000, choices=[16000, 24000], help="GPT-Live とやり取りする音の速さ (24000 が標準)")
    ap.add_argument("--no-fast-backend", dest="fast_backend", action="store_false", help="調べものの優先処理を使わない")
    ap.add_argument("--no-cached-greeting", action="store_true", help="録っておいた挨拶を使わない")
    ap.add_argument("--backend", default="gpt-6-luna", help="調べもの・考えごとを任せるモデル。none で使わない")
    ap.add_argument("--gate", type=float, default=120, help="これより小さい返事の音は無音として送らない")
    ap.add_argument("--idle-min", type=float, default=5, help="この分数だれも話しかけなければ終わる (0 で無制限)")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--wait", action="store_true", help="起動したら待機し、頭をなでる・こちらを見ると会話を始める")
    ap.add_argument("--no-camera", action="store_true", help="カメラの判定をしない")
    ap.add_argument("--cam-interval", type=float, default=4.0, help="待機中にカメラを見る間隔 (秒)。会話中はこの 2 倍")
    ap.add_argument("--present-th", type=float, default=0.7)
    ap.add_argument("--facing-th", type=float, default=0.7)
    ap.add_argument("--cam-wake", dest="cam_wake", action="store_true", help="見ているだけで会話を始める")
    ap.add_argument("--cam-leave", type=float, default=90, help="人がいなくなってこの秒数で会話を終える (0 で終えない)")
    ap.add_argument("--cam-debug", type=int, default=0, help="確認用にカメラ画像をこの枚数だけ bridge/cam_debug に保存")
    ap.add_argument("--mood-cmd", default=None, help="見張り犬: このコマンドの出力 (作業の一覧) から普段の気分を決める。省略すると使わない")
    ap.add_argument("--heartbeat-file", default=None, help="見張り犬: 最後の見回り時刻 ({\"lastBeatAt\": ISO8601}) の JSON")
    ap.add_argument("--mood-interval", type=float, default=60)
    ap.add_argument("--no-addressed", action="store_true", help="話しかけ判定をせず、すべてに返事する")
    ap.add_argument("--addressed-th", type=float, default=0.2, help="話しかけ判定でこれ未満なら聞き流す。会話は本人が長押しで始めているので、話しかけを基本にする (10/18「こんにちは」0.41 を聞き流して会話が成り立たなかった)")
    ap.add_argument("--trace", action="store_true", help="全区間の時刻と音を bridge/eval/sessions/ に残す (人の声が入るので、作業フォルダの外に出さない)")
    ap.add_argument("--pet-thanks", action="store_true", help="会話中に頭をなでられたら GPT-Live がお礼を言う (10/16 13:03 の段階の動き)")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    loop = asyncio.get_running_loop()
    loop.add_signal_handler(signal.SIGINT, STOP.set)
    loop.add_signal_handler(signal.SIGTERM, STOP.set)
    if not args.port:
        sys.path.insert(0, str(HERE.parent / "tools"))
        from stackchan_port import find_port
        args.port = find_port()
    dev = Device(args.port, loop, args.verbose)
    dec = Decider(load_key())
    world = World()
    world.cam_debug_left = args.cam_debug
    bg = []
    try:
        if args.selftest:
            await selftest(dev)
            return
        if not args.no_camera:
            bg.append(asyncio.create_task(camera_loop(dev, dec, world, args)))
        if args.mood_cmd:
            bg.append(asyncio.create_task(mood_loop(dev, dec, world, args)))
        bg.append(asyncio.create_task(touch_loop(dev, dec, world, args)))
        wake = "start" if not args.wait else None
        while not STOP.is_set():
            if wake is None:
                dev.send(b"S", b"o")
                log("待機中: 頭のセンサーを長押し (または前でこちらを見る) と会話が始まります")
                while not STOP.is_set():
                    try:
                        ev = await asyncio.wait_for(dev.events.get(), 0.5)
                    except TimeoutError:
                        continue
                    if ev in ("wake", "wake_cam"):
                        wake = ev
                        break
                if STOP.is_set():
                    break
            world.in_session = True
            try:
                reason = await run_session(dev, args, dec, world, wake)
            finally:
                world.in_session = False
            log(f"Decisions: {dec.calls} 回 {dec.by_kind} 入力 {dec.tokens} トークン ≈ ${dec.cost_usd():.4f}")
            if STOP.is_set():
                break
            if reason in ("close_requested",):
                wake = None
            else:
                log("2 秒後につなぎ直します")
                wake = "reconnect"
                await asyncio.sleep(2)
    finally:
        for b in bg:
            b.cancel()
        log(f"Decisions 合計: {dec.calls} 回 {dec.by_kind} 入力 {dec.tokens} トークン ≈ ${dec.cost_usd():.4f}")
        dev.close()


if __name__ == "__main__":
    sys.path.insert(0, str(HERE))
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        log("終了")
