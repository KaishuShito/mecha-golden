"""--trace で残した 1 回の会話 (bridge/eval/sessions/<日時>/) を分析する。

- 口から耳: 本体から届いたマイクの音 (mic_in.raw) で人の発話の終わりを波形から決め、
  本体が返事を鳴らし始めた時刻 (本体の 'speak' の知らせ + 本体の送りの DMA 64ms) までを測る
- 失敗: 返事なし (8 秒以内に鳴らさない) / かぶり (人が話している最中に本体が鳴らしている)
- 聞き取り: --script に読み上げた文のリスト (1 行 1 文) を渡すと、GPT-Live の文字起こしとの文字の誤り率 (CER) を出す
使い方: .venv/bin/python -I bridge/analyze_session.py bridge/eval/sessions/<日時> [--script sentences.txt]
"""
import argparse, json
from pathlib import Path

import numpy as np

RATE = 16000
DMA_MS = 64  # 本体の送りの DMA にたまる分 (duplex.cpp: 4 x 256 サンプル)


def edit_distance(a, b):
    d = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        prev, d[0] = d[0], i
        for j, cb in enumerate(b, 1):
            cur = d[j]
            d[j] = min(d[j] + 1, d[j - 1] + 1, prev + (ca != cb))
            prev = cur
    return d[len(b)]


def norm(s):
    return "".join(ch for ch in s if ch.isalnum())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--script")
    a = ap.parse_args()
    d = Path(a.dir)
    ev = [json.loads(l) for l in open(d / "events.jsonl")]
    mic = np.fromfile(d / "mic_in.raw", dtype="<i2").astype(float)
    # マイクのサンプル番号 → 届いた時刻 (届いた時の累計から、線形に補間)
    pts = [(e["total"], e["t"]) for e in ev if e["kind"] == "audio" and e["stream"] == "mic_in"]
    if not pts:
        print("マイクの記録がない"); return
    tot, tt = np.array([p[0] for p in pts]), np.array([p[1] for p in pts])
    t_of = lambda n: float(np.interp(n, tot, tt))
    # 人の発話: 20ms ごとの大きさが、静かな時 (下位 20%) の 4 倍を 0.2 秒以上超えた所
    w = 320
    env = np.array([np.sqrt((mic[i:i + w] ** 2).mean()) for i in range(0, len(mic) - w, w)])
    floor = np.percentile(env, 20)
    on = env > max(floor * 4, 300)
    segs, i = [], 0
    while i < len(on):
        if on[i]:
            j = i
            while j < len(on) and (on[j] or (j + 15 < len(on) and on[j:j + 15].any())):  # 0.3 秒までの切れ目はつなぐ
                j += 1
            if j - i >= 10:
                segs.append((i * w, j * w))
            i = j
        else:
            i += 1
    speaks = [e["t"] for e in ev if e["kind"] == "device" and e.get("ev") == "speak"]
    listens = [e["t"] for e in ev if e["kind"] == "device" and e.get("ev") == "listen"]
    lat, miss, overlap = [], 0, 0
    print(f"人の発話 {len(segs)} 回")
    for s0, s1 in segs:
        t_end = t_of(s1)
        t_start = t_of(s0)
        # 人が話している最中に、本体が鳴らしていたか (speak 〜 listen の間に発話の終わりが入る)
        busy = any(sp < t_end and (not [l for l in listens if l > sp] or min(l for l in listens if l > sp) > t_start) for sp in speaks)
        nxt = [sp for sp in speaks if sp > t_end]
        if busy:
            overlap += 1
            tag = "かぶり"
        elif not nxt or nxt[0] - t_end > 8:
            miss += 1
            tag = "返事なし"
        else:
            l = nxt[0] + DMA_MS / 1000 - t_end
            lat.append(l)
            tag = f"{l:.2f}s"
        print(f"  {t_start:7.1f}〜{t_end:7.1f}s ({(s1 - s0) / RATE:.1f}s)  → {tag}")
    if lat:
        print(f"口から耳: n={len(lat)} 中央値 {np.median(lat):.2f}s / 90% {np.percentile(lat, 90):.2f}s")
    print(f"返事なし {miss} / かぶり {overlap}")
    barge = [e for e in ev if e["kind"] == "device" and e.get("ev") in ("barge", "interrupt")]
    print(f"さえぎり (本体が判定) {len([b for b in barge if b['ev'] == 'barge'])} 回 / タッチ {len([b for b in barge if b['ev'] == 'interrupt'])} 回")
    # 文字起こし
    heard, cur, last = [], "", None
    for e in ev:
        if e["kind"] == "in_text":
            if last is not None and e["t"] - last > 1.5 and cur:
                heard.append(cur); cur = ""
            cur += e["text"]; last = e["t"]
    if cur:
        heard.append(cur)
    print("聞き取った文:")
    for h in heard:
        print("  ", h)
    if a.script:
        ref = [l.strip() for l in open(a.script) if l.strip()]
        R, H = norm("".join(ref)), norm("".join(heard))
        print(f"文字の誤り率 (CER, 全体で並べて比較): {edit_distance(R, H) / max(1, len(R)):.1%}  (正解 {len(R)} 文字)")


if __name__ == "__main__":
    main()
