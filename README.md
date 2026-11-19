# AML Performance Suite (GTA SA Android)

**Author : ReyyXSA** · versi 1.2.0

Satu plugin untuk [AndroidModLoader](https://github.com/RusJJ/AndroidModLoader) yang berisi semua fitur kategori **Performance**:

| Fitur | Cara kerja |
|---|---|
| FPS Counter | Hitung `eglSwapBuffers` per jendela 0,5 detik |
| Frame Time Monitor | Selisih waktu antar swap; tampil terakhir / min / max / rata-rata |
| Performance Graph | Grafik batang frame time atau FPS (240 sampel) di overlay mode 2 |
| RAM Monitor | `MemAvailable` / `MemTotal` dari `/proc/meminfo` |
| Memory Usage Monitor | RSS + ruang alamat virtual (VM) proses game dari `/proc/self/status` |
| Draw Call Monitor | Hook `glDrawElements` / `glDrawArrays`, dihitung per frame (+ jumlah vertex) |
| Texture Memory Monitor | Hook `glTexImage2D` / `glCompressedTexImage2D` / `glDeleteTextures`, per level mip & cube face |
| Dynamic FPS Limiter | 30 / 40 / 45 / 60 (angka lain juga bisa), bisa diubah saat game jalan; mode *Adaptive* turun/naik otomatis |
| Low Memory Mode | Aktif otomatis bila RAM tersedia < ambang; memangkas heap native + memaksa profil hemat |
| Performance Preset | Low / Medium / High / Ultra (batas FPS, adaptive, grafik, LOD scale opsional) |
| Boost Performance | Prioritas thread game, core CPU besar, Android Performance Hint; mode MAX juga membuka vsync dan menjeda monitor GL (lihat bagian *Boost*) |
| Performance Overlay | Mini (1 baris) atau lengkap, 4 pojok, skala/opasitas bisa diatur; digambar langsung dengan OpenGL ES 2 |

Semua fitur menumpang pada hook `eglSwapBuffers`, jadi **tidak bergantung pada offset `libGTASA.so`** dan berlaku di semua versi GTA SA.

## Cara pakai

1. Pasang AML di GTA SA seperti biasa.
2. Salin `libAMLPerfSuite.so` yang **cocok dengan ABI game** ke folder mods:
   `/sdcard/Android/data/com.rockstargames.gtasa/mods/`
   - GTA SA 32-bit (mis. 2.00) → `armeabi-v7a`
   - GTA SA 64-bit → `arm64-v8a`
3. Jalankan game. File konfigurasi dibuat otomatis di:
   `/sdcard/Android/data/com.rockstargames.gtasa/files/AMLPerfSuite/AMLPerfSuite.ini`
4. Edit `.ini` kapan saja. Plugin membacanya ulang ±1 detik setelah file disimpan, tanpa restart game.

Contoh: ganti `Preset = High`, atau `[Overlay] Mode = 2` untuk panel lengkap + grafik, atau `[Limiter] FpsLimit = 45`.

## Build

Plugin ini memakai header AML. Skrip `setup-aml-headers.sh` mengunduhnya otomatis ke `third_party/AndroidModLoader`.

### A. GitHub Actions (disarankan, tanpa NDK di HP)

1. Buat repo baru di GitHub, unggah seluruh isi folder ini (termasuk `.github/`).
2. Buka tab **Actions** → workflow *Build AMLPerfSuite* berjalan otomatis (atau jalankan manual dengan *Run workflow*).
3. Unduh artifact **AMLPerfSuite** berisi `arm64-v8a/`, `armeabi-v7a/`, dan `AMLPerfSuite.ini`.
4. Untuk rilis: beri tag, mis. `git tag v1.0.0 && git push --tags`. File zip akan terlampir di halaman Releases.

### B. Termux

```bash
pkg install git make
# Pasang NDK untuk Termux (aarch64): https://github.com/lzhiyong/termux-ndk/releases
export ANDROID_NDK_HOME=$HOME/android-ndk-rXX
bash build-termux.sh
```

Hasil ada di `out/<abi>/libAMLPerfSuite.so`. Bila NDK sulit dipasang, `bash build-termux.sh --clang` mencoba build arm64 saja memakai `clang` bawaan Termux (eksperimental), atau pakai opsi A.

### C. Linux / PC

```bash
bash setup-aml-headers.sh
$ANDROID_NDK_HOME/ndk-build NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=Android.mk NDK_APPLICATION_MK=Application.mk
```

Bila package GTA SA kamu bukan `com.rockstargames.gtasa`, buka `Android.mk` dan aktifkan `-DAMLPERF_ANY_GAME`.

## Menu di dalam game

Ketuk tombol **MENU** di layar untuk membuka panel kecil di **sisi kiri layar**. Sentuhan di luar panel tetap diteruskan ke game, jadi game bisa dimainkan sambil menu terbuka. Semua perubahan disimpan otomatis ke `.ini`.

| Tab | Isi |
|---|---|
| VIEW | Mode overlay, pojok, ukuran & kepekatan teks, grafik |
| STATS | Baris statistik yang ditampilkan |
| PERF | Preset, batas FPS, adaptive limiter, Low Memory Mode |
| BOOST | Mode boost (OFF / ON / MAX) + status nyata tiap komponen di perangkatmu |
| SYS | Monitor GL (butuh restart), toast, posisi tombol, simpan / reset |
| INFO | Changelog + penjelasan fungsi tiap pengaturan (5 halaman, ketuk teks atau tombol `<` `>`) |

## Boost

Boost membantu game lebih lancar tanpa root. **Boost tidak bisa menaikkan FPS di atas batas yang dipasang game sendiri** (GTA SA umumnya 30 FPS); manfaatnya adalah berkurangnya patah-patah dan penurunan FPS. Hasilnya bergantung pada perangkat, jadi tab BOOST menampilkan komponen mana yang benar-benar berhasil.

| Mode | Yang dilakukan |
|---|---|
| OFF | Tidak melakukan apa pun (pengaturan thread dikembalikan seperti semula) |
| ON (bawaan) | Naikkan prioritas thread game, batasi thread ke core CPU besar, dan buka sesi Performance Hint Android |
| MAX | ON + buka kunci vsync (`eglSwapInterval 0`) + jeda pencacah draw call / memori tekstur. Lebih panas dan lebih boros baterai |

Status di tab BOOST: `THREAD PRIO` (NICE -8 / DENIED), `BIG CORES` (mis. 4-7 / N/A), `PERF HINT` (ON / N/A, butuh Android 13+), `VSYNC`, `GL MONITORS`.

Pengaturan lanjutan ada di `.ini` bagian `[Boost]`: `ThreadPriority`, `BigCores`, `PerfHint` (mode ON dan MAX), `UnlockVSync`, `PauseMonitors` (hanya MAX). Matikan komponen yang menurutmu mengganggu.

Batasan: tanpa root, Android bisa menolak prioritas atau afinitas (status `DENIED` / `N/A`); itu normal dan tidak berbahaya. Saat mode MAX, baris Draw Calls dan Texture Memory di overlay menampilkan `PAUSED`.

## Changelog

### v1.2.0
- **Boost Performance** (OFF / ON / MAX) dengan tab BOOST yang menampilkan status tiap komponen.
- Tampilan menu dibuat lebih polos dan lebih kecil, diletakkan di sisi kiri layar; sentuhan di luar panel diteruskan ke game.
- **Font GTA dihapus**; kembali ke font bitmap bawaan.
- Tab INFO diperbarui (changelog + fungsi BOOST).

### v1.1.0
- Tampilan menu bergaya GTA SA (diganti di v1.2.0).
- Font bergaya GTA (dihapus di v1.2.0).
- Nama author tampil di header menu dan di metadata mod AML.
- Tab **INFO**: changelog dan penjelasan fungsi tiap baris pengaturan.

### v1.0.0
- Rilis awal: FPS, frame time, grafik, RAM, memori aplikasi, draw call, memori tekstur, limiter FPS, preset, Low Memory Mode, overlay, dan menu sentuh.
