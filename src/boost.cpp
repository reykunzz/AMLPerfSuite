#ifndef _GNU_SOURCE
#define _GNU_SOURCE   // sched_setaffinity / CPU_SET
#endif

#include "boost.h"

#include <dlfcn.h>
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "perf.h"

BoostStatus g_boost;
std::atomic<bool> g_boostLean{false};

namespace
{
// ---------------------------------------------------------------- Performance Hint (dicari saat runtime)
typedef void* (*FnGetManager)();
typedef void* (*FnCreate)(void*, const int32_t*, size_t, int64_t);
typedef int   (*FnUpdate)(void*, int64_t);
typedef int   (*FnReport)(void*, int64_t);
typedef void  (*FnClose)(void*);

FnGetManager pGetManager = nullptr;
FnCreate     pCreate = nullptr;
FnUpdate     pUpdate = nullptr;
FnReport     pReport = nullptr;
FnClose      pClose = nullptr;
bool         gHintResolved = false;
bool         gHintFailed = false;       // createSession gagal: jangan coba tiap frame
void*        gSession = nullptr;
int64_t      gTargetNs = 0;

// ---------------------------------------------------------------- state thread
int      gTid = 0;
int      gSig = -1;                     // tanda pengaturan yang sudah diterapkan
int      gFrameNo = 0;
int64_t  gLastEnd = 0;

bool     gHaveOrigNice = false;
int      gOrigNice = 0;
bool     gHaveOrigSet = false;
cpu_set_t gOrigSet;

bool     gCoresDetected = false;
unsigned gBigMask = 0;

int CurrentTid()
{
    static thread_local int tl = 0;
    if(!tl) tl = (int)syscall(SYS_gettid);
    return tl;
}

// ---------------------------------------------------------------- prioritas
void ApplyPriority(int want)   // 0 = kembalikan
{
    if(want == 0)
    {
        if(g_boost.prioOk && gHaveOrigNice) setpriority(PRIO_PROCESS, (id_t)gTid, gOrigNice);
        g_boost.prioOk = false;
        g_boost.nice = 0;
        return;
    }
    if(!gHaveOrigNice)
    {
        errno = 0;
        int cur = getpriority(PRIO_PROCESS, (id_t)gTid);
        if(errno == 0) { gOrigNice = cur; gHaveOrigNice = true; }
    }
    static const int kTry[] = {-12, -8, -4};
    g_boost.prioOk = false;
    g_boost.nice = 0;
    for(int t : kTry)
    {
        if(t < want) continue;                 // jangan lebih agresif dari yang diminta
        if(setpriority(PRIO_PROCESS, (id_t)gTid, t) == 0)
        {
            g_boost.prioOk = true;
            g_boost.nice = t;
            return;
        }
    }
    PerfLog("Boost: setpriority ditolak (errno=%d)", errno);
}

// ---------------------------------------------------------------- core besar
int ReadIntFile(const char* path)
{
    FILE* f = fopen(path, "r");
    if(!f) return 0;
    int v = 0;
    if(fscanf(f, "%d", &v) != 1) v = 0;
    fclose(f);
    return v < 0 ? 0 : v;
}

void DetectCores()
{
    if(gCoresDetected) return;
    gCoresDetected = true;

    long n = sysconf(_SC_NPROCESSORS_CONF);
    if(n < 2) { g_boost.coresAvail = false; return; }
    if(n > 32) n = 32;

    int freq[32] = {0};
    int readable = 0;
    for(int i = 0; i < n; ++i)
    {
        char p[96];
        snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
        int v = ReadIntFile(p);
        if(v <= 0)
        {
            snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpu%d/cpu_capacity", i);   // alternatif
            v = ReadIntFile(p);
        }
        freq[i] = v;
        if(v > 0) ++readable;
    }
    gBigMask = Boost_PickBigCores(freq, (int)n);
    g_boost.bigMask = gBigMask;
    g_boost.coresAvail = gBigMask != 0;

    char txt[48];
    Boost_MaskText(gBigMask, txt, sizeof(txt));
    PerfLog("Boost: %ld core, %d terbaca, core besar = %s", n, readable, gBigMask ? txt : "(tidak terdeteksi)");
}

void ApplyAffinity(bool on)
{
    if(!on)
    {
        if(g_boost.coresOk && gHaveOrigSet) sched_setaffinity((pid_t)gTid, sizeof(gOrigSet), &gOrigSet);
        g_boost.coresOk = false;
        return;
    }
    DetectCores();
    g_boost.coresOk = false;
    if(!gBigMask) return;

    if(!gHaveOrigSet && sched_getaffinity((pid_t)gTid, sizeof(gOrigSet), &gOrigSet) == 0) gHaveOrigSet = true;

    cpu_set_t set;
    CPU_ZERO(&set);
    int count = 0;
    for(int i = 0; i < 32; ++i)
    {
        if(!(gBigMask & (1u << i))) continue;
        if(gHaveOrigSet && !CPU_ISSET(i, &gOrigSet)) continue;   // hormati batasan awal
        CPU_SET(i, &set);
        ++count;
    }
    if(count < 1) return;
    if(sched_setaffinity((pid_t)gTid, sizeof(set), &set) == 0) g_boost.coresOk = true;
    else PerfLog("Boost: sched_setaffinity ditolak (errno=%d)", errno);
}

// ---------------------------------------------------------------- performance hint
void ResolveHint()
{
    if(gHintResolved) return;
    gHintResolved = true;
    pGetManager = (FnGetManager)dlsym(RTLD_DEFAULT, "APerformanceHint_getManager");
    pCreate     = (FnCreate)dlsym(RTLD_DEFAULT, "APerformanceHint_createSession");
    pUpdate     = (FnUpdate)dlsym(RTLD_DEFAULT, "APerformanceHint_updateTargetWorkDuration");
    pReport     = (FnReport)dlsym(RTLD_DEFAULT, "APerformanceHint_reportActualWorkDuration");
    pClose      = (FnClose)dlsym(RTLD_DEFAULT, "APerformanceHint_closeSession");
    if(!pGetManager)   // libandroid belum tentu terbuka di RTLD_DEFAULT
    {
        void* h = dlopen("libandroid.so", RTLD_NOW);
        if(h)
        {
            pGetManager = (FnGetManager)dlsym(h, "APerformanceHint_getManager");
            pCreate     = (FnCreate)dlsym(h, "APerformanceHint_createSession");
            pUpdate     = (FnUpdate)dlsym(h, "APerformanceHint_updateTargetWorkDuration");
            pReport     = (FnReport)dlsym(h, "APerformanceHint_reportActualWorkDuration");
            pClose      = (FnClose)dlsym(h, "APerformanceHint_closeSession");
        }
    }
    g_boost.hintAvail = pGetManager && pCreate && pReport;
    PerfLog("Boost: Performance Hint %s", g_boost.hintAvail ? "tersedia" : "tidak tersedia (Android < 13?)");
}

void StopHint()
{
    if(gSession && pClose) pClose(gSession);
    gSession = nullptr;
    gTargetNs = 0;
    g_boost.hintOk = false;
}

void StartHint(int64_t targetNs)
{
    ResolveHint();
    if(!g_boost.hintAvail || gHintFailed) return;
    if(gSession)
    {
        if(targetNs != gTargetNs && pUpdate) { pUpdate(gSession, targetNs); gTargetNs = targetNs; }
        g_boost.hintOk = true;
        return;
    }
    void* mgr = pGetManager();
    if(!mgr) { gHintFailed = true; PerfLog("Boost: Performance Hint manager tidak ada"); return; }
    int32_t tids[1] = {(int32_t)gTid};
    gSession = pCreate(mgr, tids, 1, targetNs);
    if(!gSession) { gHintFailed = true; PerfLog("Boost: createSession gagal"); return; }
    gTargetNs = targetNs;
    g_boost.hintOk = true;
}

// ---------------------------------------------------------------- terapkan semua
int Signature(const Settings& u)
{
    int m = u.boostMode;
    return m | (u.boostPriority << 2) | (u.boostBigCores << 3) | (u.boostPerfHint << 4)
             | (u.boostUnlockVsync << 5) | (u.boostPauseMonitors << 6);
}

void ApplyAll(EGLDisplay dpy, const Settings& u)
{
    const int mode = u.boostMode;
    const bool on = mode >= 1;

    ApplyPriority(on && u.boostPriority ? (mode == 2 ? -12 : -8) : 0);
    ApplyAffinity(on && u.boostBigCores);
    if(on && u.boostPerfHint) StartHint(mode == 2 ? 8000000LL : 12000000LL);   // target kerja per frame
    else StopHint();

    const bool lean = mode == 2 && u.boostPauseMonitors;
    g_boostLean.store(lean, std::memory_order_relaxed);
    g_boost.lean = lean;

    if(!(mode == 2 && u.boostUnlockVsync) && g_boost.vsyncUnlocked)
    {
        if(dpy != EGL_NO_DISPLAY) eglSwapInterval(dpy, 1);
        g_boost.vsyncUnlocked = false;
    }
    g_boost.mode = mode;

    PerfLog("Boost mode %d: prio=%s(%d) core=%s hint=%s lean=%d", mode,
            g_boost.prioOk ? "ok" : "-", g_boost.nice, g_boost.coresOk ? "ok" : "-",
            g_boost.hintOk ? "ok" : "-", lean ? 1 : 0);
}
} // namespace

// ====================================================================== API
void Boost_Frame(EGLDisplay dpy, int64_t now)
{
    const Settings& u = g_perf.user;

    int tid = CurrentTid();
    if(tid != gTid)
    {
        // thread render berganti: sesi hint terikat ke thread lama, mulai ulang
        StopHint();
        gTid = tid;
        gSig = -1;
        gHaveOrigNice = false;
        gHaveOrigSet = false;
        g_boost.prioOk = false;
        g_boost.coresOk = false;
    }

    int sig = Signature(u);
    if(sig != gSig)
    {
        gSig = sig;
        ApplyAll(dpy, u);
    }

    // laporkan durasi kerja frame sebelumnya (tanpa waktu tidur limiter / menunggu swap)
    if(gSession && pReport && gLastEnd)
    {
        int64_t work = now - gLastEnd;
        if(work > 0 && work < 1000000000LL) pReport(gSession, work);
    }

    // buka kunci vsync (diulang tiap ±1 detik bila game mengembalikannya)
    if(u.boostMode == 2 && u.boostUnlockVsync && dpy != EGL_NO_DISPLAY && (gFrameNo++ % 60) == 0)
        g_boost.vsyncUnlocked = eglSwapInterval(dpy, 0) == EGL_TRUE;
}

void Boost_FrameEnd()
{
    gLastEnd = Perf_NowNs();
}

unsigned Boost_PickBigCores(const int* f, int n)
{
    if(!f || n < 4 || n > 32) return 0;   // kurang dari 4 core: tidak ada gunanya memilah

    int mx = 0, mn = 0;
    for(int i = 0; i < n; ++i)
    {
        if(f[i] > mx) mx = f[i];
        if(f[i] > 0 && (mn == 0 || f[i] < mn)) mn = f[i];
    }
    if(mx <= 0 || mn == mx) return 0;      // homogen / tak terbaca

    const double ratios[] = {0.70, 0.50};
    for(double ratio : ratios)
    {
        unsigned mask = 0;
        int count = 0;
        for(int i = 0; i < n; ++i)
            if(f[i] > 0 && f[i] >= mx * ratio) { mask |= 1u << i; ++count; }

        unsigned all = n == 32 ? 0xFFFFFFFFu : ((1u << n) - 1u);
        if(count >= 2 && mask != all) return mask;
    }
    return 0;                              // satu core besar saja: jangan dipaku ke satu core
}

void Boost_MaskText(unsigned mask, char* out, size_t n)
{
    if(!n) return;
    out[0] = 0;
    if(!mask) { snprintf(out, n, "-"); return; }
    size_t len = 0;
    int i = 0;
    while(i < 32)
    {
        if(!(mask & (1u << i))) { ++i; continue; }
        int j = i;
        while(j + 1 < 32 && (mask & (1u << (j + 1)))) ++j;
        char part[16];
        if(j > i) snprintf(part, sizeof(part), "%s%d-%d", len ? "," : "", i, j);
        else      snprintf(part, sizeof(part), "%s%d", len ? "," : "", i);
        size_t pl = strlen(part);
        if(len + pl + 1 >= n) break;
        memcpy(out + len, part, pl + 1);
        len += pl;
        i = j + 1;
    }
}
