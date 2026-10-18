#!/bin/zsh
# 開発環境をこのリポジトリの中に用意する (Mac 前提。Homebrew と Python 3.10 以上が要る)
#   ./scripts/setup.sh
# 入るもの: arduino-cli (brew)、M5Stack のボードパッケージ 3.3.9、ライブラリ、Python の仮想環境 (.venv)
# ボードパッケージのダウンロードは数 GB・数分かかる
set -e
cd "$(dirname "$0")/.."
command -v brew >/dev/null || { echo "Homebrew が必要です: https://brew.sh"; exit 1; }
command -v arduino-cli >/dev/null || brew install arduino-cli
source ./env.sh
arduino-cli config init --overwrite >/dev/null
arduino-cli core update-index
arduino-cli core install m5stack:esp32@3.3.9
arduino-cli lib install "M5Unified@0.2.26" "M5GFX@0.2.33" "IRremoteESP8266@2.9.0" "M5Unit-NFC@0.1.2"
# StackChan-BSP はライブラリマネージャーの版 (1.0.1) ではビルドできないので、GitHub の版を入れる
BSP=.arduino/user/libraries/M5StackChan
if [ ! -d "$BSP" ]; then
  git clone -q https://github.com/m5stack/StackChan-BSP "$BSP"
  git -C "$BSP" checkout -q 8d4d6fc3b7a6be379c6317c45a02a30bff8c492e
fi
# Apple シリコンで Rosetta が無いと、付属の ctags (Intel 版) が動かない。スケッチの本体は .cpp なので ctags は不要
CT=.arduino/data/packages/builtin/tools/ctags/5.8-arduino11/ctags
if [ -f "$CT" ] && ! "$CT" --version >/dev/null 2>&1; then
  mv "$CT" "$CT.orig"; printf '#!/bin/sh\nexit 0\n' > "$CT"; chmod +x "$CT"
fi
PY=$(command -v python3.12 || command -v python3.13 || command -v python3.14 || command -v python3)
"$PY" -m venv .venv
.venv/bin/pip install -q -r requirements.txt
[ -f .env ] || cp .env.example .env
echo "準備ができました。次は .env に OPENAI_API_KEY を書いて、./build.sh で書き込みます"
