#include "menu.h"

#include <atomic>
#include <functional>
#include <math.h>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "config.h"
#include "perf.h"
#include "version.h"

namespace
{
// ================================================================ input
enum { EV_DOWN = 0, EV_UP = 1, EV_MOVE = 2, MAX_PTR = 16 };
struct Ev { int kind, id, x, y; };

std::mutex gQMutex;
std::vector<Ev> gQueue;

std::atomic<bool> gOpen{false};
std::atomic<bool> gEnabled{false};
std::atomic<bool> gInputOk{false};
std::atomic<int>  gBtnX0{-1}, gBtnY0{-1}, gBtnX1{-1}, gBtnY1{-1};
std::atomic<int>  gTDown{2}, gTUp{1}, gTMove{3};
std::atomic<int>  gDbgType{-1}, gDbgId{0}, gDbgX{0}, gDbgY{0};
std::atomic<int64_t> gDbgAt{0};
std::atomic<int>  gLogged{0};
bool gCaptured[MAX_PTR] = {false};     // hanya disentuh dari thread hook

// ================================================================ state menu (thread render)
struct Hit { float x0, y0, x1, y1; int target; };
std::vector<Hit> gHits;

int  gDownTarget[MAX_PTR];
bool gDownInit = false;

int  gTab = 0;
int  gInfoPage = 0;
bool gDirty = false;
int64_t gDirtyAt = 0;
char gStatus[48] = {0};
int64_t gStatusAt = 0;
int64_t gConfirmResetAt = 0;

const int64_t kMs = 1000000LL;
const int64_t kSec = 1000000000LL;

enum { NTABS = 5, TAB_INFO = 4 };

enum Target
{
    T_NONE = -1, T_BUTTON = 1, T_CLOSE = 2,
    T_PREV = 3, T_NEXT = 4,   // halaman INFO sebelumnya / berikutnya
    T_TAB = 10,               // + indeks tab
    T_ROW = 100               // + baris * 4 + bagian (0 = seluruh baris, 1 = minus, 2 = plus)
};

Settings& U() { return g_perf.user; }

void SetStatus(const char* t)
{
    snprintf(gStatus, sizeof(gStatus), "%s", t);
    gStatusAt = Perf_NowNs();
}

void MarkDirty()
{
    gDirty = true;
    gDirtyAt = Perf_NowNs();
}

void SaveNow()
{
    bool ok = Perf_SaveConfig();
    gDirty = false;
    SetStatus(ok ? "SAVED" : "SAVE FAILED");
}

// Bila preset aktif, nilai performa yang tampil adalah nilai preset. Sebelum
// pengguna mengubahnya, salin nilai itu ke pengaturan pengguna dan pindah ke CUSTOM.
void DetachPreset()
{
    Settings& u = U();
    if(u.preset == PRESET_CUSTOM && !g_perf.lowMemActive) return;
    const Settings& e = g_perf.cfg;
    u.fpsLimit = e.fpsLimit;
    u.adaptiveLimiter = e.adaptiveLimiter;
    u.showGraph = e.showGraph;
    if(u.preset != PRESET_CUSTOM)
    {
        u.preset = PRESET_CUSTOM;
        SetStatus("PRESET -> CUSTOM");
    }
}

// ================================================================ definisi baris
struct Row
{
    enum Kind { TOGGLE, STEP, ACTION } kind = TOGGLE;
    const char* label = "";
    bool restart = false;
    std::function<void(char*, size_t)> val;   // teks nilai (ACTION: teks tombol)
    std::function<void(int)> change;          // dir = -1 / +1
};

Row MakeToggle(const char* label, bool Settings::* m, bool fromEff = false, bool restart = false)
{
    Row r; r.kind = Row::TOGGLE; r.label = label; r.restart = restart;
    r.val = [=](char* o, size_t n) {
        const Settings& s = fromEff ? g_perf.cfg : g_perf.user;
        snprintf(o, n, "%s", (s.*m) ? "ON" : "OFF");
    };
    r.change = [=](int) {
        if(fromEff) DetachPreset();
        U().*m = !(U().*m);
    };
    return r;
}

Row MakeChoice(const char* label, int Settings::* m, std::vector<const char*> names, bool wrap = true)
{
    Row r; r.kind = Row::STEP; r.label = label;
    int n = (int)names.size();
    r.val = [=](char* o, size_t sz) {
        int v = g_perf.user.*m;
        if(v < 0) v = 0;
        if(v >= n) v = n - 1;
        snprintf(o, sz, "%s", names[v]);
    };
    r.change = [=](int dir) {
        int v = U().*m + dir;
        if(wrap) v = (v % n + n) % n;
        else if(v < 0) v = 0; else if(v >= n) v = n - 1;
        U().*m = v;
    };
    return r;
}

Row MakeInt(const char* label, int Settings::* m, int lo, int hi, int step)
{
    Row r; r.kind = Row::STEP; r.label = label;
    r.val = [=](char* o, size_t n) { snprintf(o, n, "%d", g_perf.user.*m); };
    r.change = [=](int dir) {
        int v = U().*m + dir * step;
        if(v < lo) v = lo;
        if(v > hi) v = hi;
        U().*m = v;
    };
    return r;
}

Row MakeFloat(const char* label, float Settings::* m, float lo, float hi, float step, const char* fmt, float mul)
{
    Row r; r.kind = Row::STEP; r.label = label;
    r.val = [=](char* o, size_t n) { snprintf(o, n, fmt, g_perf.user.*m * mul); };
    r.change = [=](int dir) {
        float v = U().*m + dir * step;
        v = roundf(v / step) * step;           // hindari drift 0.30000001
        if(v < lo) v = lo;
        if(v > hi) v = hi;
        U().*m = v;
    };
    return r;
}

// Nilai dari daftar tetap (mis. batas FPS). 0 ditampilkan sebagai zeroName.
Row MakeList(const char* label, int Settings::* m, std::vector<int> vals, const char* zeroName, bool fromEff)
{
    Row r; r.kind = Row::STEP; r.label = label;
    r.val = [=](char* o, size_t n) {
        int v = (fromEff ? g_perf.cfg : g_perf.user).*m;
        if(v == 0 && zeroName) snprintf(o, n, "%s", zeroName); else snprintf(o, n, "%d", v);
    };
    r.change = [=](int dir) {
        if(fromEff) DetachPreset();
        int cur = U().*m, idx = 0, best = 1 << 30;
        for(int i = 0; i < (int)vals.size(); ++i)
        {
            int d = abs(vals[i] - cur);
            if(d < best) { best = d; idx = i; }
        }
        idx += dir;
        if(idx < 0) idx = 0;
        if(idx >= (int)vals.size()) idx = (int)vals.size() - 1;
        U().*m = vals[idx];
    };
    return r;
}

Row MakeAction(const char* label, std::function<void(char*, size_t)> caption, std::function<void(int)> fn)
{
    Row r; r.kind = Row::ACTION; r.label = label;
    r.val = caption; r.change = fn;
    return r;
}

const char* kTabNames[NTABS] = {"DISPLAY", "STATS", "PERF", "SYSTEM", "INFO"};
std::vector<Row> gTabs[NTABS];   // tab INFO tidak punya baris (halaman teks, lihat kInfo)
bool gRowsBuilt = false;

void ResetDefaults()
{
    Settings keep = U();
    Settings d;
    d.menuEnabled = keep.menuEnabled;
    d.menuButtonPos = keep.menuButtonPos;
    d.menuDebug = keep.menuDebug;
    d.touchDownType = keep.touchDownType;
    d.touchUpType = keep.touchUpType;
    d.touchMoveType = keep.touchMoveType;
    d.touchSymbol = keep.touchSymbol;
    d.touchOffset32 = keep.touchOffset32;
    d.touchOffset64 = keep.touchOffset64;
    d.gameTuning = keep.gameTuning;
    d.lodScaleOffset32 = keep.lodScaleOffset32;
    d.lodScaleOffset64 = keep.lodScaleOffset64;
    d.lodScaleSymbol = keep.lodScaleSymbol;
    U() = d;
}

void BuildRows()
{
    if(gRowsBuilt) return;
    gRowsBuilt = true;

    // ---- DISPLAY ----
    gTabs[0] = {
        MakeChoice("OVERLAY", &Settings::overlayMode, {"OFF", "MINI", "FULL"}),
        MakeChoice("CORNER", &Settings::overlayCorner, {"TOP LEFT", "TOP RIGHT", "BTM LEFT", "BTM RIGHT"}),
        MakeFloat("TEXT SIZE", &Settings::overlayScale, 1.0f, 6.0f, 0.5f, "%.1f", 1.0f),
        MakeFloat("OPACITY", &Settings::overlayOpacity, 0.1f, 1.0f, 0.1f, "%.0f%%", 100.0f),
        MakeToggle("GRAPH", &Settings::showGraph, true),
        MakeChoice("GRAPH TYPE", &Settings::graphMode, {"FRAME TIME", "FPS"}),
        MakeInt("GRAPH SAMPLES", &Settings::graphSamples, 20, 240, 20),
    };

    // ---- STATS ----
    gTabs[1] = {
        MakeToggle("FPS", &Settings::showFps),
        MakeToggle("FRAME TIME", &Settings::showFrameTime),
        MakeToggle("RAM FREE", &Settings::showRam),
        MakeToggle("APP MEMORY", &Settings::showMemUsage),
        MakeToggle("DRAW CALLS", &Settings::showDrawCalls),
        MakeToggle("TEXTURE MEM", &Settings::showTexMem),
    };

    // ---- PERF ----
    gTabs[2] = {
        MakeChoice("PRESET", &Settings::preset, {"CUSTOM", "LOW", "MEDIUM", "HIGH", "ULTRA"}),
        MakeList("FPS LIMIT", &Settings::fpsLimit, {0, 20, 25, 30, 40, 45, 50, 60, 90, 120}, "OFF", true),
        MakeToggle("ADAPTIVE LIMIT", &Settings::adaptiveLimiter, true),
        MakeChoice("LOW MEMORY", &Settings::lowMemMode, {"OFF", "AUTO", "ALWAYS"}),
        MakeInt("LOWMEM BELOW MB", &Settings::lowMemThresholdMB, 200, 4000, 100),
        MakeInt("TRIM EVERY SEC", &Settings::lowMemTrimSec, 5, 120, 5),
    };

    // ---- SYSTEM ----
    gTabs[3] = {
        MakeToggle("DRAW CALL MON", &Settings::drawCallMonitor, false, true),
        MakeToggle("TEXTURE MON", &Settings::textureMemoryMonitor, false, true),
        MakeToggle("TOASTS", &Settings::showToasts),
        MakeChoice("MENU BUTTON", &Settings::menuButtonPos, {"TOP LEFT", "TOP RIGHT", "BTM LEFT", "BTM RIGHT", "TOP CENTER"}),
        MakeAction("SAVE NOW",
            [](char* o, size_t n) { snprintf(o, n, "SAVE NOW"); },
            [](int) { SaveNow(); }),
        MakeAction("RESET",
            [](char* o, size_t n) {
                bool armed = Perf_NowNs() - gConfirmResetAt < 3 * kSec;
                snprintf(o, n, "%s", armed ? "TAP AGAIN TO CONFIRM" : "RESET DEFAULTS");
            },
            [](int) {
                int64_t now = Perf_NowNs();
                if(now - gConfirmResetAt < 3 * kSec)
                {
                    gConfirmResetAt = 0;
                    ResetDefaults();
                    SetStatus("DEFAULTS RESTORED");
                }
                else gConfirmResetAt = now;
            }),
    };
}

// ================================================================ halaman INFO
// Awalan baris:  '#' = judul (emas)   '|' = lanjutan/keterangan (redup)   lainnya = teks biasa.
// Maksimal 34 karakter per baris (setelah awalan) dan 11 baris per halaman; huruf kapital saja.
struct InfoPage { const char* title; const char* lines[11]; };

const InfoPage kInfo[] = {
    { "CHANGELOG", {
        "#V1.1.0 - UPDATE TAMPILAN",
        "+ UI MENU BERGAYA GTA SA",
        "+ FONT BERGAYA GTA + OUTLINE",
        "+ NAMA AUTHOR : " AMLPERF_AUTHOR,
        "+ TAB INFO: CHANGELOG & FUNGSI",
        "#V1.0.0 - RILIS AWAL",
        "+ FPS, FRAME TIME, GRAFIK",
        "+ RAM, MEMORI, DRAW CALL, TEKSTUR",
        "+ LIMITER FPS, PRESET, LOW MEMORY",
        "+ OVERLAY DAN MENU SENTUH",
        nullptr } },
    { "STATS - FUNGSI TIAP BARIS", {
        "FPS: FRAME PER DETIK.",
        "|GTA SA NORMALNYA 30 FPS.",
        "FRAME TIME: DURASI 1 FRAME (MS).",
        "|MAKIN KECIL = MAKIN HALUS.",
        "RAM FREE: RAM HP YANG MASIH BEBAS.",
        "APP MEMORY: RAM YANG DIPAKAI GAME.",
        "DRAW CALLS: PERINTAH GAMBAR/FRAME.",
        "|BANYAK = GPU LEBIH BERAT.",
        "TEXTURE MEM: PERKIRAAN MEMORI",
        "|TEKSTUR (GAMBAR) DI GPU.",
        nullptr } },
    { "PERF - FUNGSI TIAP BARIS", {
        "PRESET: PAKET SETELAN SIAP PAKAI",
        "|LOW / MEDIUM / HIGH / ULTRA.",
        "FPS LIMIT: BATAS FPS MAKSIMAL.",
        "|HANYA MENURUNKAN, TAK MENAIKKAN.",
        "ADAPTIVE LIMIT: BATAS FPS TURUN",
        "|NAIK OTOMATIS SESUAI HP.",
        "LOW MEMORY: HEMAT SAAT RAM TIPIS.",
        "LOWMEM BELOW MB: AMBANG RAM.",
        "TRIM EVERY SEC: JEDA PEMBERSIHAN",
        "|MEMORI SAAT MODE HEMAT AKTIF.",
        nullptr } },
    { "DISPLAY & SYSTEM", {
        "OVERLAY: OFF / MINI / FULL.",
        "CORNER: POJOK LAYAR OVERLAY.",
        "TEXT SIZE, OPACITY: UKURAN DAN",
        "|KEPEKATAN TEKS OVERLAY.",
        "GRAPH: GRAFIK FRAME TIME / FPS.",
        "DRAW CALL MON, TEXTURE MON:",
        "|PEMANTAU GL. * = RESTART GAME.",
        "MENU BUTTON: POSISI TOMBOL MENU.",
        "SAVE NOW: SIMPAN PENGATURAN.",
        "RESET: KEMBALI KE PENGATURAN AWAL.",
        nullptr } },
};
const int kInfoPageCount = (int)(sizeof(kInfo) / sizeof(kInfo[0]));

// ================================================================ aksi
void SetOpen(bool open)
{
    if(open == gOpen.load()) return;
    gOpen = open;
    if(!open && gDirty) SaveNow();
    gStatus[0] = 0;
}

void Activate(int target)
{
    if(target == T_BUTTON) { SetOpen(true); return; }
    if(target == T_CLOSE)  { SetOpen(false); return; }
    if(target == T_PREV || target == T_NEXT)
    {
        gInfoPage = (gInfoPage + (target == T_NEXT ? 1 : kInfoPageCount - 1)) % kInfoPageCount;
        return;
    }
    if(target >= T_TAB && target < T_TAB + NTABS) { gTab = target - T_TAB; return; }
    if(target >= T_ROW && gTab != TAB_INFO)
    {
        int idx = (target - T_ROW) / 4, part = (target - T_ROW) % 4;
        std::vector<Row>& rows = gTabs[gTab];
        if(idx < 0 || idx >= (int)rows.size()) return;
        Row& r = rows[idx];
        if(r.kind == Row::ACTION) { r.change(1); return; }
        r.change(r.kind == Row::TOGGLE ? 1 : (part == 1 ? -1 : 1));
        Perf_ApplySettings();
        MarkDirty();
    }
}

int HitTest(int x, int y)
{
    for(int i = (int)gHits.size() - 1; i >= 0; --i)
    {
        const Hit& h = gHits[i];
        if(x >= h.x0 && x < h.x1 && y >= h.y0 && y < h.y1) return h.target;
    }
    return T_NONE;
}

void Process(const Ev& e)
{
    if(e.id < 0 || e.id >= MAX_PTR) return;
    if(e.kind == EV_DOWN)
        gDownTarget[e.id] = HitTest(e.x, e.y);
    else if(e.kind == EV_UP)
    {
        int t = HitTest(e.x, e.y);
        if(t != T_NONE && t == gDownTarget[e.id]) Activate(t);
        gDownTarget[e.id] = T_NONE;
    }
}

// ================================================================ gambar
struct C { float r, g, b, a; };

// Palet bergaya GTA SA: panel hitam, aksen emas/oranye, hijau uang untuk ON.
const C kPanel   = {0.025f, 0.025f, 0.030f, 0.94f};
const C kHeader  = {0.085f, 0.065f, 0.030f, 1.00f};
const C kGold    = {0.96f, 0.76f, 0.20f, 1.00f};
const C kGoldDim = {0.58f, 0.44f, 0.12f, 1.00f};
const C kTabOn   = {0.78f, 0.44f, 0.08f, 1.00f};
const C kTabOff  = {0.13f, 0.11f, 0.08f, 1.00f};
const C kRowA    = {0.060f, 0.055f, 0.050f, 0.95f};
const C kRowB    = {0.095f, 0.085f, 0.070f, 0.95f};
const C kBtn     = {0.22f, 0.18f, 0.10f, 1.00f};
const C kValBg   = {0.040f, 0.040f, 0.036f, 1.00f};
const C kOnBg    = {0.10f, 0.30f, 0.10f, 1.00f};
const C kOffBg   = {0.28f, 0.09f, 0.08f, 1.00f};
const C kWhite   = {0.98f, 0.97f, 0.94f, 1.00f};
const C kTan     = {0.88f, 0.83f, 0.70f, 1.00f};
const C kTanDim  = {0.66f, 0.62f, 0.52f, 1.00f};
const C kGray    = {0.66f, 0.64f, 0.60f, 1.00f};
const C kGreen   = {0.55f, 1.00f, 0.50f, 1.00f};
const C kRed     = {1.00f, 0.50f, 0.45f, 1.00f};
const C kOrange  = {1.00f, 0.66f, 0.22f, 1.00f};
const C kCloseBg = {0.62f, 0.11f, 0.09f, 1.00f};

struct Ctx
{
    const MenuPainter* P;
    float S;   // piksel per "unit"
    void rect(float x, float y, float w, float h, const C& c, float am = 1.0f) const { P->rect(x, y, w, h, c.r, c.g, c.b, c.a * am); }
    void text(float x, float y, const char* s, const C& c, float scale = 1.0f) const { P->text(x, y, s, S * scale, c.r, c.g, c.b, c.a); }
    float tw(const char* s, float scale = 1.0f) const { return P->textWidth(s, S * scale); }
    void textCenter(float x, float y, float w, float h, const char* s, const C& c, float scale = 1.0f) const
    {
        text(x + (w - tw(s, scale)) * 0.5f, y + (h - 7 * S * scale) * 0.5f, s, c, scale);
    }
    // Bingkai tipis (tebal t unit) di dalam kotak x,y,w,h.
    void frame(float x, float y, float w, float h, float t, const C& c) const
    {
        float b = t * S;
        rect(x, y, w, b, c); rect(x, y + h - b, w, b, c);
        rect(x, y + b, b, h - 2 * b, c); rect(x + w - b, y + b, b, h - 2 * b, c);
    }
    void box(float x, float y, float w, float h, const C& fill, const C& border) const
    {
        rect(x, y, w, h, fill);
        frame(x, y, w, h, 1.0f, border);
    }
};

void AddHit(float x, float y, float w, float h, int target)
{
    gHits.push_back(Hit{x, y, x + w, y + h, target});
}

void DrawButton(const Ctx& g, int W, int H, int64_t now)
{
    const float S = g.S;
    const bool ok = gInputOk.load();
    const char* label = ok ? "MENU" : "NO TOUCH";
    float bw = fmaxf(34 * S, g.tw(label) + 10 * S);
    float bh = 13 * S, m = 6 * S;

    float x = m, y = m;
    switch(U().menuButtonPos)
    {
        case 1: x = W - m - bw; break;
        case 2: y = H - m - bh; break;
        case 3: x = W - m - bw; y = H - m - bh; break;
        case 4: x = (W - bw) * 0.5f; break;
        default: break;
    }
    g.rect(x, y, bw, bh, C{0.03f, 0.03f, 0.04f, 0.62f});
    g.frame(x, y, bw, bh, 0.8f, ok ? C{kGold.r, kGold.g, kGold.b, 0.70f} : C{kRed.r, kRed.g, kRed.b, 0.8f});
    g.textCenter(x, y, bw, bh, label, ok ? C{kWhite.r, kWhite.g, kWhite.b, 0.85f} : kRed);

    if(ok)
    {
        float pad = 4 * S;
        AddHit(x - pad, y - pad, bw + pad * 2, bh + pad * 2, T_BUTTON);
        gBtnX0 = (int)(x - pad); gBtnY0 = (int)(y - pad);
        gBtnX1 = (int)(x + bw + pad); gBtnY1 = (int)(y + bh + pad);
    }
    else { gBtnX0 = gBtnY0 = gBtnX1 = gBtnY1 = -1; }
    (void)now;
}

// Ukuran panel dalam unit. Dipakai juga untuk menghitung skala di Menu_Frame.
const float PW = 220, PH = 166;

void DrawInfoPage(const Ctx& g, float px, float rowsY)
{
    const float S = g.S;
    const InfoPage& pg = kInfo[gInfoPage];
    const float lineH = 9.4f;

    g.rect(px + 4 * S, rowsY, (PW - 8) * S, 105 * S, kRowA);
    g.frame(px + 4 * S, rowsY, (PW - 8) * S, 105 * S, 0.8f, kGoldDim);

    g.text(px + 8 * S, rowsY + 3 * S, pg.title, kGold);
    g.rect(px + 8 * S, rowsY + 12 * S, (PW - 16) * S, 0.8f * S, kGoldDim);

    float y = rowsY + 15 * S;
    for(int i = 0; pg.lines[i]; ++i)
    {
        const char* l = pg.lines[i];
        const C* col = &kWhite;
        float x = px + 8 * S;
        if(l[0] == '#')      { col = &kGold; ++l; }
        else if(l[0] == '|') { col = &kTanDim; ++l; x += 6 * S; }
        else if(l[0] == '+') { col = &kTan; }
        g.text(x, y, l, *col, 0.92f);
        y += lineH * S * 0.92f;
    }
    // ketuk area teks = halaman berikutnya
    AddHit(px + 4 * S, rowsY, (PW - 8) * S, 105 * S, T_NEXT);
}

void DrawPanel(const Ctx& g, int W, int H, int64_t now)
{
    const float S = g.S;
    const float px = floorf((W - PW * S) * 0.5f), py = floorf((H - PH * S) * 0.5f);

    g.rect(0, 0, (float)W, (float)H, C{0, 0, 0, 0.55f});
    g.rect(px, py, PW * S, PH * S, kPanel);
    g.frame(px, py, PW * S, PH * S, 1.2f, kGold);

    // ---- header: judul (font GTA), author, versi ----
    g.rect(px + 1.2f * S, py + 1.2f * S, (PW - 2.4f) * S, 23 * S, kHeader);
    g.rect(px + 1.2f * S, py + 24 * S, (PW - 2.4f) * S, 0.9f * S, kGold);
    g.text(px + 7 * S, py + 4 * S, AMLPERF_NAME, kGold, 1.3f);

    char who[48];
    snprintf(who, sizeof(who), "AUTHOR : %s", AMLPERF_AUTHOR);
    g.text(px + 7 * S, py + 15.5f * S, who, kTan, 0.85f);
    char ver[16];
    snprintf(ver, sizeof(ver), "V%s", AMLPERF_VERSION);
    g.text(px + (PW - 27) * S - g.tw(ver, 0.85f), py + 15.5f * S, ver, kTanDim, 0.85f);
    {
        float bx = px + (PW - 22) * S, by = py + 4 * S;
        g.box(bx, by, 16 * S, 14 * S, kCloseBg, C{1.0f, 0.55f, 0.45f, 1.0f});
        g.textCenter(bx, by, 16 * S, 14 * S, "X", kWhite);
        float pad = 2 * S;
        AddHit(bx - pad, by - pad, 16 * S + pad * 2, 14 * S + pad * 2, T_CLOSE);
    }

    // ---- tab ----
    {
        float ty = py + 27.5f * S, tw = (PW - 8) / (float)NTABS;
        for(int i = 0; i < NTABS; ++i)
        {
            float tx = px + (4 + tw * i) * S;
            bool on = i == gTab;
            g.rect(tx + 0.5f * S, ty, (tw - 1) * S, 14 * S, on ? kTabOn : kTabOff);
            if(on) g.frame(tx + 0.5f * S, ty, (tw - 1) * S, 14 * S, 0.8f, kGold);
            g.textCenter(tx, ty, tw * S, 14 * S, kTabNames[i], on ? kWhite : kTan, 0.85f);
            AddHit(tx, ty, tw * S, 14 * S, T_TAB + i);
        }
    }

    // ---- isi tab ----
    const float rowsY = py + 44.5f * S;
    const float xl = px + 8 * S, xr = px + (PW - 8) * S;

    if(gTab == TAB_INFO)
        DrawInfoPage(g, px, rowsY);
    else
    {
        std::vector<Row>& rows = gTabs[gTab];
        for(int i = 0; i < (int)rows.size() && i < 7; ++i)
        {
            Row& r = rows[i];
            float ry = rowsY + i * 15 * S;
            g.rect(px + 4 * S, ry, (PW - 8) * S, 14 * S, (i & 1) ? kRowB : kRowA);

            char val[40] = {0};
            r.val(val, sizeof(val));

            if(r.kind == Row::ACTION)
            {
                bool armed = strncmp(val, "TAP", 3) == 0;
                g.box(xl, ry + 1 * S, xr - xl, 12 * S, armed ? C{0.70f, 0.28f, 0.08f, 1.0f} : kTabOn, kGold);
                g.textCenter(xl, ry + 1 * S, xr - xl, 12 * S, val, kWhite);
                AddHit(px + 4 * S, ry, (PW - 8) * S, 14 * S, T_ROW + i * 4);
                continue;
            }

            char label[40];
            snprintf(label, sizeof(label), "%s%s", r.label, r.restart ? " *" : "");
            g.text(xl, ry + 3.5f * S, label, kTan);

            const float cw = 94 * S;               // lebar area kontrol
            const float cx = xr - cw;
            if(r.kind == Row::TOGGLE)
            {
                bool on = strcmp(val, "ON") == 0;
                g.box(cx, ry + 1 * S, cw, 12 * S, on ? kOnBg : kOffBg, on ? C{0.35f, 0.75f, 0.30f, 1} : C{0.60f, 0.22f, 0.18f, 1});
                g.textCenter(cx, ry + 1 * S, cw, 12 * S, val, on ? kGreen : kRed);
                AddHit(px + 4 * S, ry, (PW - 8) * S, 14 * S, T_ROW + i * 4);
            }
            else
            {
                g.box(cx, ry + 1 * S, 14 * S, 12 * S, kBtn, kGoldDim);
                g.textCenter(cx, ry + 1 * S, 14 * S, 12 * S, "-", kGold);
                AddHit(cx - 2 * S, ry, 18 * S, 14 * S, T_ROW + i * 4 + 1);

                g.box(cx + 16 * S, ry + 1 * S, 62 * S, 12 * S, kValBg, C{0.30f, 0.26f, 0.16f, 1});
                {
                    // nilai panjang (mis. "TOP CENTER") mengecil otomatis supaya muat di kotaknya
                    float sc = fminf(1.0f, 56 * S / fmaxf(g.tw(val), 1.0f));
                    g.textCenter(cx + 16 * S, ry + 1 * S, 62 * S, 12 * S, val, kWhite, sc);
                }

                g.box(cx + 80 * S, ry + 1 * S, 14 * S, 12 * S, kBtn, kGoldDim);
                g.textCenter(cx + 80 * S, ry + 1 * S, 14 * S, 12 * S, "+", kGold);
                AddHit(cx + 78 * S, ry, 18 * S + 2 * S, 14 * S, T_ROW + i * 4 + 2);
            }
        }
    }

    // ---- footer ----
    const float fy = rowsY + 7 * 15 * S + 4 * S;
    if(gTab == TAB_INFO)
    {
        char pg[32];
        snprintf(pg, sizeof(pg), "PAGE %d/%d  TAP = NEXT", gInfoPage + 1, kInfoPageCount);
        g.text(px + 8 * S, fy + 1 * S, pg, kGray);
        float bx = px + (PW - 8 - 14 - 2 - 14) * S;
        g.box(bx, fy - 1 * S, 14 * S, 11 * S, kBtn, kGoldDim);
        g.textCenter(bx, fy - 1 * S, 14 * S, 11 * S, "<", kGold);
        AddHit(bx - 1 * S, fy - 3 * S, 16 * S, 15 * S, T_PREV);
        float bx2 = bx + 16 * S;
        g.box(bx2, fy - 1 * S, 14 * S, 11 * S, kBtn, kGoldDim);
        g.textCenter(bx2, fy - 1 * S, 14 * S, 11 * S, ">", kGold);
        AddHit(bx2 - 1 * S, fy - 3 * S, 16 * S, 15 * S, T_NEXT);
    }
    else
    {
        char left[64];
        if(now - gStatusAt < 3 * kSec && gStatus[0]) snprintf(left, sizeof(left), "%s", gStatus);
        else if(gDirty) snprintf(left, sizeof(left), "UNSAVED...");
        else snprintf(left, sizeof(left), "* = RESTART GAME");
        g.text(px + 8 * S, fy + 1 * S, left, gDirty ? kOrange : kGray);

        char right[48] = {0};
        const C* rc = &kGray;
        if(!gInputOk.load()) { snprintf(right, sizeof(right), "NO TOUCH HOOK"); rc = &kRed; }
        else if(U().menuDebug)
            snprintf(right, sizeof(right), "T%d I%d X%d Y%d", gDbgType.load(), gDbgId.load(), gDbgX.load(), gDbgY.load());
        if(right[0]) g.text(px + (PW - 8) * S - g.tw(right), fy + 1 * S, right, *rc);
    }
}

void DrawDebugTouch(const Ctx& g, int W, int H, int64_t now)
{
    // titik sentuhan terakhir (untuk memeriksa apakah koordinat sentuhan cocok dengan layar)
    if(now - gDbgAt.load() > 2 * kSec) return;
    float x = (float)gDbgX.load(), y = (float)gDbgY.load(), S = g.S;
    g.rect(x - 6 * S, y - 0.5f * S, 12 * S, 1 * S, C{1, 0.2f, 0.9f, 0.9f});
    g.rect(x - 0.5f * S, y - 6 * S, 1 * S, 12 * S, C{1, 0.2f, 0.9f, 0.9f});
    char b[48];
    snprintf(b, sizeof(b), "T%d I%d X%d Y%d", gDbgType.load(), gDbgId.load(), gDbgX.load(), gDbgY.load());
    g.text((W - g.tw(b)) * 0.5f, H - 12 * S, b, C{1, 0.5f, 0.95f, 1});
}
} // namespace

// ====================================================================== API
void Menu_SetInputOk(bool ok) { gInputOk = ok; }
bool Menu_IsOpen() { return gOpen.load(); }

bool Menu_OnTouch(int type, int id, int x, int y)
{
    gDbgType = type; gDbgId = id; gDbgX = x; gDbgY = y; gDbgAt = Perf_NowNs();
    if(gLogged.load() < 12)
    {
        gLogged.fetch_add(1);
        PerfLog("Touch type=%d id=%d x=%d y=%d", type, id, x, y);
    }
    if(!gEnabled.load() || id < 0 || id >= MAX_PTR) return false;

    int kind = type == gTDown.load() ? EV_DOWN : (type == gTUp.load() ? EV_UP : EV_MOVE);

    if(kind == EV_DOWN)
    {
        bool inBtn = x >= gBtnX0.load() && x < gBtnX1.load() && y >= gBtnY0.load() && y < gBtnY1.load();
        gCaptured[id] = gOpen.load() || inBtn;
    }
    bool consume = gCaptured[id];
    if(consume)
    {
        std::lock_guard<std::mutex> lk(gQMutex);
        if(gQueue.size() < 128) gQueue.push_back(Ev{kind, id, x, y});
    }
    if(kind == EV_UP) gCaptured[id] = false;
    return consume;
}

void Menu_Frame(int W, int H, const MenuPainter& P)
{
    const Settings& u = g_perf.user;
    gEnabled = u.menuEnabled;
    gTDown = u.touchDownType; gTUp = u.touchUpType; gTMove = u.touchMoveType;

    if(!gDownInit) { for(int i = 0; i < MAX_PTR; ++i) gDownTarget[i] = T_NONE; gDownInit = true; }
    BuildRows();
    const int64_t now = Perf_NowNs();

    // 1) proses sentuhan dengan tata letak frame sebelumnya
    std::vector<Ev> evs;
    {
        std::lock_guard<std::mutex> lk(gQMutex);
        evs.swap(gQueue);
    }
    for(const Ev& e : evs) Process(e);

    // 2) simpan otomatis 1,2 detik setelah perubahan terakhir
    if(gDirty && now - gDirtyAt > 1200 * kMs) SaveNow();

    gHits.clear();
    if(!u.menuEnabled)
    {
        gOpen = false;
        gBtnX0 = gBtnY0 = gBtnX1 = gBtnY1 = -1;
        return;
    }

    // 3) gambar
    float S = floorf(fminf(H / (PH + 4.0f), W * 0.94f / PW));
    if(S < 2.0f) S = 2.0f;
    Ctx g{&P, S};

    if(gOpen.load())
    {
        gBtnX0 = gBtnY0 = gBtnX1 = gBtnY1 = -1;
        DrawPanel(g, W, H, now);
    }
    else
    {
        DrawButton(g, W, H, now);
        if(u.menuDebug) DrawDebugTouch(g, W, H, now);
    }
}
