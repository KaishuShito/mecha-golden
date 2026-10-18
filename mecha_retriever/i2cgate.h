// 内部 I2C (タッチ・LED・電池・スピーカー/マイクの設定・カメラ) の順番待ち。
// カメラは起動時に I2C を一度手放すので、他の処理の途中で手放すと止まる (10/16 xTaskPriorityDisinherit で再起動)。
#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace i2cgate {
inline SemaphoreHandle_t& h()
{
    static SemaphoreHandle_t s = xSemaphoreCreateRecursiveMutex();
    return s;
}
inline bool take(uint32_t ms) { return xSemaphoreTakeRecursive(h(), pdMS_TO_TICKS(ms)) == pdTRUE; }
inline void give() { xSemaphoreGiveRecursive(h()); }
}  // namespace i2cgate
