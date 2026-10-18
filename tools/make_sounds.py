"""子犬の声 (メカゴールデン用) を合成して、16kHz の WAV と本体用のヘッダー (mecha_retriever/sounds.h) を作る。
声帯の音 (高調波の束) を、口の響き (フォルマント) のフィルターに通す「ソース・フィルター」方式。録音素材は使っていない。"""
import numpy as np, wave
from scipy.signal import lfilter
from pathlib import Path

SR = 16000
rng = np.random.default_rng(7)

def contour(points, n):
    """[(時間割合, 値), ...] を n サンプルに滑らかにつなぐ"""
    t = np.linspace(0, 1, n)
    xs, ys = zip(*points)
    return np.interp(t, xs, ys)

def source(f0, jitter=0.01, bright=1.0):
    n = len(f0)
    f = f0 * (1 + jitter * rng.standard_normal(n).cumsum() / np.sqrt(np.arange(1, n + 1)))
    ph = 2 * np.pi * np.cumsum(f) / SR
    out = np.zeros(n)
    for k in range(1, 18):
        amp = (1.0 / k ** (0.9 / bright)) * (f * k < 7000)
        out += amp * np.sin(k * ph)
    return out

def resonator(x, fc, bw):
    """2 次の共鳴フィルター (フォルマント)"""
    r = np.exp(-np.pi * bw / SR)
    th = 2 * np.pi * fc / SR
    b = [1 - r]
    a = [1, -2 * r * np.cos(th), r * r]
    return lfilter(b, a, x)

def formants(x, fs):
    return sum(g * resonator(x, f, bw) for f, bw, g in fs)

def env(n, attack, release, shape=None):
    e = np.ones(n)
    a = max(1, int(attack * SR)); r = max(1, int(release * SR))
    e[:a] = np.linspace(0, 1, a) ** 0.7
    e[-r:] *= np.linspace(1, 0, r) ** 1.5
    if shape is not None:
        e *= shape
    return e

def noise(n, fc, bw):
    return resonator(rng.standard_normal(n), fc, bw)

def norm(x, peak=0.85):
    return x / (np.abs(x).max() + 1e-9) * peak

def bark(dur=0.2, f_lo=520, f_hi=820, vowel="wa", bright=1.1):
    n = int(dur * SR)
    f0 = contour([(0, f_lo * 0.9), (0.25, f_hi), (0.6, f_hi * 0.85), (1, f_lo * 0.75)], n)
    src = source(f0, 0.02, bright)
    F1 = contour([(0, 500), (0.3, 950), (1, 700)], n)  # 「わ」で口が開く
    y = np.zeros(n)
    # 時間で動くフォルマントは短い区間ごとに近似
    seg = 160
    for i in range(0, n, seg):
        s = src[max(0, i - 400):i + seg]
        f1 = F1[min(i, n - 1)]
        out = formants(s, [(f1, 200, 1.0), (1800, 260, 0.9), (3100, 400, 0.6), (4200, 500, 0.3)])
        y[i:i + seg] = out[-len(src[i:i + seg]):]
    y = lfilter([1, -0.85], [1], y)  # 高い音を持ち上げて、明るい声に
    y = y / (np.abs(y).max() + 1e-9)
    y += 0.25 * norm(noise(n, 2800, 2000)) * env(n, 0.003, dur * 0.85)
    return norm(y * env(n, 0.012, dur * 0.55))

def whine(dur=0.8, f_start=700, f_peak=1150, f_end=900, vib=6.0, nasal=True, rise=False):
    n = int(dur * SR)
    pts = [(0, f_start), (0.35, f_peak), (1, f_end)] if not rise else [(0, f_start), (0.7, f_end), (1, f_peak)]
    f0 = contour(pts, n)
    t = np.arange(n) / SR
    f0 = f0 * (1 + 0.025 * np.sin(2 * np.pi * vib * t) * np.linspace(0.3, 1, n))
    src = source(f0, 0.005, 0.8)
    fs = [(450, 120, 1.0), (1300, 200, 0.8), (2600, 300, 0.5)] if nasal else [(700, 150, 1.0), (1500, 200, 0.5)]
    y = lfilter([1, -0.7], [1], formants(src, fs))
    y = y / (np.abs(y).max() + 1e-9) + 0.06 * norm(noise(n, 3000, 2000))
    return norm(y * env(n, 0.08, 0.25), 0.75)

def pant(count=4, rate=0.16):
    parts = []
    for i in range(count):
        n = int(rate * SR)
        x = noise(n, 1500 if i % 2 == 0 else 1100, 900) + 0.5 * noise(n, 3200, 1500)
        parts.append(x * env(n, 0.02, rate * 0.7) * (1.0 if i % 2 == 0 else 0.7))
        parts.append(np.zeros(int(0.03 * SR)))
    return norm(np.concatenate(parts), 0.6)

def gap(s):
    return np.zeros(int(s * SR))

SOUNDS = {
    "wan":      ("わん (うれしい一声)", lambda: bark(0.2, 560, 860)),
    "wanwan":   ("わんわん (はしゃぐ)", lambda: np.concatenate([bark(0.17, 600, 920), gap(0.09), bark(0.19, 620, 960)])),
    "kyan":     ("きゃん (びっくり・高い一声)", lambda: bark(0.13, 900, 1400, bright=1.3)),
    "kuun":     ("くぅーん (さみしい・甘える)", lambda: whine(0.85, 760, 1180, 820)),
    "kuun_up":  ("くぅん? (なになに? 語尾が上がる)", lambda: whine(0.55, 700, 1250, 900, vib=7, rise=True)),
    "hafu":     ("はふはふ (なでられてごきげん)", lambda: pant(4, 0.15)),
    "fuun":     ("ふぅーん (満足のため息)", lambda: whine(0.7, 520, 600, 380, vib=4) * 0.8),
    "uu":       ("うー (逆なでされて、ちょっとだけ不満)", lambda: norm(formants(source(contour([(0, 260), (0.5, 300), (1, 250)], int(0.6 * SR)), 0.04, 1.4), [(500, 200, 1), (1400, 300, 0.4)]) * env(int(0.6 * SR), 0.06, 0.2), 0.6)),
}

import sys as _sys
_sys.path.insert(0, str(Path(__file__).parent))
import whine as _whine
for _k, (_d, _fn) in _whine.CANDIDATES.items():
    SOUNDS[f"kuun_{_k.lower()}"] = (f"くぅーん候補 {_k}: {_d}", _fn)

def main():
    out_dir = Path("sounds"); out_dir.mkdir(exist_ok=True)
    hdr = ["// tools/make_sounds.py で生成 (子犬の声の合成。録音素材なし)", "#pragma once", "#include <stdint.h>", ""]
    table = []
    for i, (name, (desc, fn)) in enumerate(SOUNDS.items()):
        y = fn()
        pcm = (np.clip(y, -1, 1) * 30000).astype("<i2")
        with wave.open(str(out_dir / f"{i}_{name}.wav"), "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR); w.writeframes(pcm.tobytes())
        hdr.append(f"// {i}: {name} — {desc} ({len(pcm) / SR:.2f}s)")
        hdr.append(f"static const int16_t SND_{name.upper()}[{len(pcm)}] = {{" + ",".join(map(str, pcm.tolist())) + "};")
        table.append(f"    {{SND_{name.upper()}, {len(pcm)}, \"{name}\"}},")
        print(i, name, f"{len(pcm) / SR:.2f}s", desc)
    hdr += ["", "struct Sound { const int16_t* data; uint32_t len; const char* name; };",
            "static const Sound SOUNDS[] = {", *table, "};",
            f"static constexpr int SOUND_COUNT = {len(SOUNDS)};"]
    Path("mecha_retriever/sounds.h").write_text("\n".join(hdr) + "\n")

main()
