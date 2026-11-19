#!/data/data/com.termux/files/usr/bin/bash
# Ganti ketebalan font tanpa mengirim file lagi.
# Pakai:  bash tools/setfont.sh [tipis|sedang|tebal|sangat|ANGKA 400-900]
#         bash tools/setfont.sh tebal --build     (sekaligus build ulang)
set -e
cd "$(dirname "$0")/.."
case "${1:-tebal}" in
  tipis) W=400;; sedang) W=500;; tebal) W=650;; sangat) W=800;;
  *[!0-9]*) echo "Pilihan: tipis | sedang | tebal | sangat | angka 400-900"; exit 1;;
  *) W="$1";;
esac
python3 -c "import PIL" 2>/dev/null || pkg install -y python-pillow
TTF="$HOME/.cache/amlperf-Roboto.ttf"
if [ ! -s "$TTF" ]; then
  mkdir -p "$(dirname "$TTF")"
  curl -fL -o "$TTF" "https://github.com/google/fonts/raw/main/ofl/roboto/Roboto%5Bwdth%2Cwght%5D.ttf"
fi
python3 tools/genfont.py "$TTF" "$W"
echo "Font bobot $W siap (src/font_data.h)."
if [ "$2" = "--build" ]; then bash build-termux.sh; else echo "Lanjut: bash build-termux.sh"; fi
