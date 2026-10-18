#!/bin/zsh
# ビルドして書き込む: ./build.sh
cd "$(dirname "$0")" && source ./env.sh
PORT=${STACKCHAN_PORT:-$(ls /dev/cu.usbmodem* /dev/ttyACM* 2>/dev/null | head -1)}
[ -n "$PORT" ] || { echo "スタックチャンが見つかりません (USB-C でつなぐか STACKCHAN_PORT を指定)"; exit 1; }
out=$(arduino-cli compile -b m5stack:esp32:m5stack_cores3 --output-dir build mecha_retriever 2>&1) || { echo "$out" | grep -E "error" | head -20; exit 1; }
echo "$out" | grep -E "Sketch uses"
arduino-cli upload -b m5stack:esp32:m5stack_cores3 -p "$PORT" --input-dir build mecha_retriever 2>&1 | grep -E "Hash of data verified|rror" | tail -1
