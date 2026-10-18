#include "talk.h"
#include "camera.h"
#include "sounds.h"
#include "brain.h"
#include "i2cgate.h"
#include "duplex.h"
#include <M5Unified.h>
#include <freertos/stream_buffer.h>
#include <atomic>
#include <cstdarg>
#include <cstring>
#include <math.h>
#include <mutex>

// フレーム: A5 5A <type> <len lo> <len hi> <payload>
// 本体 → Mac: 'M' マイク音声 (PCM16 16kHz mono) / 'L' ログ / 'E' 出来事 (interrupt など)
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
uint8_t moodV = 0;
char moodText[120] = "";
uint32_t moodAt = 0;
std::atomic<bool> act{false}, think{false}, intr{false}, pet{false};
std::atomic<uint8_t> md{0};
volatile float lvl = 0;
std::atomic<int> cmd{-1};
std::atomic<bool> muted{false};
std::atomic<uint8_t> duplexVol{200};  // 試験用: USB から 'V' + 数字 1 文字 (1〜9 → 音量 x28) で変える  // 試験用: USB から 'M' で音を消す / 'N' で戻す
std::atomic<bool> snapReq{false}, snapReady{false};
std::atomic<int> soundReq{-1}, soundAnim{0}, fallbackSnd{-1};
std::atomic<uint32_t> touchAt{0};
char touchBuf[48];
std::atomic<bool> touchPending{false}, endReq{false};

int16_t* ring  = nullptr;
size_t rHead   = 0, rTail = 0;
int16_t mic[4][MIC_N];
int16_t play[3][PLAY_N];
uint32_t lastHost = 0, lastAudioRx = 0, lastSerialFrame = 0;

// 本体だけで動く時 (brain.cpp) の、メモリ内の行き来。Mac とのシリアルと同じフレームを流す
std::atomic<bool> localLink{false};
StreamBufferHandle_t toTalk = nullptr, fromTalk = nullptr;
std::mutex toTalkMx;

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
    if (localLink && fromTalk && (type == 'M' || type == 'E' || type == 'V')) {
        if (xStreamBufferSpacesAvailable(fromTalk) >= len + 5) {
            xStreamBufferSend(fromTalk, h, 5, 0);
            if (len) xStreamBufferSend(fromTalk, data, len, 0);
        }
        return;
    }
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
                if (p[i] == 'v') cam::request();
                if (p[i] == 'w') cmd = '1';
                if (p[i] == 'h') cmd = '8';  // お手
                if (p[i] == 'U' || p[i] == 'D') cmd = p[i];  // 首を上げる / 戻す
            }
            break;
        case 'D': {
            if (n < 1) break;
            std::lock_guard<std::mutex> lk(capMx);
            moodV = p[0];
            const size_t len = (n - 1) < (sizeof(moodText) - 1) ? (n - 1) : (sizeof(moodText) - 1);
            memcpy(moodText, p + 1, len);
            moodText[len] = 0;
            moodAt = millis();
            break;
        }
        case 'P':
            if (n >= 1 && p[0] < SOUND_COUNT) {
                fallbackSnd = -1;
                soundReq    = p[0];
            }
            break;
        case 'K': {  // 設定の書き込み (Wi-Fi・API キー): "名前\0値"。値はログに出さない
            const uint8_t* z = (const uint8_t*)memchr(p, 0, n);
            if (z && z > p) {
                char name[32], value[256];
                const size_t nl = z - p, vl = n - nl - 1;
                if (nl < sizeof(name) && vl < sizeof(value)) {
                    memcpy(name, p, nl); name[nl] = 0;
                    memcpy(value, z + 1, vl); value[vl] = 0;
                    brain::provision(name, value);
                    memset(value, 0, sizeof(value));
                }
            }
            break;
        }
        case 'X':
            rTail = rHead;
            M5.Speaker.stop();
            break;
        default:
            break;
    }
}

enum RxSt : uint8_t { RX_IDLE, RX_M1, RX_T, RX_L0, RX_L1, RX_PAY };
struct Parser {
    RxSt st = RX_IDLE;
    uint8_t type;
    uint16_t len, got;
    uint8_t buf[16384];
};
Parser* serialP = nullptr;
Parser* localP  = nullptr;

void feed(Parser& P, const uint8_t* data, size_t n, bool legacy)
{
    for (size_t i = 0; i < n; i++) {
        const uint8_t b = data[i];
        switch (P.st) {
            case RX_IDLE:
                if (b == M0) P.st = RX_M1;
                else if (legacy && b == 'S' && !act) snapReq = true;
                else if (legacy && b == 'R' && !act) esp_restart();
                else if (legacy && b == 'W') pet = true;
                else if (legacy && b == 'M') { muted = true; logf("[talk] muted\n"); }
                else if (legacy && b >= 'a' && b <= 'i') { duplexVol = (uint8_t)((b - 'a' + 1) * 28); logf("[talk] volume %d\n", (int)duplexVol); }
                else if (legacy && b == 'N') { muted = false; logf("[talk] unmuted\n"); }
                else if (legacy && (b == 'U' || b == 'D')) cmd = b;  // 試験用: 首  // 試験用: 頭の長押しと同じ (会話を始める)
                else if (legacy && ((b >= '0' && b <= '9') || b == 'p')) cmd = b;
                break;
            case RX_M1: P.st = (b == M1) ? RX_T : RX_IDLE; break;
            case RX_T: P.type = b; P.st = RX_L0; break;
            case RX_L0: P.len = b; P.st = RX_L1; break;
            case RX_L1:
                P.len |= (uint16_t)b << 8;
                P.got = 0;
                if (P.len > sizeof(P.buf)) P.st = RX_IDLE;
                else if (P.len == 0) { if (legacy) lastSerialFrame = millis(); handleFrame(P.type, P.buf, 0); P.st = RX_IDLE; }
                else P.st = RX_PAY;
                break;
            case RX_PAY:
                P.buf[P.got++] = b;
                if (P.got == P.len) { if (legacy) lastSerialFrame = millis(); handleFrame(P.type, P.buf, P.len); P.st = RX_IDLE; }
                break;
        }
    }
}

void pumpRx()
{
    uint8_t buf[512];
    int avail;
    while ((avail = Serial.available()) > 0) {
        const int n = Serial.read(buf, avail < (int)sizeof(buf) ? avail : sizeof(buf));
        if (n > 0) feed(*serialP, buf, n, true);
    }
    if (toTalk) {
        size_t n;
        while ((n = xStreamBufferReceive(toTalk, buf, sizeof(buf), 0)) > 0) feed(*localP, buf, n, false);
    }
}

void flushLogs()
{
    char buf[256];
    size_t n;
    while ((n = xStreamBufferReceive(logBuf, buf, sizeof(buf), 0)) > 0) {
        if (act && !localLink) sendFrame('L', buf, n);
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

    auto waitCam = [] {};  // 順番待ちは i2cgate で行う
    auto micStop = [&] {
        if (micOn) { waitCam(); { i2cgate::take(portMAX_DELAY); M5.Mic.end(); i2cgate::give(); } micOn = false; micQueued = 0; }
    };
    auto spkStop = [&] {
        if (spkOn) { waitCam(); { i2cgate::take(portMAX_DELAY); M5.Speaker.end(); i2cgate::give(); } spkOn = false; }
        lvl = 0;
    };
    // 鳴き声を鳴らす (会話の返事を鳴らしている最中は鳴らさない)。鳴らす間はマイクを止める
    auto playLocal = [&](int id) {
        if (id < 0 || id >= SOUND_COUNT) return;
        if (duplex::running()) {  // しゃべりながら聞く方式: 鳴き声もため置きに入れるだけ (マイクは止めない)
            static int lastVarD[SOUND_COUNT] = {};
            int v = random(SOUND_VARIANTS);
            if (v == lastVarD[id]) v = (v + 1 + random(SOUND_VARIANTS - 1)) % SOUND_VARIANTS;
            lastVarD[id] = v;
            duplex::play(SOUNDS[id].clips[v].data, SOUNDS[id].clips[v].len);
            soundAnim = id + 1;
            logf("[sound] %s_%d\n", SOUNDS[id].name, v + 1);
            return;
        }
        if (spkOn) return;
        micStop();
        waitCam();
        { i2cgate::take(portMAX_DELAY); M5.Speaker.begin(); i2cgate::give(); }
        M5.Speaker.setVolume(muted ? 0 : 200);  //
        soundAnim = id + 1;
        static int lastVar[SOUND_COUNT] = {};
        int v = random(SOUND_VARIANTS);
        if (v == lastVar[id]) v = (v + 1 + random(SOUND_VARIANTS - 1)) % SOUND_VARIANTS;  // 同じ声が続かないように
        lastVar[id] = v;
        const SoundClip& clip = SOUNDS[id].clips[v];
        M5.Speaker.playRaw(clip.data, clip.len, RATE, false, 1, 0);
        while (M5.Speaker.isPlaying(0)) { pumpRx(); vTaskDelay(pdMS_TO_TICKS(5)); }
        waitCam();
        { i2cgate::take(portMAX_DELAY); M5.Speaker.end(); i2cgate::give(); }
        logf("[sound] %s_%d\n", SOUNDS[id].name, v + 1);
    };
    auto handleTouch = [&] {
        if (touchPending.exchange(false)) {
            const size_t n = strnlen(touchBuf, sizeof(touchBuf));
            sendFrame('E', touchBuf, n);
        }
        if (endReq.exchange(false)) sendFrame('E', "end", 3);
        const int fb = fallbackSnd.load();
        if (fb >= 0 && millis() - touchAt > 700) {  // Mac から返事がなければ、本体だけで鳴らす
            fallbackSnd = -1;
            soundReq    = fb;
        }
        const int sr = soundReq.exchange(-1);
        if (sr >= 0) playLocal(sr);
    };

    for (;;) {
        pumpRx();
        flushLogs();
        if (snapReady.exchange(false)) snapshot();
        {
            const uint8_t* jd;
            size_t jn;
            if (cam::ready(jd, jn)) {
                if (jn < 65000) {
                    Serial.setTxTimeoutMs(500);
                    sendFrame('V', jd, jn);
                    Serial.setTxTimeoutMs(act ? 50 : 0);
                }
                cam::release();
            }
        }
        const uint32_t now = millis();
        if (act && now - lastHost > 4000) {
            setActive(false);
            logf("[talk] host timeout\n");
        }
        handleTouch();
        if (duplex::running()) {
            // ---- しゃべりながら聞く方式 (マイクを止めない。エコーは本体で消す) ----
            duplex::setVolume(muted ? (uint8_t)0 : duplexVol.load());
            static int16_t mbuf[MIC_N];
            static uint32_t lastStat = 0;
            if (!act) {
                if (pet.exchange(false)) sendFrame('E', "wake", 4);
                while (duplex::readMic(mbuf, MIC_N) == MIC_N) {}  // 待機中は捨てる
                md = (uint8_t)Mode::Listen;
                lvl = duplex::level();
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            if (pet.exchange(false)) sendFrame('E', "pet", 3);
            if (intr.exchange(false)) {
                rTail = rHead;
                duplex::flush();
                sendFrame('E', "interrupt", 9);
            }
            if (duplex::takeBarge() && (Mode)md.load() == Mode::Speak) {  // 声でさえぎられた: 返事をすぐ止めて Mac に知らせる
                rTail = rHead;
                duplex::flush();
                sendFrame('E', "barge", 5);
                logf("[duplex] barge-in\n");
            }
            // マイクの音はいつも送る (返事を鳴らしている間も。だから声でさえぎれる)
            static uint32_t micSent = 0;
            while (duplex::readMic(mbuf, MIC_N) == MIC_N) { sendFrame('M', mbuf, MIC_N * 2); micSent += MIC_N; }
            // 返事の音: 少しためてから鳴らす (USB は揺れが小さいので 0.1 秒。Wi-Fi の時は多め)
            const size_t avail = ringCount();
            const size_t pre   = localLink ? RATE * 7 / 10 : RATE / 10;
            const bool speakingNow = (Mode)md.load() == Mode::Speak;
            if (avail > 0 && (speakingNow || avail >= pre || now - lastAudioRx > 150)) {
                static int16_t tmp[1024];
                while (ringCount() > 0 && duplex::queued() < RATE / 2) {
                    size_t n = 0;
                    while (n < 1024 && rTail != rHead) { tmp[n++] = ring[rTail]; rTail = (rTail + 1) % RING_N; }
                    duplex::play(tmp, n);
                }
                if (!speakingNow) { md = (uint8_t)Mode::Speak; quietSince = 0; sendFrame('E', "speak", 5); }
            }
            lvl = duplex::level();
            if ((Mode)md.load() == Mode::Speak) {
                if (ringCount() == 0 && duplex::queued() == 0) {
                    if (!quietSince) quietSince = now;
                    if (now - quietSince > 250) { md = (uint8_t)Mode::Listen; sendFrame('E', "listen", 6); }
                } else {
                    quietSince = 0;
                }
            }
            if (now - lastStat > 5000) {
                char b[120];
                duplex::stats(b, sizeof(b));
                logf("[duplex] %s\n", b);
                logf("[duplex] mic sent %u samples total\n", (unsigned)micSent);
                lastStat = now;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (!act) {
            // 待機中もマイクは動かしたままにする (止めるとカメラが映らなくなるため。音は送らずに捨てる)
            if (pet.exchange(false)) sendFrame('E', "wake", 4);
            spkStop();
            md = (uint8_t)Mode::Listen;
            if (!micOn) {
                waitCam();
                { i2cgate::take(portMAX_DELAY); micOn = M5.Mic.begin(); i2cgate::give(); }
                micQueued = 0;
            }
            if (!micOn || !M5.Mic.record(mic[micIdx], MIC_N, RATE)) vTaskDelay(pdMS_TO_TICKS(20));
            else micIdx = (micIdx + 1) % 4;
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
            // 返事の音がある程度たまってから鳴らし始める (Wi-Fi だと音がまとまって遅れて届くため、本体だけの時は多めにためる)
            const size_t pre = localLink ? RATE * 7 / 10 : RATE * 3 / 10;
            if (avail >= pre || (avail > 0 && now - lastAudioRx > 300)) {
                micStop();
                waitCam();
                { i2cgate::take(portMAX_DELAY); M5.Speaker.begin(); i2cgate::give(); }
                M5.Speaker.setVolume(muted ? 0 : 200);  //
                spkOn      = true;
                quietSince = 0;
                md         = (uint8_t)Mode::Speak;
                sendFrame('E', "speak", 5);
                continue;
            }
            if (!micOn) {
                waitCam();
                { i2cgate::take(portMAX_DELAY); micOn = M5.Mic.begin(); i2cgate::give(); }
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
                if (n < PLAY_N && millis() - lastAudioRx < 250) break;
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
                if (now - quietSince > (localLink ? 700u : 250u)) {  // 途中で音が少し遅れても、すぐマイクに戻らない
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
    serialP = new (heap_caps_malloc(sizeof(Parser), MALLOC_CAP_SPIRAM)) Parser();
    localP  = new (heap_caps_malloc(sizeof(Parser), MALLOC_CAP_SPIRAM)) Parser();
    {
        static StaticStreamBuffer_t sa, sb;
        constexpr size_t NA = 64 * 1024, NB = 64 * 1024;
        uint8_t* a = (uint8_t*)heap_caps_malloc(NA + 1, MALLOC_CAP_SPIRAM);
        uint8_t* b = (uint8_t*)heap_caps_malloc(NB + 1, MALLOC_CAP_SPIRAM);
        toTalk     = xStreamBufferCreateStatic(NA, 1, a, &sa);
        fromTalk   = xStreamBufferCreateStatic(NB, 1, b, &sb);
    }
    {
        auto mc          = M5.Mic.config();
        mc.magnification = 8;  // 標準 (2) では話し声が小さかったので 4 倍に
        M5.Mic.config(mc);
    }
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
void setLocalLink(bool on) { localLink = on; }
bool localLinkOn() { return localLink; }
bool macSeen() { return lastSerialFrame && millis() - lastSerialFrame < 5000; }
void localSend(uint8_t type, const void* data, size_t len)
{
    if (!toTalk) return;
    const uint8_t h[5] = {M0, M1, type, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8)};
    std::lock_guard<std::mutex> lk(toTalkMx);
    if (xStreamBufferSpacesAvailable(toTalk) < len + 5) return;
    xStreamBufferSend(toTalk, h, 5, 0);
    if (len) xStreamBufferSend(toTalk, data, len, 0);
}
size_t localSpace() { return toTalk ? xStreamBufferSpacesAvailable(toTalk) : 0; }
size_t localRecv(uint8_t* buf, size_t max, uint32_t waitMs)
{
    if (!fromTalk) return 0;
    return xStreamBufferReceive(fromTalk, buf, max, pdMS_TO_TICKS(waitMs));
}
Mode mode() { return (Mode)md.load(); }
bool thinking() { return think; }
float level() { return lvl; }
void interrupt() { intr = true; }
uint8_t mood(char* text, size_t tl, uint32_t& at)
{
    std::lock_guard<std::mutex> lk(capMx);
    strlcpy(text, moodText, tl);
    at = moodAt;
    return moodV;
}
void petted() { pet = true; }
void touchEvent(const char* ev, int fb)
{
    strlcpy(touchBuf, ev, sizeof(touchBuf));
    touchAt      = millis();
    fallbackSnd  = (millis() - lastHost < 5000) ? fb : -1;
    if (fallbackSnd < 0) soundReq = fb;  // Mac とつながっていなければすぐ鳴らす
    touchPending = true;
}
void endTalk() { endReq = true; }
void playSound(int id) { soundReq = id; }
int takeSoundAnim() { return soundAnim.exchange(0); }
int takeCommand() { return cmd.exchange(-1); }
bool snapshotWanted() { return snapReq.exchange(false); }
void snapshotDone() { snapReady = true; }

void captions(char* user, size_t ul, char* ai, size_t al, uint32_t& ua, uint32_t& aa)
{
    std::lock_guard<std::mutex> lk(capMx);
    strlcpy(user, capUser, ul);
    strlcpy(ai, capAi, al);
    ua = capUserAt;
    aa = capAiAt;
}

}  // namespace talk
