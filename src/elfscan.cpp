#include "elfscan.h"

#include <elf.h>
#include <link.h>
#include <stdio.h>
#include <string.h>

namespace
{
struct LibSpan
{
    bool found = false;
    uintptr_t bias = 0, lo = 0, hi = 0;
    const ElfW(Phdr)* phdr = nullptr;
    int phnum = 0;
    const char* want = nullptr;
};

int PhdrCb(struct dl_phdr_info* info, size_t, void* data)
{
    LibSpan* s = (LibSpan*)data;
    if(!info->dlpi_name || !s->want || !strstr(info->dlpi_name, s->want)) return 0;

    s->found = true;
    s->bias = (uintptr_t)info->dlpi_addr;
    s->phdr = info->dlpi_phdr;
    s->phnum = info->dlpi_phnum;
    s->lo = ~(uintptr_t)0;
    s->hi = 0;
    for(int i = 0; i < info->dlpi_phnum; ++i)
    {
        const ElfW(Phdr)& p = info->dlpi_phdr[i];
        if(p.p_type != PT_LOAD) continue;
        uintptr_t a = s->bias + p.p_vaddr, b = a + p.p_memsz;
        if(a < s->lo) s->lo = a;
        if(b > s->hi) s->hi = b;
    }
    return 1;
}

bool In(const LibSpan& s, const void* p, size_t n)
{
    uintptr_t a = (uintptr_t)p;
    return a >= s.lo && a + n <= s.hi && a + n >= a;
}
} // namespace

bool Elf_ScanLoadedLib(const char* libSubstr, const char* filter1, const char* filter2,
                       const char* exact, const char* prefer, ElfScanResult& out)
{
    out.libFound = false;
    out.preferAddr = 0;
    out.preferName[0] = 0;
    out.totalSyms = out.hitCount = out.stored = 0;
    out.exactAddr = 0;

    LibSpan s;
    s.want = libSubstr;
    dl_iterate_phdr(PhdrCb, &s);
    if(!s.found) return false;
    out.libFound = true;
    out.bias = s.bias;

    const ElfW(Dyn)* dyn = nullptr;
    for(int i = 0; i < s.phnum; ++i)
        if(s.phdr[i].p_type == PT_DYNAMIC) dyn = (const ElfW(Dyn)*)(s.bias + s.phdr[i].p_vaddr);
    if(!dyn || !In(s, dyn, sizeof(*dyn))) return false;

    uintptr_t symtab = 0, strtab = 0, hash = 0, gnuhash = 0;
    for(int guard = 0; guard < 512 && In(s, dyn, sizeof(*dyn)) && dyn->d_tag != DT_NULL; ++dyn, ++guard)
    {
        switch(dyn->d_tag)
        {
            case DT_SYMTAB:   symtab  = (uintptr_t)dyn->d_un.d_ptr; break;
            case DT_STRTAB:   strtab  = (uintptr_t)dyn->d_un.d_ptr; break;
            case DT_HASH:     hash    = (uintptr_t)dyn->d_un.d_ptr; break;
            case DT_GNU_HASH: gnuhash = (uintptr_t)dyn->d_un.d_ptr; break;
            default: break;
        }
    }
    // Pada Android nilai d_ptr berupa alamat virtual (belum + bias); pada sebagian sistem sudah.
    auto fix = [&](uintptr_t p) { return (p && p < s.lo) ? p + s.bias : p; };
    symtab = fix(symtab); strtab = fix(strtab); hash = fix(hash); gnuhash = fix(gnuhash);
    if(!symtab || !strtab) return false;

    // ---- jumlah simbol ----
    uint32_t count = 0;
    if(hash && In(s, (void*)hash, 8))
    {
        count = ((const uint32_t*)hash)[1];                       // nchain
    }
    else if(gnuhash && In(s, (void*)gnuhash, 16))
    {
        const uint32_t* h = (const uint32_t*)gnuhash;
        uint32_t nb = h[0], symoff = h[1], bloom = h[2];
        const ElfW(Addr)* bl = (const ElfW(Addr)*)(h + 4);
        const uint32_t* buckets = (const uint32_t*)(bl + bloom);
        const uint32_t* chain = buckets + nb;
        if(!In(s, buckets, (size_t)nb * 4)) return false;
        uint32_t mx = 0;
        for(uint32_t b = 0; b < nb; ++b) if(buckets[b] > mx) mx = buckets[b];
        if(mx < symoff) count = symoff;
        else
        {
            uint32_t idx = mx;
            while(In(s, &chain[idx - symoff], 4) && !(chain[idx - symoff] & 1)) ++idx;
            count = idx + 1;
        }
    }
    if(count == 0 || count > 400000) return false;
    out.totalSyms = (int)count;

    const ElfW(Sym)* syms = (const ElfW(Sym)*)symtab;
    const char* strs = (const char*)strtab;
    if(!In(s, syms, (size_t)count * sizeof(ElfW(Sym)))) return false;

    for(uint32_t i = 0; i < count; ++i)
    {
        const ElfW(Sym)& y = syms[i];
        if(y.st_shndx == SHN_UNDEF || y.st_value == 0 || y.st_name == 0) continue;
        const char* nm = strs + y.st_name;
        if(!In(s, nm, 2)) continue;
        // panjang nama aman: batasi 159 karakter
        size_t len = 0;
        while(len < 159 && In(s, nm + len, 1) && nm[len]) ++len;
        if(len == 0 || len >= 159) continue;

        char buf[160];
        memcpy(buf, nm, len);
        buf[len] = 0;
        uintptr_t addr = s.bias + (uintptr_t)y.st_value;

        if(exact && !out.exactAddr && strcmp(buf, exact) == 0) out.exactAddr = addr;

        if(prefer && *prefer && !out.preferAddr && strstr(buf, prefer))
        {
            out.preferAddr = addr;
            snprintf(out.preferName, sizeof(out.preferName), "%s", buf);
        }

        bool match = (filter1 && *filter1 && strcasestr(buf, filter1)) ||
                     (filter2 && *filter2 && strcasestr(buf, filter2));
        if(match)
        {
            ++out.hitCount;
            if(out.stored < (int)(sizeof(out.hits) / sizeof(out.hits[0])))
            {
                snprintf(out.hits[out.stored].name, sizeof(out.hits[out.stored].name), "%s", buf);
                out.hits[out.stored].addr = addr;
                ++out.stored;
            }
        }
    }
    return true;
}
