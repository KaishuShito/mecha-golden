// 本体だけで動くための「頭脳」: Wi-Fi で OpenAI に直接つなぎ、GPT-Live の会話・カメラの判定・鳴き声選び (Decisions) をする。
// Mac の bridge/live_bridge.py と同じことを本体の中でやる。Mac が USB でつながって中継している間は休む。
// Wi-Fi と API キーは、USB で一度だけ書き込む (tools/provision.py)。本体の不揮発メモリ (NVS) に入り、プログラムには含まれない。
#pragma once
#include <Arduino.h>

namespace brain {

enum class Status : uint8_t { NotConfigured, Connecting, Ready, Talking, Offline, Yield };

void begin();
Status status();
void provision(const char* name, const char* value);

}  // namespace brain
