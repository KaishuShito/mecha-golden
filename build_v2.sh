#!/bin/zsh
# 10/16 13:03 の段階 (コードで描いたメカ子犬 + GPT-Live で会話。中継は ./talk_v2.sh start) を書き込む: ./build_v2.sh
# 今の段階 (ドット絵 + 本体だけで会話) に戻すには: ./build.sh
cd "$(dirname "$0")" && source ./env.sh
PORT=${STACKCHAN_PORT:-$(ls /dev/cu.usbmodem* /dev/ttyACM* 2>/dev/null | head -1)}
[ -n "$PORT" ] || { echo "スタックチャンが見つかりません (USB-C でつなぐか STACKCHAN_PORT を指定)"; exit 1; }
arduino-cli compile -b m5stack:esp32:m5stack_cores3 --output-dir build_v2 mecha_retriever_v2 2>&1 | grep -E "error|Sketch uses" &&
arduino-cli upload -b m5stack:esp32:m5stack_cores3 -p "$PORT" --input-dir build_v2 mecha_retriever_v2 2>&1 | grep -E "Hash of data verified|rror" | tail -1
