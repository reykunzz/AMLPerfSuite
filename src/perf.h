#pragma once
#include <stdint.h>
#include <stddef.h>
#include "config.h"

enum { HIST_N = 240 };

struct MemSnapshot
{
    int totalMB = 0;     // RAM fisik total
    int availMB = 0;     // RAM yang masih tersedia
    int rssMB = 0;       // memori resident proses game
    int vmMB = 0;        // ruang alamat virtual proses (penting di game 32-bit)
    int swapMB = 0;      // swap yang dipakai proses
    bool valid = false;
};

struct PerfState
{
    // ---- Frame ----
    float fps = 0, frameMs = 0;            // FPS rata-rata 0,5 dtk terakhir, frame time terakhir
    float avgMs = 0, minMs = 0, maxMs = 0; // statistik ~120 frame terakhir
    float low1Fps = 0;                     // "1% low"
    float hist[HIST_N] = {0};              // riwayat frame time (ms)
    int   histHead = 0, histCount = 0;

    // ---- GL ----
    uint32_t drawCalls = 0;                // draw call pada frame sebelumnya
    uint32_t drawVerts = 0;
    int64_t  texBytes = 0;                 // perkiraan memori tekstur
    int      texCount = 0;

    // ---- Memori ----
    MemSnapshot mem;

    // ---- Limiter / preset ----
    int  targetFps = 0;                    // batas yang sedang berlaku (0 = bebas)
    bool lowMemActive = false;

    // ---- Pengaturan ----
    Settings user;                         // isi file .ini
    Settings cfg;                          // setelah preset / low-memory diterapkan
};

extern PerfState g_perf;

// Dipanggil sekali saat plugin dimuat. iniPath = lokasi file konfigurasi.
void Perf_Init(const char* iniPath);

// Dipanggil dari hook eglSwapBuffers.
void Perf_OnFrame(int64_t nowNs);   // sebelum swap
void Perf_AfterSwap();              // sesudah swap (limiter tidur di sini)

// Dipanggil dari hook GL.
void Perf_CountDraw(int verts);
void Perf_TexSet(unsigned texId, int slot, size_t bytes);
void Perf_TexDelete(unsigned texId);

int64_t Perf_NowNs();

// Dipakai menu dalam game (thread render): terapkan g_perf.user yang baru diubah,
// dan simpan ke file .ini (komentar dipertahankan, tanpa memicu muat-ulang).
void Perf_ApplySettings();
bool Perf_SaveConfig();

// Diimplementasikan di main.cpp (satu-satunya file yang menyentuh API AML).
void PerfLog(const char* fmt, ...);
void PerfToast(const char* fmt, ...);
void Game_ApplyLodScale(float scale, const Settings& s); // scale < 0 = kembalikan nilai asli
