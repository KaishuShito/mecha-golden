// 会話モード: USB シリアルで Mac の中継 (bridge/live_bridge.py) とつなぎ、
// マイクの音を送り、返事の音を鳴らす。シリアルの送受信はすべてこのモジュールの専用タスクが行う。
#pragma once
#include <Arduino.h>

namespace talk {

enum class Mode : uint8_t { Listen, Speak };

void begin(const void* snapBuf, size_t snapLen, int snapW, int snapH, const char* (*stateName)());
void logf(const char* fmt, ...);
bool active();
Mode mode();
bool thinking();
float level();
void captions(char* user, size_t userLen, char* ai, size_t aiLen, uint32_t& userAt, uint32_t& aiAt);
void interrupt();
void petted();
// 触った出来事を Mac に送る (例 "touch:stroke:middle")。Mac の返事 ('P' 鳴き声番号) が来なければ fallback を鳴らす
void touchEvent(const char* ev, int fallbackSound);
void endTalk();
void playSound(int id);
// 鳴らした鳴き声の番号 (+1)。0 なら無し。画面の動きに使う
int takeSoundAnim();

// 本体だけで動く時 (brain.cpp): Mac のシリアルと同じフレームをメモリ内でやり取りする
void setLocalLink(bool on);
bool localLinkOn();
bool macSeen();
void localSend(uint8_t type, const void* data, size_t len);
size_t localRecv(uint8_t* buf, size_t max, uint32_t waitMs);
size_t localSpace();
// 見張り犬の気分 (Mac から 'D' フレーム)。at=0 ならまだ来ていない
uint8_t mood(char* text, size_t textLen, uint32_t& at);
int takeCommand();
// 画面の取り込み: 描き終わったところで snapshotWanted() を見て、絵をコピーしてから snapshotDone()
bool snapshotWanted();
void snapshotDone();

}  // namespace talk
