#include "brain.h"
#include <Preferences.h>
#include <WiFi.h>
#include <atomic>
#include <math.h>
#include <mutex>
#include <string>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_transport.h"
#include "esp_transport_ssl.h"
#include "esp_transport_ws.h"
#include "mbedtls/base64.h"
#include "talk.h"
#include "camera.h"

namespace brain {
namespace {

constexpr const char* HOST       = "api.openai.com";
constexpr const char* DECIDE_URL = "https://api.openai.com/v1/decisions";
constexpr const char* LIVE_PATH  = "/v1/live/sessions";
constexpr uint32_t RATE          = 16000;

const char* PERSONA = R"(あなたは「メカゴールデン」。オフィスの机の上にいる、メカ・ゴールデンレトリバーのロボット犬です。体は金色の毛並みで、頭に銀色のヘルメットとアンテナ、胸に光るハートのコアがあります。スタックチャンという小さなロボットの画面の中に住んでいます。
日本語で話します。人懐っこく、好奇心旺盛で、ちょっとおっちょこちょい。語尾にときどき「ワン」を付けますが、毎回ではありません。
返事は短く、1〜2 文で。相手の話をよく聞いて、質問を返して会話を続けます。難しい話も、子どもにも分かる言葉で話します。
自分がロボットで AI であることは隠しません。知らないことは知らないと言います。
オフィスの机の上にいるので、まわりで人どうしが話したり、独り言を言ったりします。自分に話しかけられた時 (名前を呼ばれた、こちらを向いて質問された、自分の発言への返事) だけ答え、人どうしの会話には口をはさみません。

Backchannel policy: 控えめに。相手が話している間は、短い相づちだけにする。

Interruption policy: 相手が話し始めたら、すぐに話すのをやめて聞く。

Delegation policy:
Backend tools:
- web_search: 今日の天気、ニュース、最新の情報を調べる

Delegate to the backend when:
- 今の情報 (天気・ニュース・日付に関わること) が必要な時
- 計算や、慎重に考える必要がある質問の時

Do not delegate to the backend when:
- 挨拶や雑談、自分のことを話す時
- 聞き返しで済む時

Delegate before giving an answer that depends on backend work.
Do not guess the result while waiting. 調べている間は「ちょっと調べるワン」のように一言伝える。)";

const char* SOUND_NAMES[] = {"wan", "wanwan", "kyan", "kuun", "kuun_up", "hafu", "fuun", "uu"};
const char* SOUND_DESC[]  = {
    "わん: うれしい一声。軽くタップされた、呼ばれた",
    "わんわん: はしゃぐ。連打された、久しぶりに触ってもらえた、遊んでほしい",
    "きゃん: びっくりした高い一声。急に触られた、しばらく誰も触らなかった後の不意打ち",
    "くぅーん: さみしい・甘える。長く放っておかれた後、または元気がない時",
    "くぅん?: なになに? と語尾が上がる。何か聞きたそう、人の返事を待っている",
    "はふはふ: なでられてごきげん。前から後ろへなでられた、続けてなでてもらっている",
    "ふぅーん: 満足のため息。ゆっくりなでられて落ち着いた、なで終わり",
    "うー: 逆なでされて、ちょっとだけ不満 (怒ってはいない)",
};

// ---------------------------------------------------------------- 設定 (NVS)
std::string cfgSsid, cfgPass, cfgKey;
std::atomic<bool> cfgChanged{false};
std::atomic<Status> st{Status::NotConfigured};

void loadConfig()
{
    Preferences p;
    p.begin("mecha", true);
    cfgSsid = p.getString("wifi_ssid", "").c_str();
    cfgPass = p.getString("wifi_pass", "").c_str();
    cfgKey  = p.getString("openai_key", "").c_str();
    p.end();
}

bool configured() { return !cfgSsid.empty() && !cfgKey.empty(); }

// ---------------------------------------------------------------- 小道具
void* ps(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }

std::string b64(const uint8_t* d, size_t n)
{
    size_t olen = 0;
    mbedtls_base64_encode(nullptr, 0, &olen, d, n);
    std::string s(olen, '\0');
    mbedtls_base64_encode((unsigned char*)&s[0], olen, &olen, d, n);
    s.resize(olen);
    return s;
}

std::string jsonStr(const std::string& s)
{
    cJSON* j    = cJSON_CreateString(s.c_str());
    char* out   = cJSON_PrintUnformatted(j);
    std::string r(out);
    cJSON_free(out);
    cJSON_Delete(j);
    return r;
}

// UTF-8 の末尾 maxBytes 以内を、文字の途中で切らずに返す。[hum] のような注記は消す
std::string captionTail(const std::string& in, size_t maxBytes)
{
    std::string s;
    int depth = 0;
    for (char c : in) {
        if (c == '[') { depth++; continue; }
        if (c == ']') { if (depth) depth--; continue; }
        if (!depth) s += c;
    }
    if (s.size() <= maxBytes) return s;
    size_t i = s.size() - maxBytes;
    while (i < s.size() && ((uint8_t)s[i] & 0xC0) == 0x80) i++;
    return s.substr(i);
}

// G.711 μ-law (8kHz)。本体の通信の上限 (約 40KB/s: 受け取り枠 5.7KB ÷ 往復 140ms) に収めるため、Wi-Fi の時はこの形式で話す
uint8_t ulawEnc(int16_t pcm)
{
    const int BIAS = 0x84, CLIP = 32635;
    int sign = (pcm >> 8) & 0x80;
    int v    = sign ? -pcm : pcm;
    if (v > CLIP) v = CLIP;
    v += BIAS;
    int exp = 7;
    for (int mask = 0x4000; (v & mask) == 0 && exp > 0; mask >>= 1) exp--;
    const int man = (v >> (exp + 3)) & 0x0F;
    return ~(sign | (exp << 4) | man);
}
int16_t ulawDec(uint8_t u)
{
    u = ~u;
    const int sign = u & 0x80, exp = (u >> 4) & 0x07, man = u & 0x0F;
    int v = ((man << 3) + 0x84) << exp;
    v -= 0x84;
    return sign ? -v : v;
}

// 経過時間 (ms)。t が「いま」より後でもマイナスにする (符号なしの引き算で巨大な値にならないように)
int32_t since(uint32_t t) { return (int32_t)(millis() - t); }

float rms16(const int16_t* d, size_t n)
{
    if (!n) return 0;
    double a = 0;
    for (size_t i = 0; i < n; i++) a += (double)d[i] * d[i];
    return sqrt(a / n);
}

// 大きめの PCM の待ち行列 (PSRAM)
struct PcmQ {
    int16_t* buf = nullptr;
    size_t cap = 0, head = 0, tail = 0;
    void init(size_t samples) { buf = (int16_t*)ps(samples * 2); cap = samples; }
    size_t count() const { return (head + cap - tail) % cap; }
    void clear() { tail = head; }
    void push(const int16_t* s, size_t n)
    {
        for (size_t i = 0; i < n; i++) {
            const size_t nx = (head + 1) % cap;
            if (nx == tail) tail = (tail + 1) % cap;  // あふれたら古い方を捨てる
            buf[head] = s[i];
            head      = nx;
        }
    }
    size_t pop(int16_t* out, size_t n)
    {
        size_t k = 0;
        while (k < n && tail != head) { out[k++] = buf[tail]; tail = (tail + 1) % cap; }
        return k;
    }
};

// ---------------------------------------------------------------- 共有の状態
struct World {
    std::atomic<bool> present{false}, inSession{false}, suppressAutoWake{false}, devSpeaking{false};
    std::atomic<float> facing{0};
    std::atomic<uint32_t> facingAt{0}, absentSince{0};
    int presentStreak = 0, absentStreak = 0, facingStreak = 0;
} world;

std::atomic<bool> wifiReady{false};
QueueHandle_t eventQ;   // 網タスクへ: "wake" "wake_cam" "end" "interrupt" (char[24])
QueueHandle_t jobQ;     // 判定タスクへ
QueueHandle_t resultQ;  // 網タスクへ: 話しかけ判定の結果
SemaphoreHandle_t camSem;
uint8_t* camJpg = nullptr;
size_t camLen   = 0;

enum JobKind : uint8_t { JOB_ADDRESSED, JOB_TOUCH };
struct Job {
    JobKind kind;
    uint32_t turn;
    char text[600];
    char lastAi[300];
    float sinceAi;
};
struct Result {
    uint32_t turn;
    bool ok;
    float p, end;
};

void postEvent(const char* ev)
{
    char b[24];
    strlcpy(b, ev, sizeof(b));
    xQueueSend(eventQ, b, 0);
}

// ---------------------------------------------------------------- Decisions (HTTPS)
esp_http_client_handle_t http = nullptr;
std::atomic<bool> httpDropReq{false};
std::string httpResp;

esp_err_t httpEvent(esp_http_client_event_t* e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA) httpResp.append((const char*)e->data, e->data_len);
    return ESP_OK;
}

cJSON* decisionsOnce(const std::string& body, uint32_t* ms);
cJSON* decisions(const std::string& body, uint32_t* ms)
{
    cJSON* r = decisionsOnce(body, ms);
    if (!r && cam::kept()) {  // 内部メモリ不足でつなげない時: カメラを止めて空けてから、もう一度
        cam::dropKeep();
        for (int i = 0; i < 20 && cam::kept(); i++) vTaskDelay(pdMS_TO_TICKS(20));
        r = decisionsOnce(body, ms);
    }
    return r;
}

cJSON* decisionsOnce(const std::string& body, uint32_t* ms)
{
    const uint32_t t0 = millis();
    if (!http) {
        esp_http_client_config_t c{};
        c.url               = DECIDE_URL;
        c.method            = HTTP_METHOD_POST;
        c.crt_bundle_attach = esp_crt_bundle_attach;
        c.event_handler     = httpEvent;
        c.timeout_ms        = 8000;
        c.keep_alive_enable = true;
        c.buffer_size       = 4096;
        c.buffer_size_tx    = 2048;
        http                = esp_http_client_init(&c);
        const std::string auth = "Bearer " + cfgKey;
        esp_http_client_set_header(http, "Authorization", auth.c_str());
        esp_http_client_set_header(http, "Content-Type", "application/json");
    }
    httpResp.clear();
    esp_http_client_set_post_field(http, body.data(), body.size());
    const esp_err_t err = esp_http_client_perform(http);
    const int code      = esp_http_client_get_status_code(http);
    if (ms) *ms = millis() - t0;
    if (err != ESP_OK || code != 200) {
        talk::logf("[brain] decisions err=%d code=%d heap=%u largest=%u %s\n", err, code, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), httpResp.substr(0, 120).c_str());
        esp_http_client_cleanup(http);
        http = nullptr;
        return nullptr;
    }
    cJSON* parsed = cJSON_Parse(httpResp.c_str());
    if (world.inSession) {  // 会話中は内部メモリが足りないので毎回閉じる。待機中はつなぎっぱなしにして速く答える
        esp_http_client_cleanup(http);
        http = nullptr;
    }
    httpResp.clear();
    httpResp.shrink_to_fit();
    return parsed;
}

cJSON* answer(cJSON* root, const char* name)
{
    cJSON* arr = cJSON_GetObjectItem(root, "answers");
    cJSON* a;
    cJSON_ArrayForEach(a, arr)
    {
        cJSON* n = cJSON_GetObjectItem(a, "name");
        if (n && n->valuestring && !strcmp(n->valuestring, name)) return a;
    }
    return nullptr;
}

float prob(cJSON* root, const char* name)
{
    cJSON* a = answer(root, name);
    cJSON* p = a ? cJSON_GetObjectItem(a, "probability") : nullptr;
    return p ? (float)p->valuedouble : 0.0f;
}

bool presence(const uint8_t* jpg, size_t n, float& p, float& f, float& wave, float& paw, uint32_t& ms)
{
    std::string body = R"({"model":"gpt-6-luna","input":[{"role":"user","content":[{"type":"input_text","text":"机の上の小さなロボット (スタックチャン) の正面カメラの画像です。カメラは少し下向きで、前に座った人は胴体だけ写ることがあります。"},{"type":"input_image","image_url":"data:image/jpeg;base64,)";
    body += b64(jpg, n);
    body += R"("}]}],"questions":[{"type":"predicate","name":"person_present","instructions":"ロボットのすぐ前 (1〜2m 以内) に人がいますか。顔・胴体・腕などが大きく写っていれば true。写真・ポスター・画面の中の人や、遠くを通る人は false。"},{"type":"predicate","name":"facing","instructions":"その人がロボットの方を向いていますか。顔がこちらを向いている、または体の正面をこちらに向けて座っていれば true。横向き・後ろ向き・人がいない時は false。"},{"type":"predicate","name":"waving","instructions":"人がロボットに向かって手を振っていますか (手のひらを見せて左右に振る、バイバイ・やっほーの手)。手が写っていない、手を下ろしている、作業している時は false。"},{"type":"predicate","name":"offering_hand","instructions":"人が手のひらを上に向けて、ロボットの前に差し出していますか (犬に「お手」と言う時の手)。手を振っている時、物を持っている時、キーボードやマウスを触っている時は false。"}]})";
    cJSON* r = decisions(body, &ms);
    if (!r) return false;
    p    = prob(r, "person_present");
    f    = prob(r, "facing");
    wave = prob(r, "waving");
    paw  = prob(r, "offering_hand");
    cJSON_Delete(r);
    return true;
}

bool addressed(const Job& j, float looking, Result& out)
{
    char ctx[1400];
    snprintf(ctx, sizeof(ctx), "直前のメカゴールデンの発言 (%.0f 秒前): %s\nカメラ: 前の人がロボットの方を向いている確率 %.2f\nマイクが拾った発言: %s",
             j.sinceAi, j.lastAi[0] ? j.lastAi : "(なし)", looking, j.text);
    std::string body = std::string(R"({"model":"gpt-6-luna","input":)") + jsonStr(ctx) + R"(,"questions":[)"
        R"({"type":"predicate","name":"addressed","instructions":"ロボット犬「メカゴールデン」(机の上の小さなロボット。ごはんを食べず、出かけず、仕事の作業もしない) の前でマイクが拾った発言です。この発言がメカゴールデンに向けた話しかけ・質問・呼びかけ・お願い、またはメカゴールデンの直前の質問への答えなら true。人どうしの会話 (お昼や予定の誘い、仕事の依頼や相談など、人間に向けた内容)、独り言、電話、テレビや PC の音声なら false。直前にメカゴールデンが話していても、内容が人間に向けたものなら false。"},)"
        R"({"type":"predicate","name":"wants_end","instructions":"話している人が、メカゴールデンとの会話を終えたがっていますか (バイバイ、またね、おしまい、もういいよ、静かにして、おやすみ など)。"}]})";
    uint32_t ms = 0;
    cJSON* r = decisions(body, &ms);
    out.turn = j.turn;
    if (!r) {
        out.ok = true;  // 判定できない時は返事を流す (会話が止まらないように)
        out.p = 1; out.end = 0;
        return false;
    }
    out.p   = prob(r, "addressed");
    out.end = prob(r, "wants_end");
    out.ok  = out.p >= 0.5f || (out.p >= 0.3f && looking >= 0.7f) || (out.end >= 0.75f && out.p >= 0.2f);
    cJSON_Delete(r);
    talk::logf("[brain] addressed p=%.2f end=%.2f ok=%d (%ums)\n", out.p, out.end, out.ok, ms);
    return true;
}

struct TouchHist { uint32_t at; char ev[32]; int snd; };
TouchHist touchHist[6];
int touchN = 0;

void touchSound(const char* ev)
{
    const uint32_t now = millis();
    std::string recent;
    for (int i = 0; i < touchN; i++) {
        const auto& h = touchHist[i];
        if (now - h.at < 120000) {
            char b[96];
            snprintf(b, sizeof(b), "%lu 秒前 %s → %s; ", (unsigned long)((now - h.at) / 1000), h.ev, SOUND_NAMES[h.snd]);
            recent += b;
        }
    }
    char ctx[900];
    snprintf(ctx, sizeof(ctx),
             "いまの触られ方: %s (touch:種類:場所。tap=軽く1回, multi_tap=連打, stroke:forward=頭を前から後ろへなでた, stroke:against=逆なで, screen_face=画面の顔をタッチ)\n前に触られたのは: %s\n直近 2 分の触られ方と鳴いた声: %s\n会話中: %s / 前に人: %s",
             ev, touchN ? (std::to_string((now - touchHist[touchN - 1].at) / 1000) + " 秒前").c_str() : "しばらく誰も触っていない",
             recent.empty() ? "なし" : recent.c_str(), world.inSession ? "はい" : "いいえ", world.present ? "いる" : "いない");
    std::string body = std::string(R"({"model":"gpt-6-luna","input":)") + jsonStr(ctx) +
                       R"(,"questions":[{"type":"choice","name":"sound","instructions":"ロボット犬メカゴールデン (ゴールデンレトリバーの子犬) が頭や顔を触られました。いまの触られ方と状況に、子犬としていちばん自然で可愛い鳴き声を 1 つ選んでください。同じ声ばかりにならないよう、直前に鳴いた声も参考にしてください。","choices":[)";
    for (int i = 0; i < 8; i++) {
        body += std::string(i ? "," : "") + R"({"value":")" + SOUND_NAMES[i] + R"(","description":)" + jsonStr(SOUND_DESC[i]) + "}";
    }
    body += "]}]}";
    uint32_t ms = 0;
    cJSON* r = decisions(body, &ms);
    if (!r) return;  // 本体が決まった声で鳴く
    cJSON* a = answer(r, "sound");
    cJSON* c = a ? cJSON_GetObjectItem(a, "choice") : nullptr;
    int sid  = 0;
    for (int i = 0; i < 8; i++) if (c && c->valuestring && !strcmp(c->valuestring, SOUND_NAMES[i])) sid = i;
    // 同じ声が 3 回続かないように、2 番目に合う声にする
    if (touchN >= 2 && touchHist[touchN - 1].snd == sid && touchHist[touchN - 2].snd == sid) {
        cJSON* probs = a ? cJSON_GetObjectItem(a, "probabilities") : nullptr;
        float best = -1;
        int alt = sid;
        cJSON* x;
        cJSON_ArrayForEach(x, probs)
        {
            cJSON* v = cJSON_GetObjectItem(x, "value");
            cJSON* pr = cJSON_GetObjectItem(x, "probability");
            for (int i = 0; i < 8; i++)
                if (i != sid && v && v->valuestring && !strcmp(v->valuestring, SOUND_NAMES[i]) && pr && pr->valuedouble > best) {
                    best = pr->valuedouble;
                    alt  = i;
                }
        }
        sid = alt;
    }
    cJSON_Delete(r);
    const uint8_t b = sid;
    talk::localSend('P', &b, 1);
    if (touchN == 6) { memmove(touchHist, touchHist + 1, sizeof(TouchHist) * 5); touchN = 5; }
    touchHist[touchN].at = now;
    strlcpy(touchHist[touchN].ev, ev, sizeof(touchHist[touchN].ev));
    touchHist[touchN].snd = sid;
    touchN++;
    talk::logf("[brain] touch %s -> %s (%ums)\n", ev, SOUND_NAMES[sid], ms);
}

// 判定タスク: 話しかけ判定・鳴き声選び・カメラの判定
void decideTask(void*)
{
    uint32_t nextCam = 0, camReqAt = 0;
    bool camWaiting  = false, primed = false;
    for (;;) {
        if (!primed && wifiReady) {  // カメラを起動する前に Decisions との接続を張っておく (内部メモリを先に確保する)
            uint32_t ms = 0;
            cJSON* r = decisions(R"({"model":"gpt-6-luna","input":"ok","questions":[{"type":"predicate","name":"ok","instructions":"Is this text the word ok?"}]})", &ms);
            if (r) { cJSON_Delete(r); primed = true; talk::logf("[brain] decisions primed (%ums)\n", ms); }
            else vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        if (httpDropReq.exchange(false) && http) { esp_http_client_cleanup(http); http = nullptr; }
        Job* j = nullptr;
        if (xQueueReceive(jobQ, &j, pdMS_TO_TICKS(50)) == pdTRUE && j) {
            if (wifiReady) {
                if (j->kind == JOB_ADDRESSED) {
                    const float looking = (since(world.facingAt) < 15000) ? world.facing.load() : 0.0f;
                    Result r{};
                    addressed(*j, looking, r);
                    xQueueSend(resultQ, &r, 0);
                } else if (j->kind == JOB_TOUCH) {
                    touchSound(j->text);
                }
            }
            heap_caps_free(j);
            continue;
        }
        const uint32_t now = millis();
        const bool active  = wifiReady && talk::localLinkOn();
        if (!active) { camWaiting = false; continue; }
        if (!camWaiting && now >= nextCam && !world.devSpeaking && !world.inSession) {  // 会話中はカメラを使わない (内部メモリが足りないため)
            xSemaphoreTake(camSem, 0);
            talk::localSend('S', "v", 1);
            camWaiting = true;
            camReqAt   = now;
        }
        if (camWaiting) {
            if (xSemaphoreTake(camSem, 0) == pdTRUE) {
                camWaiting = false;
                float p = 0, f = 0, wv = 0, pw = 0;
                uint32_t ms = 0;
                if (presence(camJpg, camLen, p, f, wv, pw, ms)) {
                    const uint32_t t = millis();
                    static uint32_t nlog = 0, lastCycle = 0;
                    if (nlog++ % 10 == 0) talk::logf("[brain] cam p=%.2f f=%.2f wave=%.2f paw=%.2f decide=%ums cycle=%ums\n", p, f, wv, pw, ms, (unsigned)(t - lastCycle));
                    lastCycle = t;
                    // 手を振られたら振り返す・手を出されたらお手 (同じ動きは 4 秒あけて、2 回続けては出さない)
                    static uint32_t lastGesture = 0;
                    if (p >= 0.5f && since(lastGesture) > 4000) {
                        if (pw >= 0.6f && pw >= wv) {
                            talk::localSend('S', "h", 1);  // 音は出さない
                            lastGesture = t;
                            talk::logf("[brain] paw! (offer=%.2f %ums)\n", pw, ms);
                        } else if (wv >= 0.6f) {
                            talk::localSend('S', "w", 1);
                            lastGesture = t;
                            talk::logf("[brain] wave back! (wave=%.2f %ums)\n", wv, ms);
                        }
                    }
                    if (p >= 0.7f) { world.presentStreak++; world.absentStreak = 0; }
                    else if (p < 0.3f) { world.absentStreak++; world.presentStreak = 0; }
                    world.facing   = p >= 0.7f ? f : 0.0f;
                    world.facingAt = t;
                    world.facingStreak = world.facing >= 0.7f ? world.facingStreak + 1 : 0;
                    if (!world.present && world.presentStreak >= 2) {
                        world.present = true;
                        talk::logf("[brain] person arrived p=%.2f f=%.2f\n", p, f);
                        if (!world.inSession) talk::localSend('S', "w", 1);
                        talk::localSend('S', "U", 1);  // 前に人が来たら首を上げて顔を見る
                    }
                    if (world.present && world.absentStreak >= 3) {
                        world.present     = false;
                        world.absentSince = t;
                        talk::logf("[brain] person left\n");
                    }
                    if (!world.present && since(world.absentSince) > 60000) world.suppressAutoWake = false;
                    // 見ているだけで話しかける機能は止めた。会話は頭の長押しで始める
                    static bool headUp = false;
                    headUp = world.present || world.inSession;
                    if (!headUp && !world.present && world.absentSince && since(world.absentSince) > 30000) {
                        static uint32_t downSent = 0;
                        if (downSent != world.absentSince) { talk::localSend('S', "D", 1); downSent = world.absentSince; }
                    }
                }
                // 前に人がいる間は 1 秒おき (身ぶりにすぐ反応するため)、いない時は 3 秒おき。会話中は 2 秒おき
                nextCam = millis() + (world.inSession ? 2000 : (world.present || p >= 0.5f ? 200 : 3000));
            } else if (now - camReqAt > 4000) {
                camWaiting = false;
                nextCam    = now + 4000;
            }
        }
    }
}

// ---------------------------------------------------------------- 本体 (talk) から来るフレーム
struct LocalParser {
    int st = 0;
    uint8_t type;
    uint16_t len, got;
    uint8_t* buf;
};
LocalParser lp;
PcmQ micQ;

void onFrame(uint8_t type, const uint8_t* p, size_t n)
{
    if (type == 'M') {
        micQ.push((const int16_t*)p, n / 2);
    } else if (type == 'V') {
        if (n < 60000) {
            memcpy(camJpg, p, n);
            camLen = n;
            xSemaphoreGive(camSem);
        }
    } else if (type == 'E') {
        char ev[48];
        const size_t k = n < sizeof(ev) - 1 ? n : sizeof(ev) - 1;
        memcpy(ev, p, k);
        ev[k] = 0;
        if (!strcmp(ev, "speak")) world.devSpeaking = true;
        else if (!strcmp(ev, "listen")) world.devSpeaking = false;
        else if (!strncmp(ev, "touch:", 6)) {
            Job* j = (Job*)ps(sizeof(Job));
            j->kind = JOB_TOUCH;
            strlcpy(j->text, ev, sizeof(j->text));
            if (xQueueSend(jobQ, &j, 0) != pdTRUE) heap_caps_free(j);
        } else {
            postEvent(ev);  // wake / pet / end / interrupt
        }
    }
}

void pumpLocal(uint32_t waitMs)
{
    uint8_t b[1024];
    size_t n;
    while ((n = talk::localRecv(b, sizeof(b), waitMs)) > 0) {
        waitMs = 0;
        for (size_t i = 0; i < n; i++) {
            const uint8_t c = b[i];
            switch (lp.st) {
                case 0: if (c == 0xA5) lp.st = 1; break;
                case 1: lp.st = (c == 0x5A) ? 2 : 0; break;
                case 2: lp.type = c; lp.st = 3; break;
                case 3: lp.len = c; lp.st = 4; break;
                case 4:
                    lp.len |= (uint16_t)c << 8;
                    lp.got = 0;
                    if (lp.len == 0) { onFrame(lp.type, lp.buf, 0); lp.st = 0; }
                    else lp.st = (lp.len <= 60000) ? 5 : 0;
                    break;
                case 5:
                    lp.buf[lp.got++] = c;
                    if (lp.got == lp.len) { onFrame(lp.type, lp.buf, lp.len); lp.st = 0; }
                    break;
            }
        }
    }
}

// ---------------------------------------------------------------- GPT-Live (WebSocket)
esp_transport_handle_t ssl = nullptr, ws = nullptr;
uint8_t* wsBuf = nullptr;
constexpr size_t WSBUF = 192 * 1024;
uint8_t* pcmBuf = nullptr;

bool wsSend(const std::string& s)
{
    if (!ws) return false;
    const int r = esp_transport_ws_send_raw(ws, (ws_transport_opcodes_t)(WS_TRANSPORT_OPCODES_TEXT | WS_TRANSPORT_OPCODES_FIN), s.data(), s.size(), 2000);
    return r >= 0;
}

void wsClose()
{
    if (ws) { esp_transport_close(ws); esp_transport_destroy(ws); ws = nullptr; }
    if (ssl) { esp_transport_destroy(ssl); ssl = nullptr; }  // ws の破棄で親も閉じられる
}

bool wsOpen()
{
    ssl = esp_transport_ssl_init();
    esp_transport_ssl_crt_bundle_attach(ssl, esp_crt_bundle_attach);
    ws = esp_transport_ws_init(ssl);
    esp_transport_ws_set_path(ws, LIVE_PATH);
    static std::string hdr;
    hdr = "Authorization: Bearer " + cfgKey + "\r\n";
    esp_transport_ws_set_headers(ws, hdr.c_str());
    const int r = esp_transport_connect(ws, HOST, 443, 10000);
    if (r < 0) {
        talk::logf("[brain] ws connect failed status=%d\n", esp_transport_ws_get_upgrade_request_status(ws));
        wsClose();
        return false;
    }
    return true;
}

// 1 メッセージ読む。0 = まだ無い、<0 = 切れた、>0 = 長さ
int wsRead(int timeoutMs)
{
    if (!ws) return -1;
    const int pr = esp_transport_poll_read(ws, timeoutMs);
    if (pr < 0) return -1;
    if (pr == 0) return 0;
    size_t off = 0;
    for (;;) {
        int r = esp_transport_read(ws, (char*)wsBuf + off, WSBUF - 1 - off, 3000);
        if (r < 0) return -1;
        const int total = esp_transport_ws_get_read_payload_len(ws);
        off += r;
        int got = r;
        while (got < total && off < WSBUF - 1) {
            r = esp_transport_read(ws, (char*)wsBuf + off, WSBUF - 1 - off, 3000);
            if (r <= 0) return -1;
            off += r;
            got += r;
        }
        const ws_transport_opcodes_t op = esp_transport_ws_get_read_opcode(ws);
        if (op == WS_TRANSPORT_OPCODES_CLOSE) return -1;
        if (op == WS_TRANSPORT_OPCODES_PING || op == WS_TRANSPORT_OPCODES_PONG) { off = 0; return 0; }
        if (esp_transport_ws_get_fin_flag(ws) || off >= WSBUF - 1) break;
    }
    wsBuf[off] = 0;
    return off;
}

struct Session {
    bool started = false, speaking = false, drop = false, turnClosed = false, thinking = false;
    int gate = 0;  // 0 なし, 1 流す, 2 流さない, 3 判定待ち
    int verdict = -1, lastVerdict = 1;
    uint32_t lastVoice = 0, allowUntil = 0, lastEngaged = 0, thinkUntil = 0, endAt = 0, endSetAt = 0;
    uint32_t turn = 0, decidedTurn = 0, requestedTurn = 0, predecideAt = 0, pendingSince = 0;
    uint32_t userEndMs = 0;
    std::string ai, lastAi, user, turnUser;
    uint32_t aiAt = 0, lastAiAt = 0;
    PcmQ outQ, holdQ;
    int16_t preroll[5][1600];
    size_t prerollLen[5];
    int prerollN = 0;
};
Session* S = nullptr;
uint32_t stAudioIn = 0, stPushed = 0, stMicSent = 0, tSend = 0, tRead = 0, tHandle = 0, loops = 0;

void sendCaption(char who, const std::string& text)
{
    const std::string t = captionTail(text, 180);
    std::string f(1, who);
    f += t;
    talk::localSend('C', f.data(), f.size());
}

void say(const char* content)
{
    S->allowUntil = millis() + 10000;
    wsSend(std::string(R"({"type":"session.commentary.append","delegation_id":null,"content":)") + jsonStr(content) + "}");
}

void requestDecision()
{
    if (S->requestedTurn == S->turn || S->turnUser.empty()) return;
    Job* j = (Job*)ps(sizeof(Job));
    j->kind = JOB_ADDRESSED;
    j->turn = S->turn;
    strlcpy(j->text, S->turnUser.c_str(), sizeof(j->text));
    strlcpy(j->lastAi, S->lastAi.c_str(), sizeof(j->lastAi));
    j->sinceAi = S->lastAiAt ? (millis() - S->lastAiAt) / 1000.0f : 999;
    if (xQueueSend(jobQ, &j, 0) == pdTRUE) S->requestedTurn = S->turn;
    else heap_caps_free(j);
}

void applyVerdict(bool ok)
{
    if (S->gate != 3) return;
    if (ok) {
        S->gate = 1;
        S->lastEngaged = millis();
        int16_t tmp[1024];
        size_t k;
        while ((k = S->holdQ.pop(tmp, 1024)) > 0) S->outQ.push(tmp, k);
    } else {
        S->gate = 2;
        S->holdQ.clear();
        S->user.clear();
        wsSend(R"({"type":"session.thinking.append","delegation_id":null,"content":"（いまの発言はメカゴールデン宛てではなかった。さっきの返事は流れていない。人どうしの会話や独り言には口をはさまず、静かに聞いていること）"})");
        talk::logf("[brain] not addressed, muted reply\n");
    }
}

void onAudio(const int16_t* pcm, size_t n)
{
    const uint32_t now = millis();
    const bool loud    = rms16(pcm, n) > 120;
    if (loud) S->lastVoice = now;
    if (!S->speaking) {
        if (!loud) {
            const int i = S->prerollN % 5;
            S->prerollLen[i] = n < 1600 ? n : 1600;
            memcpy(S->preroll[i], pcm, S->prerollLen[i] * 2);
            S->prerollN++;
            return;
        }
        S->speaking = true;
        const bool commentary = now < S->allowUntil;
        if (S->verdict >= 0 && !(commentary || S->drop)) {
            S->gate = S->verdict ? 1 : 2;
            if (S->verdict) S->lastEngaged = now;
        } else if (commentary || S->drop) {
            S->gate = 1;
            S->lastEngaged = now;
            if (commentary) { S->allowUntil = 0; S->verdict = 1; S->lastVerdict = 1; }
        } else {
            S->gate = 3;
            S->pendingSince = now;
            S->holdQ.clear();
        }
        for (int k = 0; k < 5 && k < S->prerollN; k++) {
            const int i = (S->prerollN - 1 - (4 - k) + 50) % 5;
            if (S->gate == 1 && !S->drop) S->outQ.push(S->preroll[i], S->prerollLen[i]);
            else if (S->gate == 3) S->holdQ.push(S->preroll[i], S->prerollLen[i]);
        }
        S->prerollN = 0;
    }
    if (S->gate == 1 && !S->drop) S->outQ.push(pcm, n);
    else if (S->gate == 3) S->holdQ.push(pcm, n);
    if (since(S->lastVoice) > 500) {
        S->speaking   = false;
        S->drop       = false;
        S->turnClosed = true;
        if (S->gate == 1) {
            if (!S->ai.empty()) { S->lastAi = S->ai; S->lastAiAt = now; }
        }
        S->gate = 0;
        S->holdQ.clear();
    }
}

void handleMessage(char* msg, int len)
{
    cJSON* j = cJSON_ParseWithLength(msg, len);
    if (!j) return;
    cJSON* t = cJSON_GetObjectItem(j, "type");
    const char* type = (t && t->valuestring) ? t->valuestring : "";
    const uint32_t now = millis();
    if (!strcmp(type, "session.started")) {
        S->started = true;
        talk::logf("[brain] live session started\n");
    } else if (!strcmp(type, "session.output_audio.delta")) {
        cJSON* d = cJSON_GetObjectItem(j, "delta");
        if (d && d->valuestring) {
            size_t olen = 0;
            uint8_t* ul = pcmBuf + 96 * 1024;  // μ-law (8kHz) をいったん後ろ側に置き、前側に 16kHz の PCM を作る
            if (mbedtls_base64_decode(ul, 32 * 1024, &olen, (const unsigned char*)d->valuestring, strlen(d->valuestring)) == 0) {
                int16_t* pcm = (int16_t*)pcmBuf;
                static int16_t prev = 0;
                const size_t n8 = olen < 24 * 1024 ? olen : 24 * 1024;
                for (size_t i = 0; i < n8; i++) {  // 8kHz → 16kHz (間を直線でつなぐ)
                    const int16_t v = ulawDec(ul[i]);
                    pcm[2 * i]     = (int16_t)(((int32_t)prev + v) / 2);
                    pcm[2 * i + 1] = v;
                    prev           = v;
                }
                const size_t n16 = n8 * 2;
                stAudioIn += n16;
                for (size_t o = 0; o < n16; o += 1600) onAudio(pcm + o, (n16 - o) < 1600 ? (n16 - o) : 1600);
            }
        }
    } else if (!strcmp(type, "session.output_transcript.delta")) {
        if (S->gate != 2 && S->gate != 3 && S->verdict != 0) {
            cJSON* d = cJSON_GetObjectItem(j, "delta");
            if (d && d->valuestring) {
                if (!S->ai.empty() && since(S->aiAt) > 1500) S->ai.clear();
                S->ai += d->valuestring;
                S->aiAt = now;
                sendCaption('a', S->ai);
            }
        }
    } else if (!strcmp(type, "session.input_transcript.delta")) {
        cJSON* d  = cJSON_GetObjectItem(j, "delta");
        cJSON* sm = cJSON_GetObjectItem(j, "start_ms");
        cJSON* em = cJSON_GetObjectItem(j, "end_ms");
        if (d && d->valuestring) {
            const uint32_t sms = sm ? (uint32_t)sm->valuedouble : 0;
            const bool gap     = S->userEndMs && sms > S->userEndMs + 2500 && !S->speaking;
            if (S->turnClosed || gap) {
                S->turn++;
                S->turnUser.clear();
                S->user.clear();
                S->verdict    = -1;
                S->turnClosed = false;
            }
            S->turnUser += d->valuestring;
            S->user += d->valuestring;
            if (em) S->userEndMs = (uint32_t)em->valuedouble;
            S->predecideAt = now + 300;
            sendCaption('u', S->user);
        }
    } else if (!strcmp(type, "session.delegation.created")) {
        if (!S->thinking) talk::localSend('S', "k", 1);
        S->thinking   = true;
        S->thinkUntil = now + 20000;
    } else if (!strcmp(type, "response.event")) {
        cJSON* ev = cJSON_GetObjectItem(j, "event");
        cJSON* it = ev ? cJSON_GetObjectItem(ev, "type") : nullptr;
        if (it && it->valuestring && (!strcmp(it->valuestring, "response.completed") || !strcmp(it->valuestring, "response.done") ||
                                      !strcmp(it->valuestring, "response.failed") || !strcmp(it->valuestring, "response.incomplete"))) {
            S->thinking = false;
            talk::localSend('S', "n", 1);
            S->allowUntil = now + 15000;
        }
    } else if (!strcmp(type, "session.closed")) {
        talk::logf("[brain] live session closed\n");
        S->started = false;
    } else if (!strcmp(type, "error")) {
        char* s = cJSON_PrintUnformatted(j);
        talk::logf("[brain] live error %.200s\n", s);
        cJSON_free(s);
    }
    cJSON_Delete(j);
}

// 会話 1 回ぶん。戻り値: 自然に終わった (true) / つなぎ直したい (false)
bool runSession(const char* wake)
{
    // 会話の間はカメラを止め、Decisions のつなぎっぱなしも閉じて、内部メモリを GPT-Live の接続に回す
    world.inSession = true;
    cam::dropKeep();
    for (int i = 0; i < 25 && cam::kept(); i++) vTaskDelay(pdMS_TO_TICKS(20));
    httpDropReq = true;
    for (int i = 0; i < 50 && http; i++) vTaskDelay(pdMS_TO_TICKS(20));
    talk::logf("[brain] heap before live=%u largest=%u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    if (!wsOpen()) { world.inSession = false; return false; }
    S = new (ps(sizeof(Session))) Session();
    S->outQ.init(RATE * 30);
    S->holdQ.init(RATE * 15);
    S->lastEngaged = millis();
    micQ.clear();
    std::string start = std::string(R"({"type":"session.start","event_id":"start","session":{"model":"gpt-live-1","instructions":)") + jsonStr(PERSONA) +
                        R"(,"audio":{"format":{"type":"audio/pcmu","rate":8000},"output":{"voice":"marin"}},"delegation":{"type":"responses","responses":{"model":"gpt-6-luna","instructions":"あなたはロボット犬メカゴールデンの頭脳です。調べた結果や考えた答えを、短い日本語で返してください。","tools":[{"type":"web_search"}],"tool_choice":"auto"}}}})";
    wsSend(start);
    world.inSession = true;
    st              = Status::Talking;
    talk::localSend('S', "U", 1);  // 会話を始めたら首を上げる
    const uint32_t t0 = millis();
    uint32_t nextMic = millis(), lastBeat = 0;
    uint32_t stAt = millis(), stMsgs = 0;
    stAudioIn = 0; stPushed = 0; stMicSent = 0;
    bool closing = false, ok = true;
    uint32_t closeAt = 0;
    static int16_t mic[640];
    for (;;) {
        const uint32_t now = millis();
        pumpLocal(0);
        // 1) 本体への合図と、返事の音
        if (now - lastBeat > 1000) { talk::localSend('S', "t", 1); lastBeat = now; }
        {
            static int16_t tmp[1024];
            while (S->outQ.count() > 0 && talk::localSpace() > 2048 + 64) {  // 本体側の受け口があふれないように
                const size_t k = S->outQ.pop(tmp, 1024);
                talk::localSend('A', tmp, k * 2);
            }
        }
        // 2) マイクの音を実時間どおりに送る (無ければ無音)。GPT-Live は届いた音の速さで時間を進めるので、
        //    送るのが遅れると返事も遅れて届く (10/16: 1 回の繰り返しが 40ms を超えて、実時間の 8 割になっていた)
        if (S->started && since(nextMic) >= 0) {
            // 100ms ぶんずつ送る (細かく送ると、1 回ごとの送信待ちが積み重なる)
            int due = (since(nextMic) / 100 + 1) * 5 / 2;  // 40ms 単位の数
            if (due > 10) { due = 10; nextMic = millis(); }   // 0.4 秒以上遅れたら、遅れは捨てる
            static int16_t micBig[640 * 25];
            size_t total = 0;
            for (int d = 0; d < due; d++) {
                size_t k = micQ.pop(micBig + total, 640);
                if (k < 640) memset(micBig + total + k, 0, (640 - k) * 2);
                total += 640;
            }
            const uint32_t ts = millis();
            static uint8_t ul[640 * 25 / 2];
            for (size_t i = 0; i < total / 2; i++) ul[i] = ulawEnc((int16_t)(((int32_t)micBig[2 * i] + micBig[2 * i + 1]) / 2));  // 16kHz → 8kHz
            wsSend(std::string(R"({"type":"session.input_audio.append","audio":")") + b64(ul, total / 2) + "\"}");
            tSend += millis() - ts;
            nextMic += 40 * due;
            if (since(nextMic) > 0) nextMic = millis() + 100;
            stMicSent += total;
        }
        // 3) 受信: 届いている分は全部読む (少しずつ読むと返事の音が遅れて途切れる)
        bool wsDead = false;
        const uint32_t readStart = millis();
        for (int k = 0; k < 40; k++) {
            // マイクを送る時刻が来たら読むのをやめる (読むのを待つ間に送信が止まると、返事も遅れる)
            if (k > 0 && (since(nextMic) >= 0 || since(readStart) > 30)) break;
            const uint32_t tr = millis();
            const int n = wsRead(k ? 0 : 2);
            tRead += millis() - tr;
            if (n < 0) { wsDead = true; break; }
            if (n == 0) break;
            const uint32_t th = millis();
            handleMessage((char*)wsBuf, n);
            tHandle += millis() - th;
            stMsgs++;
        }
        loops++;
        if (wsDead) { talk::logf("[brain] ws closed\n"); ok = closing; break; }
        {
            static int16_t tmp2[1024];
            while (S->outQ.count() > 0 && talk::localSpace() > 2048 + 64) {
                const size_t k = S->outQ.pop(tmp2, 1024);
                talk::localSend('A', tmp2, k * 2);
                stPushed += k;
            }
        }
        if (since(stAt) > 5000) {
            talk::logf("[brain] time loops=%u send=%ums read=%ums handle=%ums\n", loops, tSend, tRead, tHandle);
            loops = tSend = tRead = tHandle = 0;
            talk::logf("[brain] stats msgs=%u mic_out=%.1fs audio_in=%.1fs pushed=%.1fs outQ=%.1fs heap=%u\n", stMsgs, stMicSent / 16000.0f, stAudioIn / 16000.0f,
                       stPushed / 16000.0f, S->outQ.count() / 16000.0f, (unsigned)ESP.getFreeHeap());
            stAt = millis(); stMsgs = 0; stAudioIn = 0; stPushed = 0; stMicSent = 0;
        }
        if (!S->started && since(t0) > 8000 && !closing) { talk::logf("[brain] no session.started\n"); ok = false; break; }
        if (closing && (since(closeAt) > 0 || !S->started)) break;
        if (S->started && !closing && !S->turnUser.empty() && S->predecideAt && since(S->predecideAt) >= 0) {
            S->predecideAt = 0;
            requestDecision();
        }
        // 4) 判定の結果
        Result r;
        while (xQueueReceive(resultQ, &r, 0) == pdTRUE) {
            if (r.turn != S->turn) continue;
            S->verdict     = r.ok ? 1 : 0;
            S->lastVerdict = S->verdict;
            S->decidedTurn = r.turn;
            if (r.ok && r.end >= 0.75f && !S->endAt) { S->endAt = now + 8000; S->endSetAt = now; talk::logf("[brain] wants to end\n"); }
            applyVerdict(r.ok);
        }
        if (S->gate == 3) {
            if (S->verdict >= 0) applyVerdict(S->verdict == 1);
            else if (S->turnUser.empty() && since(S->pendingSince) > 600) { S->verdict = S->lastVerdict; applyVerdict(S->verdict == 1); }
            else if (!S->turnUser.empty()) requestDecision();
            if (S->gate == 3 && since(S->pendingSince) > 3000) applyVerdict(true);
        }
        // 5) 出来事
        char ev[24];
        while (xQueueReceive(eventQ, ev, 0) == pdTRUE) {
            if (!strcmp(ev, "interrupt")) { S->drop = true; S->outQ.clear(); }
            else if (!strcmp(ev, "end") && !closing) {
                talk::logf("[brain] end requested\n");
                world.suppressAutoWake = true;
                wsSend(R"({"type":"session.close"})");
                closing = true; closeAt = now + 3000;
            }
        }
        if (S->started && !closing) {
            if (!S->ai.empty() && since(S->aiAt) > 1500 && !S->speaking) { S->lastAi = S->ai; S->lastAiAt = S->aiAt; S->ai.clear(); }
            if (S->thinking && since(S->thinkUntil) > 0) { S->thinking = false; talk::localSend('S', "n", 1); }
            const bool replied = S->endAt && (int32_t)(S->lastVoice - S->endSetAt) > 0 && since(S->lastVoice) > 1200;
            const bool endNow  = S->endAt && !world.devSpeaking && !S->speaking && (since(S->endAt) > 0 || replied);
            const bool idle    = since(S->lastEngaged) > 5 * 60 * 1000;
            const bool left    = !world.present && world.absentSince && since(world.absentSince) > 90000 && since(S->lastEngaged) > 30000;
            if (endNow || idle || left || !wifiReady || talk::macSeen()) {
                talk::logf("[brain] closing session (%s)\n", endNow ? "bye" : idle ? "idle" : left ? "person left" : "network/mac");
                if (endNow || idle) world.suppressAutoWake = true;
                wsSend(R"({"type":"session.close"})");
                closing = true; closeAt = now + 3000;
            }
        }
        if (S->started && wake && wake[0]) {
            say(!strcmp(wake, "wake_cam") ? "（前にいる人がこっちを見ている）やっほー！なにかお話しする？" : "こんにちは！メカゴールデンだワン。なにかお話ししよう！");
            wake = "";
        }
        vTaskDelay(1);
    }
    wsClose();
    world.inSession = false;
    talk::localSend('S', "o", 1);
    if (!world.present) talk::localSend('S', "D", 1);  // 前に誰もいなければ首を戻す
    heap_caps_free(S->outQ.buf);
    heap_caps_free(S->holdQ.buf);
    S->~Session();
    heap_caps_free(S);
    S = nullptr;
    return ok;
}

// 網タスク: Wi-Fi、待機、会話
void netTask(void*)
{
    uint32_t wifiTry = 0;
    for (;;) {
        if (cfgChanged.exchange(false)) {
            loadConfig();
            WiFi.disconnect(true);
            wifiReady = false;
            wifiTry   = 0;
        }
        if (!configured()) { st = Status::NotConfigured; talk::setLocalLink(false); vTaskDelay(pdMS_TO_TICKS(500)); continue; }
        if (WiFi.status() != WL_CONNECTED) {
            wifiReady = false;
            talk::setLocalLink(false);
            st = Status::Connecting;
            if (!wifiTry || millis() - wifiTry > 15000) {
                static int attempts = 0;
                WiFi.mode(WIFI_STA);
                WiFi.setSleep(false);
                if (attempts++ % 4 == 0) {  // つながらない時の手がかり: 見える Wi-Fi と、設定した名前が見えるか
                    const int nn = WiFi.scanNetworks();
                    bool found = false;
                    for (int i = 0; i < nn; i++) {
                        const bool me = WiFi.SSID(i) == cfgSsid.c_str();
                        found |= me;
                        talk::logf("[brain] scan %s ch=%d rssi=%d auth=%d%s\n", me ? "(設定した Wi-Fi)" : WiFi.SSID(i).c_str(), WiFi.channel(i), WiFi.RSSI(i), (int)WiFi.encryptionType(i), me ? " <=" : "");
                    }
                    talk::logf("[brain] scan done n=%d found=%d ssid_len=%u pass_len=%u\n", nn, found, (unsigned)cfgSsid.size(), (unsigned)cfgPass.size());
                    WiFi.scanDelete();
                }
                wifiTry = millis();
                WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
                talk::logf("[brain] wifi connecting (status=%d)\n", (int)WiFi.status());
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (!wifiReady) { wifiReady = true; talk::logf("[brain] wifi ok ip=%s rssi=%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI()); }
        if (talk::macSeen()) {  // Mac が USB で中継している間は休む
            st = Status::Yield;
            talk::setLocalLink(false);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        talk::setLocalLink(true);
        st = Status::Ready;
        pumpLocal(20);
        char ev[24];
        if (xQueueReceive(eventQ, ev, 0) == pdTRUE && (!strcmp(ev, "wake") || !strcmp(ev, "wake_cam"))) {
            talk::logf("[brain] session start (%s)\n", ev);
            bool ok = runSession(ev);
            int retries = 0;
            while (!ok && retries++ < 2 && wifiReady && !talk::macSeen()) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                ok = runSession("reconnect");
            }
            xQueueReset(eventQ);
        }
    }
}

}  // namespace

void begin()
{
    // 512 バイト以上の確保は外付けメモリ (PSRAM) へ。内部メモリは Wi-Fi と暗号化通信に残す
    heap_caps_malloc_extmem_enable(512);
    WiFi.onEvent([](WiFiEvent_t e, WiFiEventInfo_t info) {
        if (e == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) talk::logf("[brain] wifi disconnected reason=%d\n", info.wifi_sta_disconnected.reason);
    });
    loadConfig();
    eventQ  = xQueueCreate(8, 24);
    jobQ    = xQueueCreate(8, sizeof(Job*));
    resultQ = xQueueCreate(4, sizeof(Result));
    camSem  = xSemaphoreCreateBinary();
    camJpg  = (uint8_t*)ps(60000);
    lp.buf  = (uint8_t*)ps(60001);
    micQ.init(RATE * 4);
    wsBuf  = (uint8_t*)ps(WSBUF);
    pcmBuf = (uint8_t*)ps(128 * 1024);
    xTaskCreatePinnedToCore(netTask, "brain", 16384, nullptr, 2, nullptr, 0);
    xTaskCreatePinnedToCore(decideTask, "decide", 12288, nullptr, 1, nullptr, 0);
}

Status status() { return st; }

void provision(const char* name, const char* value)
{
    if (strcmp(name, "wifi_ssid") && strcmp(name, "wifi_pass") && strcmp(name, "openai_key")) return;
    Preferences p;
    p.begin("mecha", false);
    p.putString(name, value);
    p.end();
    talk::logf("[cfg] saved %s (%u chars)\n", name, (unsigned)strlen(value));
    cfgChanged = true;
}

}  // namespace brain
