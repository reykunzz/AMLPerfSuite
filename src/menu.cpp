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

#include "boost.h"
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
std::atomic<int>  gPnX0{-1}, gPnY0{-1}, gPnX1{-1}, gPnY1{-1};   // panel terbuka (sentuhan di luar panel diteruskan ke game)
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

enum { NTABS = 6, TAB_INFO = 5 };

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
    enum Kind { TOGGLE, STEP, ACTION, INFO } kind = TOGGLE;
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

// Baris hanya-baca: label di kiri, nilai di kanan (mis. status komponen boost).
Row MakeInfo(const char* label, std::function<void(char*, size_t)> text)
{
    Row r; r.kind = Row::INFO; r.label = label;
    r.val = text;
    return r;
}

Row MakeAction(const char* label, std::function<void(char*, size_t)> caption, std::function<void(int)> fn)
{
    Row r; r.kind = Row::ACTION; r.label = label;
    r.val = caption; r.change = fn;
    return r;
}

const char* kTabNames[NTABS] = {"VIEW", "STATS", "PERF", "BOOST", "SYS", "INFO"};
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

    // ---- VIEW ----
    gTabs[0] = {
        MakeChoice("OVERLAY", &Settings::overlayMode, {"OFF", "MINI", "FULL"}),
        MakeChoice("CORNER", &Settings::overlayCorner, {"TOP LEFT", "TOP RIGHT", "LOWER LEFT", "LOWER RIGHT"}),
        MakeFloat("TEXT SIZE", &Settings::overlayScale, 1.0f, 6.0f, 0.5f, "%.1f", 1.0f),
        MakeFloat("OPACITY", &Settings::overlayOpacity, 0.1f, 1.0f, 0.1f, "%.0f%%", 100.0f),
        MakeToggle("GRAPH", &Settings::showGraph, true),
        MakeChoice("GRAPH TYPE", &Settings::graphMode, {"FRAME MS", "FPS"}),
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

    // ---- BOOST: mode + status nyata tiap komponen di perangkat ini ----
    gTabs[3] = {
        MakeChoice("BOOST", &Settings::boostMode, {"OFF", "ON", "MAX"}, false),
        MakeInfo("THREAD PRIO", [](char* o, size_t n) {
            if(g_boost.mode == 0 || !g_perf.user.boostPriority) snprintf(o, n, "OFF");
            else if(g_boost.prioOk) snprintf(o, n, "NICE %d", g_boost.nice);
            else snprintf(o, n, "DENIED");
        }),
        MakeInfo("BIG CORES", [](char* o, size_t n) {
            if(g_boost.mode == 0 || !g_perf.user.boostBigCores) snprintf(o, n, "OFF");
            else if(g_boost.coresOk) Boost_MaskText(g_boost.bigMask, o, n);
            else snprintf(o, n, "N/A");
        }),
        MakeInfo("PERF HINT", [](char* o, size_t n) {
            if(g_boost.mode == 0 || !g_perf.user.boostPerfHint) snprintf(o, n, "OFF");
            else snprintf(o, n, "%s", g_boost.hintOk ? "ON" : "N/A");
        }),
        MakeInfo("VSYNC", [](char* o, size_t n) {
            snprintf(o, n, "%s", g_boost.vsyncUnlocked ? "UNLOCKED" : "NORMAL");
        }),
        MakeInfo("GL MONITORS", [](char* o, size_t n) {
            snprintf(o, n, "%s", g_boost.lean ? "PAUSED" : "ACTIVE");
        }),
    };

    // ---- SYS ----
    gTabs[4] = {
        MakeToggle("DRAW MONITOR", &Settings::drawCallMonitor, false, true),
        MakeToggle("TEXTURE MONITOR", &Settings::textureMemoryMonitor, false, true),
        MakeToggle("TOASTS", &Settings::showToasts),
        MakeChoice("MENU BUTTON", &Settings::menuButtonPos, {"TOP LEFT", "TOP RIGHT", "LOWER LEFT", "LOWER RIGHT", "TOP MID"}),
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
// Awalan baris:  '#' = judul bagian   '|' = lanjutan/keterangan (redup, menjorok)   '+' = butir.
// Maksimal 8 baris per halaman dan lebar teks muat di panel (diperiksa tes). Tulis HURUF KAPITAL:
// Font_Case() (font.h) mengubahnya menjadi huruf kalimat saat digambar.
struct InfoPage { const char* title; const char* lines[10]; };

const InfoPage kInfo[] = {
    { "CHANGELOG", {
        "#V1.2.2 - FONT LEBIH TEBAL",
        "#V1.2.1 - FONT HALUS BARU",
        "#V1.2.0 - BOOST + MENU BARU",
        "+ FITUR BOOST PERFORMANCE",
        "+ MENU SIMPEL, DI KIRI LAYAR",
        "#V1.1.0 - TAB INFO, AUTHOR",
        "#V1.0.0 - RILIS AWAL",
        "+ OVERLAY, LIMITER, PRESET",
        nullptr } },
    { "STATS - FUNGSI", {
        "FPS: FRAME/DETIK, GTA SA 30",
        "FRAME TIME: DURASI 1 FRAME",
        "|KECIL = HALUS.",
        "RAM FREE: RAM HP YANG BEBAS",
        "APP MEMORY: RAM DIPAKAI GAME",
        "DRAW CALLS: PERINTAH GAMBAR",
        "|PER FRAME. BANYAK = BERAT.",
        "TEXTURE MEM: PERKIRAAN GPU",
        nullptr } },
    { "PERF - FUNGSI", {
        "PRESET: PAKET SETELAN",
        "|LOW SAMPAI ULTRA.",
        "FPS LIMIT: BATAS FPS MAKS.",
        "|HANYA MENURUNKAN FPS.",
        "ADAPTIVE: BATAS TURUN/NAIK",
        "|OTOMATIS SESUAI HP.",
        "LOW MEMORY: HEMAT SAAT RAM",
        "|TIPIS (TRIM MEMORI, 30 FPS)",
        nullptr } },
    { "BOOST - FUNGSI", {
        "ON: PRIORITAS THREAD GAME,",
        "|CORE BESAR CPU, PERF HINT.",
        "MAX: ON + BUKA VSYNC +",
        "|JEDA MONITOR DRAW/TEKSTUR.",
        "MAX = LEBIH PANAS, BOROS.",
        "FPS TAK MELEBIHI BATAS GAME.",
        "|HASIL: LEBIH SEDIKIT LAG.",
        "CEK STATUS DI TAB BOOST.",
        nullptr } },
    { "VIEW DAN SYS - FUNGSI", {
        "OVERLAY, CORNER: MODE DAN",
        "|POJOK OVERLAY.",
        "TEXT SIZE, OPACITY: UKURAN",
        "|DAN KEPEKATAN OVERLAY.",
        "GRAPH: GRAFIK FRAME/FPS",
        "DRAW, TEXTURE MONITOR:",
        "|PEMANTAU GL (* = RESTART)",
        "MENU BUTTON, SAVE, RESET.",
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
        if(r.kind == Row::INFO) return;
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
// Tampilan sengaja polos: panel gelap, satu warna aksen, tanpa hiasan.
struct C { float r, g, b, a; };

const C kPanel  = {0.065f, 0.070f, 0.080f, 0.93f};
const C kLine   = {0.30f, 0.32f, 0.36f, 1.00f};
const C kAccent = {0.20f, 0.50f, 0.80f, 1.00f};
const C kHead   = {0.50f, 0.74f, 0.96f, 1.00f};
const C kText   = {0.90f, 0.92f, 0.94f, 1.00f};
const C kDim    = {0.56f, 0.59f, 0.64f, 1.00f};
const C kBtn    = {0.17f, 0.18f, 0.21f, 1.00f};
const C kValBg  = {0.11f, 0.12f, 0.14f, 1.00f};
const C kStripe = {1.00f, 1.00f, 1.00f, 0.035f};
const C kRed    = {1.00f, 0.50f, 0.45f, 1.00f};
const C kOrange = {1.00f, 0.66f, 0.22f, 1.00f};

struct Ctx
{
    const MenuPainter* P;
    float S;        // piksel layar per unit (1 unit = 1 piksel font 5x7)
    float line;     // tebal garis tipis, dalam piksel
    void rect(float x, float y, float w, float h, const C& c) const { P->rect(x, y, w, h, c.r, c.g, c.b, c.a); }
    void text(float x, float y, const char* s, const C& c) const { P->text(x, y, s, S, c.r, c.g, c.b, c.a); }
    float tw(const char* s) const { return P->textWidth(s, S); }
    void textCenter(float x, float y, float w, float h, const char* s, const C& c) const
    {
        text(x + (w - tw(s)) * 0.5f, y + (h - 7 * S) * 0.5f, s, c);
    }
    void textRight(float xr, float y, float h, const char* s, const C& c) const
    {
        text(xr - tw(s), y + (h - 7 * S) * 0.5f, s, c);
    }
    void frame(float x, float y, float w, float h, const C& c) const
    {
        rect(x, y, w, line, c); rect(x, y + h - line, w, line, c);
        rect(x, y + line, line, h - 2 * line, c); rect(x + w - line, y + line, line, h - 2 * line, c);
    }
};

void AddHit(float x, float y, float w, float h, int target)
{
    gHits.push_back(Hit{x, y, x + w, y + h, target});
}

// Ukuran panel dalam unit.
const float PW = 190, PH = 150;
const float RH = 13;            // tinggi satu baris
const int   MAX_ROWS = 7;

// Skala panel: kecil, tidak lebih dari ±48% tinggi dan ±28% lebar layar.
float PanelScale(int W, int H)
{
    float s = floorf(fminf(0.48f * H / PH, 0.28f * W / PW));
    return s < 2.0f ? 2.0f : s;
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
    g.rect(x, y, bw, bh, C{0.04f, 0.045f, 0.05f, 0.55f});
    g.frame(x, y, bw, bh, C{kLine.r, kLine.g, kLine.b, 0.7f});
    g.textCenter(x, y, bw, bh, label, ok ? C{kText.r, kText.g, kText.b, 0.85f} : kRed);

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

void DrawInfoPage(const Ctx& g, float px, float rowsY)
{
    const float S = g.S;
    const InfoPage& pg = kInfo[gInfoPage];
    const float areaH = MAX_ROWS * RH;

    g.text(px + 6 * S, rowsY + 2 * S, pg.title, kHead);
    g.rect(px + 6 * S, rowsY + 11.5f * S, (PW - 12) * S, g.line, kLine);

    float y = rowsY + 14.5f * S;
    for(int i = 0; pg.lines[i]; ++i)
    {
        const char* l = pg.lines[i];
        const C* col = &kText;
        float x = px + 6 * S;
        if(l[0] == '#')      { col = &kHead; ++l; }
        else if(l[0] == '|') { col = &kDim; x += 6 * S; ++l; }
        g.text(x, y, l, *col);
        y += 9.2f * S;
    }
    (void)areaH;
    AddHit(px + 4 * S, rowsY, (PW - 8) * S, areaH * S, T_NEXT);   // ketuk teks = halaman berikutnya
}

void DrawRow(const Ctx& g, float px, float ry, int i, Row& r)
{
    const float S = g.S;
    if(i & 1) g.rect(px + 3 * S, ry, (PW - 6) * S, RH * S, kStripe);

    char val[40] = {0};
    r.val(val, sizeof(val));

    const float xl = px + 6 * S, xr = px + (PW - 6) * S;
    const float bh = 10 * S, by = ry + 1.5f * S;

    if(r.kind == Row::ACTION)
    {
        bool armed = strncmp(val, "TAP", 3) == 0;
        g.rect(xl, by, xr - xl, bh, armed ? C{0.70f, 0.30f, 0.10f, 1.0f} : kBtn);
        g.textCenter(xl, by, xr - xl, bh, val, kText);
        AddHit(px + 3 * S, ry, (PW - 6) * S, RH * S, T_ROW + i * 4);
        return;
    }

    char label[40];
    snprintf(label, sizeof(label), "%s%s", r.label, r.restart ? " *" : "");
    g.text(xl, ry + (RH - 7) * 0.5f * S, label, kText);

    if(r.kind == Row::INFO)
    {
        bool good = strcmp(val, "OFF") != 0 && strcmp(val, "N/A") != 0 && strcmp(val, "DENIED") != 0 && strcmp(val, "NORMAL") != 0 && strcmp(val, "ACTIVE") != 0;
        g.textRight(xr, ry, RH * S, val, good ? kHead : kDim);
        return;
    }

    const float cw = 78 * S;          // lebar area kontrol
    const float cx = xr - cw;
    if(r.kind == Row::TOGGLE)
    {
        bool on = strcmp(val, "ON") == 0;
        g.rect(cx, by, cw, bh, on ? kAccent : kValBg);
        g.textCenter(cx, by, cw, bh, val, on ? kText : kDim);
        AddHit(px + 3 * S, ry, (PW - 6) * S, RH * S, T_ROW + i * 4);
    }
    else
    {
        const float bw = 10 * S, vw = 56 * S, gap = 1 * S;
        g.rect(cx, by, bw, bh, kBtn);
        g.textCenter(cx, by, bw, bh, "-", kText);
        AddHit(cx - 2 * S, ry, bw + 3 * S, RH * S, T_ROW + i * 4 + 1);

        g.rect(cx + bw + gap, by, vw, bh, kValBg);
        g.textCenter(cx + bw + gap, by, vw, bh, val, kText);

        g.rect(cx + bw + gap + vw + gap, by, bw, bh, kBtn);
        g.textCenter(cx + bw + gap + vw + gap, by, bw, bh, "+", kText);
        AddHit(cx + bw + gap + vw, ry, bw + 3 * S, RH * S, T_ROW + i * 4 + 2);
    }
}

void DrawPanel(const Ctx& g, int W, int H, int64_t now)
{
    const float S = g.S;
    // sisi kiri layar, sejajar vertikal di tengah; layar di luar panel tetap bisa dimainkan
    const float px = floorf(4 * S), py = floorf((H - PH * S) * 0.5f);
    (void)W;

    g.rect(px, py, PW * S, PH * S, kPanel);
    g.frame(px, py, PW * S, PH * S, kLine);
    gPnX0 = (int)px; gPnY0 = (int)py; gPnX1 = (int)(px + PW * S); gPnY1 = (int)(py + PH * S);

    // ---- header ----
    g.text(px + 6 * S, py + 4 * S, AMLPERF_NAME, kText);
    {
        char who[48];
        snprintf(who, sizeof(who), "AUTHOR : %s", AMLPERF_AUTHOR);
        g.text(px + 6 * S, py + 13.5f * S, who, kDim);
        char ver[16];
        snprintf(ver, sizeof(ver), "V%s", AMLPERF_VERSION);
        g.text(px + (PW - 6) * S - g.tw(ver), py + 13.5f * S, ver, kDim);

        float bx = px + (PW - 14) * S, by = py + 2 * S, bs = 11 * S;
        g.textCenter(bx, by, bs, bs, "X", kText);
        float pad = 3 * S;
        AddHit(bx - pad, by - pad, bs + pad * 2, bs + pad * 2, T_CLOSE);
    }
    g.rect(px + 3 * S, py + 23.5f * S, (PW - 6) * S, g.line, kLine);

    // ---- tab: lebar menyesuaikan teks ----
    {
        float sumText = 0;
        for(int i = 0; i < NTABS; ++i) sumText += g.tw(kTabNames[i]) / S;
        const float extra = ((PW - 8) - sumText) / NTABS;
        float tx = px + 4 * S;
        const float ty = py + 26.5f * S, th = 12 * S;
        for(int i = 0; i < NTABS; ++i)
        {
            float tw = (g.tw(kTabNames[i]) / S + extra) * S;
            bool on = i == gTab;
            g.rect(tx + 0.5f * S, ty, tw - 1 * S, th, on ? kAccent : kValBg);
            g.textCenter(tx, ty, tw, th, kTabNames[i], on ? kText : kDim);
            AddHit(tx, ty, tw, th, T_TAB + i);
            tx += tw;
        }
    }

    // ---- isi tab ----
    const float rowsY = py + 41 * S;
    if(gTab == TAB_INFO)
        DrawInfoPage(g, px, rowsY);
    else
    {
        std::vector<Row>& rows = gTabs[gTab];
        for(int i = 0; i < (int)rows.size() && i < MAX_ROWS; ++i)
            DrawRow(g, px, rowsY + i * RH * S, i, rows[i]);
    }

    // ---- footer ----
    const float fy = rowsY + MAX_ROWS * RH * S + 3 * S;
    g.rect(px + 3 * S, fy - 2 * S, (PW - 6) * S, g.line, kLine);
    if(gTab == TAB_INFO)
    {
        char pg[32];
        snprintf(pg, sizeof(pg), "PAGE %d/%d", gInfoPage + 1, kInfoPageCount);
        g.text(px + 6 * S, fy + 2 * S, pg, kDim);
        const float bw = 14 * S, bh = 9 * S;
        float bx2 = px + (PW - 6) * S - bw, bx = bx2 - bw - 2 * S;
        g.rect(bx, fy + 1 * S, bw, bh, kBtn);  g.textCenter(bx, fy + 1 * S, bw, bh, "<", kText);
        g.rect(bx2, fy + 1 * S, bw, bh, kBtn); g.textCenter(bx2, fy + 1 * S, bw, bh, ">", kText);
        AddHit(bx - 2 * S, fy - 1 * S, bw + 4 * S, bh + 4 * S, T_PREV);
        AddHit(bx2 - 2 * S, fy - 1 * S, bw + 4 * S, bh + 4 * S, T_NEXT);
    }
    else
    {
        char left[64];
        if(now - gStatusAt < 3 * kSec && gStatus[0]) snprintf(left, sizeof(left), "%s", gStatus);
        else if(gDirty) snprintf(left, sizeof(left), "UNSAVED...");
        else snprintf(left, sizeof(left), "* = RESTART GAME");
        g.text(px + 6 * S, fy + 2 * S, left, gDirty ? kOrange : kDim);

        char right[48] = {0};
        const C* rc = &kDim;
        if(!gInputOk.load()) { snprintf(right, sizeof(right), "NO TOUCH HOOK"); rc = &kRed; }
        else if(U().menuDebug)
            snprintf(right, sizeof(right), "T%d I%d X%d Y%d", gDbgType.load(), gDbgId.load(), gDbgX.load(), gDbgY.load());
        if(right[0]) g.text(px + (PW - 6) * S - g.tw(right), fy + 2 * S, right, *rc);
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
        bool inPanel = gOpen.load() && x >= gPnX0.load() && x < gPnX1.load() && y >= gPnY0.load() && y < gPnY1.load();
        gCaptured[id] = inPanel || inBtn;
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
        gPnX0 = gPnY0 = gPnX1 = gPnY1 = -1;
        return;
    }

    // 3) gambar
    const float S = PanelScale(W, H);
    Ctx g{&P, S, fmaxf(1.0f, floorf(S * 0.5f))};

    if(gOpen.load())
    {
        gBtnX0 = gBtnY0 = gBtnX1 = gBtnY1 = -1;
        DrawPanel(g, W, H, now);
    }
    else
    {
        gPnX0 = gPnY0 = gPnX1 = gPnY1 = -1;
        DrawButton(g, W, H, now);
        if(u.menuDebug) DrawDebugTouch(g, W, H, now);
    }
}
