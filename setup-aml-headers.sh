#!/usr/bin/env bash
# Mengambil source AndroidModLoader (hanya untuk header mod) ke third_party/AndroidModLoader.
# Dipakai oleh GitHub Actions maupun build lokal / Termux.
set -euo pipefail
cd "$(dirname "$0")"

DEST="third_party/AndroidModLoader"

if [ -f "$DEST/mod/amlmod.h" ] || [ -f include/mod/amlmod.h ]; then
    echo "[aml] Header AML sudah ada, dilewati."
    exit 0
fi

mkdir -p third_party
rm -rf "$DEST"

echo "[aml] Mengunduh AndroidModLoader ..."
if ! git clone --depth 1 --recurse-submodules --shallow-submodules \
        https://github.com/RusJJ/AndroidModLoader.git "$DEST" 2>/dev/null; then
    echo "[aml] Clone dengan submodule gagal, mencoba tanpa submodule ..."
    rm -rf "$DEST"
    git clone --depth 1 https://github.com/RusJJ/AndroidModLoader.git "$DEST"
fi

if [ ! -f "$DEST/mod/amlmod.h" ]; then
    FOUND="$(find "$DEST" -name amlmod.h | head -n1 || true)"
    if [ -z "$FOUND" ]; then
        echo "[aml] ERROR: amlmod.h tidak ditemukan di repo AML." >&2
        exit 1
    fi
    # Susun ulang supaya #include <mod/amlmod.h> tetap berjalan
    mkdir -p include/mod
    cp -r "$(dirname "$FOUND")"/. include/mod/
    echo "[aml] Header disalin ke include/mod dari: $(dirname "$FOUND")"
fi

echo "[aml] OK."
