#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unordered_map>
#include <vector>

const char* kDefaultIni =
"; ============================================================\n"
";  AML Performance Suite - konfigurasi\n"
";  File ini dibaca ulang otomatis ~1 detik setelah disimpan.\n"
";  Kecuali dinyatakan lain, perubahan langsung berlaku tanpa restart game.\n"
"; ============================================================\n"
"\n"
"[General]\n"
"; Preset performa: Custom / Low / Medium / High / Ultra\n"
"; Custom = pakai nilai di file ini apa adanya.\n"
"Preset = Custom\n"
"; Tampilkan toast saat Low Memory Mode / preset berubah (1 = ya)\n"
"ShowToasts = 0\n"
"\n"
"[Overlay]\n"
"; 0 = mati, 1 = mini (satu baris kecil), 2 = lengkap\n"
"Mode = 1\n"
"; 0 = kiri-atas, 1 = kanan-atas, 2 = kiri-bawah, 3 = kanan-bawah\n"
"Corner = 0\n"
"; Pembesaran font (1.0 = sangat kecil, 2.0 = normal, 3.0 = besar)\n"
"Scale = 2.0\n"
"Opacity = 0.85\n"
"; Jarak dari tepi layar dalam piksel (naikkan bila terpotong notch)\n"
"Margin = 24\n"
"\n"
"; Baris yang ditampilkan di overlay\n"
"ShowFPS = 1\n"
"ShowFrameTime = 1\n"
"ShowRAM = 1\n"
"ShowMemoryUsage = 1\n"
"ShowDrawCalls = 1\n"
"ShowTextureMemory = 1\n"
"\n"
"; Batas warna: FPS >= Good hijau, >= Warn kuning, selain itu merah\n"
"FpsGood = 28\n"
"FpsWarn = 20\n"
"\n"
"[Graph]\n"
"; Grafik tampil di bawah teks pada overlay mode 2\n"
"Show = 1\n"
"; 0 = frame time (ms), 1 = FPS\n"
"Mode = 0\n"
"; Jumlah sampel (maks 240)\n"
"Samples = 120\n"
"MaxMs = 50\n"
"MaxFps = 60\n"
"\n"
"[Monitors]\n"
"; Hook di libGLESv2.so. Perubahan hanya berlaku setelah game dijalankan ulang.\n"
"; Matikan salah satunya bila game crash di perangkatmu.\n"
"DrawCallMonitor = 1\n"
"TextureMemoryMonitor = 1\n"
"\n"
"[Limiter]\n"
"; 0 = mati. Contoh: 30 / 40 / 45 / 60\n"
"; Catatan: limiter hanya MENURUNKAN FPS. Batas atas ditentukan game sendiri.\n"
"FpsLimit = 0\n"
"; 1 = otomatis turun/naik antar 60-45-40-30 (maksimum = FpsLimit)\n"
"Adaptive = 0\n"
"\n"
"[LowMemory]\n"
"; 0 = mati, 1 = otomatis (aktif bila RAM tersedia < ThresholdMB), 2 = selalu aktif\n"
"Mode = 1\n"
"ThresholdMB = 700\n"
"; Jeda (detik) pemangkasan heap native saat mode aktif\n"
"TrimIntervalSec = 10\n"
"\n"
"[GameTuning]\n"
"; OPSIONAL dan eksperimental. Preset dapat mengatur LOD distance scale game,\n"
"; tapi alamatnya berbeda tiap versi libGTASA.so. Isi salah satu:\n"
";   - offset (hex) dari base libGTASA.so, atau\n"
";   - nama simbol yang di-export libGTASA.so\n"
"; Selama Enabled = 0, bagian ini tidak menyentuh memori game.\n"
"Enabled = 0\n"
"LodScaleOffset32 = 0\n"
"LodScaleOffset64 = 0\n"
"LodScaleSymbol =\n"
"\n"
"[Boost]\n"
"; Boost performance, membantu game lebih lancar (tanpa root). Tidak menaikkan FPS di atas\n"
"; batas game sendiri; manfaatnya: lebih sedikit patah-patah dan penurunan FPS.\n"
"; 0 = mati, 1 = ON (prioritas thread + core besar + performance hint),\n"
"; 2 = MAX (ON + buka vsync + jeda monitor draw call/tekstur). MAX lebih panas dan boros baterai.\n"
"Mode = 1\n"
"; Komponen (berlaku bila Mode >= 1)\n"
"ThreadPriority = 1\n"
"BigCores = 1\n"
"PerfHint = 1\n"
"; Hanya Mode = 2\n"
"UnlockVSync = 1\n"
"PauseMonitors = 1\n"
"\n"
"[Menu]\n"
"; Menu pengaturan di dalam game. Ketuk tombol MENU di layar untuk membuka.\n"
"Enabled = 1\n"
"; Posisi tombol: 0 kiri-atas, 1 kanan-atas, 2 kiri-bawah, 3 kanan-bawah, 4 tengah-atas\n"
"Button = 4\n"
"; 1 = tampilkan titik & nilai sentuhan terakhir (untuk memeriksa input)\n"
"Debug = 0\n"
"; Nilai 'type' sentuhan dari game: jari turun / naik / geser.\n"
"; Ubah bila menu tidak merespons (lihat Debug dan file AMLPerfSuite.log).\n"
"TouchDownType = 2\n"
"TouchUpType = 1\n"
"TouchMoveType = 3\n"
"; Fungsi sentuhan libGTASA.so. Kosong = coba nama bawaan. Atau isi offset (hex).\n"
"TouchSymbol =\n"
"TouchOffset32 = 0\n"
"TouchOffset64 = 0\n";

namespace
{
typedef std::unordered_map<std::string, std::string> KV;

std::string Trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while(a < b && isspace((unsigned char)s[a])) ++a;
    while(b > a && isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string Lower(std::string s)
{
    for(auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

void Parse(const char* text, KV& kv)
{
    std::string section;
    const char* p = text;
    while(*p)
    {
        const char* e = strchr(p, '\n');
        std::string line = e ? std::string(p, e - p) : std::string(p);
        p = e ? e + 1 : p + line.size();

        line = Trim(line);
        if(line.empty() || line[0] == ';' || line[0] == '#') continue;

        if(line[0] == '[')
        {
            size_t close = line.find(']');
            if(close != std::string::npos) section = Lower(Trim(line.substr(1, close - 1)));
            continue;
        }

        size_t eq = line.find('=');
        if(eq == std::string::npos) continue;
        std::string key = Lower(Trim(line.substr(0, eq)));
        std::string val = line.substr(eq + 1);
        size_t cmt = val.find(" ;");              // komentar di akhir baris
        if(cmt != std::string::npos) val = val.substr(0, cmt);
        kv[section + "." + key] = Trim(val);
    }
}

const std::string* Find(const KV& kv, const char* k)
{
    auto it = kv.find(k);
    return it == kv.end() ? nullptr : &it->second;
}

int GetInt(const KV& kv, const char* k, int def)
{
    const std::string* v = Find(kv, k);
    if(!v || v->empty()) return def;
    return (int)strtol(v->c_str(), nullptr, 0);
}

unsigned GetUInt(const KV& kv, const char* k, unsigned def)
{
    const std::string* v = Find(kv, k);
    if(!v || v->empty()) return def;
    return (unsigned)strtoul(v->c_str(), nullptr, 0);
}

float GetFloat(const KV& kv, const char* k, float def)
{
    const std::string* v = Find(kv, k);
    if(!v || v->empty()) return def;
    return (float)atof(v->c_str());
}

bool GetBool(const KV& kv, const char* k, bool def)
{
    const std::string* v = Find(kv, k);
    if(!v || v->empty()) return def;
    std::string l = Lower(*v);
    if(l == "1" || l == "true" || l == "yes" || l == "on") return true;
    if(l == "0" || l == "false" || l == "no" || l == "off") return false;
    return def;
}

std::string GetStr(const KV& kv, const char* k, const char* def)
{
    const std::string* v = Find(kv, k);
    return v ? *v : std::string(def);
}

int ParsePreset(const std::string& v)
{
    std::string l = Lower(v);
    if(l == "low" || l == "1") return PRESET_LOW;
    if(l == "medium" || l == "med" || l == "2") return PRESET_MEDIUM;
    if(l == "high" || l == "3") return PRESET_HIGH;
    if(l == "ultra" || l == "4") return PRESET_ULTRA;
    return PRESET_CUSTOM;
}

int Clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
float Clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
} // namespace

bool Config_Load(const char* path, Settings& s)
{
    std::string text;
    FILE* f = fopen(path, "rb");
    if(f)
    {
        char buf[4096];
        size_t n;
        while((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
        fclose(f);
    }

    KV kv;
    Parse(f ? text.c_str() : kDefaultIni, kv);

    Settings d; // nilai default struct
    s = d;

    s.preset      = ParsePreset(GetStr(kv, "general.preset", "Custom"));
    s.showToasts  = GetBool(kv, "general.showtoasts", d.showToasts);

    s.overlayMode    = Clampi(GetInt(kv, "overlay.mode", d.overlayMode), 0, 2);
    s.overlayCorner  = Clampi(GetInt(kv, "overlay.corner", d.overlayCorner), 0, 3);
    s.overlayScale   = Clampf(GetFloat(kv, "overlay.scale", d.overlayScale), 1.0f, 6.0f);
    s.overlayOpacity = Clampf(GetFloat(kv, "overlay.opacity", d.overlayOpacity), 0.1f, 1.0f);
    s.overlayMargin  = Clampi(GetInt(kv, "overlay.margin", d.overlayMargin), 0, 400);

    s.showFps        = GetBool(kv, "overlay.showfps", d.showFps);
    s.showFrameTime  = GetBool(kv, "overlay.showframetime", d.showFrameTime);
    s.showRam        = GetBool(kv, "overlay.showram", d.showRam);
    s.showMemUsage   = GetBool(kv, "overlay.showmemoryusage", d.showMemUsage);
    s.showDrawCalls  = GetBool(kv, "overlay.showdrawcalls", d.showDrawCalls);
    s.showTexMem     = GetBool(kv, "overlay.showtexturememory", d.showTexMem);
    s.fpsGood        = Clampi(GetInt(kv, "overlay.fpsgood", d.fpsGood), 1, 240);
    s.fpsWarn        = Clampi(GetInt(kv, "overlay.fpswarn", d.fpsWarn), 1, 240);

    s.showGraph    = GetBool(kv, "graph.show", d.showGraph);
    s.graphMode    = Clampi(GetInt(kv, "graph.mode", d.graphMode), 0, 1);
    s.graphSamples = Clampi(GetInt(kv, "graph.samples", d.graphSamples), 20, 240);
    s.graphMaxMs   = Clampf(GetFloat(kv, "graph.maxms", d.graphMaxMs), 10.0f, 500.0f);
    s.graphMaxFps  = Clampf(GetFloat(kv, "graph.maxfps", d.graphMaxFps), 10.0f, 240.0f);

    s.drawCallMonitor       = GetBool(kv, "monitors.drawcallmonitor", d.drawCallMonitor);
    s.textureMemoryMonitor  = GetBool(kv, "monitors.texturememorymonitor", d.textureMemoryMonitor);

    s.fpsLimit        = Clampi(GetInt(kv, "limiter.fpslimit", d.fpsLimit), 0, 240);
    s.adaptiveLimiter = GetBool(kv, "limiter.adaptive", d.adaptiveLimiter);

    s.lowMemMode        = Clampi(GetInt(kv, "lowmemory.mode", d.lowMemMode), 0, 2);
    s.lowMemThresholdMB = Clampi(GetInt(kv, "lowmemory.thresholdmb", d.lowMemThresholdMB), 64, 8192);
    s.lowMemTrimSec     = Clampi(GetInt(kv, "lowmemory.trimintervalsec", d.lowMemTrimSec), 1, 600);

    s.gameTuning       = GetBool(kv, "gametuning.enabled", d.gameTuning);
    s.lodScaleOffset32 = GetUInt(kv, "gametuning.lodscaleoffset32", 0);
    s.lodScaleOffset64 = GetUInt(kv, "gametuning.lodscaleoffset64", 0);
    s.lodScaleSymbol   = GetStr(kv, "gametuning.lodscalesymbol", "");

    s.boostMode          = Clampi(GetInt(kv, "boost.mode", d.boostMode), 0, 2);
    s.boostPriority      = GetBool(kv, "boost.threadpriority", d.boostPriority);
    s.boostBigCores      = GetBool(kv, "boost.bigcores", d.boostBigCores);
    s.boostPerfHint      = GetBool(kv, "boost.perfhint", d.boostPerfHint);
    s.boostUnlockVsync   = GetBool(kv, "boost.unlockvsync", d.boostUnlockVsync);
    s.boostPauseMonitors = GetBool(kv, "boost.pausemonitors", d.boostPauseMonitors);

    s.menuEnabled   = GetBool(kv, "menu.enabled", d.menuEnabled);
    s.menuButtonPos = Clampi(GetInt(kv, "menu.button", d.menuButtonPos), 0, 4);
    s.menuDebug     = GetBool(kv, "menu.debug", d.menuDebug);
    s.touchDownType = GetInt(kv, "menu.touchdowntype", d.touchDownType);
    s.touchUpType   = GetInt(kv, "menu.touchuptype", d.touchUpType);
    s.touchMoveType = GetInt(kv, "menu.touchmovetype", d.touchMoveType);
    s.touchSymbol   = GetStr(kv, "menu.touchsymbol", "");
    s.touchOffset32 = GetUInt(kv, "menu.touchoffset32", 0);
    s.touchOffset64 = GetUInt(kv, "menu.touchoffset64", 0);

    return f != nullptr;
}

// ---------------------------------------------------------------- simpan
namespace
{
struct Entry { const char* sec; const char* key; std::string val; };

std::string Si(int v) { return std::to_string(v); }
std::string Sb(bool v) { return v ? "1" : "0"; }
std::string Sf(float v) { char b[32]; snprintf(b, sizeof(b), "%.2f", v); return b; }
std::string Sh(unsigned v) { char b[32]; snprintf(b, sizeof(b), "0x%X", v); return b; }

bool IsHeader(const std::string& t, std::string* name)
{
    if(t.empty() || t[0] != '[') return false;
    size_t close = t.find(']');
    if(close == std::string::npos) return false;
    if(name) *name = Lower(Trim(t.substr(1, close - 1)));
    return true;
}

void MergeEntry(std::vector<std::string>& lines, const Entry& e)
{
    const std::string sec = Lower(e.sec), key = Lower(e.key);
    const std::string newLine = std::string(e.key) + " = " + e.val;

    int header = -1;
    for(size_t i = 0; i < lines.size(); ++i)
    {
        std::string name;
        if(IsHeader(Trim(lines[i]), &name) && name == sec) { header = (int)i; break; }
    }
    if(header < 0)
    {
        if(!lines.empty() && !Trim(lines.back()).empty()) lines.push_back("");
        lines.push_back(std::string("[") + e.sec + "]");
        lines.push_back(newLine);
        return;
    }

    int lastContent = header;
    for(size_t i = (size_t)header + 1; i < lines.size(); ++i)
    {
        std::string t = Trim(lines[i]);
        if(IsHeader(t, nullptr)) break;
        if(!t.empty()) lastContent = (int)i;
        if(t.empty() || t[0] == ';' || t[0] == '#') continue;
        size_t eq = t.find('=');
        if(eq == std::string::npos) continue;
        if(Lower(Trim(t.substr(0, eq))) == key) { lines[i] = newLine; return; }
    }
    lines.insert(lines.begin() + lastContent + 1, newLine);
}
} // namespace

bool Config_Save(const char* path, const Settings& s)
{
    std::string text;
    bool existed = false;
    if(FILE* f = fopen(path, "rb"))
    {
        char buf[4096];
        size_t n;
        while((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
        fclose(f);
        existed = true;
    }
    if(!existed) text = kDefaultIni;

    std::vector<std::string> lines;
    {
        size_t p = 0;
        while(p <= text.size())
        {
            size_t e = text.find('\n', p);
            if(e == std::string::npos) { if(p < text.size()) lines.push_back(text.substr(p)); break; }
            std::string ln = text.substr(p, e - p);
            if(!ln.empty() && ln.back() == '\r') ln.pop_back();
            lines.push_back(ln);
            p = e + 1;
        }
    }

    const Entry entries[] = {
        {"General", "Preset", Config_PresetName(s.preset)},
        {"General", "ShowToasts", Sb(s.showToasts)},
        {"Overlay", "Mode", Si(s.overlayMode)},
        {"Overlay", "Corner", Si(s.overlayCorner)},
        {"Overlay", "Scale", Sf(s.overlayScale)},
        {"Overlay", "Opacity", Sf(s.overlayOpacity)},
        {"Overlay", "Margin", Si(s.overlayMargin)},
        {"Overlay", "ShowFPS", Sb(s.showFps)},
        {"Overlay", "ShowFrameTime", Sb(s.showFrameTime)},
        {"Overlay", "ShowRAM", Sb(s.showRam)},
        {"Overlay", "ShowMemoryUsage", Sb(s.showMemUsage)},
        {"Overlay", "ShowDrawCalls", Sb(s.showDrawCalls)},
        {"Overlay", "ShowTextureMemory", Sb(s.showTexMem)},
        {"Overlay", "FpsGood", Si(s.fpsGood)},
        {"Overlay", "FpsWarn", Si(s.fpsWarn)},
        {"Graph", "Show", Sb(s.showGraph)},
        {"Graph", "Mode", Si(s.graphMode)},
        {"Graph", "Samples", Si(s.graphSamples)},
        {"Graph", "MaxMs", Sf(s.graphMaxMs)},
        {"Graph", "MaxFps", Sf(s.graphMaxFps)},
        {"Monitors", "DrawCallMonitor", Sb(s.drawCallMonitor)},
        {"Monitors", "TextureMemoryMonitor", Sb(s.textureMemoryMonitor)},
        {"Limiter", "FpsLimit", Si(s.fpsLimit)},
        {"Limiter", "Adaptive", Sb(s.adaptiveLimiter)},
        {"LowMemory", "Mode", Si(s.lowMemMode)},
        {"LowMemory", "ThresholdMB", Si(s.lowMemThresholdMB)},
        {"LowMemory", "TrimIntervalSec", Si(s.lowMemTrimSec)},
        {"Boost", "Mode", Si(s.boostMode)},
        {"Boost", "ThreadPriority", Sb(s.boostPriority)},
        {"Boost", "BigCores", Sb(s.boostBigCores)},
        {"Boost", "PerfHint", Sb(s.boostPerfHint)},
        {"Boost", "UnlockVSync", Sb(s.boostUnlockVsync)},
        {"Boost", "PauseMonitors", Sb(s.boostPauseMonitors)},
        {"Menu", "Enabled", Sb(s.menuEnabled)},
        {"Menu", "Button", Si(s.menuButtonPos)},
        {"Menu", "Debug", Sb(s.menuDebug)},
        {"Menu", "TouchDownType", Si(s.touchDownType)},
        {"Menu", "TouchUpType", Si(s.touchUpType)},
        {"Menu", "TouchMoveType", Si(s.touchMoveType)},
        {"Menu", "TouchSymbol", s.touchSymbol},
        {"Menu", "TouchOffset32", Sh(s.touchOffset32)},
        {"Menu", "TouchOffset64", Sh(s.touchOffset64)},
    };
    for(const Entry& e : entries) MergeEntry(lines, e);

    std::string out;
    for(const std::string& l : lines) { out += l; out += '\n'; }

    std::string tmp = std::string(path) + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if(!f) return false;
    size_t w = fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    if(w != out.size()) { remove(tmp.c_str()); return false; }
    if(rename(tmp.c_str(), path) != 0)
    {
        // beberapa penyimpanan tidak mengizinkan rename menimpa: tulis langsung
        remove(tmp.c_str());
        FILE* d = fopen(path, "wb");
        if(!d) return false;
        bool ok = fwrite(out.data(), 1, out.size(), d) == out.size();
        fclose(d);
        return ok;
    }
    return true;
}

const char* Config_PresetName(int preset)
{
    switch(preset)
    {
        case PRESET_LOW:    return "LOW";
        case PRESET_MEDIUM: return "MEDIUM";
        case PRESET_HIGH:   return "HIGH";
        case PRESET_ULTRA:  return "ULTRA";
        default:            return "CUSTOM";
    }
}

Settings Config_Effective(const Settings& user, bool lowMemActive)
{
    Settings e = user;
    int preset = user.preset;
    if(lowMemActive) preset = PRESET_LOW; // Low Memory Mode memaksa profil hemat

    switch(preset)
    {
        case PRESET_LOW:
            e.fpsLimit = 30;  e.adaptiveLimiter = false;
            e.showGraph = false;  e.lodScale = 0.70f;
            break;
        case PRESET_MEDIUM:
            e.fpsLimit = 40;  e.adaptiveLimiter = true;
            e.lodScale = 0.85f;
            break;
        case PRESET_HIGH:
            e.fpsLimit = 60;  e.adaptiveLimiter = true;
            e.lodScale = 1.00f;
            break;
        case PRESET_ULTRA:
            e.fpsLimit = 0;   e.adaptiveLimiter = false;
            e.lodScale = 1.25f;
            break;
        default:
            e.lodScale = -1.0f;
            break;
    }
    return e;
}
