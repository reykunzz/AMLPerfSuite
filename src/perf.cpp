#include "perf.h"

#include <algorithm>
#include <atomic>
#include <errno.h>
#include <malloc.h>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unordered_map>
#include <vector>
#include <dlfcn.h>

#ifndef M_PURGE
#define M_PURGE (-101)
#endif

PerfState g_perf;

namespace
{
// ---------------------------------------------------------------- waktu
const int64_t kMs = 1000000LL;
const int64_t kSec = 1000000000LL;

// ---------------------------------------------------------------- penghitung GL
std::atomic<uint32_t> gDraws{0};
std::atomic<uint32_t> gVerts{0};

std::mutex gTexMutex;
std::unordered_map<unsigned, std::unordered_map<int, size_t>> gTex;
std::atomic<int64_t> gTexBytes{0};
std::atomic<int>     gTexCount{0};

// ---------------------------------------------------------------- state internal
char    gIniPath[512] = {0};
time_t  gIniMTime = 0;

int64_t gWinStart = 0, gLastFrame = 0, gLastSlow = 0;
int     gWinFrames = 0;

// limiter
int64_t gNextDeadline = 0;
int     gAppliedTarget = -1;

// adaptive
std::vector<int> gTiers;
int  gTierIdx = 0;
int  gBadWin = 0, gGoodWin = 0, gUpHold = 20;

// low memory
int64_t gLastTrim = 0;
int     gLowMemRecoverTicks = 0;

float   gLastLod = -2.0f;

// ---------------------------------------------------------------- util
bool ReadMeminfo(MemSnapshot& m)
{
    FILE* f = fopen("/proc/meminfo", "r");
    if(!f) return false;
    char line[160];
    long totalKB = 0, availKB = 0;
    while(fgets(line, sizeof(line), f))
    {
        if(!strncmp(line, "MemTotal:", 9))          totalKB = atol(line + 9);
        else if(!strncmp(line, "MemAvailable:", 13)) availKB = atol(line + 13);
        if(totalKB && availKB) break;
    }
    fclose(f);
    m.totalMB = (int)(totalKB / 1024);
    m.availMB = (int)(availKB / 1024);
    return totalKB > 0;
}

bool ReadSelfStatus(MemSnapshot& m)
{
    FILE* f = fopen("/proc/self/status", "r");
    if(!f) return false;
    char line[160];
    long rss = 0, vm = 0, swap = 0;
    while(fgets(line, sizeof(line), f))
    {
        if(!strncmp(line, "VmRSS:", 6))       rss = atol(line + 6);
        else if(!strncmp(line, "VmSize:", 7)) vm = atol(line + 7);
        else if(!strncmp(line, "VmSwap:", 7)) swap = atol(line + 7);
    }
    fclose(f);
    m.rssMB = (int)(rss / 1024);
    m.vmMB = (int)(vm / 1024);
    m.swapMB = (int)(swap / 1024);
    return rss > 0;
}

void SleepUntilNs(int64_t t)
{
    timespec ts;
    ts.tv_sec = (time_t)(t / kSec);
    ts.tv_nsec = (long)(t % kSec);
    while(clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {}
}

time_t FileMTime(const char* path)
{
    struct stat st;
    return stat(path, &st) == 0 ? st.st_mtime : 0;
}

// ---------------------------------------------------------------- adaptive limiter
void RebuildTiers(int cap)
{
    gTiers.clear();
    static const int kAll[] = {60, 45, 40, 30};
    if(cap > 0)
    {
        bool hasCap = false;
        for(int t : kAll) if(t == cap) hasCap = true;
        if(!hasCap && cap > 30) gTiers.push_back(cap);
        for(int t : kAll) if(t <= cap) gTiers.push_back(t);
    }
    if(gTiers.empty() && cap > 0) gTiers.push_back(cap);
    gTierIdx = 0;
    gBadWin = gGoodWin = 0;
    gUpHold = 20;
}

void AdaptiveEvaluate()
{
    const Settings& c = g_perf.cfg;
    if(!c.adaptiveLimiter || c.fpsLimit <= 0 || gTiers.empty()) return;

    int target = gTiers[gTierIdx];
    float fps = g_perf.fps;

    if(fps < target * 0.85f) { ++gBadWin; gGoodWin = 0; }
    else if(fps >= target * 0.97f) { ++gGoodWin; gBadWin = 0; }
    else { gBadWin = 0; }

    if(gBadWin >= 6 && gTierIdx + 1 < (int)gTiers.size())      // 3 detik buruk -> turun
    {
        ++gTierIdx;
        gBadWin = gGoodWin = 0;
        if(gUpHold < 120) gUpHold *= 2;                         // jangan cepat-cepat naik lagi
    }
    else if(gGoodWin >= gUpHold && gTierIdx > 0)                // stabil -> coba naik
    {
        --gTierIdx;
        gBadWin = gGoodWin = 0;
    }
}

int ComputeTargetFps()
{
    const Settings& c = g_perf.cfg;
    if(c.fpsLimit <= 0) return 0;
    if(c.adaptiveLimiter && !gTiers.empty()) return gTiers[gTierIdx];
    return c.fpsLimit;
}

// ---------------------------------------------------------------- preset / game tuning
void RecomputeEffective()
{
    g_perf.cfg = Config_Effective(g_perf.user, g_perf.lowMemActive);

    static int tierCap = -1;
    if(g_perf.cfg.fpsLimit != tierCap)
    {
        tierCap = g_perf.cfg.fpsLimit;
        RebuildTiers(tierCap);
    }

    // Tuning game hanya jika diaktifkan; bila tidak, kembalikan nilai asli.
    float lod = g_perf.cfg.gameTuning ? g_perf.cfg.lodScale : -1.0f;
    if(lod != gLastLod)
    {
        Game_ApplyLodScale(lod, g_perf.cfg);
        gLastLod = lod;
    }
}

void ReloadConfigIfChanged()
{
    time_t m = FileMTime(gIniPath);
    if(m == 0 || m == gIniMTime) return;
    gIniMTime = m;
    Settings s;
    Config_Load(gIniPath, s);
    g_perf.user = s;
    RecomputeEffective();
    PerfLog("Konfigurasi dimuat ulang (preset %s)", Config_PresetName(s.preset));
}

// ---------------------------------------------------------------- low memory mode
void UpdateLowMemory(int64_t now)
{
    const Settings& u = g_perf.user;
    bool want = g_perf.lowMemActive;

    if(u.lowMemMode == 0)
        want = false;
    else if(u.lowMemMode == 2)
        want = true;
    else if(g_perf.mem.valid)
    {
        if(g_perf.mem.availMB < u.lowMemThresholdMB)
        {
            want = true;
            gLowMemRecoverTicks = 0;
        }
        else if(g_perf.lowMemActive && g_perf.mem.availMB > u.lowMemThresholdMB * 5 / 4)
        {
            // histeresis: tunggu RAM longgar selama 5 detik sebelum keluar dari mode
            if(++gLowMemRecoverTicks >= 5) want = false;
        }
        else gLowMemRecoverTicks = 0;
    }

    if(want != g_perf.lowMemActive)
    {
        g_perf.lowMemActive = want;
        RecomputeEffective();
        PerfLog("Low Memory Mode %s (RAM tersedia %d MB)", want ? "AKTIF" : "nonaktif", g_perf.mem.availMB);
        if(g_perf.user.showToasts) PerfToast("Low Memory Mode %s", want ? "ON" : "OFF");
    }

    if(g_perf.lowMemActive && now - gLastTrim >= (int64_t)u.lowMemTrimSec * kSec)
    {
        gLastTrim = now;
        // Kembalikan heap native yang sudah free ke OS. Fungsi dicari saat runtime
        // karena tidak tersedia di semua level API.
        typedef int (*TrimFn)(size_t);
        static TrimFn trim = (TrimFn)dlsym(RTLD_DEFAULT, "malloc_trim");
        if(trim) trim(0);
        mallopt(M_PURGE, 0);
    }
}

// ---------------------------------------------------------------- statistik frame
void ComputeFrameStats()
{
    PerfState& p = g_perf;
    int n = std::min(p.histCount, 120);
    if(n <= 0) return;

    float tmp[120];
    float sum = 0, mn = 1e9f, mx = 0;
    for(int i = 0; i < n; ++i)
    {
        int idx = (p.histHead - 1 - i + HIST_N) % HIST_N;
        float v = p.hist[idx];
        tmp[i] = v;
        sum += v;
        if(v < mn) mn = v;
        if(v > mx) mx = v;
    }
    p.avgMs = sum / n;
    p.minMs = mn;
    p.maxMs = mx;

    int k = (int)(n * 0.99f);
    if(k >= n) k = n - 1;
    std::nth_element(tmp, tmp + k, tmp + n);
    p.low1Fps = tmp[k] > 0.01f ? 1000.0f / tmp[k] : 0.0f;
}
} // namespace

// ====================================================================== API
int64_t Perf_NowNs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * kSec + ts.tv_nsec;
}

void Perf_Init(const char* iniPath)
{
    snprintf(gIniPath, sizeof(gIniPath), "%s", iniPath);
    Settings s;
    Config_Load(gIniPath, s);
    gIniMTime = FileMTime(gIniPath);
    g_perf.user = s;
    g_perf.cfg = s;
    gLastLod = -2.0f;
    RecomputeEffective();
    PerfLog("Siap. Preset=%s Limit=%d Overlay=%d", Config_PresetName(s.preset), g_perf.cfg.fpsLimit, s.overlayMode);
}

void Perf_CountDraw(int verts)
{
    gDraws.fetch_add(1, std::memory_order_relaxed);
    if(verts > 0) gVerts.fetch_add((uint32_t)verts, std::memory_order_relaxed);
}

void Perf_TexSet(unsigned texId, int slot, size_t bytes)
{
    if(texId == 0) return;
    std::lock_guard<std::mutex> lk(gTexMutex);
    auto& levels = gTex[texId];
    auto it = levels.find(slot);
    if(it == levels.end())
    {
        if(levels.empty()) gTexCount.fetch_add(1);
        levels[slot] = bytes;
        gTexBytes.fetch_add((int64_t)bytes);
    }
    else
    {
        gTexBytes.fetch_add((int64_t)bytes - (int64_t)it->second);
        it->second = bytes;
    }
}

void Perf_TexDelete(unsigned texId)
{
    std::lock_guard<std::mutex> lk(gTexMutex);
    auto it = gTex.find(texId);
    if(it == gTex.end()) return;
    for(auto& kv : it->second) gTexBytes.fetch_sub((int64_t)kv.second);
    gTex.erase(it);
    gTexCount.fetch_sub(1);
}

void Perf_OnFrame(int64_t now)
{
    PerfState& p = g_perf;

    if(gLastFrame)
    {
        float ms = (float)((now - gLastFrame) / 1000) / 1000.0f;
        if(ms < 1000.0f) // abaikan jeda besar (app di-background, loading, dll.)
        {
            p.hist[p.histHead] = ms;
            p.histHead = (p.histHead + 1) % HIST_N;
            if(p.histCount < HIST_N) ++p.histCount;
            p.frameMs = ms;
            ++gWinFrames;
        }
    }
    gLastFrame = now;
    if(!gWinStart) { gWinStart = now; gLastSlow = now; }

    // penghitung GL untuk frame yang baru selesai
    p.drawCalls = gDraws.exchange(0, std::memory_order_relaxed);
    p.drawVerts = gVerts.exchange(0, std::memory_order_relaxed);
    p.texBytes = gTexBytes.load();
    p.texCount = gTexCount.load();

    // jendela 0,5 detik: FPS + statistik + adaptive limiter
    if(now - gWinStart >= 500 * kMs)
    {
        p.fps = gWinFrames * 1e9f / (float)(now - gWinStart);
        gWinFrames = 0;
        gWinStart = now;
        ComputeFrameStats();
        AdaptiveEvaluate();
    }

    // tugas 1 detik: memori, low-memory, reload konfigurasi
    if(now - gLastSlow >= kSec)
    {
        gLastSlow = now;
        MemSnapshot m;
        bool a = ReadMeminfo(m);
        bool b = ReadSelfStatus(m);
        m.valid = a || b;
        p.mem = m;
        UpdateLowMemory(now);
        ReloadConfigIfChanged();
    }

    int target = ComputeTargetFps();
    if(target != gAppliedTarget)
    {
        gAppliedTarget = target;
        gNextDeadline = 0;
    }
    p.targetFps = target;
}

void Perf_AfterSwap()
{
    int target = g_perf.targetFps;
    if(target <= 0) { gNextDeadline = 0; return; }

    int64_t interval = kSec / target;
    int64_t now = Perf_NowNs();

    if(gNextDeadline == 0) gNextDeadline = now + interval;
    else gNextDeadline += interval;

    if(now < gNextDeadline)
        SleepUntilNs(gNextDeadline);
    else if(now - gNextDeadline > interval)
        gNextDeadline = now;   // terlalu telat: sinkron ulang, jangan "mengejar" tanpa batas
}
