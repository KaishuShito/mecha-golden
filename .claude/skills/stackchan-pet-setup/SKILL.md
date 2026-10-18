---
name: stackchan-pet-setup
description: Set up "Mecha Golden" (this repository) on a newly purchased M5Stack StackChan (CoreS3) from scratch — explain cost and privacy, install the toolchain, back up the factory firmware, flash the firmware, place the OpenAI API key, start the Mac-side GPT-Live bridge, and verify that the pet talks. Use when the user asks to set up, install, flash, or try this StackChan pet, or says "スタックチャンのセットアップをして".
---

# Mecha Golden セットアップ (StackChan / CoreS3)

あなた (Claude Code) は、買ったばかりのスタックチャンを持っている利用者のために、このリポジトリを動くところまで順に進める。
利用者は電子工作が初めてかもしれない。専門用語を避け、1 ステップずつ「何をするか・なぜか」を短く伝えてから実行する。
人の手が要る所 (USB をつなぐ、ボタンを押す、API キーを用意する、Mac の許可を押す) は、はっきり頼んで、終わったと言われてから進む。

## 0. 最初に説明して、同意をもらう (必ず)
次の 4 点を利用者に説明し、「進めてよい」と言われてから始める。
- **費用**: 会話している間だけ OpenAI GPT-Live に 1 分あたり $0.05 (1 時間 約 $3)。待機中はかからない。Decisions API は 1 回 約 $0.00006 (1 時間使っても数セント)。天気などの調べものは使った時だけ別料金
- **音の扱い**: 会話している間、スタックチャンのマイクの音は OpenAI に送られる。待機中は送らない。会話は頭のセンサーを長押しした時だけ始まる
- **工場ファームが消える**: 書き込むと、買った時の AI 機能は消える。先にバックアップを取るので、いつでも戻せる (手順 3)
- **必要なもの**: Mac (Apple シリコン)、スタックチャン、USB-C ケーブル、GPT-Live と Decisions API が使える OpenAI の API キー。作業時間は 20〜30 分 (ダウンロードが数 GB ある)

## 1. 道具の確認
```sh
sw_vers; uname -m; command -v brew git python3
```
- Homebrew が無ければ、利用者に https://brew.sh の 1 行を実行してもらう (Mac のパスワード入力が要るので、あなたは実行しない)
- Python は 3.10 以上 (`python3 --version`)。古ければ `brew install python@3.12`

## 2. スタックチャンをつなぐ (利用者の手)
- 利用者に「スタックチャンの胴体の USB-C を Mac につないでください」と頼む
- Mac が「アクセサリの接続を許可しますか」と聞いてきたら「許可」を押してもらう
- 見えるか確かめる: `ls /dev/cu.usbmodem*` (1 つ出れば OK)
- 出ない時: ケーブルを挿し直す → それでもだめなら、本体の RST ボタンを 2 秒ほど長押しして、中の LED が緑になったら離してもらう (書き込み待ちの状態になる)

## 3. 開発環境の用意と、工場ファームのバックアップ
```sh
./scripts/setup.sh
```
- arduino-cli (brew)、M5Stack のボードパッケージ 3.3.9、ライブラリ、Python の仮想環境 (.venv) を、このリポジトリの中に入れる。数分〜10 分かかる。途中経過を短く伝える
- 終わったら **書き込む前にバックアップ** (16MB、約 1 分):
```sh
mkdir -p backup && .venv/bin/esptool read-flash 0 ALL backup/factory.bin
.venv/bin/esptool verify-flash 0 backup/factory.bin   # "digest matched" なら OK
```
- `backup/` は Git に入らない (Wi-Fi などの設定が入っているので、人に渡さない)

## 4. API キーを置く (利用者の手)
- `.env` (setup.sh が .env.example から作る) の `OPENAI_API_KEY=` の後ろに、利用者自身にキーを書いてもらう
- キーをチャットに貼らせない。貼られたら、あなたは `.env` に書いてから「チャットの記録に残るので、念のため OpenAI の画面でキーを作り直すと安全です」と伝える
- 確かめる (値は表示しない): `awk -F= '/^OPENAI_API_KEY=/{print (length($2)>20?"set":"empty")}' .env`

## 5. 書き込む
```sh
./build.sh
```
- 「Hash of data verified」が出れば成功。画面に黒い背景のドット絵の子犬が出る
- 失敗した時:
  - `Failed to connect` / ポートが無い → 手順 2 の RST 長押しをしてもらってから、もう一度
  - `bad CPU type in executable` (ctags) → `./scripts/setup.sh` をもう一度 (Rosetta が無い Mac の回避を入れ直す)
  - PY32IOExpander の型のエラー → StackChan-BSP がライブラリマネージャーの古い版。`.arduino/user/libraries/M5StackChan` を消して setup.sh をもう一度
  - 中継が動いている (ポートが使用中) → `./talk.sh stop` してから

## 6. マイクとスピーカーの確認 (API は使わない)
```sh
.venv/bin/python -I bridge/live_bridge.py --selftest
```
- 「4 秒録音します」で利用者にスタックチャンへ話しかけてもらい、続いてスタックチャンから日本語の声が流れる
- 利用者に「声が聞こえましたか」と聞く。録音の大きさは出力の `rms=` (話しかけて 100 以上なら OK)

## 7. 会話してみる
```sh
./talk.sh start
```
- 利用者に「頭のてっぺんのセンサーを 1 秒長押しして、挨拶が聞こえたら『こんにちは』と話しかけてください」と頼む
- 動いているか: `tail -n 30 bridge/bridge.log` に「会話を始めました」と、🗣 (聞き取った言葉)・🐶 (返事) の行が出る
- 使い方を伝える:
  - 終わり方: 「バイバイ」と言う / 頭をもう一度長押し / 画面右上の end x / 5 分話しかけなければ自然に終わる
  - 返事の途中で画面か頭を触ると止まる
  - 頭を軽く叩く・なでると、子犬の声で鳴く
  - 性格は `bridge/persona.md` を書き換えると変わる (次の会話から)
- 止める: `./talk.sh stop`

## 8. (任意) 本体だけで動かす (Wi-Fi)
利用者が望んだ時だけ。先に「キーが本体に入るので、本体を人に渡す時はキーを止める必要がある」「音は電話くらいの質」と伝える。
1. `.env` に `STACKCHAN_WIFI_SSID` / `STACKCHAN_WIFI_PASS` を利用者に書いてもらう (**2.4GHz のみ**。名前に「5G」が付く Wi-Fi は不可。ログイン画面が出る Wi-Fi も不可)
2. `.venv/bin/python -I tools/provision.py --use-main-key` (値は表示されない)
3. `./talk.sh stop` して USB を抜く → 5 秒で本体だけのモードになる。画面右上の wifi が明るければ準備完了
- つながらない時は、USB をつないだまま `screen` などでログを見ると理由が出る (201 = Wi-Fi が見つからない → 5GHz の可能性 / 202 = パスワード違い)

## 9. 工場出荷の状態に戻す
```sh
./talk.sh stop; .venv/bin/esptool write-flash 0 backup/factory.bin
```
バックアップが無い時は、M5Burner (M5Stack 公式) で「StackChan」→ Only Official → Burn。

## 終わりに報告すること
- 何をしたか (バックアップの場所、書き込みの成否、会話の確認結果)
- 費用と音の扱いをもう一度ひとことで
- うまくいかなかった所と、試したこと
