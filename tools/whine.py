"""子犬の「くぅーん」(鼻鳴き) を作り直す。
「く」(短い息の音) → 「ぅー」(口をすぼめた母音、高さが少し上がって下がる) → 「ん」(鼻に抜けて小さくなる)。
声帯の音に息の音を混ぜ、口の響き (フォルマント) を時間で動かす。"""
import numpy as np
from scipy.signal import lfilter, butter

SR = 16000
rng = np.random.default_rng(11)

def curve(points, n):
    t = np.linspace(0, 1, n)
    xs, ys = zip(*points)
    return np.interp(t, xs, ys)

def tv_res(x, fc, bw):
    """時間で中心が動く 2 次の共鳴フィルター"""
    y = np.zeros_like(x)
    y1 = y2 = 0.0
    r = np.exp(-np.pi * bw / SR)
    for i in range(len(x)):
        c = 2 * r * np.cos(2 * np.pi * fc[i] / SR)
        v = (1 - r) * x[i] + c * y1 - r * r * y2
        y[i] = v
        y2, y1 = y1, v
    return y

def voice(f0):
    n = len(f0)
    jit = 1 + 0.006 * np.convolve(rng.standard_normal(n), np.ones(80) / 80, "same") * 8
    ph = 2 * np.pi * np.cumsum(f0 * jit) / SR
    out = np.zeros(n)
    for k in range(1, 16):
        out += (1.0 / k) * (f0 * k < 6500) * np.sin(k * ph + rng.uniform(0, 6.28))
    shimmer = 1 + 0.12 * np.convolve(rng.standard_normal(n), np.ones(160) / 160, "same") * 10
    return out * shimmer

def kuun(dur=0.9, f_start=540, f_peak=650, f_end=380, peak_at=0.3, breath=0.3, nasal_from=0.68, vib=5.0, loud=0.8):
    n = int(dur * SR)
    t = np.arange(n) / SR
    f0 = curve([(0, f_start), (peak_at, f_peak), (nasal_from, (f_peak + f_end) / 2 + 20), (1, f_end)], n)
    f0 *= 1 + 0.02 * np.sin(2 * np.pi * vib * t) * curve([(0, 0), (0.3, 1), (1, 0.6)], n)
    src = voice(f0) + breath * rng.standard_normal(n) * 0.35
    F1 = curve([(0, 380), (0.15, 340), (nasal_from, 330), (1, 260)], n)
    F2 = curve([(0, 950), (0.2, 850), (nasal_from, 880), (1, 1150)], n)
    F3 = curve([(0, 2500), (1, 2300)], n)
    y = tv_res(src, F1, 90) * 1.0 + tv_res(src, F2, 140) * 0.55 + tv_res(src, F3, 250) * 0.25
    nas = curve([(0, 0), (nasal_from, 0), (1, 1)], n)
    y = y * (1 - 0.55 * nas) + lfilter([0.08], [1, -0.92], y) * nas * 2.0  # 「ん」は鼻に抜けてこもる
    amp = curve([(0, 0), (0.06, 0.75), (peak_at, 1.0), (nasal_from, 0.7), (0.92, 0.25), (1, 0)], n) ** 1.2
    y = y * amp
    k = np.zeros(n)  # 「く」の短い息
    kb = int(0.018 * SR)
    k[:kb] = lfilter(*_bp(2600, 1800), rng.standard_normal(kb)) * np.linspace(1, 0, kb) * 0.6
    y = np.concatenate([k[:kb], np.zeros(int(0.012 * SR)), y])
    b, a = butter(2, 280 / (SR / 2), "high")  # 小さなスピーカーで鳴らない低いうなりを切る
    y = lfilter(b, a, y)
    return y / (np.abs(y).max() + 1e-9) * loud

def _bp(fc, bw):
    r = np.exp(-np.pi * bw / SR)
    return [1 - r], [1, -2 * r * np.cos(2 * np.pi * fc / SR), r * r]

CANDIDATES = {
    "A": ("低めで鼻にかかった「くぅーん」", lambda: kuun(0.9, 520, 640, 370)),
    "B": ("短めで甘えた「くぅん」", lambda: kuun(0.55, 600, 720, 470, peak_at=0.35, nasal_from=0.6)),
    "C": ("2 回「くぅん、くぅーん」", lambda: np.concatenate([kuun(0.42, 600, 700, 520, nasal_from=0.6, loud=0.7), np.zeros(int(0.12 * SR)), kuun(0.8, 560, 680, 380)])),
    "D": ("息が多めの、か細い「きゅーん」(少し高め)", lambda: kuun(0.85, 760, 900, 560, breath=0.6, vib=6.5)),
}
