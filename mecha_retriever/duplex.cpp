#include "duplex.h"
#include <M5Unified.h>
#include <driver/i2s_std.h>
#include <atomic>
#include <math.h>
#include "esp_afe_aec.h"
#include "i2cgate.h"
#include "talk.h"

namespace duplex {
namespace {

constexpr uint32_t RATE   = 16000;
constexpr size_t PLAY_N   = RATE * 30;   // 鳴らす音のため置き (30 秒)
constexpr size_t MIC_N    = RATE * 4;    // エコーを消したマイクの音のため置き
constexpr size_t HIST_N   = 8192;        // スピーカーに送った音の履歴 (エコーの手本)
constexpr float MIC_GAIN  = 96.0f;       // マイクのデジタル増幅 (アナログを 18dB 下げた分を、エコーを消した後で戻す)

i2s_chan_handle_t tx = nullptr, rx = nullptr;
afe_aec_handle_t* aec = nullptr;
int chunk = 0;
std::atomic<bool> run{false};
std::atomic<uint8_t> vol{200};
volatile float lvl = 0, micRms = 0, rawRms = 0, outRms = 0;
volatile int refDelay = 0;  // 手本をどれだけ遅らせるか (サンプル)。自動で合わせる
std::atomic<bool> barge{false};
volatile float resRatio = 0.05f;  // エコーの残り ÷ スピーカーに送った音 (しゃべっている間に学習する)
volatile uint32_t bargeCount = 0, duckChunks = 0;
volatile uint32_t loops = 0, aecUs = 0, loopUs = 0, rxShort = 0, peakRaw = 0, pushed = 0, popped = 0;

struct Ring {
    int16_t* b = nullptr;
    size_t cap = 0;
    volatile size_t h = 0, t = 0;
    void init(size_t n) { b = (int16_t*)heap_caps_malloc(n * 2, MALLOC_CAP_SPIRAM); cap = n; }
    size_t count() const { return (h + cap - t) % cap; }
    size_t push(const int16_t* s, size_t n)
    {
        size_t k = 0;
        for (; k < n; k++) {
            const size_t nx = (h + 1) % cap;
            if (nx == t) break;
            b[h] = s[k];
            h    = nx;
        }
        return k;
    }
    size_t pop(int16_t* o, size_t n)
    {
        size_t k = 0;
        while (k < n && t != h) { o[k++] = b[t]; t = (t + 1) % cap; }
        return k;
    }
    void clear() { t = h; }
};
Ring playR, micR;
int16_t* hist = nullptr;  // スピーカーに送った音 (音量をかけた後)
size_t histPos = 0;

void es7210(uint8_t r, uint8_t v) { M5.In_I2C.writeRegister(0x40, r, &v, 1, 400000); }
void aw88298(uint8_t r, uint16_t v)
{
    v = __builtin_bswap16(v);
    M5.In_I2C.writeRegister(0x36, r, (const uint8_t*)&v, 2, 400000);
}

// マイク (ES7210) とスピーカーのアンプ (AW88298) の設定。M5Unified の CoreS3 用の設定と同じ値
void codecInit()
{
    i2cgate::take(portMAX_DELAY);
    es7210(0x00, 0xFF);
    static const uint8_t mic[][2] = {
        {0x00, 0x41}, {0x01, 0x1f}, {0x06, 0x00}, {0x07, 0x20}, {0x08, 0x10}, {0x09, 0x30}, {0x0A, 0x30}, {0x20, 0x0a},
        {0x21, 0x2a}, {0x22, 0x0a}, {0x23, 0x2a}, {0x02, 0xC1}, {0x04, 0x01}, {0x05, 0x00}, {0x11, 0x60}, {0x40, 0x42},
        // マイクの増幅は 0x1B (33dB) → 0x15 (15dB) に下げる。スピーカーがすぐ横で、自分の声で入力が割れたため (10/18)
        {0x41, 0x70}, {0x42, 0x70}, {0x43, 0x10}, {0x44, 0x10}, {0x45, 0x00}, {0x46, 0x00}, {0x47, 0x00}, {0x48, 0x00},
        {0x49, 0x00}, {0x4A, 0x00}, {0x4B, 0x00}, {0x4C, 0xFF}, {0x01, 0x14},
    };
    for (auto& d : mic) es7210(d[0], d[1]);
    M5.In_I2C.bitOn(0x58, 0x02, 0b00000100, 400000);  // スピーカーのアンプの電源 (AW9523)
    aw88298(0x61, 0x0673);                            // boost off
    aw88298(0x04, 0x4040);                            // I2S 有効、アンプ on
    aw88298(0x05, 0x0008);
    aw88298(0x06, 0x14C3);                            // 16kHz、BCK 32fs
    aw88298(0x0C, 0x0064);                            // 音量はソフトでかける
    i2cgate::give();
}

// スピーカーに送った音と、マイクの音のずれ (エコーが戻ってくるまでの遅れ) を、相互相関で探す (整数で計算して軽くする)
// mic[t] ≒ ref[t - d] となる d を探す。両方の履歴は同じ時刻にそろえてためている (1 塊ごとに同じ数ずつ足す)
int16_t* micHist = nullptr;
size_t micHistPos = 0;
int16_t* refCopy = nullptr;
int16_t* micCopy = nullptr;
void estimateDelay()
{
    constexpr int W = 2048, MAXD = 4096;
    if (!refCopy) {
        refCopy = (int16_t*)heap_caps_malloc(HIST_N * 2, MALLOC_CAP_SPIRAM);
        micCopy = (int16_t*)heap_caps_malloc(HIST_N * 2, MALLOC_CAP_SPIRAM);
    }
    for (size_t i = 0; i < HIST_N; i++) {  // 古い → 新しい
        refCopy[i] = hist[(histPos + i) % HIST_N];
        micCopy[i] = micHist[(micHistPos + i) % HIST_N];
    }
    const int16_t* m = micCopy + HIST_N - W;  // マイクの直近 W サンプル
    int64_t e = 0;
    for (int j = 0; j < W; j += 2) e += (int32_t)m[j] * m[j];
    if (e < (int64_t)(W / 2) * 30 * 30) { talk::logf("[duplex] echo search skipped (quiet mic)\n"); return; }
    float best = 0;
    int bestD  = refDelay;
    for (int d = 0; d <= MAXD; d += 4) {
        const int16_t* r = refCopy + HIST_N - W - d;
        int64_t c = 0, rr = 0;
        for (int j = 0; j < W; j += 2) { c += (int32_t)m[j] * r[j]; rr += (int32_t)r[j] * r[j]; }
        if (rr <= 0) continue;
        const float score = (float)c / sqrtf((float)rr * (float)e);
        if (score > best) { best = score; bestD = d; }
    }
    talk::logf("[duplex] echo search best=%d score=%.2f (now %d)\n", bestD, best, refDelay);
    if (best > 0.25f) refDelay = (refDelay == 768 ? bestD : (refDelay * 3 + bestD) / 4);
}

void task(void*)
{
    int16_t* rxb  = (int16_t*)heap_caps_malloc(chunk * 4, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    int16_t* txb  = (int16_t*)heap_caps_malloc(chunk * 4, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    int16_t* in   = (int16_t*)heap_caps_aligned_calloc(16, chunk * 2, 2, MALLOC_CAP_SPIRAM);
    int16_t* out  = (int16_t*)heap_caps_aligned_calloc(16, chunk, 2, MALLOC_CAP_SPIRAM);
    int16_t* mono = (int16_t*)heap_caps_malloc(chunk * 2, MALLOC_CAP_SPIRAM);
    int16_t* micRaw = (int16_t*)heap_caps_malloc(chunk * 2, MALLOC_CAP_SPIRAM);
    uint32_t lastEst = 0;
    for (;;) {
        size_t got = 0;
        if (i2s_channel_read(rx, rxb, chunk * 4, &got, pdMS_TO_TICKS(200)) != ESP_OK || got < (size_t)chunk * 4) { rxShort++; continue; }
        // マイク: 左右 (MIC1/MIC2) の平均
        const uint32_t t0 = micros();
        int64_t acc = 0;
        for (int i = 0; i < chunk; i++) {
            micRaw[i] = (int16_t)(((int32_t)rxb[2 * i] + rxb[2 * i + 1]) / 2);
            acc += (int32_t)micRaw[i] * micRaw[i];
            const uint32_t a = micRaw[i] < 0 ? -micRaw[i] : micRaw[i];
            if (a > peakRaw) peakRaw = a;
        }
        rawRms = sqrtf((float)(acc / chunk));
        static uint32_t dbg = 0;
        if (dbg++ % 300 == 0) talk::logf("[duplex] rx sample %d %d %d %d\n", rxb[0], rxb[1], rxb[100], rxb[101]);
        // スピーカー: ため置きから 1 塊取り出して、音量をかけて送る
        const size_t k = playR.pop(mono, chunk);
        for (size_t i = k; i < (size_t)chunk; i++) mono[i] = 0;
        const float g = vol / 255.0f;
        int64_t pa = 0;
        const int gi = (int)(g * 256);
        for (int i = 0; i < chunk; i++) {
            const int16_t v = (int16_t)((mono[i] * gi) >> 8);
            txb[2 * i] = txb[2 * i + 1] = v;
            hist[histPos] = v;
            histPos       = (histPos + 1) % HIST_N;
            pa += (int32_t)v * v;
        }
        const float pr = sqrtf((float)(pa / chunk));
        lvl = pr / 4000.0f > 1 ? 1 : pr / 4000.0f;
        size_t w = 0;
        i2s_channel_write(tx, txb, chunk * 4, &w, pdMS_TO_TICKS(200));
        // エコー除去: [マイク, 手本] を交互に並べて渡す。手本は refDelay だけ前に送った音
        for (int i = 0; i < chunk; i++) {
            const size_t hi = (histPos + HIST_N - chunk - refDelay + i) % HIST_N;
            in[2 * i]     = micRaw[i];
            in[2 * i + 1] = hist[hi];
        }
        for (int i = 0; i < chunk; i++) { micHist[micHistPos] = micRaw[i]; micHistPos = (micHistPos + 1) % HIST_N; }
        static uint32_t loudSince = 0;
        if (pr > 300) { if (!loudSince) loudSince = millis(); } else loudSince = 0;
        if (loudSince && millis() - loudSince > 300 && millis() - lastEst > 4000) {  // 鳴らし始めて 0.3 秒たってから (エコーが戻ってから) 探す
            lastEst = millis();
            estimateDelay();
        }
        const uint32_t ta = micros();
        afe_aec_process(aec, in, out);
        aecUs = (aecUs * 7 + (micros() - ta)) / 8;
        // ---- しゃべっている間のエコーの残り対策 (スピーカーホンと同じ考え方) ----
        // 残りの大きさ ≒ resRatio × (いまの手本の大きさ)。それをはっきり超えたら「人が話している」とみなす
        int64_t ra = 0, ob = 0;
        for (int i = 0; i < chunk; i++) { ra += (int32_t)in[2 * i + 1] * in[2 * i + 1]; ob += (int32_t)out[i] * out[i]; }
        const float refR = sqrtf((float)(ra / chunk)), outR = sqrtf((float)(ob / chunk));
        static int nearRun = 0;
        static float refEnv = 0;  // 手本の大きさの包絡 (鳴らし終わってもしばらく残りが出るので、ゆっくり下げる)
        refEnv = refR > refEnv ? refR : refEnv * 0.88f;
        const bool robotTalking = refEnv > 60;
        float scale = 1.0f;
        if (robotTalking) {
            // 残りの「上側」の比率を追う: 上がる時は速く、下がる時はゆっくり
            const float x = outR / refEnv;
            const float expect = resRatio * refEnv + 8;
            const bool nearTalk = outR > expect * 3 && outR > 60;
            if (!nearTalk) resRatio = x > resRatio ? resRatio * 0.8f + x * 0.2f : resRatio * 0.995f;
            if (resRatio > 0.6f) resRatio = 0.6f;
            if (resRatio < 0.01f) resRatio = 0.01f;
            nearRun = nearTalk ? nearRun + 1 : 0;
            if (nearRun >= 5 && !barge) {  // 0.16 秒続いたら、さえぎった
                barge = true; bargeCount++;
                talk::logf("[duplex] near ref=%.0f env=%.0f out=%.0f expect=%.0f ratio=%.3f\n", refR, refEnv, outR, expect, resRatio);
            }
            if (!nearTalk) { scale = 0.1f; duckChunks++; }  // 人が話していなければ絞る
        } else {
            nearRun = 0;
        }
        int64_t oa = 0;
        for (int i = 0; i < chunk; i++) {
            int32_t v = (int32_t)(out[i] * MIC_GAIN * scale);
            v         = v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
            out[i]    = (int16_t)v;
            oa += v * v;
        }
        outRms = sqrtf((float)(oa / chunk));
        loopUs = (loopUs * 7 + (micros() - t0)) / 8;
        micRms = outRms;
        pushed += micR.push(out, chunk);
        loops++;
    }
}

}  // namespace

bool begin()
{
    playR.init(PLAY_N);
    micR.init(MIC_N);
    hist    = (int16_t*)heap_caps_calloc(HIST_N, 2, MALLOC_CAP_SPIRAM);
    micHist = (int16_t*)heap_caps_calloc(HIST_N, 2, MALLOC_CAP_SPIRAM);
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    cc.dma_desc_num  = 4;
    cc.dma_frame_num = 256;
    cc.auto_clear    = true;
    if (i2s_new_channel(&cc, &tx, &rx) != ESP_OK) { talk::logf("[duplex] i2s_new_channel failed\n"); return false; }
    i2s_std_config_t sc{};
    sc.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(RATE);
    sc.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    sc.gpio_cfg.mclk = GPIO_NUM_0;
    sc.gpio_cfg.bclk = GPIO_NUM_34;
    sc.gpio_cfg.ws   = GPIO_NUM_33;
    sc.gpio_cfg.dout = GPIO_NUM_13;
    sc.gpio_cfg.din  = GPIO_NUM_14;
    if (i2s_channel_init_std_mode(tx, &sc) != ESP_OK || i2s_channel_init_std_mode(rx, &sc) != ESP_OK) {
        talk::logf("[duplex] i2s init failed\n");
        return false;
    }
    i2s_channel_enable(tx);
    i2s_channel_enable(rx);
    codecInit();
    aec = afe_aec_create("MR", 4, AFE_TYPE_FD, AFE_MODE_LOW_COST);  // FD = 全二重向け (残ったエコーを非線形処理でさらに消す)
    if (!aec) { talk::logf("[duplex] aec create failed\n"); return false; }
    aec_set_nlp_level(aec->handle, AEC_NLP_LEVEL_VERYAGGR);  // 残ったエコーをいちばん強く消す (xiaozhi も同じ設定)
    chunk    = afe_aec_get_chunksize(aec);
    refDelay = 256 * 3;  // 初めの見当: 送りの DMA にたまる分 (自動で合わせ直す)
    run      = true;
    xTaskCreatePinnedToCore(task, "duplex", 6144, nullptr, 5, nullptr, 1);  // 画面の処理と同じコアだが優先する (コア 0 は Wi-Fi・会話・カメラで混む)
    talk::logf("[duplex] started chunk=%d heap=%u\n", chunk, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return true;
}

bool running() { return run; }
size_t play(const int16_t* s, size_t n) { return playR.push(s, n); }
size_t queued() { return playR.count(); }
void flush() { playR.clear(); }
void setVolume(uint8_t v) { vol = v; }
size_t readMic(int16_t* out, size_t max)
{
    if (micR.count() < max) return 0;  // 1 フレームぶんそろってから取り出す (途中で取り出すと半端が捨てられる)
    const size_t k = micR.pop(out, max);
    popped += k;
    return k;
}
float level() { return lvl; }
float micLevel() { return micRms; }
bool takeBarge() { return barge.exchange(false); }
void debugRegs()
{
    i2cgate::take(portMAX_DELAY);
    uint8_t r01 = 0, r43 = 0, r11 = 0, r4b = 0, r40 = 0;
    M5.In_I2C.readRegister(0x40, 0x01, &r01, 1, 400000);
    M5.In_I2C.readRegister(0x40, 0x43, &r43, 1, 400000);
    M5.In_I2C.readRegister(0x40, 0x11, &r11, 1, 400000);
    M5.In_I2C.readRegister(0x40, 0x4B, &r4b, 1, 400000);
    M5.In_I2C.readRegister(0x40, 0x40, &r40, 1, 400000);
    i2cgate::give();
    talk::logf("[duplex] es7210 r01=%02x r11=%02x r40=%02x r43=%02x r4b=%02x\n", r01, r11, r40, r43, r4b);
}

void stats(char* buf, size_t n)
{
    snprintf(buf, n, "loops=%u raw=%d aec_out=%d delay=%d queued=%u aec=%uus loop=%uus rxShort=%u peakRaw=%u micQ=%u pushed=%u popped=%u res=%.3f barge=%u duck=%u", (unsigned)loops, (int)rawRms, (int)outRms, refDelay, (unsigned)playR.count(), (unsigned)aecUs, (unsigned)loopUs, (unsigned)rxShort, (unsigned)peakRaw, (unsigned)micR.count(), (unsigned)pushed, (unsigned)popped, resRatio, (unsigned)bargeCount, (unsigned)duckChunks);
    peakRaw = 0;
}

}  // namespace duplex
