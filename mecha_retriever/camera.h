// CoreS3 のカメラ (GC0308)。内部 I2C を画面のタッチなどと共有するので、撮る間だけ借りる (busy() の間は I2C を使わない)。
#pragma once
#include <Arduino.h>

namespace cam {
void begin();
bool busy();
void request();
// JPEG ができていれば data/len を返す。使い終わったら release()
bool ready(const uint8_t*& data, size_t& len);
void release();
// 起動したままのカメラを止めて内部メモリを返す (暗号化通信を始められない時に使う)
void dropKeep();
bool kept();
}
