#!/usr/bin/env python3
"""Membangkitkan src/font_data.h: atlas huruf halus (anti-aliasing) dari Roboto (SIL Open Font License, lihat FONT-LICENSE-Roboto.txt).
Pakai:  python3 tools/genfont.py /path/Roboto.ttf [bobot 400-900, bawaan 650]
Atlas disimpan 8-bit alpha, dikompres run-length untuk angka nol (0,n)."""
import sys
from PIL import Image, ImageDraw, ImageFont

AW, AH = 512, 128          # ukuran atlas (pangkat 2)
EM_PX = 32                 # ukuran font di atlas (piksel)
EM_UNIT = 10.6             # tinggi em dalam "satuan" teks (1 satuan = 1 piksel font 5x7 lama pada skala 1)
PAD = 2
path = sys.argv[1]
WEIGHT = int(sys.argv[2]) if len(sys.argv) > 2 else 650   # 400 tipis .. 900 sangat tebal
font = ImageFont.truetype(path, EM_PX)
try:
    font.set_variation_by_axes([WEIGHT, 100])   # Roboto variabel: sumbu [bobot, lebar]
except Exception:
    pass
asc, desc = font.getmetrics()
atlas = Image.new("L", (AW, AH), 0)
x = y = PAD; rowh = 0
glyphs = []
for code in range(0x20, 0x7F):
    ch = chr(code)
    adv = font.getlength(ch)
    l, t, r, b = font.getbbox(ch, anchor="ls")     # relatif ke garis dasar
    w, h = max(0, r - l), max(0, b - t)
    if w and h:
        if x + w + PAD > AW: x = PAD; y += rowh + PAD; rowh = 0
        img = Image.new("L", (w, h), 0)
        ImageDraw.Draw(img).text((-l, -t), ch, font=font, fill=255, anchor="ls")
        atlas.paste(img, (x, y))
        glyphs.append((code, adv, l, -t, x, y, w, h))   # oy = tinggi di atas garis dasar
        x += w + PAD; rowh = max(rowh, h)
    else:
        glyphs.append((code, adv, 0, 0, 0, 0, 0, 0))
# blok putih 4x4 untuk persegi polos
if x + 6 > AW: x = PAD; y += rowh + PAD; rowh = 0
wx, wy = x, y
for j in range(4):
    for i in range(4): atlas.putpixel((wx + i, wy + j), 255)
y_used = y + 4 + PAD
data = list(atlas.tobytes())
rle = []; i = 0
while i < len(data):
    if data[i] == 0:
        n = 0
        while i < len(data) and data[i] == 0 and n < 255: n += 1; i += 1
        rle += [0, n]
    else:
        rle.append(data[i]); i += 1
out = []
out.append("// DIBANGKITKAN oleh tools/genfont.py dari Roboto (SIL OFL 1.1, lihat FONT-LICENSE-Roboto.txt). Jangan diedit tangan.")
out.append("#pragma once\n#include <stdint.h>")
out.append(f"enum {{ FONT_AW = {AW}, FONT_AH = {AH}, FONT_WHITE_X = {wx}, FONT_WHITE_Y = {wy} }};")
out.append(f"static const float kFontPxPerUnit = {EM_PX / EM_UNIT:.5f}f;   // piksel atlas per satuan teks")
out.append("struct FontGlyph { float adv, ox, oy; uint16_t x, y, w, h; };   // adv/ox/oy dalam piksel atlas; oy = di atas garis dasar")
out.append("static const FontGlyph kFontGlyphs[95] = {")
for (code, adv, ox, oy, gx, gy, gw, gh) in glyphs:
    out.append(f"  {{{adv:.3f}f,{ox},{oy},{gx},{gy},{gw},{gh}}},")
out.append("};")
out.append(f"static const unsigned kFontRleLen = {len(rle)};")
out.append("static const uint8_t kFontRle[] = {")
for k in range(0, len(rle), 32):
    out.append("  " + ",".join(str(v) for v in rle[k:k+32]) + ",")
out.append("};")
open("src/font_data.h", "w").write("\n".join(out) + "\n")
atlas.crop((0, 0, AW, y_used)).save("/tmp/atlas_preview.png")
print("glyphs", len(glyphs), "rle bytes", len(rle), "atlas rows used", y_used, "asc/desc", asc, desc)
