"""sounds/eleven/ の WAV (ElevenLabs で生成した子犬の声) の前後の無音を切って音量をそろえ、本体用の mecha_retriever/sounds.h を作る。
PICKS で、それぞれの声にどの候補を使うかを決める。"""
import wave, numpy as np
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SR = 16000
GROUPS = [  # (名前, 説明, 最長秒)。sounds/eleven/<名前>_1〜3.wav を全部使い、鳴らすたびに本体がランダムに選ぶ
    ("wan", "わん (うれしい一声)", 0.8),
    ("wanwan", "わんわん (はしゃぐ)", 1.6),
    ("kyan", "きゃん (びっくり)", 0.7),
    ("kuun", "くぅーん (さみしい・甘える)", 1.6),
    ("kuun_up", "くぅん? (なになに?)", 1.2),
    ("hafu", "はふはふ (ごきげん)", 1.6),
    ("fuun", "ふぅーん (満足のため息)", 1.4),
    ("uu", "うー (逆なでされて、ちょっと不満)", 1.4),
]
VARIANTS = 3

def prep(path, max_sec):
    x = np.frombuffer(wave.open(str(path)).readframes(10**8), "<i2").astype(float)
    env = np.convolve(np.abs(x), np.ones(160) / 160, "same")
    on = np.where(env > env.max() * 0.03)[0]
    a, b = max(0, on[0] - 320), min(len(x), on[-1] + 800)
    x = x[a:b][: int(max_sec * SR)]
    f = min(len(x) // 4, int(0.02 * SR))
    x[:f] *= np.linspace(0, 1, f)
    x[-f * 2:] *= np.linspace(1, 0, f * 2)
    return (x / (np.abs(x).max() + 1e-9) * 0.9 * 32767).astype("<i2")

hdr = ["// tools/build_sounds_h.py で生成。声は ElevenLabs の効果音生成 (eleven_text_to_sound_v2) で作ったゴールデンレトリバーの子犬の声",
       "#pragma once", "#include <stdint.h>", ""]
table = []
for name, desc, mx in GROUPS:
    refs = []
    for v in range(1, VARIANTS + 1):
        pcm = prep(ROOT / "sounds" / "eleven" / f"{name}_{v}.wav", mx)
        var = f"SND_{name.upper()}_{v}"
        hdr.append(f"// {name}_{v} — {desc} ({len(pcm) / SR:.2f}s)")
        hdr.append(f"static const int16_t {var}[{len(pcm)}] = {{" + ",".join(map(str, pcm.tolist())) + "};")
        refs.append(f"{{{var}, {len(pcm)}}}")
        print(name, v, f"{len(pcm) / SR:.2f}s")
    table.append(f"    {{\"{name}\", {{{', '.join(refs)}}}}},")
hdr += ["", "struct SoundClip { const int16_t* data; uint32_t len; };",
        f"static constexpr int SOUND_VARIANTS = {VARIANTS};",
        "struct Sound { const char* name; SoundClip clips[SOUND_VARIANTS]; };",
        "static const Sound SOUNDS[] = {", *table, "};",
        f"static constexpr int SOUND_COUNT = {len(GROUPS)};"]
(ROOT / "mecha_retriever" / "sounds.h").write_text("\n".join(hdr) + "\n")
