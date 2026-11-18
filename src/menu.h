#pragma once
// Menu pengaturan di dalam game (touch). Logika murni C++, tanpa API AML.

// Perintah gambar yang disediakan overlay.cpp (supaya menu memakai renderer yang sama).
struct MenuPainter
{
    void  (*rect)(float x, float y, float w, float h, float r, float g, float b, float a);
    void  (*text)(float x, float y, const char* s, float scale, float r, float g, float b, float a);
    float (*textWidth)(const char* s, float scale);
};

// Dipanggil dari hook sentuhan game (thread mana pun).
// Mengembalikan true bila sentuhan dipakai menu dan TIDAK boleh diteruskan ke game.
bool Menu_OnTouch(int type, int id, int x, int y);

// Dipanggil tiap frame dari thread render (di dalam Overlay_Draw):
// memproses sentuhan, lalu menggambar tombol / panel menu lewat painter.
void Menu_Frame(int screenW, int screenH, const MenuPainter& painter);

// Diisi main.cpp: true bila hook sentuhan berhasil dipasang.
void Menu_SetInputOk(bool ok);

bool Menu_IsOpen();
