#!/bin/zsh
# メカゴールデンの会話中継を動かす / 止める / 様子を見る
#   ./talk.sh start   … 裏で待機 (頭をなでる・前でこちらを見ると会話が始まり、5 分話しかけられなければ待機に戻る)。カメラ判定・見張り犬・話しかけ判定つき
#   ./talk.sh stop    … 止める
#   ./talk.sh log     … 会話のログを見る
cd "$(dirname "$0")"
case "$1" in
  start)
    [ -f bridge/bridge.pid ] && kill -0 $(cat bridge/bridge.pid) 2>/dev/null && { echo "already running"; exit 0; }
    nohup .venv/bin/python -I bridge/live_bridge.py --wait --idle-min 5 --no-camera ${TRACE:+--trace} >> bridge/bridge.log 2>&1 &
    echo $! > bridge/bridge.pid; echo "started pid $(cat bridge/bridge.pid)";;
  stop)
    [ -f bridge/bridge.pid ] && kill -INT $(cat bridge/bridge.pid) 2>/dev/null; sleep 3; rm -f bridge/bridge.pid; echo stopped;;
  log) tail -n 40 bridge/bridge.log;;
  *) echo "usage: $0 start|stop|log";;
esac
