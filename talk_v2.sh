#!/bin/zsh
# 10/16 13:03 の段階の中継 (カメラ・見張り犬・話しかけ判定なし、なでられたらお礼)
#   ./talk_v2.sh start / stop / log
cd "$(dirname "$0")"
case "$1" in
  start)
    [ -f bridge/bridge.pid ] && kill -0 $(cat bridge/bridge.pid) 2>/dev/null && { echo "already running"; exit 0; }
    nohup .venv/bin/python -I bridge/live_bridge.py --wait --idle-min 5 --no-camera --no-addressed --pet-thanks >> bridge/bridge.log 2>&1 &
    echo $! > bridge/bridge.pid; echo "started pid $(cat bridge/bridge.pid)";;
  stop)
    [ -f bridge/bridge.pid ] && kill -INT $(cat bridge/bridge.pid) 2>/dev/null; sleep 3; rm -f bridge/bridge.pid; echo stopped;;
  log) tail -n 40 bridge/bridge.log;;
  *) echo "usage: $0 start|stop|log";;
esac
