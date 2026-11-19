// AML Performance Suite untuk GTA San Andreas Android
// Satu plugin berisi: FPS Counter, Frame Time Monitor, RAM Monitor, Memory Usage Monitor,
// Draw Call Monitor, Texture Memory Monitor, Performance Graph, Dynamic FPS Limiter,
// Low Memory Mode, Performance Preset, dan Performance Overlay.
//
// Hanya file ini yang memakai API AML. Modul lain (config/perf/overlay) murni C++ + GLES2.

#include <mod/amlmod.h>
#include <mod/logger.h>

#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "boost.h"
#include "config.h"
#include "elfscan.h"
#include "menu.h"
#include "overlay.h"
#include "perf.h"
#include "version.h"

// Token MYMOD (tanpa tanda kutip) harus sama dengan nilai di version.h.
MYMOD(net.amlperf.suite, AML Performance Suite, 1.2.1, ReyyXSA)
#ifndef AMLPERF_ANY_GAME
NEEDGAME(com.rockstargames.gtasa)
#endif

static uintptr_t gGame = 0;
static char gLogPath[512] = {0};

static void FileLog(const char* line)
{
    if(!gLogPath[0]) return;
    FILE* f = fopen(gLogPath, "ab");
    if(!f) return;
    fprintf(f, "%s\n", line);
    fclose(f);
}

// ---------------------------------------------------------------- jembatan ke modul lain
void PerfLog(const char* fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    logger->Info("%s", buf);
    FileLog(buf);
}

void PerfToast(const char* fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    aml->ShowToast(false, "%s", buf);
}

// Alamat float "LOD distance scale" milik game (opsional, lihat [GameTuning] di .ini).
void Game_ApplyLodScale(float scale, const Settings& s)
{
    static uintptr_t addr = 0;
    static bool resolved = false;
    static bool haveOrig = false;
    static float orig = 1.0f;

    if(!resolved)
    {
        resolved = true;
        if(gGame)
        {
            unsigned off = BYBIT(s.lodScaleOffset32, s.lodScaleOffset64);
            if(off) addr = gGame + off;
            else if(!s.lodScaleSymbol.empty()) addr = aml->GetSym(gGame, s.lodScaleSymbol.c_str());
        }
        if(addr) logger->Info("LOD scale address = 0x%X", (unsigned)addr);
    }
    if(!addr) return;

    if(scale < 0.0f)                      // kembalikan nilai asli
    {
        if(haveOrig) aml->WriteFloat(addr, orig);
        return;
    }
    if(!haveOrig) { orig = aml->ReadFloat(addr); haveOrig = true; }
    aml->WriteFloat(addr, scale);
}

// ---------------------------------------------------------------- hook EGL / GL
static EGLBoolean (*o_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;
static void (*o_glDrawElements)(GLenum, GLsizei, GLenum, const void*) = nullptr;
static void (*o_glDrawArrays)(GLenum, GLint, GLsizei) = nullptr;
static void (*o_glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) = nullptr;
static void (*o_glCompressedTexImage2D)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void*) = nullptr;
static void (*o_glDeleteTextures)(GLsizei, const GLuint*) = nullptr;

static EGLBoolean H_eglSwapBuffers(EGLDisplay dpy, EGLSurface surf)
{
    const int64_t now = Perf_NowNs();
    Perf_OnFrame(now);
    Boost_Frame(dpy, now);
    Overlay_Draw(dpy, surf);
    EGLBoolean r = o_eglSwapBuffers(dpy, surf);
    Perf_AfterSwap();
    Boost_FrameEnd();
    return r;
}

static void H_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices)
{
    if(!Boost_Lean() && !Overlay_IsDrawing()) Perf_CountDraw(count);
    o_glDrawElements(mode, count, type, indices);
}

static void H_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    if(!Boost_Lean() && !Overlay_IsDrawing()) Perf_CountDraw(count);
    o_glDrawArrays(mode, first, count);
}

static size_t BytesPerPixel(GLenum format, GLenum type)
{
    switch(type)
    {
        case GL_UNSIGNED_SHORT_5_6_5:
        case GL_UNSIGNED_SHORT_4_4_4_4:
        case GL_UNSIGNED_SHORT_5_5_5_1:
            return 2;
        case GL_UNSIGNED_BYTE:
            switch(format)
            {
                case GL_ALPHA:
                case GL_LUMINANCE:       return 1;
                case GL_LUMINANCE_ALPHA: return 2;
                case GL_RGB:             return 3;
                case GL_RGBA:            return 4;
                default:                 return 4;
            }
        default:
            return 4;
    }
}

static bool TexSlot(GLenum target, GLint level, GLenum* bindingQuery, int* slot)
{
    if(target == GL_TEXTURE_2D)
    {
        *bindingQuery = GL_TEXTURE_BINDING_2D;
        *slot = level;
        return true;
    }
    if(target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z)
    {
        *bindingQuery = GL_TEXTURE_BINDING_CUBE_MAP;
        *slot = level + (int)(target - GL_TEXTURE_CUBE_MAP_POSITIVE_X) * 32;
        return true;
    }
    return false;
}

static void H_glTexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border,
                           GLenum format, GLenum type, const void* pixels)
{
    o_glTexImage2D(target, level, ifmt, w, h, border, format, type, pixels);
    if(Boost_Lean() || Overlay_IsDrawing()) return;

    GLenum query; int slot;
    if(!TexSlot(target, level, &query, &slot)) return;
    GLint id = 0;
    glGetIntegerv(query, &id);
    Perf_TexSet((unsigned)id, slot, (size_t)w * (size_t)h * BytesPerPixel(format, type));
}

static void H_glCompressedTexImage2D(GLenum target, GLint level, GLenum ifmt, GLsizei w, GLsizei h,
                                     GLint border, GLsizei imageSize, const void* data)
{
    o_glCompressedTexImage2D(target, level, ifmt, w, h, border, imageSize, data);
    if(Boost_Lean()) return;

    GLenum query; int slot;
    if(!TexSlot(target, level, &query, &slot)) return;
    GLint id = 0;
    glGetIntegerv(query, &id);
    Perf_TexSet((unsigned)id, slot, (size_t)imageSize);
}

static void H_glDeleteTextures(GLsizei n, const GLuint* textures)
{
    for(GLsizei i = 0; i < n && textures; ++i) Perf_TexDelete(textures[i]);
    o_glDeleteTextures(n, textures);
}

static void MakeReadable(void* addr)
{
    // Android 10+ arm64: kode library sistem execute-only. Tambahkan izin baca
    // supaya AML bisa membaca prolog fungsi sebelum memasang hook.
    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t page = (uintptr_t)addr & ~(uintptr_t)(ps - 1);
    mprotect((void*)page, (size_t)ps * 2, PROT_READ | PROT_EXEC);
}

static bool InstallHook(const char* lib, const char* sym, void* hook, void** orig)
{
    void* h = dlopen(lib, RTLD_NOW);
    void* addr = h ? dlsym(h, sym) : nullptr;
    if(!addr)
    {
        PerfLog("Simbol %s tidak ditemukan di %s", sym, lib);
        return false;
    }
    MakeReadable(addr);
    bool ok = aml->Hook(addr, hook, orig);
    PerfLog("Hook %s : %s", sym, ok ? "OK" : "GAGAL");
    return ok;
}

// ---------------------------------------------------------------- hook sentuhan (menu)
// Fungsi sentuhan GTA SA: AND_TouchEvent(int type, int id, int x, int y).
// Hook dibuat meneruskan 8 argumen mentah supaya aman bila tanda tangan aslinya sedikit berbeda.
typedef void (*TouchFn)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
static TouchFn o_Touch = nullptr;

static void H_Touch(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3,
                    uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7)
{
    if(Menu_OnTouch((int)a0, (int)a1, (int)a2, (int)a3)) return;   // dipakai menu: jangan teruskan ke game
    o_Touch(a0, a1, a2, a3, a4, a5, a6, a7);
}

static bool InstallTouchHook(const Settings& u)
{
    PerfLog("Hook sentuhan: mencari fungsi di libGTASA.so (gGame=0x%lX) ...", (unsigned long)gGame);

    // Pindai tabel simbol libGTASA.so langsung dari memori (tanpa API AML).
    ElfScanResult* r = new ElfScanResult();
    const char* exact = u.touchSymbol.empty() ? nullptr : u.touchSymbol.c_str();
    // Log hanya simbol "TouchEvent" dan fungsi JNI (Java_...) agar daftarnya ringkas;
    // AND_TouchEvent dicari di SEMUA simbol lewat parameter 'prefer'.
    bool scanned = Elf_ScanLoadedLib("libGTASA.so", "TouchEvent", "Java_", exact, "AND_TouchEvent", *r);

    if(!scanned)
        PerfLog("Pemindaian simbol libGTASA.so GAGAL (library %s, simbol %d)",
                r->libFound ? "ditemukan" : "tidak ditemukan", r->totalSyms);
    else
    {
        PerfLog("libGTASA.so: base=0x%lX, %d simbol, %d cocok 'TouchEvent'/'Java_'",
                (unsigned long)r->bias, r->totalSyms, r->hitCount);
        for(int i = 0; i < r->stored; ++i)
            PerfLog("  sym %s  +0x%lX", r->hits[i].name, (unsigned long)(r->hits[i].addr - r->bias));
    }

    uintptr_t addr = 0;
    char how[200] = {0};

    if(exact && r->exactAddr)
    {
        addr = r->exactAddr;
        snprintf(how, sizeof(how), "simbol %s", exact);
    }
    if(!addr && scanned && r->preferAddr)
    {
        addr = r->preferAddr;
        snprintf(how, sizeof(how), "simbol %s", r->preferName);
    }
    else if(scanned)
        PerfLog("AND_TouchEvent TIDAK ada di tabel simbol libGTASA.so.");
    if(!addr && gGame)
    {
        unsigned off = BYBIT(u.touchOffset32, u.touchOffset64);
        if(off) { addr = gGame + off; snprintf(how, sizeof(how), "offset 0x%X", off); }
    }
    delete r;

    if(!addr)
    {
        PerfLog("Fungsi sentuhan TIDAK ditemukan. Kirim log ini; atau isi [Menu] TouchSymbol / TouchOffset di .ini.");
        return false;
    }

    MakeReadable((void*)addr);
    bool ok = aml->Hook((void*)addr, (void*)H_Touch, (void**)&o_Touch);
    PerfLog("Hook sentuhan (%s) : %s", how, ok ? "OK" : "GAGAL");
    return ok && o_Touch;
}

// ---------------------------------------------------------------- jalur konfigurasi
static void MakeDirs(const char* path)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for(char* p = tmp + 1; *p; ++p)
    {
        if(*p == '/')
        {
            *p = 0;
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    mkdir(tmp, 0777);
}

static void PrepareConfig(char* outIni, size_t n)
{
    const char* base = aml->GetAndroidDataPath();
    char dir[400];
    if(base && *base)
    {
        size_t len = strlen(base);
        snprintf(dir, sizeof(dir), "%s%sAMLPerfSuite", base, (base[len - 1] == '/') ? "" : "/");
    }
    else snprintf(dir, sizeof(dir), "/sdcard/Android/data/com.rockstargames.gtasa/files/AMLPerfSuite");

    MakeDirs(dir);
    snprintf(outIni, n, "%s/AMLPerfSuite.ini", dir);
    snprintf(gLogPath, sizeof(gLogPath), "%s/AMLPerfSuite.log", dir);
    if(FILE* lf = fopen(gLogPath, "wb")) fclose(lf);     // log baru tiap start game

    if(access(outIni, F_OK) != 0)
    {
        FILE* f = fopen(outIni, "wb");
        if(f)
        {
            fwrite(kDefaultIni, 1, strlen(kDefaultIni), f);
            fclose(f);
            logger->Info("Konfigurasi default dibuat: %s", outIni);
        }
        else logger->Error("Gagal membuat konfigurasi di %s", outIni);
    }
}

// ---------------------------------------------------------------- siklus hidup mod
ON_MOD_PRELOAD()
{
    logger->SetTag("AMLPerfSuite");
}

ON_MOD_LOAD()
{
    logger->SetTag("AMLPerfSuite");
    gGame = aml->GetLib("libGTASA.so");

    char ini[512];
    PrepareConfig(ini, sizeof(ini));
    Perf_Init(ini);

    // Inti plugin: semua fitur menumpang di hook ini.
    if(!InstallHook("libEGL.so", "eglSwapBuffers", (void*)H_eglSwapBuffers, (void**)&o_eglSwapBuffers))
    {
        logger->Error("eglSwapBuffers gagal di-hook, plugin tidak aktif.");
        return;
    }

    const Settings& u = g_perf.user;
    if(u.drawCallMonitor)
    {
        InstallHook("libGLESv2.so", "glDrawElements", (void*)H_glDrawElements, (void**)&o_glDrawElements);
        InstallHook("libGLESv2.so", "glDrawArrays",   (void*)H_glDrawArrays,   (void**)&o_glDrawArrays);
    }
    if(u.textureMemoryMonitor)
    {
        InstallHook("libGLESv2.so", "glTexImage2D",           (void*)H_glTexImage2D,           (void**)&o_glTexImage2D);
        InstallHook("libGLESv2.so", "glCompressedTexImage2D", (void*)H_glCompressedTexImage2D, (void**)&o_glCompressedTexImage2D);
        InstallHook("libGLESv2.so", "glDeleteTextures",       (void*)H_glDeleteTextures,       (void**)&o_glDeleteTextures);
    }

    if(u.menuEnabled)
        Menu_SetInputOk(InstallTouchHook(u));
    else
        Menu_SetInputOk(false);

    PerfLog("AML Performance Suite %s by %s dimuat. Config: %s", AMLPERF_VERSION, AMLPERF_AUTHOR, ini);
}
