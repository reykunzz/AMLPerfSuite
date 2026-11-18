#pragma once
#include <EGL/egl.h>

// Menggambar overlay performa ke framebuffer default. Panggil tepat sebelum eglSwapBuffers.
void Overlay_Draw(EGLDisplay dpy, EGLSurface surf);

// true bila thread ini sedang menggambar overlay (agar draw call overlay tidak ikut dihitung).
bool Overlay_IsDrawing();
