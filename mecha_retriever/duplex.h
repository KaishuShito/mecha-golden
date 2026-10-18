// しゃべりながら聞く (全二重) 音声。マイクとスピーカーを同じ I2S で同時に動かし、
// スピーカーに送った音を手本にして、マイクに入った自分の声を ESP-SR のエコー除去 (AEC) で消す。
// M5Unified の Mic / Speaker は使わない (交互にしか動かせないため)。
#pragma once
#include <Arduino.h>

namespace duplex {

bool begin();               // 失敗したら false (その時は従来の交互方式で動かす)
bool running();
// 返事の音・鳴き声を鳴らす (16kHz mono)。ためておいて順に鳴らす
size_t play(const int16_t* s, size_t n);
size_t queued();            // まだ鳴らしていない量 (サンプル)
void flush();               // 鳴らす予定の音を捨てる (さえぎられた時)
void setVolume(uint8_t v);  // 0〜255
// エコーを消したマイクの音を取り出す (16kHz mono)。無ければ 0
size_t readMic(int16_t* out, size_t max);
float level();              // いま鳴らしている音の大きさ (0〜1、口パク用)
float micLevel();           // マイクの音の大きさ (rms)
bool takeBarge();           // しゃべっている間に人が話し始めた (声でさえぎった) か
void stats(char* buf, size_t n);
void debugRegs();

}  // namespace duplex
