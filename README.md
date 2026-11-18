# AML Performance Suite (GTA SA Android)

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

Ketuk tombol **MENU** kecil di layar (default: tengah-atas) untuk membuka panel pengaturan dengan 4 tab:
DISPLAY (overlay & grafik), STATS (baris yang ditampilkan), PERF (preset, limiter FPS, Low Memory),
SYSTEM (monitor GL, posisi tombol, simpan / reset). Perubahan langsung berlaku dan otomatis
disimpan ke `AMLPerfSuite.ini` (komentar di file tetap utuh). Tanda `*` = perlu restart game.

Menu memakai hook fungsi sentuhan game (`AND_TouchEvent`). Bila tombol tampil merah "NO TOUCH",
hook-nya tidak ketemu: lihat `AMLPerfSuite.log` di folder yang sama dengan `.ini`, lalu isi
`[Menu] TouchSymbol` atau `TouchOffset64`. Bila tombol bisa diketuk tapi tidak merespons, set
`[Menu] Debug = 1` dan sesuaikan `TouchDownType` / `TouchUpType` / `TouchMoveType` dengan nilai
`type` yang tertulis di log (12 sentuhan pertama dicatat).

## Catatan penting

- **Limiter hanya menurunkan FPS.** Batas atas ditentukan game; GTA SA umumnya berjalan 30 FPS, jadi nilai 45/60 baru berguna bila game/mod lain sudah menaikkan FPS-nya.
- **Draw Call & Texture Memory Monitor** memakai inline hook pada `libGLESv2.so`. Ini bagian yang paling bergantung pada perangkat. Bila game crash saat start, set `DrawCallMonitor = 0` dan/atau `TextureMemoryMonitor = 0` (berlaku setelah game dijalankan ulang). Memori tekstur adalah *perkiraan* dari ukuran upload, bukan angka resmi driver.
- **Low Memory Mode** tidak bisa mengecilkan tekstur game. Yang dilakukan: memangkas heap native (`malloc_trim` / `M_PURGE` bila tersedia), memaksa limiter 30 FPS, mematikan grafik, dan (opsional) menurunkan LOD scale.
- **LOD scale per preset** hanya aktif bila `[GameTuning] Enabled = 1` dan kamu mengisi offset atau nama simbol yang benar untuk versi `libGTASA.so`-mu. Secara bawaan bagian ini tidak menyentuh memori game.
- Sebagian besar kontrol dilakukan lewat file `.ini` (hot-reload), bukan menu di dalam game.

## Struktur

```
src/main.cpp      deklarasi mod AML, hook EGL/GL, jalur config, game tuning (satu-satunya file yang memakai API AML)
src/perf.cpp      statistik frame, memori, limiter, adaptive, low-memory, penghitung GL
src/overlay.cpp   renderer overlay GLES2 (teks bitmap 5x7, panel, grafik)
src/config.cpp    parser .ini, preset, nilai default
Android.mk / Application.mk   ndk-build (armeabi-v7a + arm64-v8a, API 21, c++_static)
```
