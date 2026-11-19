#pragma once
// Boost Performance: membantu game berjalan lebih lancar tanpa root.
//
// Yang dilakukan (semua hanya pada thread render/game, tidak menyentuh memori game):
//   ThreadPriority : naikkan prioritas thread (nice negatif)
//   BigCores       : batasi thread ke core CPU "besar" (big.LITTLE)
//   PerfHint       : Android Performance Hint (ADPF, Android 12/13+) agar CPU tidak menurunkan clock
//   UnlockVSync    : eglSwapInterval(0) agar frame tidak "tertahan" ke kelipatan refresh rate   [MAX]
//   PauseMonitors  : hentikan pencacah draw call / memori tekstur (mengurangi beban hook GL)    [MAX]
//
// Boost TIDAK bisa menaikkan FPS di atas batas yang dipasang game sendiri; manfaatnya ada pada
// berkurangnya patah-patah dan penurunan FPS. Hasil tiap komponen dilaporkan di g_boost.

#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <EGL/egl.h>

struct BoostStatus
{
    int      mode = 0;              // mode yang sedang berlaku: 0 OFF, 1 ON, 2 MAX
    bool     prioOk = false;        // prioritas berhasil dinaikkan
    int      nice = 0;              // nilai nice yang tercapai (negatif = lebih tinggi)
    bool     coresAvail = true;     // false = CPU homogen / tidak bisa dideteksi
    bool     coresOk = false;       // afinitas berhasil dipasang
    unsigned bigMask = 0;           // bitmask core besar yang terdeteksi
    bool     hintAvail = false;     // API Performance Hint ada di perangkat
    bool     hintOk = false;        // sesi hint aktif
    bool     vsyncUnlocked = false;
    bool     lean = false;          // monitor GL dijeda
};

extern BoostStatus g_boost;
extern std::atomic<bool> g_boostLean;
inline bool Boost_Lean() { return g_boostLean.load(std::memory_order_relaxed); }

// Dipanggil dari hook eglSwapBuffers (thread render): awal dan akhir (setelah limiter tidur).
void Boost_Frame(EGLDisplay dpy, int64_t nowNs);
void Boost_FrameEnd();

// Fungsi bantu (diuji terpisah).
// maxFreq[i] = frekuensi maksimum core i (0 = tidak terbaca). Mengembalikan bitmask core
// besar, atau 0 bila CPU homogen / tidak ada pemisahan yang masuk akal (minimal 2 core besar).
unsigned Boost_PickBigCores(const int* maxFreq, int n);
// Bitmask -> "4-7" atau "0-1,6-7".
void Boost_MaskText(unsigned mask, char* out, size_t n);
