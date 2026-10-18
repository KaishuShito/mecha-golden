#include "talk.h"
#include <M5Unified.h>
#include <freertos/stream_buffer.h>
#include <atomic>
#include <cstdarg>
#include <cstring>
#include <math.h>
#include <mutex>

// フレーム: A5 5A <type> <len lo> <len hi> <payload>
// 本体 → Mac: 'M' マイク音声 (PCM16 16kHz mono) / 'L' ログ / 'E' 出来事 (interrupt / pet / wake など)
// Mac → 本体: 'A' 返事の音声 / 'C' 字幕 (先頭 1 バイト 'u'=あなた 'a'=ペット) / 'S' 状態 ('t' 会話開始 'o' 終了 'k' 考え中 'n' 考え終わり) / 'X' 再生を止める
namespace talk {
namespace {

constexpr uint32_t RATE   = 16000;
constexpr size_t MIC_N    = 320;
constexpr size_t PLAY_N   = 1024;
constexpr size_t RING_N   = RATE * 30;
constexpr uint8_t M0 = 0xA5, M1 = 0x5A;

StreamBufferHandle_t logBuf;
std::mutex capMx;
char capUser[200] = "", capAi[240] = "";
uint32_t capUserAt = 0, capAiAt = 0;
std::atomic<bool> act{false}, think{false}, intr{false}, pet{false};
std::atomic<uint8_t> md{0};
volatile float lvl = 0;
std::atomic<int> cmd{-1};

int16_t* ring  = nullptr;
size_t rHead   = 0, rTail = 0;
int16_t mic[4][MIC_N];
int16_t play[3][PLAY_N];
uint32_t lastHost = 0, lastAudioRx = 0;

const void* snapB = nullptr;
size_t snapL      = 0;
int snapW = 0, snapH = 0;
const char* (*stName)() = nullptr;

size_t ringCount() { return (rHead + RING_N - rTail) % RING_N; }

void ringPush(const int16_t* s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const size_t nx = (rHead + 1) % RING_N;
        if (nx == rTail) return;
        ring[rHead] = s[i];
        rHead       = nx;
    }
}

void sendFrame(uint8_t type, const void* data, size_t len)
{
    const uint8_t h[5] = {M0, M1, type, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8)};
    Serial.write(h, 5);
    if (len) Serial.write((const uint8_t*)data, len);
}

void setActive(bool on)
{
    if (act.load() == on) return;
    act = on;
    Serial.setTxTimeoutMs(on ? 50 : 0);
    if (!on) {
        think = false;
        rTail = rHead;
    }
}

void snapshot()
{
    if (!snapB) return;
    Serial.setTxTimeoutMs(500);
    Serial.printf("SNAP %d %d %s\n", snapW, snapH, stName ? stName() : "-");
    const uint8_t* b = (const uint8_t*)snapB;
    for (size_t o = 0; o < snapL; o += 1024) Serial.write(b + o, (snapL - o) < 1024 ? (snapL - o) : 1024);
    Serial.flush();
    Serial.print("\nENDSNAP\n");
    Serial.setTxTimeoutMs(act ? 50 : 0);
}

void handleFrame(uint8_t type, const uint8_t* p, size_t n)
{
    lastHost = millis();
    switch (type) {
        case 'A':
            if (act) {
                ringPush((const int16_t*)p, n / 2);
                lastAudioRx = millis();
            }
            break;
        case 'C': {
            if (n < 1) break;
            std::lock_guard<std::mutex> lk(capMx);
            char* dst     = p[0] == 'u' ? capUser : capAi;
            const size_t cap = p[0] == 'u' ? sizeof(capUser) : sizeof(capAi);
            const size_t len = (n - 1) < (cap - 1) ? (n - 1) : (cap - 1);
            memcpy(dst, p + 1, len);
            dst[len] = 0;
            (p[0] == 'u' ? capUserAt : capAiAt) = millis();
            break;
        }
        case 'S':
            for (size_t i = 0; i < n; i++) {
                if (p[i] == 't') setActive(true);
                if (p[i] == 'o') setActive(false);
                if (p[i] == 'k') think = true;
                if (p[i] == 'n') think = false;
            }
            break;
        case 'X':
            rTail = rHead;
            M5.Speaker.stop();
            break;
        default:
            break;
    }
}

enum RxSt : uint8_t { RX_IDLE, RX_M1, RX_T, RX_L0, RX_L1, RX_PAY };
RxSt rxs = RX_IDLE;
uint8_t ptype;
uint16_t plen, pgot;
uint8_t pbuf[4096];

void pumpRx()
{
    uint8_t buf[512];
    int avail;
    while ((avail = Serial.available()) > 0) {
        const int n = Serial.read(buf, avail < (int)sizeof(buf) ? avail : sizeof(buf));
        for (int i = 0; i < n; i++) {
            const uint8_t b = buf[i];
            switch (rxs) {
                case RX_IDLE:
                    if (b == M0) rxs = RX_M1;
                    else if (b == 'S' && !act) snapshot();
                    else if ((b >= '0' && b <= '9') || b == 'p') cmd = b;
                    break;
                case RX_M1: rxs = (b == M1) ? RX_T : RX_IDLE; break;
                case RX_T: ptype = b; rxs = RX_L0; break;
                case RX_L0: plen = b; rxs = RX_L1; break;
                case RX_L1:
                    plen |= (uint16_t)b << 8;
                    pgot = 0;
                    if (plen > sizeof(pbuf)) rxs = RX_IDLE;
                    else if (plen == 0) { handleFrame(ptype, pbuf, 0); rxs = RX_IDLE; }
                    else rxs = RX_PAY;
                    break;
                case RX_PAY:
                    pbuf[pgot++] = b;
                    if (pgot == plen) { handleFrame(ptype, pbuf, plen); rxs = RX_IDLE; }
                    break;
            }
        }
    }
}

void flushLogs()
{
    char buf[256];
    size_t n;
    while ((n = xStreamBufferReceive(logBuf, buf, sizeof(buf), 0)) > 0) {
        if (act) sendFrame('L', buf, n);
        else Serial.write((const uint8_t*)buf, n);
    }
}

float rms(const int16_t* s, size_t n)
{
    if (!n) return 0;
    double acc = 0;
    for (size_t i = 0; i < n; i++) acc += (double)s[i] * s[i];
    return sqrt(acc / n);
}

void task(void*)
{
    bool micOn = false, spkOn = false;
    int micIdx = 0, micQueued = 0, playIdx = 0;
    uint32_t quietSince = 0, lastMicLog = 0;
    float micPeak = 0;

    auto micStop = [&] {
        if (micOn) { M5.Mic.end(); micOn = false; micQueued = 0; }
    };
    auto spkStop = [&] {
        if (spkOn) { M5.Speaker.end(); spkOn = false; }
        lvl = 0;
    };

    for (;;) {
        pumpRx();
        flushLogs();
        const uint32_t now = millis();
        if (act && now - lastHost > 4000) {
            setActive(false);
            logf("[talk] host timeout\n");
        }
        if (!act) {
            if (pet.exchange(false)) sendFrame('E', "wake", 4);
            micStop();
            spkStop();
            md = (uint8_t)Mode::Listen;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (pet.exchange(false)) sendFrame('E', "pet", 3);
        if (intr.exchange(false)) {
            rTail = rHead;
            if (spkOn) M5.Speaker.stop();
            sendFrame('E', "interrupt", 9);
        }

        const size_t avail = ringCount();
        if ((Mode)md.load() == Mode::Listen) {
            if (avail >= RATE * 3 / 10 || (avail > 0 && now - lastAudioRx > 150)) {
                micStop();
                M5.Speaker.begin();
                M5.Speaker.setVolume(200);
                spkOn      = true;
                quietSince = 0;
                md         = (uint8_t)Mode::Speak;
                sendFrame('E', "speak", 5);
                continue;
            }
            if (!micOn) {
                micOn = M5.Mic.begin();
                micQueued = 0;
                if (!micOn) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
            }
            if (M5.Mic.record(mic[micIdx], MIC_N, RATE)) {
                micQueued++;
                if (micQueued >= 3) {
                    const int16_t* done = mic[(micIdx + 2) % 4];
                    sendFrame('M', done, MIC_N * 2);
                    const float r = rms(done, MIC_N);
                    if (r > micPeak) micPeak = r;
                }
                micIdx = (micIdx + 1) % 4;
            }
            if (now - lastMicLog > 3000) {
                logf("[mic] peak_rms=%d\n", (int)micPeak);
                micPeak    = 0;
                lastMicLog = now;
            }
        } else {
            size_t left = ringCount();
            while (M5.Speaker.isPlaying(0) < 2 && left > 0) {
                size_t n = left < PLAY_N ? left : PLAY_N;
                if (n < PLAY_N && millis() - lastAudioRx < 120) break;
                for (size_t i = 0; i < n; i++) {
                    play[playIdx][i] = ring[rTail];
                    rTail            = (rTail + 1) % RING_N;
                }
                const float r = rms(play[playIdx], n);
                lvl           = r / 4000.0f > 1 ? 1 : r / 4000.0f;
                M5.Speaker.playRaw(play[playIdx], n, RATE, false, 1, 0);
                playIdx = (playIdx + 1) % 3;
                left    = ringCount();
            }
            if (left == 0 && M5.Speaker.isPlaying(0) == 0) {
                if (!quietSince) quietSince = now;
                lvl = 0;
                if (now - quietSince > 350) {
                    spkStop();
                    md = (uint8_t)Mode::Listen;
                    sendFrame('E', "listen", 6);
                }
            } else {
                quietSince = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(4));
        }
    }
}

}  // namespace

void begin(const void* sb, size_t sl, int w, int h, const char* (*sn)())
{
    snapB  = sb;
    snapL  = sl;
    snapW  = w;
    snapH  = h;
    stName = sn;
    ring   = (int16_t*)heap_caps_malloc(RING_N * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    logBuf = xStreamBufferCreate(4096, 1);
    xTaskCreatePinnedToCore(task, "talk", 8192, nullptr, 3, nullptr, 0);
}

void logf(const char* fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    static std::mutex logMx;
    std::lock_guard<std::mutex> lk(logMx);
    if (n > 0 && logBuf) xStreamBufferSend(logBuf, buf, n < (int)sizeof(buf) ? n : sizeof(buf) - 1, 0);
}

bool active() { return act; }
Mode mode() { return (Mode)md.load(); }
bool thinking() { return think; }
float level() { return lvl; }
void interrupt() { intr = true; }
void petted() { pet = true; }
int takeCommand() { return cmd.exchange(-1); }

void captions(char* user, size_t ul, char* ai, size_t al, uint32_t& ua, uint32_t& aa)
{
    std::lock_guard<std::mutex> lk(capMx);
    strlcpy(user, capUser, ul);
    strlcpy(ai, capAi, al);
    ua = capUserAt;
    aa = capAiAt;
}

}  // namespace talk
