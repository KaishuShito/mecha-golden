// 会話モード (10/16 13:03 の段階): USB シリアルで Mac の中継 (bridge/live_bridge.py) とつなぎ、
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
int takeCommand();

}  // namespace talk
