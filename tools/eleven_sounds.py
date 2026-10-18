"""ElevenLabs の効果音生成 (eleven_text_to_sound_v2) で、ゴールデンレトリバーの子犬の声を作る。
キーは環境変数 ELEVENLABS_API_KEY (~/.zshrc)。出力は 16kHz PCM を WAV にして sounds/eleven/ に置く。"""
import json, os, sys, time, wave, urllib.request
from pathlib import Path

OUT = Path(__file__).resolve().parent.parent / "sounds" / "eleven"
STYLE = "close-up studio recording, clean, no background noise, no music"
PROMPTS = {
    "wan":     ("A golden retriever puppy gives one short happy bark, cute and soft", 0.6),
    "wanwan":  ("A golden retriever puppy gives two quick playful barks, excited and cute", 1.0),
    "kyan":    ("A golden retriever puppy gives one short surprised yelp, cute", 0.5),
    "kuun":    ("A golden retriever puppy whines softly and sweetly, a short cute whimper asking for attention", 1.2),
    "kuun_up": ("A golden retriever puppy makes a short curious whine that rises at the end, like asking a question", 0.8),
    "hafu":    ("A golden retriever puppy pants happily after being petted, soft quick breaths", 1.2),
    "fuun":    ("A golden retriever puppy lets out a contented sigh through its nose, relaxed and sleepy", 1.0),
    "uu":      ("A golden retriever puppy makes a tiny playful grumble, not aggressive, cute", 1.0),
}

def gen(name, text, dur, variant):
    body = json.dumps({"text": f"{text}. {STYLE}.", "duration_seconds": dur, "prompt_influence": 0.6}).encode()
    req = urllib.request.Request("https://api.elevenlabs.io/v1/sound-generation?output_format=pcm_16000", data=body,
                                 headers={"xi-api-key": os.environ["ELEVENLABS_API_KEY"], "Content-Type": "application/json"})
    raw = urllib.request.urlopen(req, timeout=60).read()
    import numpy as np
    st = np.frombuffer(raw[: len(raw) - len(raw) % 4], "<i2").reshape(-1, 2).astype(np.int32)  # pcm_16000 は左右 2ch で届く
    pcm = (st.mean(axis=1)).astype("<i2").tobytes()
    p = OUT / f"{name}_{variant}.wav"
    with wave.open(str(p), "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000); w.writeframes(pcm)
    return p, len(pcm) / 32000

if __name__ == "__main__":
    OUT.mkdir(parents=True, exist_ok=True)
    names = sys.argv[1:] or list(PROMPTS)
    variants = int(os.environ.get("VARIANTS", "3"))
    for n in names:
        for v in range(1, variants + 1):
            t0 = time.time()
            p, sec = gen(n, *PROMPTS[n], v)
            print(f"{p.name} {sec:.2f}s ({time.time() - t0:.1f}s)", flush=True)
