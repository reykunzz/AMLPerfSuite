#include "overlay.h"

#include <GLES2/gl2.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "gtafont.h"
#include "menu.h"
#include "perf.h"

namespace
{
thread_local bool tlDrawing = false;

// ---------------------------------------------------------------- sumber daya GL
struct Gfx
{
    EGLContext ctx = EGL_NO_CONTEXT;
    GLuint prog = 0, tex = 0;
    GLint locRes = -1, locTex = -1;
    bool ok = false;
    bool failed = false;
    int  w = 0, h = 0, sizeAge = 1000;
};
Gfx gGfx;

// Metrik teks logis (tidak berubah dari versi sebelumnya): 1 huruf = 6 x 7 satuan.
enum { CELL_W = 6 };

const char* kVS =
    "attribute vec2 aPos;\n"
    "attribute vec2 aUV;\n"
    "attribute vec4 aCol;\n"
    "uniform vec2 uRes;\n"
    "varying vec2 vUV;\n"
    "varying vec4 vCol;\n"
    "void main(){\n"
    "  vec2 p = aPos / uRes;\n"
    "  gl_Position = vec4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);\n"
    "  vUV = aUV; vCol = aCol;\n"
    "}\n";

const char* kFS =
    "precision mediump float;\n"
    "uniform sampler2D uTex;\n"
    "varying vec2 vUV;\n"
    "varying vec4 vCol;\n"
    "void main(){\n"
    "  vec4 t = texture2D(uTex, vUV);\n"
    "  gl_FragColor = vec4(mix(vec3(0.0), vCol.rgb, t.r), vCol.a * t.a);\n"   // t.r = isi huruf, t.a = isi + outline
    "}\n";

GLuint Compile(GLenum type, const char* src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if(!ok)
    {
        char log[256] = {0};
        glGetShaderInfoLog(s, sizeof(log) - 1, nullptr, log);
        PerfLog("Shader overlay gagal: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

bool CreateGfx(EGLContext ctx)
{
    Gfx& g = gGfx;
    g.ctx = ctx;
    g.ok = false;

    GLuint vs = Compile(GL_VERTEX_SHADER, kVS);
    GLuint fs = Compile(GL_FRAGMENT_SHADER, kFS);
    if(!vs || !fs) return false;

    g.prog = glCreateProgram();
    glAttachShader(g.prog, vs);
    glAttachShader(g.prog, fs);
    glBindAttribLocation(g.prog, 0, "aPos");
    glBindAttribLocation(g.prog, 1, "aUV");
    glBindAttribLocation(g.prog, 2, "aCol");
    glLinkProgram(g.prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = 0;
    glGetProgramiv(g.prog, GL_LINK_STATUS, &linked);
    if(!linked)
    {
        PerfLog("Link program overlay gagal");
        glDeleteProgram(g.prog);
        g.prog = 0;
        return false;
    }
    g.locRes = glGetUniformLocation(g.prog, "uRes");
    g.locTex = glGetUniformLocation(g.prog, "uTex");

    // atlas font GTA (LUMINANCE_ALPHA: L = isi huruf, A = isi + outline hitam)
    std::vector<unsigned char> atlas((size_t)gtafont::AW * gtafont::AH * 2);
    gtafont::BuildAtlas(atlas.data());

    GLint prevTex = 0, prevAlign = 4;
    GLint prevActive = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevAlign);

    glGenTextures(1, &g.tex);
    glBindTexture(GL_TEXTURE_2D, g.tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, gtafont::AW, gtafont::AH, 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, atlas.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glPixelStorei(GL_UNPACK_ALIGNMENT, prevAlign);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    glActiveTexture((GLenum)prevActive);

    g.ok = true;
    return true;
}

// ---------------------------------------------------------------- batch vertex
struct Vtx { float x, y, u, v, r, g, b, a; };
std::vector<Vtx> gV;

struct Col { float r, g, b, a; };
const Col kWhite  = {1.00f, 1.00f, 1.00f, 1.0f};
const Col kGreen  = {0.35f, 1.00f, 0.45f, 1.0f};
const Col kYellow = {1.00f, 0.90f, 0.25f, 1.0f};
const Col kRed    = {1.00f, 0.32f, 0.30f, 1.0f};
const Col kOrange = {1.00f, 0.62f, 0.20f, 1.0f};
const Col kGray   = {0.70f, 0.72f, 0.75f, 1.0f};

Col WithAlpha(Col c, float a) { c.a *= a; return c; }

void Quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, const Col& c)
{
    Vtx a = {x0, y0, u0, v0, c.r, c.g, c.b, c.a};
    Vtx b = {x1, y0, u1, v0, c.r, c.g, c.b, c.a};
    Vtx d = {x1, y1, u1, v1, c.r, c.g, c.b, c.a};
    Vtx e = {x0, y1, u0, v1, c.r, c.g, c.b, c.a};
    gV.push_back(a); gV.push_back(b); gV.push_back(d);
    gV.push_back(a); gV.push_back(d); gV.push_back(e);
}

void Rect(float x, float y, float w, float h, const Col& c)
{
    float u, v;
    gtafont::WhiteUV(&u, &v);
    Quad(x, y, x + w, y + h, u, v, u, v, c);
}

// Satu huruf: kotak logis 5 x 7 satuan (x, y = pojok kiri-atas). Quad digambar sedikit lebih
// besar supaya outline dan kemiringan ikut tergambar.
void Glyph(float x, float y, char ch, float s, const Col& c)
{
    using namespace gtafont;
    if(ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
    if(ch < 0x20 || ch > 0x5F) ch = '?';
    int idx = ch - 0x20;
    if(idx == 0) return;

    const float k = s / (float)K;                  // piksel layar per piksel atlas
    const float cx = (float)((idx % COLS) * CW), cy = (float)((idx / COLS) * CH);
    Quad(x - PADL * k, y - PADT * k, x + (CW - PADL) * k, y + (CH - PADT) * k,
         (cx + 0.5f) / AW, (cy + 0.5f) / AH, (cx + CW - 0.5f) / AW, (cy + CH - 0.5f) / AH, c);
}

// Outline hitam sudah menyatu di atlas, jadi cukup satu lintasan.
void Text(float x, float y, const char* str, float s, const Col& c, float alpha)
{
    Col fg = WithAlpha(c, alpha);
    for(const char* p = str; *p; ++p, x += CELL_W * s)
        Glyph(x, y, *p, s, fg);
}

float TextWidth(const char* str, float s) { return (float)strlen(str) * CELL_W * s; }

// ---------------------------------------------------------------- isi overlay
struct Line { char text[96]; Col col; };

Col FpsColor(float fps, const Settings& c)
{
    if(fps >= c.fpsGood) return kGreen;
    if(fps >= c.fpsWarn) return kYellow;
    return kRed;
}

void FmtMem(char* out, size_t n, int mb)
{
    if(mb >= 1024) snprintf(out, n, "%.1fG", mb / 1024.0f);
    else snprintf(out, n, "%dM", mb);
}

void BuildLines(std::vector<Line>& lines)
{
    const PerfState& p = g_perf;
    const Settings& c = p.cfg;
    char buf[96];

    auto add = [&](const char* t, const Col& col) {
        Line l; snprintf(l.text, sizeof(l.text), "%s", t); l.col = col; lines.push_back(l);
    };

    if(c.overlayMode == 1)   // mini: satu baris
    {
        std::string s;
        auto part = [&](const char* t) { if(!s.empty()) s += "  "; s += t; };
        if(c.showFps)       { snprintf(buf, sizeof(buf), "%.0f FPS", p.fps); part(buf); }
        if(c.showFrameTime) { snprintf(buf, sizeof(buf), "%.1f MS", p.frameMs); part(buf); }
        if(c.showRam && p.mem.valid)
        {
            char f[16]; FmtMem(f, sizeof(f), p.mem.availMB);
            snprintf(buf, sizeof(buf), "RAM %s", f); part(buf);
        }
        if(c.showDrawCalls) { snprintf(buf, sizeof(buf), "DC %u", p.drawCalls); part(buf); }
        if(p.lowMemActive)  part("LOWMEM");
        if(!s.empty()) add(s.c_str(), c.showFps ? FpsColor(p.fps, c) : kWhite);
        return;
    }

    // lengkap
    if(c.showFps)
    {
        snprintf(buf, sizeof(buf), "FPS %.0f  AVG %.1f  1%% LOW %.0f", p.fps, p.avgMs > 0 ? 1000.0f / p.avgMs : 0.0f, p.low1Fps);
        add(buf, FpsColor(p.fps, c));
    }
    if(c.showFrameTime)
    {
        snprintf(buf, sizeof(buf), "FRAME %.1f MS  MIN %.1f  MAX %.1f", p.frameMs, p.minMs, p.maxMs);
        add(buf, kWhite);
    }
    if(c.showRam && p.mem.valid)
    {
        snprintf(buf, sizeof(buf), "RAM FREE %d / %d MB", p.mem.availMB, p.mem.totalMB);
        add(buf, p.mem.availMB < p.user.lowMemThresholdMB ? kOrange : kWhite);
    }
    if(c.showMemUsage && p.mem.valid)
    {
        snprintf(buf, sizeof(buf), "APP RSS %d MB  VM %d MB", p.mem.rssMB, p.mem.vmMB);
        add(buf, kWhite);
    }
    if(c.showDrawCalls)
    {
        if(c.drawCallMonitor)
        {
            snprintf(buf, sizeof(buf), "DRAW %u CALLS  %.1fK VERTS", p.drawCalls, p.drawVerts / 1000.0f);
            add(buf, kWhite);
        }
        else add("DRAW CALLS: MONITOR OFF", kGray);
    }
    if(c.showTexMem)
    {
        if(c.textureMemoryMonitor)
        {
            snprintf(buf, sizeof(buf), "TEX %.0f MB  (%d)", p.texBytes / (1024.0 * 1024.0), p.texCount);
            add(buf, kWhite);
        }
        else add("TEXTURE MEM: MONITOR OFF", kGray);
    }

    char lim[24];
    if(p.targetFps > 0) snprintf(lim, sizeof(lim), "%d", p.targetFps); else snprintf(lim, sizeof(lim), "OFF");
    snprintf(buf, sizeof(buf), "LIMIT %s%s  PRESET %s", lim, c.adaptiveLimiter && c.fpsLimit > 0 ? " (AUTO)" : "", Config_PresetName(p.user.preset));
    add(buf, kGray);
    if(p.lowMemActive) add("LOW MEMORY MODE ACTIVE", kOrange);
}

void BuildGraph(float x, float y, float w, float h, float s, float opacity)
{
    const PerfState& p = g_perf;
    const Settings& c = p.cfg;
    int n = c.graphSamples;
    if(n > p.histCount) n = p.histCount;
    if(n <= 1) return;

    Rect(x, y, w, h, WithAlpha(Col{0, 0, 0, 0.45f}, opacity));

    float barW = w / c.graphSamples;
    float goodMs = 1000.0f / (float)c.fpsGood * 1.1f;
    float warnMs = 1000.0f / (float)c.fpsWarn;

    for(int i = 0; i < n; ++i)
    {
        int idx = (p.histHead - n + i + HIST_N * 2) % HIST_N;
        float ms = p.hist[idx];
        float norm = c.graphMode == 0 ? ms / c.graphMaxMs : (ms > 0.01f ? (1000.0f / ms) / c.graphMaxFps : 0.0f);
        if(norm > 1.0f) norm = 1.0f;
        if(norm < 0.02f) norm = 0.02f;
        const Col& col = ms <= goodMs ? kGreen : (ms <= warnMs ? kYellow : kRed);
        float bx = x + (c.graphSamples - n + i) * barW;
        Rect(bx, y + h - h * norm, barW > 1.0f ? barW - 0.5f : barW, h * norm, WithAlpha(col, opacity));
    }

    char lbl[48];
    if(c.graphMode == 0) snprintf(lbl, sizeof(lbl), "FRAME TIME 0-%.0f MS", c.graphMaxMs);
    else snprintf(lbl, sizeof(lbl), "FPS 0-%.0f", c.graphMaxFps);
    Text(x + 2 * s, y + 2 * s, lbl, s * 0.75f, kGray, opacity);
}

// ---------------------------------------------------------------- simpan / pulihkan state GL
struct SavedAttrib { GLint enabled, size, type, norm, stride, buf; GLvoid* ptr; };

struct SavedState
{
    GLint prog, arrayBuf, elemBuf, activeTex, tex0, fbo, viewport[4];
    GLint blendSrcRGB, blendDstRGB, blendSrcA, blendDstA, blendEqRGB, blendEqA;
    GLint align, vao;
    GLboolean blend, depth, cull, scissor, stencil, dither;
    GLboolean colorMask[4], depthMask;
    SavedAttrib attr[3];
    bool hasVao;
};

typedef void (*PFNBindVAO)(GLuint);
PFNBindVAO gBindVAO = nullptr;
bool gVaoChecked = false;

void SaveState(SavedState& s)
{
    glGetIntegerv(GL_CURRENT_PROGRAM, &s.prog);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.arrayBuf);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &s.elemBuf);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s.fbo);
    glGetIntegerv(GL_VIEWPORT, s.viewport);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s.activeTex);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &s.align);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.tex0);
    glActiveTexture((GLenum)s.activeTex);

    glGetIntegerv(GL_BLEND_SRC_RGB, &s.blendSrcRGB);
    glGetIntegerv(GL_BLEND_DST_RGB, &s.blendDstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &s.blendSrcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &s.blendDstA);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &s.blendEqRGB);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &s.blendEqA);
    s.blend = glIsEnabled(GL_BLEND);
    s.depth = glIsEnabled(GL_DEPTH_TEST);
    s.cull = glIsEnabled(GL_CULL_FACE);
    s.scissor = glIsEnabled(GL_SCISSOR_TEST);
    s.stencil = glIsEnabled(GL_STENCIL_TEST);
    s.dither = glIsEnabled(GL_DITHER);
    glGetBooleanv(GL_COLOR_WRITEMASK, s.colorMask);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &s.depthMask);

    for(int i = 0; i < 3; ++i)
    {
        SavedAttrib& a = s.attr[i];
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &a.enabled);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a.size);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &a.type);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &a.norm);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &a.stride);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &a.buf);
        a.ptr = nullptr;
        glGetVertexAttribPointerv(i, GL_VERTEX_ATTRIB_ARRAY_POINTER, &a.ptr);
    }

    // Konteks ES 3.x: pastikan VAO default aktif supaya client-side array berlaku.
    s.hasVao = false;
    if(!gVaoChecked)
    {
        gVaoChecked = true;
        const char* ver = (const char*)glGetString(GL_VERSION);
        if(ver && strstr(ver, "OpenGL ES 3"))
            gBindVAO = (PFNBindVAO)eglGetProcAddress("glBindVertexArray");
    }
    if(gBindVAO)
    {
        glGetIntegerv(0x85B5 /* GL_VERTEX_ARRAY_BINDING */, &s.vao);
        s.hasVao = true;
        gBindVAO(0);
    }
}

void RestoreState(const SavedState& s)
{
    if(s.hasVao && gBindVAO) gBindVAO((GLuint)s.vao);

    for(int i = 0; i < 3; ++i)
    {
        const SavedAttrib& a = s.attr[i];
        glBindBuffer(GL_ARRAY_BUFFER, (GLuint)a.buf);
        if(a.size > 0 && a.type)
            glVertexAttribPointer(i, a.size, (GLenum)a.type, (GLboolean)a.norm, a.stride, a.ptr);
        if(a.enabled) glEnableVertexAttribArray(i); else glDisableVertexAttribArray(i);
    }

    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)s.arrayBuf);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)s.elemBuf);
    glUseProgram((GLuint)s.prog);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)s.fbo);
    glViewport(s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)s.tex0);
    glActiveTexture((GLenum)s.activeTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, s.align);

    glBlendFuncSeparate((GLenum)s.blendSrcRGB, (GLenum)s.blendDstRGB, (GLenum)s.blendSrcA, (GLenum)s.blendDstA);
    glBlendEquationSeparate((GLenum)s.blendEqRGB, (GLenum)s.blendEqA);
    if(s.blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if(s.depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if(s.cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if(s.scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if(s.stencil) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    if(s.dither) glEnable(GL_DITHER); else glDisable(GL_DITHER);
    glColorMask(s.colorMask[0], s.colorMask[1], s.colorMask[2], s.colorMask[3]);
    glDepthMask(s.depthMask);
}
} // namespace

// ---------------------------------------------------------------- jembatan ke menu
static void PaintRect(float x, float y, float w, float h, float r, float g, float b, float a)
{
    Rect(x, y, w, h, Col{r, g, b, a});
}

static void PaintText(float x, float y, const char* str, float s, float r, float g, float b, float a)
{
    Text(x, y, str, s, Col{r, g, b, 1.0f}, a);
}

static float PaintTextWidth(const char* str, float s) { return TextWidth(str, s); }

bool Overlay_IsDrawing() { return tlDrawing; }

// Geometri overlay statistik (teks + grafik) ke gV.
static void BuildOverlayGeometry(const Settings& c, int screenW, int screenH)
{
    std::vector<Line> lines;
    BuildLines(lines);

    const float s = c.overlayScale;
    const float pad = 3 * s;
    const float lineH = 9 * s;
    float textW = 0;
    for(auto& l : lines) textW = fmaxf(textW, TextWidth(l.text, s));

    const bool graph = c.overlayMode == 2 && c.showGraph && g_perf.histCount > 1;
    float graphH = 20 * s;
    float graphW = fmaxf(textW, c.graphSamples * fmaxf(1.0f, s * 0.5f));
    float boxW = fmaxf(textW, graph ? graphW : 0) + pad * 2;
    float boxH = lines.size() * lineH + pad * 2 + (graph ? graphH + pad : 0);
    if(lines.empty() && !graph) return;

    float bx = (float)c.overlayMargin;
    float by = (float)c.overlayMargin;
    if(c.overlayCorner == 1 || c.overlayCorner == 3) bx = screenW - c.overlayMargin - boxW;
    if(c.overlayCorner == 2 || c.overlayCorner == 3) by = screenH - c.overlayMargin - boxH;

    Rect(bx, by, boxW, boxH, Col{0, 0, 0, 0.40f * c.overlayOpacity});

    float ty = by + pad;
    for(auto& l : lines)
    {
        Text(bx + pad, ty, l.text, s, l.col, c.overlayOpacity);
        ty += lineH;
    }
    if(graph)
        BuildGraph(bx + pad, ty + pad * 0.5f, boxW - pad * 2, graphH, s, c.overlayOpacity);
}

void Overlay_Draw(EGLDisplay dpy, EGLSurface surf)
{
    const bool wantMenu = g_perf.user.menuEnabled;
    if((g_perf.cfg.overlayMode == 0 && !wantMenu) || gGfx.failed) return;

    EGLContext ctx = eglGetCurrentContext();
    if(ctx == EGL_NO_CONTEXT) return;

    // ukuran surface (disegarkan tiap ~1 detik)
    if(++gGfx.sizeAge > 60 || gGfx.w <= 0)
    {
        gGfx.sizeAge = 0;
        EGLint w = 0, h = 0;
        eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
        eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
        gGfx.w = w; gGfx.h = h;
    }
    if(gGfx.w <= 0 || gGfx.h <= 0) return;

    tlDrawing = true;

    SavedState st;
    SaveState(st);

    if(!gGfx.ok || gGfx.ctx != ctx)
    {
        gGfx.ok = false;
        if(!CreateGfx(ctx))
        {
            gGfx.failed = true;   // jangan coba lagi tiap frame
            RestoreState(st);
            tlDrawing = false;
            return;
        }
    }

    // ---- susun geometri ----
    gV.clear();
    gV.reserve(4096);

    const Settings& c = g_perf.cfg;
    if(c.overlayMode != 0) BuildOverlayGeometry(c, gGfx.w, gGfx.h);

    if(wantMenu)
    {
        static const MenuPainter painter = {PaintRect, PaintText, PaintTextWidth};
        Menu_Frame(gGfx.w, gGfx.h, painter);
    }

    if(gV.empty()) { RestoreState(st); tlDrawing = false; return; }

    // ---- gambar ----
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, gGfx.w, gGfx.h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_FALSE);

    glUseProgram(gGfx.prog);
    glUniform2f(gGfx.locRes, (float)gGfx.w, (float)gGfx.h);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gGfx.tex);
    glUniform1i(gGfx.locTex, 0);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    const char* base = (const char*)gV.data();
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), base + 0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), base + 8);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Vtx), base + 16);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)gV.size());

    RestoreState(st);
    tlDrawing = false;
}
