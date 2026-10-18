"""画像生成したポーズの帯 (petgen/run/gen/*.png、マゼンタ背景) から、本体用のドット絵スプライトを作る。

1. マゼンタを抜いて、1 コマずつ切り出す
2. 生成画像の「1 ドット = 何ピクセルか」を推定し、本物のドット (1 マス = 1 点) に縮める
3. 全ポーズ共通の 63 色のパレットに減色し、行ごとにランレングス圧縮して mecha_retriever/sprites.h に書く
4. 確認用に、黒背景の一覧 (petgen/qa/sprites_sheet.png) と動き (petgen/qa/*.gif) を作る
"""
import json
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
GEN = ROOT / "petgen/run/gen"
QA = ROOT / "petgen/qa"

# (状態, 帯のファイル, コマ数, 各コマの表示時間 ms) — 時間は hatch-pet の animation-rows.md に合わせた
STATES = [
    ("idle", "idle", 6, [280, 110, 110, 140, 140, 320]),
    ("waving", "waving", 4, [140, 140, 140, 280]),
    ("jumping", "jumping", 5, [140, 140, 140, 140, 280]),
    ("failed", "failed", 8, [140] * 7 + [240]),
    ("waiting", "waiting", 6, [150] * 5 + [260]),
    ("working", "running", 6, [120] * 5 + [220]),
    ("review", "review", 6, [150] * 5 + [280]),
    ("talking", "talking", 4, [110, 110, 110, 110]),
    ("happy", "happy", 6, [140] * 5 + [240]),
    ("listening", "listening", 4, [220, 220, 220, 220]),
    ("paw", "paw", 4, [180, 220, 900, 300]),
]


def fg_mask(a):
    r, g, b = a[..., 0].astype(int), a[..., 1].astype(int), a[..., 2].astype(int)
    magenta = (r > 150) & (b > 150) & (g < 120) & (np.abs(r - b) < 90)
    return ~magenta


def split_frames(mask, n):
    cols = mask.sum(axis=0) > 2
    runs, x = [], 0
    W = len(cols)
    while x < W:
        if cols[x]:
            s = x
            while x < W and cols[x]:
                x += 1
            runs.append([s, x])
        else:
            x += 1
    runs = [r for r in runs if r[1] - r[0] > 8]
    while len(runs) > n:  # 近すぎる塊 (しっぽ・アンテナが離れている時) はつなぐ
        gaps = [runs[i + 1][0] - runs[i][1] for i in range(len(runs) - 1)]
        i = int(np.argmin(gaps))
        runs[i] = [runs[i][0], runs[i + 1][1]]
        del runs[i + 1]
    if len(runs) != n:  # 塊で分けられない時は等分
        W = mask.shape[1]
        runs = [[int(W * i / n), int(W * (i + 1) / n)] for i in range(n)]
    return runs


def block_size(a, mask):
    """同じ色が横に続く長さの最頻値 = 生成画像の 1 ドットの大きさ"""
    lens = []
    for y in range(0, a.shape[0], 3):
        row, m = a[y], mask[y]
        run = 1
        for x in range(1, a.shape[1]):
            if m[x] and m[x - 1] and np.abs(row[x].astype(int) - row[x - 1].astype(int)).sum() < 24:
                run += 1
            else:
                if 2 <= run <= 20:
                    lens.append(run)
                run = 1
    if not lens:
        return 4
    h = np.bincount(lens)
    return max(2, int(np.argmax(h[2:]) + 2))


def to_grid(img, mask, f):
    """f ピクセル (小数でもよい) 四方を 1 ドットに。ずれは元の絵と最も合う位置を選ぶ"""
    best = None
    H, W = mask.shape
    steps = 6
    for oy in np.linspace(0, f, steps, endpoint=False):
        for ox in np.linspace(0, f, steps, endpoint=False):
            ys = (np.arange(oy + f / 2, H, f)).astype(int)
            xs = (np.arange(ox + f / 2, W, f)).astype(int)
            if len(ys) == 0 or len(xs) == 0:
                continue
            small = img[np.ix_(ys, xs)]
            sm = mask[np.ix_(ys, xs)]
            # 元の絵との差 (間引いた点の周り 1/3 の範囲で比べる)
            r = max(1, int(f / 3))
            err = 0
            for dy in (-r, r):
                for dx in (-r, r):
                    yy = np.clip(ys + dy, 0, H - 1)
                    xx = np.clip(xs + dx, 0, W - 1)
                    err += np.abs(img[np.ix_(yy, xx)].astype(int) - small.astype(int)).sum()
            if best is None or err < best[0]:
                best = (err, small, sm)
    return best[1], best[2]


def clean(sm):
    """孤立した 1 ドットを消す"""
    out = sm.copy()
    H, W = sm.shape
    for y in range(H):
        for x in range(W):
            if sm[y, x]:
                n = sm[max(0, y - 1) : y + 2, max(0, x - 1) : x + 2].sum() - 1
                if n == 0:
                    out[y, x] = False
    return out


def main():
    QA.mkdir(parents=True, exist_ok=True)
    rows = {}
    for state, src, n, dur in STATES:
        a = np.array(Image.open(GEN / f"{src}.png").convert("RGB"))
        m = fg_mask(a)
        runs = split_frames(m, n)
        b = block_size(a, m)
        frames = []
        for x0, x1 in runs:
            sub, sm = a[:, x0:x1], m[:, x0:x1]
            ys = np.where(sm.any(axis=1))[0]
            frames.append((sub, sm, ys[0], ys[-1]))
        rows[state] = (frames, b, dur)
        print(f"{state}: {len(frames)} frames, dot~{b}px")

    # 全ポーズで犬の背の高さをそろえる (行ごとの「座った時の高さ」の中央値を TARGET_H ドットにする)
    TARGET_H = 46
    small_frames = {}
    for state, (frames, b0, dur) in rows.items():
        heights = sorted(f[3] - f[2] for f in frames)
        b = heights[len(heights) // 2] / TARGET_H
        base_bottom = max(f[3] for f in frames)
        out = []
        for sub, sm, top, bot in frames:
            g, gm = to_grid(sub, sm, b)
            gm = clean(gm)
            yy, xx = np.where(gm)
            g, gm = g[yy.min() : yy.max() + 1, xx.min() : xx.max() + 1], gm[yy.min() : yy.max() + 1, xx.min() : xx.max() + 1]
            lift = int(round((base_bottom - bot) / b))  # ジャンプなどで浮いている高さ
            out.append((g, gm, lift))
        small_frames[state] = (out, dur)

    # 全コマ共通の大きさ (下ぞろえ・中央ぞろえ)
    Wc = max(g.shape[1] for fr, _ in small_frames.values() for g, _, _ in fr) + 2
    Hc = max(g.shape[0] + lift for fr, _ in small_frames.values() for g, _, lift in fr) + 2
    print("cell", Wc, Hc)

    # 減色 (透明 + 63 色)
    allpix = np.concatenate([g[gm] for fr, _ in small_frames.values() for g, gm, _ in fr])
    pal_img = Image.fromarray(allpix.reshape(1, -1, 3).astype("uint8")).quantize(colors=63, method=Image.Quantize.MEDIANCUT)
    pal = np.array(pal_img.getpalette()[: 63 * 3]).reshape(-1, 3)

    def nearest(c):
        return int(np.argmin(((pal - c) ** 2).sum(axis=1))) + 1

    cache = {}
    hdr = ["// tools/sprites.py で生成。絵は Cockpit の画像生成 (gpt-image-2) で、hatch-pet の手順で作ったドット絵", "#pragma once", "#include <stdint.h>", ""]
    hdr.append(f"static constexpr int SPR_W = {Wc}, SPR_H = {Hc};")
    p565 = []
    for r, g, bb in pal:
        p565.append(((int(r) & 0xF8) << 8) | ((int(g) & 0xFC) << 3) | (int(bb) >> 3))
    hdr.append("static const uint16_t SPR_PAL[64] = {0x0000, " + ", ".join(f"0x{v:04X}" for v in p565) + "};")
    blobs, index = [], []
    previews = {}
    for state, (fr, dur) in small_frames.items():
        ids = []
        prev_imgs = []
        for g, gm, lift in fr:
            cell = np.zeros((Hc, Wc), dtype=np.uint8)
            oy = Hc - 1 - lift - g.shape[0]
            ox = (Wc - g.shape[1]) // 2
            for y in range(g.shape[0]):
                for x in range(g.shape[1]):
                    if gm[y, x]:
                        c = tuple(g[y, x])
                        if c not in cache:
                            cache[c] = nearest(np.array(c))
                        cell[oy + y, ox + x] = cache[c]
            # ランレングス: (回数, 色) の組。回数は 1〜255
            enc = []
            flat = cell.flatten()
            i = 0
            while i < len(flat):
                j = i
                while j < len(flat) and flat[j] == flat[i] and j - i < 255:
                    j += 1
                enc += [j - i, int(flat[i])]
                i = j
            ids.append(len(blobs))
            blobs.append(enc)
            rgb = np.zeros((Hc, Wc, 3), dtype=np.uint8)
            for k in range(1, 64):
                rgb[cell == k] = pal[k - 1]
            prev_imgs.append(rgb)
        index.append((state, ids, dur))
        previews[state] = prev_imgs

    total = 0
    for i, enc in enumerate(blobs):
        hdr.append(f"static const uint8_t SPR_F{i}[{len(enc)}] = {{" + ",".join(map(str, enc)) + "};")
        total += len(enc)
    hdr.append("static const uint8_t* const SPR_FRAMES[] = {" + ", ".join(f"SPR_F{i}" for i in range(len(blobs))) + "};")
    hdr.append("struct SprAnim { const char* name; uint8_t count; uint8_t first; uint16_t ms[8]; };")
    hdr.append("static const SprAnim SPR_ANIMS[] = {")
    for state, ids, dur in index:
        hdr.append(f"    {{\"{state}\", {len(ids)}, {ids[0]}, {{{', '.join(map(str, dur))}}}}},")
    hdr.append("};")
    hdr.append(f"static constexpr int SPR_ANIM_COUNT = {len(index)};")
    (ROOT / "mecha_retriever/sprites.h").write_text("\n".join(hdr) + "\n")
    print("frames", len(blobs), "rle bytes", total)

    # 確認用: 黒背景の一覧と GIF (本体と同じく 3 倍)
    S = 3
    rowsimg = []
    for state, imgs in previews.items():
        strip = np.zeros((Hc * S + 8, (Wc * S + 8) * 8, 3), dtype=np.uint8)
        for i, im in enumerate(imgs):
            up = np.repeat(np.repeat(im, S, 0), S, 1)
            strip[4 : 4 + Hc * S, 4 + i * (Wc * S + 8) : 4 + i * (Wc * S + 8) + Wc * S] = up
        rowsimg.append(strip)
        gif = [Image.fromarray(np.repeat(np.repeat(im, S, 0), S, 1)) for im in imgs]
        gif[0].save(QA / f"{state}.gif", save_all=True, append_images=gif[1:], duration=dict((s, d) for s, _, d in index)[state], loop=0)
    Image.fromarray(np.concatenate(rowsimg, 0)).save(QA / "sprites_sheet.png")


if __name__ == "__main__":
    main()
