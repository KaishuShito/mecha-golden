// メカ・ゴールデンレトリバー (StackChan / M5Stack CoreS3 用デジタルペット)
// 絵はすべてコードで描いている (外部画像なし)。
// 状態の種類は Codex Pets の 9 行 (idle / waving / jumping / waiting / running / review / failed) を参考にした。
// サーボ (首) はこのスケッチでは動かさない。
// 会話モード (talk.cpp): Mac の bridge/live_bridge.py がつながると、GPT-Live と話す。

#include <Arduino.h>
#include <M5StackChan.h>
#include <math.h>
#include <esp_log.h>
#include "talk.h"
#include "camera.h"
#include "brain.h"
#include "duplex.h"
#include "i2cgate.h"
#include "sprites.h"

static constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static uint16_t mix(uint16_t a, uint16_t b, float t)
{
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
    int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
    return (uint16_t)((int(ar + (br - ar) * t) << 11) | (int(ag + (bg - ag) * t) << 5) | int(ab + (bb - ab) * t));
}

namespace C {
constexpr uint16_t BG_TOP  = rgb(16, 22, 44);
constexpr uint16_t BG_BOT  = rgb(40, 52, 92);
constexpr uint16_t FLOOR   = rgb(52, 64, 104);
constexpr uint16_t FLOOR_L = rgb(70, 84, 128);
constexpr uint16_t DOT     = rgb(48, 66, 110);
constexpr uint16_t SHADOW  = rgb(30, 36, 64);
constexpr uint16_t OUTLINE = rgb(74, 42, 18);
constexpr uint16_t FUR     = rgb(232, 166, 72);
constexpr uint16_t FUR_L   = rgb(250, 210, 136);
constexpr uint16_t FUR_D   = rgb(196, 124, 46);
constexpr uint16_t METAL   = rgb(164, 176, 190);
constexpr uint16_t METAL_L = rgb(226, 232, 240);
constexpr uint16_t METAL_D = rgb(92, 102, 118);
constexpr uint16_t METAL_O = rgb(44, 50, 64);
constexpr uint16_t CYAN    = rgb(70, 236, 255);
constexpr uint16_t CYAN_D  = rgb(18, 70, 100);
constexpr uint16_t EYE     = rgb(28, 24, 36);
constexpr uint16_t WHITE   = rgb(255, 255, 255);
constexpr uint16_t NOSE    = rgb(40, 30, 32);
constexpr uint16_t MOUTH   = rgb(120, 34, 44);
constexpr uint16_t TONGUE  = rgb(255, 122, 142);
constexpr uint16_t TONGUE_D= rgb(214, 78, 100);
constexpr uint16_t BLUSH   = rgb(255, 146, 140);
constexpr uint16_t RED     = rgb(255, 84, 96);
constexpr uint16_t PINK    = rgb(255, 112, 168);
constexpr uint16_t SWEAT   = rgb(130, 206, 255);
constexpr uint16_t TEXT    = rgb(232, 238, 252);
constexpr uint16_t PILL    = rgb(28, 38, 70);
constexpr uint16_t YELLOW  = rgb(255, 226, 90);
constexpr uint16_t TRANSP  = rgb(255, 0, 255);
}  // namespace C

static M5Canvas canvas(&M5.Display);
static M5Canvas bg(&M5.Display);
static M5Canvas head(&canvas);
static uint8_t* snapBuf = nullptr;
static int headRest = 50;  // 休みの角度 (0.1° 単位)
static M5Canvas spr(&canvas);  // ドット絵 1 コマ (SPR_W x SPR_H)。3 倍にして描く

static constexpr int W = 320, H = 240;
static constexpr int HW = 200, HH = 176, HX = 100, HY = 86;  // 頭スプライトと、その中の頭の中心

enum State : uint8_t { IDLE, WAVING, JUMPING, WAITING, WORKING, REVIEW, FAILED, PETTED, PAW, STATE_COUNT };
struct StateInfo {
    const char* id;
    const char* label;
    uint32_t dur;
};
static const StateInfo INFO[STATE_COUNT] = {
    {"idle", "まったり", 0},          {"waving", "やっほー!", 2600},  {"jumping", "ジャンプ!", 1900},
    {"waiting", "なになに?", 4200},   {"running", "おしごと中", 5000}, {"review", "チェック中", 4200},
    {"failed", "しっぱい…", 3200},    {"petted", "なでなで!", 3000},    {"paw", "お手!", 2200},
};

enum Eyes : uint8_t { EYE_NORMAL, EYE_BLINK, EYE_HAPPY, EYE_VISOR, EYE_NARROW, EYE_SAD };
struct Face {
    float lookX = 0, lookY = 0;
    Eyes eyes = EYE_NORMAL;
    bool mouthOpen = false, tongue = false, sadMouth = false, browUp = false;
    float mouthAmt = 0;
    float earA = 0.22f, earDrop = 0, blush = 0.5f, scan = 0, pant = 0, antennaWob = 0;
    uint16_t antenna = C::CYAN;
    uint8_t ledPhase = 0;
};

struct Particle {
    float x, y, vx, vy;
    uint32_t born;
    bool alive;
};
static Particle parts[8];

static State state = IDLE;
static uint32_t stateStart = 0, nextAuto = 0, nextBlink = 0, blinkUntil = 0, nextLook = 0, lookHold = 0;
static uint32_t lastSpawn = 0, lastLed = 0, frames = 0, fpsT = 0;
static float lookX = 0, lookY = 0, lookTX = 0, lookTY = 0;

static void setState(State s)
{
    state      = s;
    stateStart = millis();
    if (s == IDLE) nextAuto = millis() + random(7000, 13000);
    talk::logf("[state] %s\n", INFO[s].id);
}

// 回転した楕円を三角形の扇で塗る
static void rotEllipse(LovyanGFX& g, float cx, float cy, float rx, float ry, float a, uint16_t c)
{
    constexpr int N = 22;
    const float ca = cosf(a), sa = sinf(a);
    float px[N], py[N];
    for (int i = 0; i < N; i++) {
        const float th = i * 2 * PI / N, x = rx * cosf(th), y = ry * sinf(th);
        px[i] = cx + x * ca - y * sa;
        py[i] = cy + x * sa + y * ca;
    }
    for (int i = 0; i < N; i++) {
        const int j = (i + 1) % N;
        g.fillTriangle(lroundf(cx), lroundf(cy), lroundf(px[i]), lroundf(py[i]), lroundf(px[j]), lroundf(py[j]), c);
    }
}

static void oEllipse(LovyanGFX& g, int x, int y, int rx, int ry, uint16_t c, int ol = 2)
{
    g.fillEllipse(x, y, rx + ol, ry + ol, C::OUTLINE);
    g.fillEllipse(x, y, rx, ry, c);
}

static void oRoundRect(LovyanGFX& g, int x, int y, int w, int h, int r, uint16_t c, uint16_t ol = C::OUTLINE)
{
    g.fillRoundRect(x - 2, y - 2, w + 4, h + 4, r + 2, ol);
    g.fillRoundRect(x, y, w, h, r, c);
}

static void heart(LovyanGFX& g, int x, int y, int s, uint16_t c)
{
    g.fillCircle(x - s / 2, y - s / 4, s / 2 + 1, c);
    g.fillCircle(x + s / 2, y - s / 4, s / 2 + 1, c);
    g.fillTriangle(x - s - 1, y - s / 8, x + s + 1, y - s / 8, x, y + s, c);
}

static void sparkle(LovyanGFX& g, int x, int y, int s, uint16_t c)
{
    g.fillTriangle(x - s, y, x + s, y, x, y - s * 2, c);
    g.fillTriangle(x - s, y, x + s, y, x, y + s * 2, c);
    g.fillTriangle(x, y - s, x, y + s, x - s * 2, y, c);
    g.fillTriangle(x, y - s, x, y + s, x + s * 2, y, c);
}

/* ---------------------------------------------------------------- 背景 */
static void buildBackground()
{
    for (int y = 0; y < H; y++) bg.drawFastHLine(0, y, W, mix(C::BG_TOP, C::BG_BOT, y / 214.0f));
    for (int y = 14; y < 206; y += 24)
        for (int x = 12 + ((y / 24) % 2) * 12; x < W; x += 24) bg.fillCircle(x, y, 1, C::DOT);
    bg.fillRect(0, 214, W, H - 214, C::FLOOR);
    bg.drawFastHLine(0, 214, W, C::FLOOR_L);
    for (int i = -6; i <= 6; i++) bg.drawLine(160 + i * 18, 214, 160 + i * 60, H, C::FLOOR_L);
}

/* ---------------------------------------------------------------- 頭 (head スプライトに描く) */
static void drawEye(LovyanGFX& g, int ex, int ey, const Face& f, bool left)
{
    switch (f.eyes) {
        case EYE_NORMAL:
        case EYE_SAD: {
            const int x = ex + lroundf(f.lookX * 4);
            const int y = ey + lroundf(f.lookY * 3) + (f.eyes == EYE_SAD ? 3 : 0);
            g.fillEllipse(x, y, 11, 14, C::EYE);
            g.fillCircle(x + 4, y - 6, 4, C::WHITE);
            g.fillCircle(x - 4, y + 6, 2, C::CYAN);
            if (f.eyes == EYE_SAD) {
                for (int k = 0; k < 3; k++) {
                    if (left) g.drawLine(ex - 11, ey - 15 + k, ex + 9, ey - 22 + k, C::OUTLINE);
                    else g.drawLine(ex - 9, ey - 22 + k, ex + 11, ey - 15 + k, C::OUTLINE);
                }
            }
            break;
        }
        case EYE_BLINK:
            g.fillRoundRect(ex - 11, ey, 22, 4, 2, C::EYE);
            break;
        case EYE_HAPPY:
            g.fillArc(ex, ey + 6, 9, 13, 200, 340, C::EYE);
            break;
        case EYE_NARROW: {
            const int x = ex + lroundf(f.lookX * 5);
            g.fillEllipse(x, ey + 3, 11, 7, C::EYE);
            g.fillCircle(x + 4, ey + 1, 2, C::WHITE);
            g.fillRect(ex - 13, ey - 6, 26, 5, C::FUR);
            g.fillRect(ex - 13, ey - 2, 26, 2, C::OUTLINE);
            if (f.browUp && !left) {
                for (int k = 0; k < 3; k++) g.drawLine(ex - 9, ey - 16 + k, ex + 10, ey - 21 + k, C::OUTLINE);
            }
            break;
        }
        case EYE_VISOR:
            break;
    }
}

static void drawEar(LovyanGFX& g, float px, float py, float a, float drop, bool left)
{
    const float ry = 34, rx = 18;
    const float ang = left ? a : -a;
    const float cx = px - 0.8f * ry * sinf(ang);
    const float cy = py + 0.8f * ry * cosf(ang) + drop;
    rotEllipse(g, cx, cy, rx + 2, ry + 2, ang, C::OUTLINE);
    rotEllipse(g, cx, cy, rx, ry, ang, C::FUR_D);
    rotEllipse(g, cx + (left ? 3 : -3), cy - 4, rx - 8, ry - 12, ang, mix(C::FUR_D, C::FUR, 0.45f));
}

static void drawHead(const Face& f)
{
    LovyanGFX& g = head;
    g.fillScreen(C::TRANSP);
    const int hx = HX, hy = HY;

    // アンテナ
    const int ax0 = hx + 18, ay0 = hy - 56, ax1 = hx + 24 + lroundf(f.antennaWob), ay1 = hy - 76;
    for (int k = -1; k <= 1; k++) g.drawLine(ax0 + k, ay0, ax1 + k, ay1, C::METAL_O);
    g.fillCircle(ax1, ay1, 6, C::METAL_O);
    g.fillCircle(ax1, ay1, 5, f.antenna);
    g.fillCircle(ax1 - 2, ay1 - 2, 1, C::WHITE);

    // 頭
    oEllipse(g, hx, hy, 66, 56, C::FUR);
    g.fillEllipse(hx, hy - 24, 30, 18, C::FUR_L);

    // ヘルメットの板
    oRoundRect(g, hx - 36, hy - 62, 72, 22, 10, C::METAL, C::METAL_O);
    g.fillRoundRect(hx - 28, hy - 58, 40, 3, 1, C::METAL_L);
    g.fillCircle(hx - 28, hy - 49, 2, C::METAL_D);
    g.fillCircle(hx + 28, hy - 49, 2, C::METAL_D);
    for (int i = 0; i < 3; i++) {
        const bool on = (f.ledPhase % 3) == i;
        g.fillCircle(hx - 12 + i * 12, hy - 49, 3, on ? C::CYAN : C::CYAN_D);
    }

    // 目
    if (f.eyes == EYE_VISOR) {
        g.fillRoundRect(hx - 46, hy - 22, 92, 28, 13, C::METAL_O);
        g.fillRoundRect(hx - 42, hy - 18, 84, 20, 10, C::CYAN_D);
        const int sx = hx - 34 + lroundf(f.scan * 68);
        g.fillRoundRect(sx - 14, hy - 15, 28, 14, 6, mix(C::CYAN_D, C::CYAN, 0.35f));
        g.fillRoundRect(sx - 6, hy - 15, 12, 14, 5, C::CYAN);
    } else {
        drawEye(g, hx - 27, hy - 6, f, true);
        drawEye(g, hx + 27, hy - 6, f, false);
    }

    // ほっぺ
    if (f.blush > 0.01f) {
        const uint16_t b = mix(C::FUR, C::BLUSH, f.blush);
        g.fillEllipse(hx - 44, hy + 18, 9, 5, b);
        g.fillEllipse(hx + 44, hy + 18, 9, 5, b);
    }

    // マズルと口
    g.fillEllipse(hx, hy + 24, 31, 22, C::FUR_L);
    if (f.mouthAmt > 0.05f) {
        const int r = 5 + lroundf(9 * f.mouthAmt);
        g.fillEllipse(hx, hy + 24 + r / 3, 10 + r / 3, 2 + r, C::OUTLINE);
        g.fillEllipse(hx, hy + 24 + r / 3, 8 + r / 3, r, C::MOUTH);
        if (r > 8) g.fillEllipse(hx, hy + 24 + r / 3 + r / 2, 6 + r / 4, r / 2, C::TONGUE);
        g.fillRect(hx - 1, hy + 18, 3, 5, C::NOSE);
    } else if (f.mouthOpen) {
        g.fillArc(hx, hy + 22, 0, 14, 0, 180, C::OUTLINE);
        g.fillArc(hx, hy + 22, 0, 12, 0, 180, C::MOUTH);
        if (f.tongue) {
            const int ty = hy + 30 + lroundf(f.pant);
            g.fillEllipse(hx, ty, 8, 6, C::TONGUE);
            g.drawFastVLine(hx, ty - 3, 6, C::TONGUE_D);
        }
        g.fillRect(hx - 1, hy + 18, 3, 5, C::NOSE);
        g.fillRect(hx - 14, hy + 21, 29, 2, C::NOSE);
    } else if (f.sadMouth) {
        g.fillArc(hx, hy + 34, 6, 8, 200, 340, C::NOSE);
    } else {
        g.fillRect(hx - 1, hy + 18, 3, 7, C::NOSE);
        g.fillArc(hx - 7, hy + 24, 6, 8, 0, 180, C::NOSE);
        g.fillArc(hx + 7, hy + 24, 6, 8, 0, 180, C::NOSE);
    }
    g.fillEllipse(hx, hy + 12, 11, 8, C::NOSE);
    g.fillEllipse(hx - 3, hy + 9, 4, 2, C::METAL_L);

    // 耳とボルト
    drawEar(g, hx - 50, hy - 30, f.earA, f.earDrop, true);
    drawEar(g, hx + 50, hy - 30, f.earA, f.earDrop, false);
    for (int s = -1; s <= 1; s += 2) {
        const int bx = hx + s * 50, by = hy - 30;
        g.fillCircle(bx, by, 8, C::METAL_O);
        g.fillCircle(bx, by, 6, C::METAL);
        g.fillCircle(bx - 2, by - 2, 2, C::METAL_L);
        g.fillCircle(bx + 1, by + 1, 2, C::METAL_D);
    }
}

/* ---------------------------------------------------------------- しっぽ */
static void drawTail(LovyanGFX& g, float x, float y, float base, float wag, float phase)
{
    constexpr int N = 5;
    float px[N + 1], py[N + 1];
    float a = base;
    px[0] = x;
    py[0] = y;
    for (int i = 0; i < N; i++) {
        a += -0.16f + wag * sinf(phase - i * 0.6f) * 0.42f;
        px[i + 1] = px[i] + cosf(a) * 11;
        py[i + 1] = py[i] + sinf(a) * 11;
    }
    for (int i = 0; i <= N; i++) g.fillCircle(lroundf(px[i]), lroundf(py[i]), 10 - i * 0.5f, C::OUTLINE);
    g.fillCircle(lroundf(px[N]), lroundf(py[N]), 12, C::OUTLINE);
    for (int i = 0; i <= N; i++) g.fillCircle(lroundf(px[i]), lroundf(py[i]), 8 - i * 0.5f, i % 2 ? C::FUR : C::FUR_D);
    g.fillCircle(lroundf(px[N]), lroundf(py[N]), 10, C::FUR_L);
    for (int i = 0; i < N; i++) {
        const int mx = lroundf((px[i] + px[i + 1]) / 2), my = lroundf((py[i] + py[i + 1]) / 2);
        g.fillCircle(mx, my, 3, C::METAL_O);
        g.fillCircle(mx, my, 2, C::METAL);
    }
}

/* ---------------------------------------------------------------- 体 */
static void drawPaw(LovyanGFX& g, int x, int y)
{
    oRoundRect(g, x, y, 24, 11, 5, C::METAL, C::METAL_O);
    g.fillRect(x + 7, y + 4, 2, 7, C::METAL_D);
    g.fillRect(x + 15, y + 4, 2, 7, C::METAL_D);
    g.fillRect(x + 3, y + 2, 12, 2, C::METAL_L);
}

static void drawBody(float t, float by, float tailBase, float tailWag, float tailSpeed, bool wave, uint16_t core)
{
    LovyanGFX& g = canvas;
    const int bx = 160, b = lroundf(by);

    drawTail(g, bx + 40, 208 + b, tailBase, tailWag, t * tailSpeed);

    oEllipse(g, bx - 44, 212 + b, 20, 13, C::FUR_D);
    oEllipse(g, bx + 44, 212 + b, 20, 13, C::FUR_D);
    oEllipse(g, bx, 186 + b, 48, 40, C::FUR);
    g.fillEllipse(bx, 194 + b, 30, 28, C::FUR_L);

    // 胸のプレートとコア
    oRoundRect(g, bx - 20, 166 + b, 40, 30, 9, C::METAL, C::METAL_O);
    g.fillRoundRect(bx - 15, 170 + b, 30, 3, 1, C::METAL_L);
    g.fillCircle(bx - 15, 191 + b, 2, C::METAL_D);
    g.fillCircle(bx + 15, 191 + b, 2, C::METAL_D);
    g.fillCircle(bx, 182 + b, 10, C::METAL_O);
    heart(g, bx, 182 + b, 7, core);

    // 前足
    oRoundRect(g, bx - 34, 196 + b, 20, 26, 9, C::FUR);
    drawPaw(g, bx - 36, 216 + b);
    if (!wave) {
        oRoundRect(g, bx + 14, 196 + b, 20, 26, 9, C::FUR);
        drawPaw(g, bx + 12, 216 + b);
    }

    // 首輪
    oRoundRect(g, bx - 40, 144 + b, 80, 12, 6, C::METAL_D, C::METAL_O);
    for (int i = -3; i <= 3; i++) g.fillCircle(bx + i * 11, 150 + b, 1, C::METAL_L);

    if (wave) {
        const float d = -0.55f + 0.4f * sinf(t * 2 * PI * 2.4f);
        const float sx = bx + 30, sy = 186 + b;
        const float cx = sx + cosf(d) * 16, cy = sy + sinf(d) * 16;
        rotEllipse(g, cx, cy, 22, 12, d, C::OUTLINE);
        rotEllipse(g, cx, cy, 20, 10, d, C::FUR);
        const int px = lroundf(sx + cosf(d) * 34), py = lroundf(sy + sinf(d) * 34);
        g.fillCircle(px, py, 12, C::METAL_O);
        g.fillCircle(px, py, 10, C::METAL);
        g.fillCircle(px - 3, py - 3, 3, C::METAL_L);
        g.fillCircle(px + 2, py + 3, 2, C::METAL_D);
    }
}

static void drawTag(int by)
{
    LovyanGFX& g = canvas;
    const int x = 160, y = 162 + by;
    g.fillCircle(x, y, 12, C::METAL_O);
    g.fillCircle(x, y, 10, C::YELLOW);
    g.setFont(&fonts::Font0);
    g.setTextDatum(middle_center);
    g.setTextColor(C::OUTLINE);
    g.drawString("AGI", x + 1, y + 1);
}

/* ---------------------------------------------------------------- 飾り */
static void drawGear(LovyanGFX& g, int x, int y, float a)
{
    for (int i = 0; i < 8; i++) {
        const float th = a + i * PI / 4;
        rotEllipse(g, x + cosf(th) * 15, y + sinf(th) * 15, 5, 4, th, C::METAL);
    }
    g.fillCircle(x, y, 14, C::METAL);
    g.fillCircle(x, y, 6, C::PILL);
}

static void spawnHeart(float x, float y)
{
    for (auto& p : parts) {
        if (!p.alive) {
            p = {x, y, (float)random(-10, 11) / 20.0f, -0.9f - random(0, 6) / 10.0f, millis(), true};
            return;
        }
    }
}

static void drawParticles()
{
    const uint32_t now = millis();
    for (auto& p : parts) {
        if (!p.alive) continue;
        const uint32_t age = now - p.born;
        if (age > 1500) {
            p.alive = false;
            continue;
        }
        p.x += p.vx;
        p.y += p.vy;
        heart(canvas, lroundf(p.x), lroundf(p.y), age > 1100 ? 5 : 7, mix(C::PINK, C::BG_TOP, age > 1100 ? (age - 1100) / 400.0f : 0));
    }
}

static void drawLabel(const char* label, uint16_t dot)
{
    canvas.setFont(&fonts::lgfxJapanGothic_16);
    const int w = canvas.textWidth(label);
    canvas.fillSmoothRoundRect(6, 6, w + 30, 24, 12, C::PILL);
    canvas.fillSmoothCircle(19, 18, 4, dot);
    canvas.setTextDatum(middle_left);
    canvas.setTextColor(C::TEXT);
    canvas.drawString(label, 28, 19);
}

// 字幕: 末尾から 2 行ぶん (幅 296px) を下に出す
static size_t utf8Len(uint8_t c) { return c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1; }

static void drawCaption(const char* text, bool user)
{
    if (!text[0]) return;
    canvas.setFont(&fonts::lgfxJapanGothic_16);
    static char lines[8][160];
    int nl = 0;
    size_t li = 0;
    lines[0][0] = 0;
    for (size_t i = 0; text[i];) {
        const size_t k = utf8Len((uint8_t)text[i]);
        if (li + k >= sizeof(lines[0]) - 1 || (li > 0 && (memcpy(lines[nl] + li, text + i, k), lines[nl][li + k] = 0, canvas.textWidth(lines[nl]) > 296))) {
            lines[nl][li] = 0;
            nl = (nl + 1) % 8;
            li = 0;
        }
        memcpy(lines[nl] + li, text + i, k);
        li += k;
        lines[nl][li] = 0;
        i += k;
    }
    const int last = nl, prev = (nl + 7) % 8;
    const bool two = lines[prev][0] && last != prev && text[0] && canvas.textWidth(text) > 296;
    const int h = two ? 46 : 26, y = 236 - h;
    canvas.fillSmoothRoundRect(4, y, 312, h, 10, user ? rgb(60, 52, 20) : C::PILL);
    canvas.setTextDatum(top_left);
    canvas.setTextColor(user ? C::YELLOW : C::TEXT);
    if (two) canvas.drawString(lines[prev], 12, y + 5);
    canvas.drawString(lines[last], 12, y + (two ? 25 : 5));
}

// 字幕 (黒い画面用): 白い文字だけ。末尾から 2 行ぶん
static void drawMinimalCaption(const char* text, bool user)
{
    if (!text[0]) return;
    canvas.setFont(&fonts::lgfxJapanGothic_16);
    static char lines[8][160];
    int nl = 0;
    size_t li = 0;
    lines[0][0] = 0;
    for (size_t i = 0; text[i];) {
        const size_t k = utf8Len((uint8_t)text[i]);
        memcpy(lines[nl] + li, text + i, k);
        lines[nl][li + k] = 0;
        if (li > 0 && (canvas.textWidth(lines[nl]) > 300 || li + k >= sizeof(lines[0]) - 1)) {
            lines[nl][li] = 0;
            nl = (nl + 1) % 8;
            li = 0;
            memcpy(lines[nl], text + i, k);
            lines[nl][k] = 0;
        }
        li += k;
        i += k;
    }
    const int prev = (nl + 7) % 8;
    const bool two = canvas.textWidth(text) > 300 && lines[prev][0];
    canvas.setTextDatum(middle_center);
    canvas.setTextColor(user ? rgb(255, 226, 120) : rgb(235, 238, 245));
    if (two) canvas.drawString(lines[prev], 160, 204);
    canvas.drawString(lines[nl], 160, two ? 224 : 222);
}

/* ---------------------------------------------------------------- LED (本体の 12 個) */
static void updateLeds(float t)
{
    if (millis() - lastLed < 80) return;
    lastLed = millis();
    const float br = 0.55f + 0.45f * sinf(t * 2.2f);
    for (int i = 0; i < 12; i++) {
        uint8_t r = 0, g = 0, b = 0;
        if (talk::active() && state == IDLE) {
            if (talk::mode() == talk::Mode::Speak) { const float l = talk::level(); r = 10 + 50 * l; g = 6 + 20 * l; b = 20 + 40 * l; }
            else { g = 14 + 20 * br; b = 24 + 26 * br; }
            M5StackChan.setRgbColor(i, r, g, b);
            continue;
        }
        switch (state) {
            case IDLE: r = 40 * br; g = 22 * br; b = 2; break;
            case WAVING:
            case JUMPING: {
                const float h = fmodf(t * 1.5f + i / 12.0f, 1.0f) * 6;
                const int k = (int)h;
                const float f = h - k;
                const uint8_t v = 40, q = v * (1 - f), u = v * f;
                const uint8_t tbl[6][3] = {{v, u, 0}, {q, v, 0}, {0, v, u}, {0, q, v}, {u, 0, v}, {v, 0, q}};
                r = tbl[k % 6][0]; g = tbl[k % 6][1]; b = tbl[k % 6][2];
                break;
            }
            case WAITING: r = g = b = 22 * br; break;
            case WORKING: {
                const int pos = ((int)(t * 10)) % 6;
                const bool on = (i % 6) == pos;
                g = on ? 40 : 6; b = on ? 50 : 8;
                break;
            }
            case REVIEW: g = 10; b = 40; break;
            case FAILED: r = 30 * br; break;
            case PETTED: r = 50 * br; g = 10 * br; b = 24 * br; break;
            default: break;
        }
        M5StackChan.setRgbColor(i, r, g, b);
    }
    M5StackChan.refreshRgb();
}

/* ---------------------------------------------------------------- setup / loop */
void setup()
{
    esp_log_level_set("*", ESP_LOG_NONE);  // ESP-IDF のログがシリアルのフレームに混ざらないように
    Serial.setRxBufferSize(8192);
    Serial.setTxBufferSize(4096);
    Serial.begin(115200);
    Serial.setTxTimeoutMs(0);
    M5StackChan.begin();
    randomSeed(esp_random());

    M5.Display.setRotation(1);
    canvas.setColorDepth(16);
    canvas.setPsram(true);
    canvas.createSprite(W, H);
    bg.setColorDepth(16);
    bg.setPsram(true);
    bg.createSprite(W, H);
    head.setColorDepth(16);
    head.setPsram(true);  // 内部メモリは Wi-Fi と暗号化通信に空けておく
    head.createSprite(HW, HH);
    head.setPivot(HX, HY);
    spr.setColorDepth(16);
    spr.setPsram(false);
    spr.createSprite(SPR_W, SPR_H);
    spr.setPivot(SPR_W / 2.0f, SPR_H / 2.0f);
    buildBackground();
    snapBuf = (uint8_t*)heap_caps_malloc(W * H * 2, MALLOC_CAP_SPIRAM);
    // しゃべりながら聞く方式を先に起動する (会話のタスクが先に動くと、M5Unified のマイクがマイク部品をリセットしてしまう)
    const bool dx = duplex::begin();
    talk::begin(snapBuf, W * H * 2, W, H, [] { return INFO[state].id; });
    if (!dx) talk::logf("[duplex] 使えないので、交互方式で動かす\n");
    cam::begin();
    brain::begin();
    M5StackChan.TouchSensor.setHoldThresh(1000);
    talk::logf("[boot] reset_reason=%d\n", (int)esp_reset_reason());
    talk::logf("[head] boot pitch=%d yaw=%d\n", M5StackChan.Motion.getCurrentPitchAngle(), M5StackChan.Motion.getCurrentYawAngle());

    setState(IDLE);
    nextBlink = millis() + 1500;
    talk::logf("[boot] mecha retriever ready\n");
}

static const State MOOD_STATE[] = {IDLE, WORKING, REVIEW, WAITING, FAILED};

void loop()
{
    // I2C を使う処理は順番待ちが取れた時だけ (取れない = カメラなどが使っている → このコマは飛ばす)
    const bool gate    = i2cgate::take(0);
    const bool camBusy = !gate;
    if (gate) { M5StackChan.update(); i2cgate::give(); }
    const int c = talk::takeCommand();
    if (c >= '0' && c < '0' + STATE_COUNT) setState((State)(c - '0'));
    // 首: 'U' = 上を向く / 'D' = 休みの角度に戻す (ゆっくり、休み +15°、5〜60° の範囲)
    if (c == 'U' || c == 'D') {
        static int last = 0;
        if (c != last) {
            const int target = c == 'U' ? 200 : 50;  // 上 = 20°、休み = 5° (公式の安全範囲 5〜85° の内側)
            M5StackChan.Motion.moveY(target, 150);
            talk::logf("[head] %s -> %d (now=%d)\n", c == 'U' ? "up" : "rest", target, M5StackChan.Motion.getCurrentPitchAngle());
            last = c;
        }
    }
    if (c == 'p') duplex::debugRegs();
    if (c == 'p' && i2cgate::take(500)) talk::logf("[status] pitch=%d yaw=%d state=%s free_heap=%u psram=%u bat_v=%.2f bat_ma=%.0f axp_level=%d vbus=%d\n", M5StackChan.Motion.getCurrentPitchAngle(), M5StackChan.Motion.getCurrentYawAngle(), INFO[state].id, ESP.getFreeHeap(), ESP.getFreePsram(),
                             M5StackChan.getBatteryVoltage(), M5StackChan.getBatteryCurrent(), M5.Power.getBatteryLevel(), (int)(M5.Power.Axp2101.getVBUSVoltage() * 1000)), i2cgate::give();
    const bool talking = talk::active();

    const uint32_t now = millis();
    const float t = now / 1000.0f;
    const uint32_t el = now - stateStart;

    // 入力
    // 画面: 顔をタッチ = 鳴き声 / 顔以外 = 手を振る・ジャンプ / 会話中の右上「おわる」= 会話をやめる
    // 頭のセンサー: タップ・連打 = 鳴き声 / 前から後ろへなでる = ごきげん / 逆なで = ちょっと不満
    //             長押し (1 秒) = 待機中なら会話を始める、会話中なら会話をやめる
    auto td = M5.Touch.getDetail();
    if (camBusy) td = decltype(td){};
    auto& ts = M5StackChan.TouchSensor;
    static int lastZone = 1;
    if (!camBusy) {
        const auto& in = ts.getIntensities();
        int best = -1, bv = 0;
        for (int z = 0; z < 3; z++) if (in[z] > bv) { bv = in[z]; best = z; }
        if (best >= 0) lastZone = best;
    }
    static const char* ZONE[] = {"front", "middle", "back"};
    char ev[48];
    const bool tapDecided = !camBusy && ts.wasDecideClickCount();
    const bool endButton  = talking && td.wasPressed() && td.x > 236 && td.y < 44;
    if (endButton) {
        talk::endTalk();
        talk::playSound(3);
        talk::logf("[touch] end button\n");
    } else {
        if (talking && talk::mode() == talk::Mode::Speak && (td.wasPressed() || tapDecided)) {
            talk::interrupt();
            talk::logf("[touch] interrupt\n");
        }
        if (td.wasPressed()) {
            const bool onHead = td.y < 150 && abs(td.x - 160) < 80;
            setState(onHead ? PETTED : (random(2) ? WAVING : JUMPING));
            if (onHead) talk::touchEvent("touch:tap:screen_face", 0);
            talk::logf("[touch] screen x=%d y=%d\n", td.x, td.y);
        }
    }
    if (td.isPressed()) {
        lookTX   = constrain((td.x - 160) / 70.0f, -1.f, 1.f);
        lookTY   = constrain((td.y - 90) / 60.0f, -1.f, 1.f);
        lookHold = now + 1500;
        if (state == PETTED) stateStart = now;
    }
    if (tapDecided) {
        const int n = ts.getClickCount();
        snprintf(ev, sizeof(ev), "touch:%s:%s", n >= 2 ? "multi_tap" : "tap", ZONE[lastZone]);
        talk::touchEvent(ev, n >= 2 ? 1 : 0);
        setState(PETTED);
    }
    if (!camBusy && ts.wasHold()) {
        if (talking) {
            talk::endTalk();
            talk::playSound(3);
        } else {
            talk::petted();
            talk::playSound(4);
        }
        talk::logf("[touch] head hold\n");
    }
    if (!camBusy && ts.wasSwipedForward()) {
        talk::touchEvent("touch:stroke:forward", 5);
        setState(PETTED);
    }
    if (!camBusy && ts.wasSwipedBackward()) {
        talk::touchEvent("touch:stroke:against", 7);
        setState(REVIEW);
    }
    {
        static const State SOUND_STATE[] = {JUMPING, JUMPING, JUMPING, PETTED, WAITING, PETTED, PETTED, REVIEW};
        const int sa = talk::takeSoundAnim();
        if (sa > 0 && sa <= 8) setState(SOUND_STATE[sa - 1]);
    }

    // 状態の切り替え (見張り犬の気分が来ていれば、その気分を長めに続ける)
    static char moodText[120];
    uint32_t moodAt     = 0;
    const uint8_t moodV = talk::mood(moodText, sizeof(moodText), moodAt);
    const bool moodFresh = moodAt && now - moodAt < 180000 && moodV < 5;
    const State moodState = moodFresh ? MOOD_STATE[moodV] : IDLE;
    const uint32_t dur = (!talking && moodFresh && state == moodState) ? 15000 : INFO[state].dur;
    if (state != IDLE && el > dur) {
        setState(IDLE);
        if (!talking && moodFresh && moodState != IDLE) nextAuto = now + random(2500, 5000);
    }
    if (talking && state == IDLE) nextAuto = now + 8000;
    if (state == IDLE && now > nextAuto) {
        if (moodFresh) {
            if (moodState != IDLE) {
                setState(moodState);
            } else {
                static const State calm[] = {WAVING, JUMPING, REVIEW};
                if (random(3) == 0) setState(calm[random(3)]);
                else nextAuto = now + random(6000, 12000);
            }
        } else {
            static const State pool[] = {WAVING, WAITING, WORKING, REVIEW, JUMPING, WAITING, WORKING, FAILED};
            setState(pool[random(sizeof(pool) / sizeof(pool[0]))]);
        }
    }

    // 視線
    if (now > lookHold && now > nextLook) {
        lookTX   = random(-10, 11) / 14.0f;
        lookTY   = random(-6, 7) / 14.0f;
        nextLook = now + random(1500, 4000);
    }
    lookX += (lookTX - lookX) * 0.18f;
    lookY += (lookTY - lookY) * 0.18f;

    // まばたき
    if (now > nextBlink) {
        blinkUntil = now + 130;
        nextBlink  = now + random(2000, 5500);
    }
    const bool blinking = now < blinkUntil;

    // 状態ごとの見た目
    Face f;
    f.lookX      = lookX;
    f.lookY      = lookY;
    f.earA       = 0.22f + 0.03f * sinf(t * 1.3f);
    f.antennaWob = 2.0f * sinf(t * 2.0f);
    f.ledPhase   = (uint8_t)(t * 3);
    f.antenna    = ((int)(t * 1.2f) % 2) ? C::CYAN : C::CYAN_D;
    float by = 1.6f * sinf(t * 2 * PI / 2.6f), jy = 0, tilt = 1.5f * sinf(t * 0.8f), hdx = 0;
    float tailBase = -0.3f, tailWag = 0.35f, tailSpeed = 2 * PI * 1.4f, shadowK = 1.0f;
    bool wave = false;
    uint16_t core = mix(C::CYAN_D, C::CYAN, 0.5f + 0.5f * sinf(t * 3.0f));

    switch (state) {
        case IDLE:
            break;
        case WAVING:
            f.eyes      = EYE_HAPPY;
            f.mouthOpen = f.tongue = true;
            f.pant      = 1.5f * sinf(t * 18);
            f.earA      = 0.36f + 0.08f * sinf(t * 14);
            f.blush     = 0.8f;
            wave        = true;
            tilt        = 6 * sinf(t * 2 * PI * 1.2f);
            tailWag     = 0.7f;
            tailSpeed   = 2 * PI * 4.5f;
            tailBase    = -0.55f;
            break;
        case JUMPING: {
            const float p = (el % 900) / 900.0f;
            float h = 0;
            if (p < 0.15f) by += 4 * (p / 0.15f);
            else if (p < 0.75f) h = sinf((p - 0.15f) / 0.6f * PI);
            else if (p < 0.9f) by += 4 * (1 - (p - 0.75f) / 0.15f);
            jy          = -30 * h;
            shadowK     = 1 - 0.35f * h;
            f.eyes      = h > 0.2f ? EYE_HAPPY : EYE_NORMAL;
            f.mouthOpen = true;
            f.tongue    = h > 0.2f;
            f.earA      = 0.22f + 0.4f * h;
            f.earDrop   = -4 * h;
            tailWag     = 0.6f;
            tailSpeed   = 2 * PI * 4;
            break;
        }
        case WAITING:
            f.lookX   = 0.6f;
            f.lookY   = -0.7f;
            tilt      = 13 + 2 * sinf(t * 2.5f);
            tailWag   = 0.25f;
            tailSpeed = 2 * PI * 0.9f;
            f.earA    = 0.3f;
            break;
        case WORKING:
            f.eyes     = EYE_VISOR;
            f.scan     = 0.5f + 0.5f * sinf(t * 4.5f);
            f.antenna  = ((int)(t * 8) % 2) ? C::YELLOW : C::CYAN_D;
            f.ledPhase = (uint8_t)(t * 10);
            tilt       = 0;
            core       = mix(C::CYAN_D, C::CYAN, 0.5f + 0.5f * sinf(t * 12));
            tailWag    = 0.15f;
            tailSpeed  = 2 * PI * 0.8f;
            break;
        case REVIEW:
            f.eyes    = EYE_NARROW;
            f.browUp  = true;
            f.lookX   = sinf(t * 1.6f);
            tilt      = -6;
            tailWag   = 0.12f;
            tailSpeed = 2 * PI * 0.5f;
            break;
        case FAILED:
            f.eyes      = EYE_SAD;
            f.sadMouth  = true;
            f.lookY     = 0.6f;
            f.earA      = 0.04f;
            f.earDrop   = 8;
            f.blush     = 0;
            f.antenna   = ((int)(t * 2) % 2) ? C::RED : C::CYAN_D;
            by         += 4;
            tilt        = -3;
            tailBase    = 0.35f;
            tailWag     = 0.06f;
            tailSpeed   = 2 * PI * 0.4f;
            core        = mix(C::CYAN_D, C::RED, 0.4f + 0.3f * sinf(t * 2));
            break;
        case PETTED:
            f.eyes      = EYE_HAPPY;
            f.mouthOpen = f.tongue = true;
            f.pant      = 1.5f * sinf(t * 16);
            f.blush     = 1.0f;
            f.earA      = 0.08f;
            tilt        = 7 * sinf(t * 2 * PI * 0.9f);
            tailWag     = 0.8f;
            tailSpeed   = 2 * PI * 5.0f;
            tailBase    = -0.55f;
            core        = mix(C::PINK, C::WHITE, 0.3f + 0.3f * sinf(t * 6));
            if (now - lastSpawn > 260) {
                lastSpawn = now;
                spawnHeart(160 + random(-70, 71), 60 + random(-10, 20));
            }
            break;
        default:
            break;
    }
    if (blinking && (f.eyes == EYE_NORMAL || f.eyes == EYE_SAD || f.eyes == EYE_NARROW)) f.eyes = EYE_BLINK;
    (void)by; (void)jy; (void)tilt; (void)hdx; (void)tailBase; (void)tailWag; (void)tailSpeed; (void)wave; (void)core; (void)shadowK;

    // ---------------------------------------------------------------- 描画: 黒い画面に、生成したドット絵の子犬 (sprites.h) と小さな文字
    // 状態 → アニメ: 0 idle 1 waving 2 jumping 3 failed 4 waiting 5 working 6 review 7 talking 8 happy 9 listening
    static const uint8_t ANIM_OF[STATE_COUNT] = {0, 1, 2, 4, 5, 6, 3, 8, 10};
    static const char* LABEL_EN[STATE_COUNT]  = {"chilling", "hi!", "yay!", "what's up?", "working...", "checking...", "oops...", "happy!", "paw!"};
    int anim          = ANIM_OF[state];
    const char* label = LABEL_EN[state];
    int forcedFrame   = -1;
    if (talking && state == IDLE) {
        if (talk::thinking()) {
            anim  = 5;
            label = "thinking...";
        } else if (talk::mode() == talk::Mode::Speak) {
            anim  = 7;
            label = "talking";
            const float lv = talk::level();  // 声の大きさで口の開き (0 閉じ / 1・3 半分 / 2 開く)
            forcedFrame    = lv < 0.05f ? 0 : (lv < 0.3f ? (((now / 120) % 2) ? 1 : 3) : 2);
        } else {
            anim  = 9;
            label = "listening...";
        }
    }
    static int curAnim = -1, curFrame = 0, shown = -1;
    static uint32_t frameAt = 0;
    if (anim != curAnim) { curAnim = anim; curFrame = 0; frameAt = now; }
    const SprAnim& A = SPR_ANIMS[anim];
    if (now - frameAt >= A.ms[curFrame]) { curFrame = (curFrame + 1) % A.count; frameAt = now; }
    const int fi = A.first + (forcedFrame >= 0 ? forcedFrame % A.count : curFrame);
    if (fi != shown) {  // コマが変わった時だけ、圧縮したドットを広げる
        shown = fi;
        uint16_t* px = (uint16_t*)spr.getBuffer();
        const uint8_t* p = SPR_FRAMES[fi];
        int k = 0;
        while (k < SPR_W * SPR_H) {
            const int cnt = *p++, ci = *p++;
            const uint16_t c = ci ? __builtin_bswap16(SPR_PAL[ci]) : __builtin_bswap16(C::TRANSP);
            for (int j = 0; j < cnt && k < SPR_W * SPR_H; j++) px[k++] = c;
        }
    }
    canvas.fillScreen(TFT_BLACK);
    spr.pushRotateZoom(&canvas, 160, 8 + SPR_H * 3 / 2, 0, 3, 3, C::TRANSP);

    // 下の文字: 会話中で字幕があれば字幕 (日本語)、なければ状態 (小さなドット文字)
    bool captionShown = false;
    if (talking) {
        static char cu[200], ca[240];
        uint32_t ua = 0, aa = 0;
        talk::captions(cu, sizeof(cu), ca, sizeof(ca), ua, aa);
        const bool userNewer = ua > aa;
        const uint32_t at    = userNewer ? ua : aa;
        if (at && now - at < 8000) {
            drawMinimalCaption(userNewer ? cu : ca, userNewer);
            captionShown = true;
        }
    }
    if (!captionShown) {
        canvas.setFont(&fonts::Font0);
        canvas.setTextSize(2);
        canvas.setTextDatum(middle_center);
        canvas.setTextColor(rgb(225, 228, 235));
        canvas.drawString(label, 160, 222);
        canvas.setTextSize(1);
    }
    {
        // 右上: 電池と Wi-Fi (目立たない灰色)。会話中は「end」(タッチで会話をやめる)
        static uint32_t batAt = 0;
        static float batV = 0, vbus = 0;
        if ((batAt == 0 || now - batAt > 10000) && i2cgate::take(0)) {
            batAt = now;
            batV  = M5StackChan.getBatteryVoltage();
            vbus  = M5.Power.Axp2101.getVBUSVoltage();
            i2cgate::give();
        }
        canvas.setFont(&fonts::Font0);
        canvas.setTextSize(1);
        canvas.setTextDatum(top_right);
        if (talking) {
            canvas.setTextSize(2);
            canvas.setTextColor(rgb(150, 150, 160));
            canvas.drawString("end x", 312, 10);
            canvas.setTextSize(1);
        } else if (batV > 2.5f) {
            const int pct  = constrain((int)lroundf((batV - 3.45f) / (4.15f - 3.45f) * 100), 0, 100);
            const bool usb = vbus > 4.0f;
            char b[16];
            snprintf(b, sizeof(b), usb ? "usb" : "%d%%", pct);
            canvas.setTextColor(pct > 15 || usb ? rgb(110, 110, 120) : C::RED);
            canvas.drawString(b, 312, 8);
            const auto bs  = brain::status();
            const bool on  = bs == brain::Status::Ready || bs == brain::Status::Talking;
            canvas.setTextColor(on ? rgb(110, 110, 120) : rgb(55, 55, 60));
            canvas.drawString("wifi", 312 - canvas.textWidth(b) - 8, 8);
        }
    }

    canvas.pushSprite(0, 0);
    if (talk::snapshotWanted()) {
        memcpy(snapBuf, canvas.getBuffer(), W * H * 2);
        talk::snapshotDone();
    }
    if (i2cgate::take(0)) { updateLeds(t); i2cgate::give(); }

    frames++;
    if (now - fpsT > 10000) {
        talk::logf("[fps] %.1f state=%s\n", frames * 1000.0f / (now - fpsT), INFO[state].id);
        frames = 0;
        fpsT   = now;
    }
}
