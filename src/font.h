#pragma once
// Font halus (anti-aliasing, proporsional) untuk overlay dan menu.
// Data atlas dibangkitkan dari Roboto oleh tools/genfont.py (lihat font_data.h).
//
// Semua teks di kode ditulis HURUF KAPITAL; Font_Case() mengubahnya saat digambar menjadi
// huruf kalimat ("THREAD PRIO" -> "Thread prio") dengan singkatan tetap kapital (FPS, RAM, GL ...).
// Ukuran: 1 satuan teks = 1 piksel font 5x7 lama pada skala 1, jadi seluruh tata letak lama tetap berlaku.

#include <ctype.h>
#include <stddef.h>
#include <string.h>
#include "font_data.h"

// Dekompres atlas (FONT_AW * FONT_AH byte, alpha) ke buf.
inline void Font_Unpack(uint8_t* buf)
{
    size_t o = 0, total = (size_t)FONT_AW * FONT_AH;
    for(unsigned i = 0; i < kFontRleLen && o < total;)
    {
        uint8_t v = kFontRle[i++];
        if(v == 0)
        {
            unsigned n = i < kFontRleLen ? kFontRle[i++] : 0;
            while(n-- && o < total) buf[o++] = 0;
        }
        else buf[o++] = v;
    }
    while(o < total) buf[o++] = 0;
    // blok putih untuk persegi polos
    for(int y = 0; y < 4; ++y)
        for(int x = 0; x < 4; ++x) buf[(size_t)(FONT_WHITE_Y + y) * FONT_AW + FONT_WHITE_X + x] = 255;
}

inline const FontGlyph& Font_Glyph(char ch)
{
    unsigned char c = (unsigned char)ch;
    if(c < 0x20 || c > 0x7E) c = '?';
    return kFontGlyphs[c - 0x20];
}

// Kata yang dipertahankan / ditulis dengan huruf tertentu. Dicari setelah huruf kapital penuh.
inline const char* Font_Special(const char* w, size_t n)
{
    static const char* const kWords[] = {
        "FPS", "RAM", "GL", "CPU", "GPU", "UI", "ID", "MS", "MB", "GB", "KB", "DC", "RSS", "VM", "LOD",
        "AML", "GTA", "SA", "MAX", "ADPF", "ANDROID", "RGB", "HUD", "SYS", "BTM", "TEX", "MON", "SDK", "API",
        nullptr};
    static const char* const kMapped[][2] = {
        {"REYYXSA", "ReyyXSA"}, {"VSYNC", "VSync"}, {"ANDROID", "Android"}, {"LOWMEM", "LowMem"},
        {"PERF", "Perf"}, {"BTM", "Btm"}, {"TEX", "Tex"}, {"MON", "Mon"}, {"SYS", "Sys"}, {"MAX", "Max"},
        {nullptr, nullptr}};
    for(int i = 0; kMapped[i][0]; ++i)
        if(strlen(kMapped[i][0]) == n && strncmp(kMapped[i][0], w, n) == 0) return kMapped[i][1];
    for(int i = 0; kWords[i]; ++i)
        if(strlen(kWords[i]) == n && strncmp(kWords[i], w, n) == 0) return w;   // tetap kapital
    return nullptr;
}

// Ubah teks KAPITAL menjadi huruf kalimat. Aman untuk in == out tidak; out berukuran n (termasuk NUL).
inline void Font_Case(const char* in, char* out, size_t n)
{
    size_t o = 0;
    bool sentenceStart = true;      // huruf pertama string atau setelah ". "
    const char* p = in;
    while(*p && o + 1 < n)
    {
        if(!isalpha((unsigned char)*p))
        {
            if(*p == '.' && (p[1] == ' ')) sentenceStart = true;
            out[o++] = *p++;
            continue;
        }
        const char* st = p;
        bool hasLower = false;
        while(isalpha((unsigned char)*p)) { if(islower((unsigned char)*p)) hasLower = true; ++p; }
        size_t len = (size_t)(p - st);
        bool afterDigit = st > in && isdigit((unsigned char)st[-1]);
        bool beforeDigit = isdigit((unsigned char)*p);
        const char* special = hasLower ? nullptr : Font_Special(st, len);
        for(size_t i = 0; i < len && o + 1 < n; ++i)
        {
            char c = st[i];
            if(hasLower) { out[o++] = c; continue; }                      // sudah huruf campuran: jangan diubah
            if(special)       c = special[i];
            else if(len == 1 || afterDigit || (beforeDigit && len <= 2)) { /* huruf tunggal / satuan: kapital */ }
            else              c = (char)(i == 0 && sentenceStart ? c : tolower((unsigned char)c));
            out[o++] = c;
        }
        // singkatan di awal kalimat tetap tertulis menurut aturannya
        sentenceStart = false;
    }
    out[o] = 0;
}

// Lebar teks (satuan teks) setelah Font_Case, pada skala s.
inline float Font_Width(const char* str, float s)
{
    char buf[200];
    Font_Case(str, buf, sizeof(buf));
    float w = 0;
    for(const char* p = buf; *p; ++p) w += Font_Glyph(*p).adv;
    return w / kFontPxPerUnit * s;
}
