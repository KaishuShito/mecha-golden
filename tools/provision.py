"""本体に Wi-Fi と OpenAI の API キーを書き込む (USB で一度だけ)。値は表示しない。
リポジトリ直下の .env から読む:
  STACKCHAN_WIFI_SSID / STACKCHAN_WIFI_PASS   … オフィスの 2.4GHz Wi-Fi
  STACKCHAN_OPENAI_API_KEY                     … スタックチャン専用のキー (無ければ --use-main-key で OPENAI_API_KEY を使う)
本体の不揮発メモリ (NVS) に入り、プログラムやこのフォルダには残らない。
注意: 中継 (talk.sh) が動いていても書き込める。"""
import sys, time, serial
from pathlib import Path

from stackchan_port import load_env
env = load_env()  # リポジトリ直下の .env (引用符と前後の空白は外す)
key = env.get("STACKCHAN_OPENAI_API_KEY") or (env.get("OPENAI_API_KEY") if "--use-main-key" in sys.argv else None)
items = {"wifi_ssid": env.get("STACKCHAN_WIFI_SSID"), "wifi_pass": env.get("STACKCHAN_WIFI_PASS", ""), "openai_key": key}
missing = [k for k, v in items.items() if v is None or (k != "wifi_pass" and not v)]
if missing:
    raise SystemExit(f"足りない設定: {missing} (.env を確認)")
s = serial.Serial(find_port(), 115200, timeout=0.3)
time.sleep(0.3)
for k, v in items.items():
    p = k.encode() + b"\0" + v.encode()
    s.write(b"\xa5\x5aK" + len(p).to_bytes(2, "little") + p)
    time.sleep(0.4)
time.sleep(0.5)
out = s.read(100000).decode(errors="replace")
print("\n".join(l for l in out.splitlines() if l.startswith("[cfg]") or "[cfg]" in l) or "(本体の返事が見えない。talk.sh を止めてから再実行すると確認できる)")
