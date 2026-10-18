#include "camera.h"
#include <M5Unified.h>
#include <esp_camera.h>
#include <driver/gpio.h>
#include <img_converters.h>
#include <atomic>
#include "talk.h"
#include "i2cgate.h"

namespace cam {
namespace {
std::atomic<bool> busyF{false}, req{false}, have{false};
uint8_t* frame = nullptr;
uint8_t* jpg   = nullptr;
size_t jpgLen  = 0;
TaskHandle_t th = nullptr;

camera_config_t makeConfig()
{
    camera_config_t c{};
    c.pin_pwdn = -1; c.pin_reset = -1; c.pin_xclk = -1;
    c.pin_sccb_sda = 12; c.pin_sccb_scl = 11;
    c.pin_d7 = 47; c.pin_d6 = 48; c.pin_d5 = 16; c.pin_d4 = 15;
    c.pin_d3 = 42; c.pin_d2 = 41; c.pin_d1 = 40; c.pin_d0 = 39;
    c.pin_vsync = 46; c.pin_href = 38; c.pin_pclk = 45;
    c.xclk_freq_hz = 20000000;
    c.ledc_timer = LEDC_TIMER_0; c.ledc_channel = LEDC_CHANNEL_0;
    c.pixel_format = PIXFORMAT_RGB565;
    c.frame_size = FRAMESIZE_QVGA;
    c.fb_count = 1;
    c.fb_location = CAMERA_FB_IN_PSRAM;
    c.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    c.sccb_i2c_port = -1;
    return c;
}

// カメラを起動したままにする (撮るたびの起動・停止で 0.6 秒かかっていたため)。
// 内部 I2C はカメラの設定 (起動時) にしか使わないので、起動後は画面のタッチなどに返す
bool keep = false;
std::atomic<bool> dropReq{false};

bool grabKeep()
{
    if (!keep) {
        i2cgate::take(portMAX_DELAY);
        busyF = true;
        M5.In_I2C.release();
        camera_config_t cfg = makeConfig();
        const bool ok = esp_camera_init(&cfg) == ESP_OK;
        M5.In_I2C.begin();
        busyF = false;
        i2cgate::give();
        if (!ok) return false;
        keep = true;
        for (int i = 0; i < 4; i++) { camera_fb_t* f = esp_camera_fb_get(); if (f) esp_camera_fb_return(f); }  // 明るさが落ち着くまで
    }
    camera_fb_t* fb = esp_camera_fb_get();  // 前に撮ってあった古い 1 枚
    if (fb) esp_camera_fb_return(fb);
    fb = esp_camera_fb_get();  // いまの 1 枚
    if (!fb) return false;
    const bool ok = fb->len == 320 * 240 * 2;
    if (ok) memcpy(frame, fb->buf, fb->len);
    esp_camera_fb_return(fb);
    return ok;
}

bool grab()
{
    i2cgate::take(portMAX_DELAY);
    busyF = true;
    M5.In_I2C.release();
    camera_config_t cfg = makeConfig();
    bool ok = false;
    if (esp_camera_init(&cfg) == ESP_OK) {
        camera_fb_t* fb = nullptr;
        for (int i = 0; i < 5; i++) {
            fb = esp_camera_fb_get();
            if (!fb) break;
            if (i < 4) { esp_camera_fb_return(fb); fb = nullptr; }
        }
        if (fb && fb->len == 320 * 240 * 2) {
            memcpy(frame, fb->buf, fb->len);
            ok = true;
        }
        if (fb) esp_camera_fb_return(fb);
        esp_camera_deinit();
    }
    M5.In_I2C.begin();
    busyF = false;
    i2cgate::give();
    return ok;
}

void task(void*)
{
    int fails = 0, keepTried = 0;
    for (;;) {
        if (dropReq.exchange(false) && keep) {
            // カメラを止めたら、内部 I2C を画面側 (タッチ・スピーカーのアンプ・電池) に返し直す
            // (10/16: 返し忘れて、会話中にスピーカーが鳴らず、画面も遅くなった)
            i2cgate::take(portMAX_DELAY);
            esp_camera_deinit();
            M5.In_I2C.release();
            M5.In_I2C.begin();
            i2cgate::give();
            keep = false;
            keepTried = 0;
        }
        if (req.exchange(false) && !have) {
            const uint32_t t0 = millis();
            bool got = keep ? grabKeep() : false;
            if (!got && keepTried < 3) { keepTried++; got = grabKeep(); }
            if (!got) {
                if (keep) {
                    i2cgate::take(portMAX_DELAY);
                    esp_camera_deinit();
                    M5.In_I2C.release();
                    M5.In_I2C.begin();
                    i2cgate::give();
                    keep = false;
                }
                got = grab();
                if (!got) { vTaskDelay(pdMS_TO_TICKS(150)); got = grab(); }
            }
            fails = got ? 0 : fails + 1;
            if (fails >= 6) {
                // カメラが戻らない時は、しばらく休ませる (前は本体を再起動していたが、会話が途切れるのでやめた 10/18)
                if (fails == 6) talk::logf("[cam] stuck, pausing camera\n");
                req = false;
                vTaskDelay(pdMS_TO_TICKS(60000));
                fails = 3;
            }
            if (got) {
                if (jpg) { free(jpg); jpg = nullptr; }
                // 半分の大きさ (160x120) にしてから送る: Wi-Fi で送る量が 1/4 になり、判定が速くなる
                {
                    uint16_t* src = (uint16_t*)frame;
                    uint16_t* dst = (uint16_t*)frame;  // 同じ場所に前から詰める (読む位置の方が常に先)
                    for (int y = 0; y < 120; y++)
                        for (int x = 0; x < 160; x++) {
                            int r = 0, g = 0, b = 0;
                            for (int dy = 0; dy < 2; dy++)
                                for (int dx = 0; dx < 2; dx++) {
                                    const uint16_t v = __builtin_bswap16(src[(y * 2 + dy) * 320 + x * 2 + dx]);
                                    r += (v >> 11) & 31; g += (v >> 5) & 63; b += v & 31;
                                }
                            dst[y * 160 + x] = __builtin_bswap16((uint16_t)(((r / 4) << 11) | ((g / 4) << 5) | (b / 4)));
                        }
                }
                if (fmt2jpg(frame, 160 * 120 * 2, 160, 120, PIXFORMAT_RGB565, 70, &jpg, &jpgLen)) {
                    have = true;
                    static uint32_t nlog = 0;
                    if (nlog++ % 20 == 0) talk::logf("[cam] %s %ums %uB\n", keep ? "keep" : "init", (unsigned)(millis() - t0), (unsigned)jpgLen);
                } else {
                    talk::logf("[cam] jpeg failed\n");
                }
            } else {
                talk::logf("[cam] grab failed\n");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
}  // namespace

void begin()
{
    frame = (uint8_t*)heap_caps_malloc(320 * 240 * 2, MALLOC_CAP_SPIRAM);
    xTaskCreatePinnedToCore(task, "cam", 6144, nullptr, 1, &th, 0);
}
bool busy() { return busyF; }
void request() { req = true; }
bool ready(const uint8_t*& d, size_t& n)
{
    if (!have) return false;
    d = jpg;
    n = jpgLen;
    return true;
}
void release() { have = false; }
void dropKeep() { dropReq = true; }
bool kept() { return keep; }
}  // namespace cam
