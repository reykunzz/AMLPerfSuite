#!/data/data/com.termux/files/usr/bin/bash
# Build AML Performance Suite di Termux.
#
# Cara utama : ndk-build dengan Android NDK untuk Termux (aarch64), menghasilkan
#              arm64-v8a DAN armeabi-v7a.
#              NDK Termux: https://github.com/lzhiyong/termux-ndk/releases
#              Ekstrak lalu: export ANDROID_NDK_HOME=$HOME/android-ndk-rXX
# Cadangan   : ./build-termux.sh --clang   (hanya arm64-v8a, eksperimental)
#
# Output     : out/arm64-v8a/libAMLPerfSuite.so, out/armeabi-v7a/libAMLPerfSuite.so
set -euo pipefail
cd "$(dirname "$0")"

MODE="ndk"
[ "${1:-}" = "--clang" ] && MODE="clang"

for tool in git make; do
    command -v "$tool" >/dev/null || { echo "Install dulu: pkg install $tool"; exit 1; }
done

bash ./setup-aml-headers.sh

AML_ROOT="$PWD/third_party/AndroidModLoader"
mkdir -p out

if [ "$MODE" = "ndk" ]; then
    NDKBUILD=""
    if command -v ndk-build >/dev/null 2>&1; then
        NDKBUILD="$(command -v ndk-build)"
    elif [ -n "${ANDROID_NDK_HOME:-}" ] && [ -x "$ANDROID_NDK_HOME/ndk-build" ]; then
        NDKBUILD="$ANDROID_NDK_HOME/ndk-build"
    fi

    if [ -z "$NDKBUILD" ]; then
        cat <<'EOF'
ndk-build tidak ditemukan.

Opsi:
  1) Pasang NDK Termux (aarch64) dari https://github.com/lzhiyong/termux-ndk/releases
     lalu:  export ANDROID_NDK_HOME=/path/ke/android-ndk-rXX
  2) Coba build cadangan (arm64 saja):  ./build-termux.sh --clang
  3) Build lewat GitHub Actions (lihat README.md), tanpa perlu NDK di HP.
EOF
        exit 1
    fi

    "$NDKBUILD" NDK_PROJECT_PATH="$PWD" APP_BUILD_SCRIPT="$PWD/Android.mk" \
        NDK_APPLICATION_MK="$PWD/Application.mk" AML_ROOT="$AML_ROOT" -j"$(nproc)"

    for abi in arm64-v8a armeabi-v7a; do
        if [ -f "libs/$abi/libAMLPerfSuite.so" ]; then
            mkdir -p "out/$abi"
            cp "libs/$abi/libAMLPerfSuite.so" "out/$abi/"
        fi
    done
else
    command -v clang++ >/dev/null || { echo "Install dulu: pkg install clang"; exit 1; }
    echo "[clang] Mode cadangan: hanya arm64-v8a, STL tidak di-link statis ke libc++_shared."
    mkdir -p out/arm64-v8a
    clang++ -shared -fPIC -std=c++17 -O2 -Wall -Wno-unused-function \
        -static-libstdc++ \
        -Iinclude -Isrc -I"$AML_ROOT" -I"$AML_ROOT/include" -I"$AML_ROOT/external" -I"$AML_ROOT/ARMPatch" \
        src/main.cpp src/config.cpp src/perf.cpp src/overlay.cpp src/menu.cpp \
        "$AML_ROOT/mod/logger.cpp" \
        -L/system/lib64 -llog -lEGL -lGLESv2 -ldl \
        -o out/arm64-v8a/libAMLPerfSuite.so
fi

cp config/AMLPerfSuite.ini out/AMLPerfSuite.ini
echo
echo "Selesai. Hasil build:"
find out -type f | sort
echo
echo "Salin .so yang cocok dengan ABI game ke:"
echo "  /sdcard/Android/data/com.rockstargames.gtasa/mods/"
