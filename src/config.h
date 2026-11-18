#pragma once
#include <string>

enum PresetId { PRESET_CUSTOM = 0, PRESET_LOW, PRESET_MEDIUM, PRESET_HIGH, PRESET_ULTRA };

// Semua pengaturan plugin. Nilai default = nilai bawaan file .ini.
struct Settings
{
    // ---- Umum ----
    int   preset = PRESET_CUSTOM;
    bool  showToasts = false;

    // ---- Overlay ----
    int   overlayMode = 1;          // 0 = mati, 1 = mini, 2 = lengkap
    int   overlayCorner = 0;        // 0 kiri-atas, 1 kanan-atas, 2 kiri-bawah, 3 kanan-bawah
    float overlayScale = 2.0f;      // pembesaran font 5x7
    float overlayOpacity = 0.85f;
    int   overlayMargin = 24;       // px dari tepi layar

    bool  showFps = true;
    bool  showFrameTime = true;
    bool  showRam = true;
    bool  showMemUsage = true;
    bool  showDrawCalls = true;
    bool  showTexMem = true;

    // ---- Grafik ----
    bool  showGraph = true;
    int   graphMode = 0;            // 0 = frame time, 1 = FPS
    int   graphSamples = 120;       // maks 240
    float graphMaxMs = 50.0f;
    float graphMaxFps = 60.0f;
    int   fpsGood = 28;             // >= ini: hijau
    int   fpsWarn = 20;             // >= ini: kuning, di bawahnya merah

    // ---- Monitor GL (hook dipasang saat game start) ----
    bool  drawCallMonitor = true;
    bool  textureMemoryMonitor = true;

    // ---- FPS limiter ----
    int   fpsLimit = 0;             // 0 = mati, 30 / 40 / 45 / 60 (atau angka lain)
    bool  adaptiveLimiter = false;  // turun/naik antar tier 60-45-40-30 secara otomatis

    // ---- Low Memory Mode ----
    int   lowMemMode = 1;           // 0 = mati, 1 = otomatis, 2 = selalu aktif
    int   lowMemThresholdMB = 700;  // aktif otomatis bila RAM tersedia di bawah ini
    int   lowMemTrimSec = 10;       // jeda pemangkasan heap saat mode aktif

    // ---- Game tuning (opsional, perlu alamat sesuai versi libGTASA.so) ----
    bool        gameTuning = false;
    unsigned    lodScaleOffset32 = 0;
    unsigned    lodScaleOffset64 = 0;
    std::string lodScaleSymbol;
    float       lodScale = -1.0f;   // diisi oleh preset; <0 = jangan diubah

    // ---- Menu dalam game ----
    bool        menuEnabled = true;
    int         menuButtonPos = 4;  // 0 kiri-atas, 1 kanan-atas, 2 kiri-bawah, 3 kanan-bawah, 4 tengah-atas
    bool        menuDebug = false;  // tampilkan titik & nilai sentuhan terakhir
    int         touchDownType = 2;  // nilai "type" pada AND_TouchEvent: jari menyentuh layar
    int         touchUpType = 1;    //                                    jari diangkat
    int         touchMoveType = 3;  //                                    jari bergeser
    std::string touchSymbol;        // nama simbol fungsi sentuhan di libGTASA.so (kosong = otomatis)
    unsigned    touchOffset32 = 0;  // alternatif: offset dari base libGTASA.so (sertakan +1 bila Thumb)
    unsigned    touchOffset64 = 0;
};

extern const char* kDefaultIni;

// Muat file .ini. Mengembalikan false bila file tidak bisa dibaca (nilai default dipakai).
bool Config_Load(const char* path, Settings& out);

// Salin pengaturan + timpa nilai performa sesuai preset / low-memory.
// Tulis pengaturan ke file .ini. Komentar & urutan file yang sudah ada dipertahankan;
// hanya nilai tiap kunci yang diperbarui (kunci yang belum ada ditambahkan).
bool Config_Save(const char* path, const Settings& s);

Settings Config_Effective(const Settings& user, bool lowMemActive);

const char* Config_PresetName(int preset);
