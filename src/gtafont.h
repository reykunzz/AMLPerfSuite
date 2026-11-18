#pragma once
// Font bergaya GTA SA (tebal, sedikit miring, outline hitam seperti teks HUD / Pricedown)
// yang dibangkitkan saat runtime dari bitmap 5x7 di font5x7.h.
//
// Bukan file font asli Rockstar: bitmap diperhalus (EPX), ditebalkan, dimiringkan,
// lalu diberi outline. Hasilnya disimpan dalam atlas 2 kanal (LUMINANCE_ALPHA):
//   L = isi huruf  -> diwarnai warna teks
//   A = isi+outline -> bagian outline otomatis hitam
// Header-only supaya build (Android.mk) tidak berubah.

#include <math.h>
#include <string.h>
#include <vector>

#include "font5x7.h"

namespace gtafont
{
enum
{
    K = 4,                       // piksel atlas per piksel logis font 5x7
    GW = 5 * K, GH = 7 * K,      // area glyph di atlas (20 x 28)
    PADL = 6, PADT = 6, PADR = 10, PADB = 6,
    CW = PADL + GW + PADR,       // lebar sel atlas
    CH = PADT + GH + PADB,       // tinggi sel atlas
    COLS = 16, ROWS = 5,
    AW = COLS * CW, AH = ROWS * CH,
    WHITE_CELL = 64              // sel putih penuh untuk menggambar kotak
};

// Parameter gaya (bisa disetel).
#ifndef GTAFONT_BOLD
#define GTAFONT_BOLD 1      // tebalkan (piksel atlas per sisi)
#endif
#ifndef GTAFONT_SMOOTH
#define GTAFONT_SMOOTH 2    // radius blur penghalus (piksel atlas)
#endif
#ifndef GTAFONT_SHEAR
#define GTAFONT_SHEAR 3     // kemiringan: geser baris paling atas ke kanan sebanyak ini
#endif
#ifndef GTAFONT_OUTLINE
#define GTAFONT_OUTLINE 3   // radius outline (piksel atlas)
#endif
const int   kBold    = GTAFONT_BOLD;
const int   kShear   = GTAFONT_SHEAR;
const int   kOutline = GTAFONT_OUTLINE;

typedef std::vector<float> Plane;   // CW x CH

inline int Idx(int x, int y) { return y * CW + x; }

// Blur kotak (2r+1) x (2r+1).
inline Plane BlurR(const Plane& s, int r)
{
    Plane d(s.size(), 0.0f);
    const float norm = (float)((2 * r + 1) * (2 * r + 1));
    for(int y = 0; y < CH; ++y)
        for(int x = 0; x < CW; ++x)
        {
            float sum = 0;
            for(int dy = -r; dy <= r; ++dy)
                for(int dx = -r; dx <= r; ++dx)
                {
                    int xx = x + dx, yy = y + dy;
                    if(xx >= 0 && yy >= 0 && xx < CW && yy < CH) sum += s[Idx(xx, yy)];
                }
            d[Idx(x, y)] = sum / norm;
        }
    return d;
}
inline Plane Blur3(const Plane& s) { return BlurR(s, 1); }

inline Plane Threshold(const Plane& s, float t)
{
    Plane d(s.size());
    for(size_t i = 0; i < s.size(); ++i) d[i] = s[i] >= t ? 1.0f : 0.0f;
    return d;
}

// Dilasi dengan kernel cakram berjari-jari r.
inline Plane Dilate(const Plane& s, int r)
{
    Plane d(s.size(), 0.0f);
    for(int y = 0; y < CH; ++y)
        for(int x = 0; x < CW; ++x)
        {
            float m = 0;
            for(int dy = -r; dy <= r && m < 1.0f; ++dy)
                for(int dx = -r; dx <= r; ++dx)
                {
                    if(dx * dx + dy * dy > r * r + r / 2) continue;
                    int xx = x + dx, yy = y + dy;
                    if(xx >= 0 && yy >= 0 && xx < CW && yy < CH && s[Idx(xx, yy)] > m) m = s[Idx(xx, yy)];
                }
            d[Idx(x, y)] = m;
        }
    return d;
}

inline void RenderGlyph(int glyph, unsigned char* la /* AW*AH*2 */, int cellX, int cellY)
{
    // 1) bitmap 5x7 -> 20x28 (pembesaran biasa; EPX sengaja tidak dipakai karena
    //    memotong semua sudut sehingga D mirip O dan B mirip 8)
    // 2) taruh di kanvas sel
    Plane f(CW * CH, 0.0f);
    for(int y = 0; y < GH; ++y)
        for(int x = 0; x < GW; ++x)
            f[Idx(PADL + x, PADT + y)] = ((kFont5x7[glyph][x / K] >> (y / K)) & 1) ? 1.0f : 0.0f;

    // 3) haluskan diagonal & bulatkan sudut sedikit: blur lalu ambang
    f = Threshold(BlurR(f, GTAFONT_SMOOTH), 0.5f);

    // 4) tebalkan
    if(kBold > 0) f = Dilate(f, kBold);

    // 5) miringkan (baris atas bergeser ke kanan)
    {
        Plane s(f.size(), 0.0f);
        int top = PADT, bottom = PADT + GH - 1;
        for(int y = 0; y < CH; ++y)
        {
            float t = (float)(bottom - y) / (float)(bottom - top);   // 1 di atas, 0 di bawah
            if(t < 0) t = 0;
            if(t > 1) t = 1;
            int shift = (int)floorf(t * kShear + 0.5f);
            for(int x = 0; x < CW; ++x)
            {
                int sx = x - shift;
                if(sx >= 0 && sx < CW) s[Idx(x, y)] = f[Idx(sx, y)];
            }
        }
        f = s;
    }

    // 6) isi huruf (anti-alias ringan) dan outline
    Plane fill = Blur3(f);
    Plane outline = Blur3(Dilate(f, kOutline));

    for(int y = 0; y < CH; ++y)
        for(int x = 0; x < CW; ++x)
        {
            float L = fill[Idx(x, y)];
            float A = fmaxf(L, outline[Idx(x, y)]);
            size_t o = ((size_t)(cellY + y) * AW + (cellX + x)) * 2;
            la[o]     = (unsigned char)(fminf(L, 1.0f) * 255.0f + 0.5f);
            la[o + 1] = (unsigned char)(fminf(A, 1.0f) * 255.0f + 0.5f);
        }
}

// Isi seluruh atlas. `la` berukuran AW * AH * 2 byte (luminance, alpha).
inline void BuildAtlas(unsigned char* la)
{
    memset(la, 0, (size_t)AW * AH * 2);
    for(int g = 1; g < 64; ++g)      // 0 = spasi, dibiarkan kosong
        RenderGlyph(g, la, (g % COLS) * CW, (g / COLS) * CH);

    int wx = (WHITE_CELL % COLS) * CW, wy = (WHITE_CELL / COLS) * CH;
    for(int y = 0; y < CH; ++y)
        for(int x = 0; x < CW; ++x)
        {
            size_t o = ((size_t)(wy + y) * AW + (wx + x)) * 2;
            la[o] = 255; la[o + 1] = 255;
        }
}

// Koordinat UV pusat sel putih (untuk kotak berwarna polos).
inline void WhiteUV(float* u, float* v)
{
    *u = ((WHITE_CELL % COLS) * CW + CW * 0.5f) / (float)AW;
    *v = ((WHITE_CELL / COLS) * CH + CH * 0.5f) / (float)AH;
}
} // namespace gtafont
