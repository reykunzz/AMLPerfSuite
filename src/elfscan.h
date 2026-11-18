#pragma once
#include <stddef.h>
#include <stdint.h>

// Pemindai tabel simbol (.dynsym) library yang sudah dimuat di memori proses.
// Tidak memakai API AML: hanya dl_iterate_phdr + struktur ELF.

struct ElfSymHit { char name[160]; uintptr_t addr; };

struct ElfScanResult
{
    bool      libFound = false;
    uintptr_t bias = 0;            // load bias (alamat dasar library)
    int       totalSyms = 0;       // jumlah entri di .dynsym
    uintptr_t exactAddr = 0;       // alamat simbol yang cocok persis dengan 'exact'
    int       hitCount = 0;        // jumlah simbol cocok filter (bisa > jumlah yang disimpan)
    int       stored = 0;
    uintptr_t preferAddr = 0;      // simbol pertama yang namanya memuat teks 'prefer' (dicari di SEMUA simbol)
    char      preferName[160] = {0};
    ElfSymHit hits[160];
};

// libSubstr : potongan nama library, mis. "libGTASA.so"
// filter1/2 : simbol yang namanya memuat teks ini (tanpa beda huruf besar/kecil) dicatat di hits[]
// exact     : nama simbol persis yang dicari (boleh nullptr)
// prefer    : teks yang dicari pada nama simbol mana pun, hasilnya di preferAddr/preferName (boleh nullptr)
// Mengembalikan true bila library ditemukan dan tabel simbolnya terbaca.
bool Elf_ScanLoadedLib(const char* libSubstr, const char* filter1, const char* filter2,
                       const char* exact, const char* prefer, ElfScanResult& out);
