/* ============================================================================
 *  Vitreous —— 原生 C 实现的液态玻璃材质演示
 *
 *  一个 Win32 窗口 + OpenGL 3.3 Core 的玻璃材质渲染器：
 *
 *    · 玻璃体：极坐标 SDF + Catmull-Rom 星形轮廓，GLSL 330 片元着色器完成
 *      倒角折射、色散、磨砂、高光、边缘线与投影。
 *    · 物理：固定步长 1/240 的拖拽弹簧 + 甩动惯量 + 果冻形变 + 按压缩放。
 *    · 背景：32 位 DIB + GDI 绘制海报 / 网格 / 图片，作为 GL 纹理上传。
 *    · 控制面板：面板内容光栅化进一张 sheet 大小的 RGBA 纹理，再由第二个
 *      全屏着色器做 24px 背景模糊、saturate(1.4)、62% 深色底、1px 边框，
 *      最后用直通 alpha 混合叠上去。
 *
 *  依赖：只依赖系统库（opengl32 / gdi32 / user32 / ole32 / comdlg32）。
 *  构建：build.bat（MSYS/MinGW: ./build.sh）。
 *
 *  用法：
 *    vitreous.exe                         正常窗口运行
 *    vitreous.exe --shot out.bmp          离屏渲染一帧并保存（用于验证）
 *    vitreous.exe --size 1200x800 --bg grid --shape pill --ms 800
 *    vitreous.exe --dpr 1 --collapse --values 0.8,0.2,0,0.9 --log
 * ========================================================================== */

#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shellapi.h>
#include <wincodec.h>

#include <GL/glcorearb.h>
#include <GL/wglext.h>

/* glcorearb.h 只在定义 GL_GLEXT_PROTOTYPES 时声明 OpenGL 1.1 入口函数；
 * 这里直接声明用到的 1.1 函数，从 opengl32.dll 静态链接。 */
extern const GLubyte *APIENTRY glGetString(GLenum name);
extern void APIENTRY glGenTextures(GLsizei n, GLuint *textures);
extern void APIENTRY glDeleteTextures(GLsizei n, const GLuint *textures);
extern void APIENTRY glBindTexture(GLenum target, GLuint texture);
extern void APIENTRY glTexParameteri(GLenum target, GLenum pname, GLint param);
extern void APIENTRY glTexImage2D(GLenum target, GLint level, GLint internalformat,
                                  GLsizei width, GLsizei height, GLint border,
                                  GLenum format, GLenum type, const void *pixels);
extern void APIENTRY glPixelStorei(GLenum pname, GLint param);
extern void APIENTRY glEnable(GLenum cap);
extern void APIENTRY glDisable(GLenum cap);
extern void APIENTRY glBlendFunc(GLenum sfactor, GLenum dfactor);
extern void APIENTRY glViewport(GLint x, GLint y, GLsizei width, GLsizei height);
extern void APIENTRY glDrawArrays(GLenum mode, GLint first, GLsizei count);
extern void APIENTRY glReadBuffer(GLenum mode);
extern void APIENTRY glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                                  GLenum format, GLenum type, void *pixels);
extern void APIENTRY glFinish(void);

#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>

/* ========================================================================== */
/*  小工具                                                                    */
/* ========================================================================== */

#define N_NODES 48
#define PI_F 3.14159265358979323846f
#define TAU_F 6.28318530717958647692f
#define H_STEP (1.0f / 240.0f)

#define IMIN(a, b) ((a) < (b) ? (a) : (b))
#define IMAX(a, b) ((a) > (b) ? (a) : (b))
#define FMIN(a, b) ((a) < (b) ? (a) : (b))
#define FMAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMPF(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))
#define CLAMPI(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))

#define RGB_HEX(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

static FILE *g_log = NULL;

static void logf_(const char *fmt, ...)
{
    va_list ap;
    if (!g_log) return;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
}

static void fatal(const char *msg)
{
    logf_("FATAL: %s\n", msg);
    MessageBoxA(NULL, msg, "Liquid Glass", MB_ICONERROR | MB_OK);
    ExitProcess(1);
}

static float now_sec(void)
{
    static LARGE_INTEGER freq;
    static int inited = 0;
    LARGE_INTEGER c;
    if (!inited) {
        QueryPerformanceFrequency(&freq);
        inited = 1;
    }
    QueryPerformanceCounter(&c);
    return (float)((double)c.QuadPart / (double)freq.QuadPart);
}

/* 圆角矩形 / 圆，单位：像素，坐标与 canvas 一样从左上角算 */
typedef struct { float x, y, w, h; } RectF;

static int rect_contains(RectF r, float x, float y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

/* ========================================================================== */
/*  画布（32 位 DIB，内存里是 0xAARRGGBB，行序自上而下）                        */
/* ========================================================================== */

typedef struct {
    int w, h;
    HBITMAP bmp;
    HDC dc;
    HGDIOBJ oldBmp;
    uint32_t *px;
} Canvas;

static void canvas_free(Canvas *c)
{
    if (c->dc) {
        if (c->oldBmp) SelectObject(c->dc, c->oldBmp);
        DeleteDC(c->dc);
    }
    if (c->bmp) DeleteObject(c->bmp);
    memset(c, 0, sizeof(*c));
}

static void canvas_init(Canvas *c, int w, int h, HDC ref)
{
    BITMAPINFO bi;
    void *bits = NULL;
    canvas_free(c);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    c->bmp = CreateDIBSection(ref, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!c->bmp) fatal("CreateDIBSection failed");
    c->dc = CreateCompatibleDC(ref);
    if (!c->dc) fatal("CreateCompatibleDC failed");
    c->oldBmp = SelectObject(c->dc, c->bmp);
    c->w = w;
    c->h = h;
    c->px = (uint32_t *)bits;
    memset(c->px, 0, (size_t)w * (size_t)h * 4);
}

static void canvas_clear(Canvas *c, uint32_t argb)
{
    int n = c->w * c->h;
    for (int i = 0; i < n; i++) c->px[i] = argb;
}

/* 直通 alpha 的 source-over 混合 */
static void px_blend(Canvas *c, int x, int y, uint32_t rgb, float a)
{
    uint32_t *p;
    uint32_t v;
    float da, oa, sr, sg, sb, dr, dg, db, rr, gg, bb;

    if ((unsigned)x >= (unsigned)c->w || (unsigned)y >= (unsigned)c->h || a <= 0.0f)
        return;
    if (a > 1.0f) a = 1.0f;

    p = &c->px[(size_t)y * (size_t)c->w + (size_t)x];
    v = *p;
    da = (float)((v >> 24) & 255) / 255.0f;
    oa = a + da * (1.0f - a);
    if (oa <= 0.0001f) {
        *p = 0;
        return;
    }
    sr = (float)((rgb >> 16) & 255) / 255.0f;
    sg = (float)((rgb >> 8) & 255) / 255.0f;
    sb = (float)(rgb & 255) / 255.0f;
    dr = (float)((v >> 16) & 255) / 255.0f;
    dg = (float)((v >> 8) & 255) / 255.0f;
    db = (float)(v & 255) / 255.0f;

    rr = (sr * a + dr * da * (1.0f - a)) / oa;
    gg = (sg * a + dg * da * (1.0f - a)) / oa;
    bb = (sb * a + db * da * (1.0f - a)) / oa;

    *p = ((uint32_t)(oa * 255.0f + 0.5f) << 24) |
         ((uint32_t)(CLAMPF(rr, 0, 1) * 255.0f + 0.5f) << 16) |
         ((uint32_t)(CLAMPF(gg, 0, 1) * 255.0f + 0.5f) << 8) |
         (uint32_t)(CLAMPF(bb, 0, 1) * 255.0f + 0.5f);
}

static void fill_rect(Canvas *c, float x0, float y0, float x1, float y1, uint32_t rgb, float a)
{
    int ix0 = (int)floorf(x0), iy0 = (int)floorf(y0);
    int ix1 = (int)ceilf(x1), iy1 = (int)ceilf(y1);
    for (int y = iy0; y < iy1; y++)
        for (int x = ix0; x < ix1; x++)
            px_blend(c, x, y, rgb, a);
}

static void fill_round_rect(Canvas *c, float x0, float y0, float x1, float y1,
                            float rad, uint32_t rgb, float a)
{
    float hx = (x1 - x0) * 0.5f, hy = (y1 - y0) * 0.5f;
    float cx = x0 + hx, cy = y0 + hy;
    int ix0, iy0, ix1, iy1;

    if (hx <= 0.0f || hy <= 0.0f) return;
    rad = FMIN(rad, FMIN(hx, hy));
    ix0 = (int)floorf(x0 - 1); iy0 = (int)floorf(y0 - 1);
    ix1 = (int)ceilf(x1 + 1);  iy1 = (int)ceilf(y1 + 1);
    for (int y = iy0; y < iy1; y++) {
        for (int x = ix0; x < ix1; x++) {
            float qx = fabsf((float)x + 0.5f - cx) - hx + rad;
            float qy = fabsf((float)y + 0.5f - cy) - hy + rad;
            float d = FMIN(FMAX(qx, qy), 0.0f) + hypotf(FMAX(qx, 0.0f), FMAX(qy, 0.0f)) - rad;
            float cov = CLAMPF(0.5f - d, 0.0f, 1.0f);
            if (cov > 0.0f) px_blend(c, x, y, rgb, a * cov);
        }
    }
}

static void fill_circle(Canvas *c, float cx, float cy, float r, uint32_t rgb, float a)
{
    int ix0 = (int)floorf(cx - r - 1), iy0 = (int)floorf(cy - r - 1);
    int ix1 = (int)ceilf(cx + r + 1),  iy1 = (int)ceilf(cy + r + 1);
    for (int y = iy0; y < iy1; y++) {
        for (int x = ix0; x < ix1; x++) {
            float d = hypotf((float)x + 0.5f - cx, (float)y + 0.5f - cy) - r;
            float cov = CLAMPF(0.5f - d, 0.0f, 1.0f);
            if (cov > 0.0f) px_blend(c, x, y, rgb, a * cov);
        }
    }
}

/* alpha 通道的高斯近似模糊（三次盒式），用于文字阴影 */
static void canvas_blur_alpha(Canvas *c, int radius)
{
    int w = c->w, h = c->h;
    float *a, *t;

    if (radius < 1 || w < 1 || h < 1) return;
    a = (float *)malloc((size_t)w * (size_t)h * sizeof(float));
    t = (float *)malloc((size_t)w * (size_t)h * sizeof(float));
    if (!a || !t) { free(a); free(t); return; }

    for (int i = 0; i < w * h; i++) a[i] = (float)((c->px[i] >> 24) & 255);

    for (int pass = 0; pass < 3; pass++) {
        for (int y = 0; y < h; y++) {                     /* 横向 */
            const float *row = a + (size_t)y * (size_t)w;
            float *out = t + (size_t)y * (size_t)w;
            for (int x = 0; x < w; x++) {
                int lo = x - radius, hi = x + radius, n = 0;
                float s = 0.0f;
                if (lo < 0) lo = 0;
                if (hi > w - 1) hi = w - 1;
                for (int k = lo; k <= hi; k++) { s += row[k]; n++; }
                out[x] = s / (float)(n > 0 ? n : 1);
            }
        }
        for (int x = 0; x < w; x++) {                     /* 纵向 */
            for (int y = 0; y < h; y++) {
                int lo = y - radius, hi = y + radius, n = 0;
                float s = 0.0f;
                if (lo < 0) lo = 0;
                if (hi > h - 1) hi = h - 1;
                for (int k = lo; k <= hi; k++) { s += t[(size_t)k * (size_t)w + (size_t)x]; n++; }
                a[(size_t)y * (size_t)w + (size_t)x] = s / (float)(n > 0 ? n : 1);
            }
        }
    }

    for (int i = 0; i < w * h; i++) {
        uint32_t v = (uint32_t)(CLAMPF(a[i], 0.0f, 255.0f) + 0.5f);
        c->px[i] = (v << 24) | (c->px[i] & 0x00FFFFFFu);
    }
    free(a);
    free(t);
}

/* ========================================================================== */
/*  GDI 文字：字体缓存 + 文字蒙版                                               */
/* ========================================================================== */

typedef struct { int px, weight, latin; HFONT f; } FontSlot;
static FontSlot g_fonts[64];
static int g_fontCount = 0;

static HFONT font_get(int px, int weight, int latin)
{
    LOGFONTW lf;
    HFONT f;

    if (px < 1) px = 1;
    weight = CLAMPI(weight, 100, 900);
    for (int i = 0; i < g_fontCount; i++)
        if (g_fonts[i].px == px && g_fonts[i].weight == weight && g_fonts[i].latin == latin)
            return g_fonts[i].f;

    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -px;
    lf.lfWeight = weight;
    lf.lfItalic = FALSE;
    lf.lfUnderline = FALSE;
    lf.lfStrikeOut = FALSE;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfOutPrecision = OUT_DEFAULT_PRECIS;
    lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    lf.lfQuality = ANTIALIASED_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    wcscpy(lf.lfFaceName, latin ? L"Segoe UI" : L"Microsoft YaHei UI");
    f = CreateFontIndirectW(&lf);
    if (!f) f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    if (g_fontCount < (int)(sizeof(g_fonts) / sizeof(g_fonts[0]))) {
        g_fonts[g_fontCount].px = px;
        g_fonts[g_fontCount].weight = weight;
        g_fonts[g_fontCount].latin = latin;
        g_fonts[g_fontCount].f = f;
        g_fontCount++;
    }
    return f;
}

static Canvas g_maskCanvas;
static uint8_t *g_dilA = NULL, *g_dilB = NULL;

static void mask_ensure(int w, int h)
{
    int nw, nh;
    if (w < 8) w = 8;
    if (h < 8) h = 8;
    if (g_maskCanvas.px && g_maskCanvas.w >= w && g_maskCanvas.h >= h) return;
    nw = IMAX(w, g_maskCanvas.w);
    nh = IMAX(h, g_maskCanvas.h);
    nw = ((nw + 63) / 64) * 64;
    nh = ((nh + 31) / 32) * 32;
    canvas_init(&g_maskCanvas, nw, nh, GetDC(NULL));
    g_dilA = (uint8_t *)realloc(g_dilA, (size_t)nw * (size_t)nh);
    g_dilB = (uint8_t *)realloc(g_dilB, (size_t)nw * (size_t)nh);
    if (!g_dilA || !g_dilB) fatal("out of memory");
}

static void font_vmetrics(int px, int weight, int latin, int *asc, int *desc, int *hgt)
{
    HDC dc;
    HFONT f;
    HGDIOBJ old;
    TEXTMETRICW tm;

    mask_ensure(64, 64);
    dc = g_maskCanvas.dc;
    f = font_get(px, weight, latin);
    old = SelectObject(dc, f);
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    if (asc) *asc = (int)tm.tmAscent;
    if (desc) *desc = (int)tm.tmDescent;
    if (hgt) *hgt = (int)tm.tmHeight;
}

static void text_measure(const wchar_t *s, int px, int weight, int latin, int *w, int *h)
{
    HDC dc;
    HFONT f;
    HGDIOBJ old;
    SIZE sz;

    mask_ensure(64, 64);
    dc = g_maskCanvas.dc;
    f = font_get(px, weight, latin);
    old = SelectObject(dc, f);
    GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
    SelectObject(dc, old);
    if (w) *w = (int)sz.cx;
    if (h) {
        int asc, desc, hgt;
        font_vmetrics(px, weight, latin, &asc, &desc, &hgt);
        *h = hgt;
    }
}

/* 在蒙版画布上以 (pad,pad) 为左上角画白字；返回文字宽高与 ascent */
static void mask_prepare(const wchar_t *s, int px, int weight, int latin, int pad,
                         int *ow, int *oh, int *oasc)
{
    int w = 0, hgt = 0, asc = 0, desc = 0;
    int cw, ch;
    HDC dc;
    HFONT f;
    HGDIOBJ old;

    mask_ensure(64, 64);
    font_vmetrics(px, weight, latin, &asc, &desc, &hgt);
    {
        SIZE sz;
        dc = g_maskCanvas.dc;
        f = font_get(px, weight, latin);
        old = SelectObject(dc, f);
        GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
        SelectObject(dc, old);
        w = (int)sz.cx;
    }
    if (w < 1) w = 1;

    cw = w + 2 * pad + 4;
    ch = hgt + 2 * pad + 4;
    mask_ensure(cw, ch);

    for (int y = 0; y < ch; y++)
        memset(g_maskCanvas.px + (size_t)y * (size_t)g_maskCanvas.w, 0, (size_t)cw * 4);

    dc = g_maskCanvas.dc;
    f = font_get(px, weight, latin);
    old = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    SetTextAlign(dc, TA_LEFT | TA_TOP);
    TextOutW(dc, pad, pad, s, (int)wcslen(s));
    SelectObject(dc, old);

    if (ow) *ow = w;
    if (oh) *oh = hgt;
    if (oasc) *oasc = asc;
}

static float mid_baseline(float cy, int px, int weight, int latin)
{
    int asc, desc, hgt;
    font_vmetrics(px, weight, latin, &asc, &desc, &hgt);
    return cy + (float)(asc - desc) * 0.5f;
}

/* 半透明文字（先渲到蒙版再合成，带抗锯齿） */
static void text_draw(Canvas *c, const wchar_t *s, float x, float baseline,
                      int px, int weight, int align, uint32_t rgb, float alpha, int latin)
{
    int w = 0, hgt = 0, asc = 0;
    int tx0, ty0;

    if (alpha <= 0.0f) return;
    mask_prepare(s, px, weight, latin, 1, &w, &hgt, &asc);
    tx0 = (int)lroundf(x - (align == 1 ? w * 0.5f : align == 2 ? (float)w : 0.0f));
    ty0 = (int)lroundf(baseline - (float)asc);

    for (int j = 0; j < hgt; j++) {
        for (int i = 0; i < w; i++) {
            uint32_t p = g_maskCanvas.px[(size_t)(j + 1) * (size_t)g_maskCanvas.w + (size_t)(i + 1)];
            int cov = (int)IMAX(IMAX((p >> 16) & 255, (p >> 8) & 255), p & 255);
            if (cov > 0) px_blend(c, tx0 + i, ty0 + j, rgb, alpha * (float)cov / 255.0f);
        }
    }
}

static void dilate_rect(const uint8_t *src, uint8_t *tmp, uint8_t *dst, int w, int h, int r)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int m = 0;
            for (int k = -r; k <= r; k++) {
                int xx = x + k;
                if (xx >= 0 && xx < w) {
                    uint8_t v = src[(size_t)y * (size_t)w + (size_t)xx];
                    if (v > m) m = v;
                }
            }
            tmp[(size_t)y * (size_t)w + (size_t)x] = (uint8_t)m;
        }
    }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int m = 0;
            for (int k = -r; k <= r; k++) {
                int yy = y + k;
                if (yy >= 0 && yy < h) {
                    uint8_t v = tmp[(size_t)yy * (size_t)w + (size_t)x];
                    if (v > m) m = v;
                }
            }
            dst[(size_t)y * (size_t)w + (size_t)x] = (uint8_t)m;
        }
    }
}

/* 描边文字（空心，只画外描边），用于 "LIQUID GLASS" */
static void text_outline(Canvas *c, const wchar_t *s, float x, float baseline,
                         int px, int weight, int align, uint32_t rgb, float alpha,
                         float thickness, int latin)
{
    int w = 0, hgt = 0, asc = 0;
    int r = IMAX(1, (int)ceilf(thickness));
    int pad = r + 2;
    int cw, ch, tx0, ty0;
    uint8_t *orig;

    if (alpha <= 0.0f) return;
    mask_prepare(s, px, weight, latin, pad, &w, &hgt, &asc);
    cw = w + 2 * pad + 4;
    ch = hgt + 2 * pad + 4;
    if (cw > g_maskCanvas.w) cw = g_maskCanvas.w;
    if (ch > g_maskCanvas.h) ch = g_maskCanvas.h;

    orig = (uint8_t *)malloc((size_t)cw * (size_t)ch);
    if (!orig) fatal("out of memory");
    for (int y = 0; y < ch; y++) {
        for (int x = 0; x < cw; x++) {
            uint32_t p = g_maskCanvas.px[(size_t)y * (size_t)g_maskCanvas.w + (size_t)x];
            orig[(size_t)y * (size_t)cw + (size_t)x] =
                (uint8_t)IMAX(IMAX((p >> 16) & 255, (p >> 8) & 255), p & 255);
        }
    }
    dilate_rect(orig, g_dilB, g_dilA, cw, ch, r);

    tx0 = (int)lroundf(x - (align == 1 ? w * 0.5f : align == 2 ? (float)w : 0.0f));
    ty0 = (int)lroundf(baseline - (float)asc);

    for (int y = 0; y < ch; y++) {
        for (int x = 0; x < cw; x++) {
            int o = orig[(size_t)y * (size_t)cw + (size_t)x];
            int d = g_dilA[(size_t)y * (size_t)cw + (size_t)x];
            int cov = d - o;
            if (cov > 0)
                px_blend(c, tx0 + x - pad, ty0 + y - pad, rgb, alpha * (float)cov / 255.0f);
        }
    }
    free(orig);
}

/* 不透明文字：直接让 GDI 画到目标画布（海报大标题 / 网格标签） */
static void text_direct(Canvas *c, const wchar_t *s, float x, float baseline,
                        int px, int weight, int align, uint32_t rgb, int latin)
{
    HDC dc = c->dc;
    HFONT f = font_get(px, weight, latin);
    HGDIOBJ old = SelectObject(dc, f);
    int asc, desc, hgt;
    UINT ta;

    font_vmetrics(px, weight, latin, &asc, &desc, &hgt);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255));
    ta = TA_TOP | (align == 1 ? TA_CENTER : (align == 2 ? TA_RIGHT : TA_LEFT));
    SetTextAlign(dc, ta);
    TextOutW(dc, (int)lroundf(x), (int)lroundf(baseline - (float)asc), s, (int)wcslen(s));
    SelectObject(dc, old);
}

/* ========================================================================== */
/*  WIC 图片解码                                                              */
/* ========================================================================== */

static const GUID kCLSID_WICImagingFactory =
    { 0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a } };
static const GUID kIID_IWICImagingFactory =
    { 0xec5ec8a9, 0xc395, 0x4314, { 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70 } };
static const GUID kGUID_WICPixelFormat32bppBGRA =
    { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0f } };

typedef struct { int w, h; uint32_t *px; } Image;

static Image g_img;
static int g_imgValid = 0;

static int load_image_wic(const wchar_t *path, Image *out)
{
    IWICImagingFactory *fac = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *conv = NULL;
    UINT w = 0, h = 0;
    int ok = 0;

    memset(out, 0, sizeof(*out));
    if (FAILED(CoCreateInstance(&kCLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                &kIID_IWICImagingFactory, (void **)&fac)))
        return 0;
    if (FAILED(fac->lpVtbl->CreateDecoderFromFilename(fac, path, NULL, GENERIC_READ,
                                                      WICDecodeMetadataCacheOnDemand, &dec)))
        goto done;
    if (FAILED(dec->lpVtbl->GetFrame(dec, 0, &frame)))
        goto done;
    if (FAILED(fac->lpVtbl->CreateFormatConverter(fac, &conv)))
        goto done;
    if (FAILED(conv->lpVtbl->Initialize(conv, (IWICBitmapSource *)frame,
                                        &kGUID_WICPixelFormat32bppBGRA,
                                        WICBitmapDitherTypeNone, NULL, 0.0,
                                        WICBitmapPaletteTypeCustom)))
        goto done;
    if (FAILED(conv->lpVtbl->GetSize(conv, &w, &h)) || w == 0 || h == 0)
        goto done;
    out->px = (uint32_t *)malloc((size_t)w * (size_t)h * 4);
    if (!out->px) goto done;
    if (FAILED(conv->lpVtbl->CopyPixels(conv, NULL, w * 4, w * h * 4, (BYTE *)out->px))) {
        free(out->px);
        out->px = NULL;
        goto done;
    }
    out->w = (int)w;
    out->h = (int)h;
    ok = 1;

done:
    if (conv) conv->lpVtbl->Release(conv);
    if (frame) frame->lpVtbl->Release(frame);
    if (dec) dec->lpVtbl->Release(dec);
    if (fac) fac->lpVtbl->Release(fac);
    return ok;
}

/* ========================================================================== */
/*  应用状态                                                                   */
/* ========================================================================== */

typedef struct { float a, b, n; } ShapeDef;

static const ShapeDef SHAPES[3] = {
    { 1.00f, 1.00f, 2.0f },  /* 圆 */
    { 1.55f, 0.72f, 4.5f },  /* 胶囊 */
    { 0.93f, 0.93f, 5.0f },  /* 圆角方 */
};

enum {
    BG_POSTER = 0,
    BG_GRID = 1,
    BG_IMAGE = 2,
};

enum {
    W_NONE = 0,
    W_TOGGLE,
    W_SHAPE0, W_SHAPE1, W_SHAPE2,
    W_BG0, W_BG1, W_BG2,
    W_SLIDER0, W_SLIDER1, W_SLIDER2, W_SLIDER3,
};

typedef struct {
    HINSTANCE inst;
    HWND hwnd;
    HDC hdc;
    HGLRC glrc;
    int glReady;

    int W, H;
    float dpr;
    float dprOverride;

    int running;
    int minimized;

    float baseR;

    int shape;
    int bgKind;
    int collapsed;

    float refract, disp, frost, jelly;

    struct {
        float x, y, vx, vy, pvx, pvy, scale, scaleV;
    } st;

    struct {
        int dragging;
        float x, y, gx, gy;
    } ptr;

    int hover;
    int active;
    int sliderDrag;

    int hintVisible;
    float hintAlpha;
    int hintFading;
    float hintFadeT0;

    int uiDirty;

    int shotMode;
    wchar_t shotPath[MAX_PATH];
    int shotMs;

    int logOn;
    int argW, argH;
} App;

static App g;

/* 无人值守交互测试 */
static int g_testClick, g_tcX, g_tcY;
static int g_testDrag, g_tdX0, g_tdY0, g_tdX1, g_tdY1;

/* 物理数组 */
static float g_theta[N_NODES];
static float g_r0[N_NODES], g_r0t[N_NODES];
static float g_off[N_NODES], g_offV[N_NODES], g_tmp[N_NODES];
static float g_radii[N_NODES];

/* 画布 / 纹理 */
static Canvas g_bgCanvas;
static Canvas g_uiCanvas;
static GLuint g_bgTex = 0, g_uiTex = 0, g_hintTex = 0;
static int g_hintW = 0, g_hintH = 0, g_hintPad = 4;

/* 布局 */
typedef struct {
    float sheetX, sheetY, sheetW, sheetH;
    RectF toggle;
    RectF seg[2];
    RectF cells[2][3];
    RectF slCell[4];
    RectF slTrack[4];
} Layout;

static Layout L;

/* GL 对象 */
static GLuint g_glassProg = 0, g_sheetProg = 0, g_spriteProg = 0, g_blurProg = 0;
static GLint g_uBg, g_uRes, g_uCenter, g_uR, g_uRefract, g_uDisp, g_uFrost, g_uEdge, g_uDpr;
static GLint g_uBlur, g_uFrostMix;
static GLint g_bTex, g_bRes, g_bStep, g_bSigma;
static GLuint g_blurTex[2] = { 0, 0 }, g_blurFbo[2] = { 0, 0 };
static int g_blurW[2] = { 0, 0 }, g_blurH[2] = { 0, 0 };
static GLint g_sScene, g_sUi, g_sRes, g_sRect, g_sRadius, g_sDpr;
static GLint g_pTex, g_pAlpha, g_pRect, g_pRes;
static GLuint g_vaoTri = 0, g_vboTri = 0, g_vaoQuad = 0, g_vboQuad = 0;
static GLuint g_sceneTex = 0, g_sceneFbo = 0;
static int g_sceneW = 0, g_sceneH = 0;
static GLuint g_outTex = 0, g_outFbo = 0;
static int g_outW = 0, g_outH = 0;

/* 提示文字 */
static const wchar_t *HINT_TEXT = L"Drag the glass \u2014 fling it into the walls";

/* ========================================================================== */
/*  GL 函数加载                                                                */
/* ========================================================================== */

#define GL_FUNCS(X) \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture) \
    X(PFNGLATTACHSHADERPROC, glAttachShader) \
    X(PFNGLBINDBUFFERPROC, glBindBuffer) \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) \
    X(PFNGLBUFFERDATAPROC, glBufferData) \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus) \
    X(PFNGLCOMPILESHADERPROC, glCompileShader) \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
    X(PFNGLCREATESHADERPROC, glCreateShader) \
    X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers) \
    X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers) \
    X(PFNGLDELETEPROGRAMPROC, glDeleteProgram) \
    X(PFNGLDELETESHADERPROC, glDeleteShader) \
    X(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D) \
    X(PFNGLGENBUFFERSPROC, glGenBuffers) \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) \
    X(PFNGLGETATTRIBLOCATIONPROC, glGetAttribLocation) \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv) \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram) \
    X(PFNGLSHADERSOURCEPROC, glShaderSource) \
    X(PFNGLUNIFORM1FPROC, glUniform1f) \
    X(PFNGLUNIFORM1FVPROC, glUniform1fv) \
    X(PFNGLUNIFORM1IPROC, glUniform1i) \
    X(PFNGLUNIFORM2FPROC, glUniform2f) \
    X(PFNGLUNIFORM4FPROC, glUniform4f) \
    X(PFNGLUSEPROGRAMPROC, glUseProgram) \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer)

#define GL_DECL(t, n) static t n = NULL;
GL_FUNCS(GL_DECL)
#undef GL_DECL

static PFNWGLSWAPINTERVALEXTPROC wglSwapIntervalEXT_ = NULL;

static int load_gl_functions(void)
{
    int missing = 0;
#define GL_LOAD(t, n) \
    do { \
        n = (t)(void *)wglGetProcAddress(#n); \
        if (!n) { logf_("missing GL function: %s\n", #n); missing++; } \
    } while (0);
    GL_FUNCS(GL_LOAD)
#undef GL_LOAD
    wglSwapIntervalEXT_ = (PFNWGLSWAPINTERVALEXTPROC)(void *)wglGetProcAddress("wglSwapIntervalEXT");
    return missing == 0;
}

/* ========================================================================== */
/*  着色器                                                                     */
/* ========================================================================== */

static const char *GLASS_VS =
    "#version 330 core\n"
    "layout(location=0) in vec2 a;\n"
    "void main(){ gl_Position = vec4(a,0.,1.); }\n";

static const char *GLASS_FS =
    "#version 330 core\n"
    "out vec4 outColor;\n"
    "uniform sampler2D uBg;\n"
    "uniform sampler2D uBlur;   // pre-blurred background (gaussian frost)\n"
    "uniform float uFrostMix;\n"
    "uniform vec2 uRes;      // canvas px\n"
    "uniform vec2 uCenter;   // px, top-left origin\n"
    "uniform float uR[48];   // radius per angle, px\n"
    "uniform float uRefract, uDisp, uFrost, uEdge, uDpr;\n"
    "const int N = 48;\n"
    "const float TAU = 6.28318530718;\n"
    "\n"
    "float radiusAt(float a){\n"
    "  float f = mod(a / TAU * float(N), float(N));\n"
    "  float fi = floor(f); float t = f - fi;\n"
    "  int i1 = int(fi) % N;\n"
    "  int i0 = (i1 + N - 1) % N, i2 = (i1 + 1) % N, i3 = (i1 + 2) % N;\n"
    "  float p0 = uR[i0], p1 = uR[i1], p2 = uR[i2], p3 = uR[i3];\n"
    "  return 0.5 * ((2.0*p1) + (-p0+p2)*t + (2.0*p0-5.0*p1+4.0*p2-p3)*t*t + (-p0+3.0*p1-3.0*p2+p3)*t*t*t);\n"
    "}\n"
    "// Signed distance + outward normal for a polar (star-shaped) outline.\n"
    "// Uses dr/dtheta evaluated on the curve, so there is no singularity at the centre.\n"
    "float polarSD(vec2 p, out vec2 n){\n"
    "  float rho = length(p);\n"
    "  float a = atan(p.y, p.x);\n"
    "  float r = radiusAt(a);\n"
    "  float h = TAU / float(N) * 0.25;\n"
    "  float dr = (radiusAt(a + h) - radiusAt(a - h)) / (2.0 * h);\n"
    "  float q = dr / max(r, 1.0);\n"
    "  vec2 er = rho > 1e-3 ? p / rho : vec2(1.0, 0.0);\n"
    "  vec2 et = vec2(-er.y, er.x);\n"
    "  vec2 g = er - q * et;\n"
    "  float gl = length(g);\n"
    "  n = g / gl;\n"
    "  return (rho - r) / gl;\n"
    "}\n"
    "\n"
    "// frost: gaussian-blurred background is blended in by uFrostMix\n"
    "vec3 sampleBg(vec2 px, float amount){\n"
    "  vec3 sharp = texture(uBg, px / uRes).rgb;\n"
    "  vec3 soft  = texture(uBlur, px / uRes).rgb;\n"
    "  return mix(sharp, soft, amount);\n"
    "}\n"
    "\n"
    "void main(){\n"
    "  vec2 frag = vec2(gl_FragCoord.x, uRes.y - gl_FragCoord.y);\n"
    "  vec2 p = frag - uCenter;\n"
    "\n"
    "  vec2 n;\n"
    "  float d = polarSD(p, n);        // signed distance in px, n = outward normal\n"
    "\n"
    "  vec3 bg = texture(uBg, frag / uRes).rgb;\n"
    "\n"
    "  // soft drop shadow, offset downward\n"
    "  vec2 nS;\n"
    "  float ds = polarSD(p - vec2(0., 14.0*uDpr), nS);\n"
    "  float shadow = (1.0 - smoothstep(-10.0*uDpr, 46.0*uDpr, ds)) * 0.32;\n"
    "  vec3 outside = bg * (1.0 - shadow);\n"
    "\n"
    "  if (d > 2.0) { outColor = vec4(outside, 1.0); return; }\n"
    "\n"
    "  // bevel profile: 0 at rim -> 1 deep inside\n"
    "  float t = clamp(-d / uEdge, 0.0, 1.0);\n"
    "  float rim = 1.0 - t;\n"
    "  // circular bevel: flat centre, steep edge\n"
    "  float bend = 1.0 - sqrt(max(1.0 - rim*rim, 0.0));\n"
    "  float mag = uRefract * uEdge * 1.15 * bend;\n"
    "\n"
    "  // slight lens magnification in the middle\n"
    "  vec2 base = uCenter + p * (1.0 - 0.08 * uRefract);\n"
    "  float k = uDisp * 0.45;\n"
    "  vec3 col;\n"
    "  col.r = sampleBg(base - n * mag * (1.0 - k), uFrostMix).r;\n"
    "  col.g = sampleBg(base - n * mag,             uFrostMix).g;\n"
    "  col.b = sampleBg(base - n * mag * (1.0 + k), uFrostMix).b;\n"
    "\n"
    "  // glass body tint + frost lift\n"
    "  col = mix(col, vec3(1.0), 0.035 + uFrost * 0.08);\n"
    "  col *= 1.04;\n"
    "\n"
    "  // lighting: key light top-left, weaker bounce bottom-right\n"
    "  vec2 L = normalize(vec2(-0.55, -0.85));\n"
    "  float edgeBand = pow(rim, 12.0);\n"
    "  float spec = pow(max(dot(n, L), 0.0), 1.6) * edgeBand * 0.75\n"
    "             + pow(max(dot(n, -L), 0.0), 1.6) * edgeBand * 0.3;\n"
    "  float fres = pow(rim, 4.0) * 0.05;\n"
    "  col += vec3(spec + fres);\n"
    "\n"
    "  // crisp 1px rim line\n"
    "  float line = 1.0 - smoothstep(0.0, 1.6*uDpr, abs(d + 0.8*uDpr));\n"
    "  col += line * (0.18 + 0.35 * max(dot(n, L), 0.0));\n"
    "\n"
    "  float alpha = clamp(0.5 - d, 0.0, 1.0);\n"
    "  outColor = vec4(mix(outside, col, alpha), 1.0);\n"
    "}\n";

/* 可分离高斯：玻璃磨砂用的背景预模糊，两遍 ping-pong */
static const char *BLUR_FS =
    "#version 330 core\n"
    "out vec4 outColor;\n"
    "uniform sampler2D uTex;\n"
    "uniform vec2 uRes;\n"
    "uniform vec2 uStep;    // tap offset, px, one axis\n"
    "uniform float uSigma;  // gaussian sigma, px\n"
    "void main(){\n"
    "  vec2 p = vec2(gl_FragCoord.x, uRes.y - gl_FragCoord.y);\n"
    "  vec2 uv = p / uRes;\n"
    "  vec2 d = uStep / uRes;\n"
    "  float sig = max(uSigma, 0.5);\n"
    "  vec3 acc = texture(uTex, uv).rgb;\n"
    "  float wsum = 1.0;\n"
    "  for (int i = 1; i <= 6; i++) {\n"
    "    float x = float(i) * length(uStep);\n"
    "    float w = exp(-(x*x) / (2.0*sig*sig));\n"
    "    acc += (texture(uTex, uv + d*float(i)).rgb + texture(uTex, uv - d*float(i)).rgb) * w;\n"
    "    wsum += 2.0*w;\n"
    "  }\n"
    "  outColor = vec4(acc / wsum, 1.0);\n"
    "}\n";

/* 面板：24px 背景模糊 + saturate(1.4) + 深色底 + 边框 + UI 纹理 */
static const char *SHEET_FS =
    "#version 330 core\n"
    "out vec4 outColor;\n"
    "uniform sampler2D uScene;\n"
    "uniform sampler2D uUi;\n"
    "uniform vec2 uRes;\n"
    "uniform vec4 uRect;   // x, y, x+w, y+h  (top-left origin)\n"
    "uniform float uRadius;\n"
    "uniform float uDpr;\n"
    "\n"
    "float sdRoundBox(vec2 p, vec2 b, float r){\n"
    "  vec2 q = abs(p) - b + r;\n"
    "  return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;\n"
    "}\n"
    "vec3 sceneTap(vec2 fc){ return texture(uScene, fc / uRes).rgb; }\n"
    "\n"
    "void main(){\n"
    "  vec2 fc = gl_FragCoord.xy;\n"
    "  vec3 scene = sceneTap(fc);\n"
    "  vec2 frag = vec2(fc.x, uRes.y - fc.y);\n"
    "  vec2 c  = (uRect.xy + uRect.zw) * 0.5;\n"
    "  vec2 hs = (uRect.zw - uRect.xy) * 0.5;\n"
    "  float d = sdRoundBox(frag - c, hs, uRadius);\n"
    "\n"
    "  // drop shadow: 0 10px 40px rgba(0,0,0,.35)\n"
    "  float ds = sdRoundBox(frag - c - vec2(0.0, 10.0*uDpr), hs, uRadius);\n"
    "  float sh = (1.0 - smoothstep(0.0, 44.0*uDpr, ds)) * 0.35;\n"
    "  vec3 col = scene * (1.0 - sh);\n"
    "\n"
    "  float cov = clamp(0.5 - d, 0.0, 1.0);\n"
    "  if (cov > 0.0) {\n"
    "    const vec2 R6[6] = vec2[6](vec2(1.,0.), vec2(.5,.866), vec2(-.5,.866),\n"
    "                               vec2(-1.,0.), vec2(-.5,-.866), vec2(.5,-.866));\n"
    "    float R = 22.0 * uDpr;\n"
    "    vec3 b = sceneTap(fc) * 0.28;\n"
    "    for (int i = 0; i < 6; i++) b += sceneTap(fc + R6[i] * R * 0.5) * 0.07;\n"
    "    for (int i = 0; i < 6; i++) b += sceneTap(fc + R6[i] * R) * 0.05;\n"
    "\n"
    "    float lum = dot(b, vec3(0.2126, 0.7152, 0.0722));\n"
    "    b = mix(vec3(lum), b, 1.4);\n"
    "\n"
    "    vec3 sheet = mix(b, vec3(22.0, 24.0, 34.0) / 255.0, 0.62);\n"
    "    float ln = 1.0 - smoothstep(0.0, 1.2*uDpr, abs(d + 0.5*uDpr));\n"
    "    sheet = mix(sheet, vec3(1.0), ln * 0.12);\n"
    "\n"
    "    vec2 uv = (frag - uRect.xy) / (uRect.zw - uRect.xy);\n"
    "    vec4 ui = texture(uUi, uv);\n"
    "    sheet = mix(sheet, ui.rgb, ui.a);\n"
    "    col = mix(col, sheet, cov);\n"
    "  }\n"
    "  outColor = vec4(col, 1.0);\n"
    "}\n";

static const char *SPRITE_VS =
    "#version 330 core\n"
    "layout(location=0) in vec2 a;\n"
    "uniform vec4 uRect;\n"
    "uniform vec2 uRes;\n"
    "out vec2 vUv;\n"
    "void main(){\n"
    "  vec2 p = uRect.xy + a * uRect.zw;\n"
    "  vec2 clip = vec2(p.x / uRes.x * 2.0 - 1.0, 1.0 - p.y / uRes.y * 2.0);\n"
    "  gl_Position = vec4(clip, 0.0, 1.0);\n"
    "  vUv = a;\n"
    "}\n";

static const char *SPRITE_FS =
    "#version 330 core\n"
    "in vec2 vUv;\n"
    "out vec4 outColor;\n"
    "uniform sampler2D uTex;\n"
    "uniform float uAlpha;\n"
    "void main(){ vec4 t = texture(uTex, vUv); outColor = vec4(t.rgb, t.a * uAlpha); }\n";

static GLuint compile_shader(GLenum type, const char *src, const char *name)
{
    GLuint s = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLchar log[8192];
        GLsizei n = 0;
        glGetShaderInfoLog(s, sizeof(log) - 1, &n, log);
        log[IMIN((int)n, (int)sizeof(log) - 1)] = 0;
        logf_("%s shader compile failed:\n%s\n", name, log);
        fatal("shader compile failed (see vitreous.log)");
    }
    return s;
}

static GLuint make_program(const char *vs, const char *fs, const char *name)
{
    GLuint v = compile_shader(GL_VERTEX_SHADER, vs, name);
    GLuint f = compile_shader(GL_FRAGMENT_SHADER, fs, name);
    GLuint p = glCreateProgram();
    GLint ok = 0;

    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLchar log[8192];
        GLsizei n = 0;
        glGetProgramInfoLog(p, sizeof(log) - 1, &n, log);
        log[IMIN((int)n, (int)sizeof(log) - 1)] = 0;
        logf_("%s link failed:\n%s\n", name, log);
        fatal("shader link failed (see vitreous.log)");
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

/* ========================================================================== */
/*  OpenGL 初始化                                                              */
/* ========================================================================== */

static float dpi_scale(HWND hwnd)
{
    typedef UINT(WINAPI * PFN_GetDpiForWindow)(HWND);
    static PFN_GetDpiForWindow pfn = NULL;
    static int tried = 0;

    if (!tried) {
        HMODULE u = GetModuleHandleW(L"user32.dll");
        if (u) pfn = (PFN_GetDpiForWindow)(void *)GetProcAddress(u, "GetDpiForWindow");
        tried = 1;
    }
    if (pfn) {
        UINT dpi = pfn(hwnd);
        if (dpi >= 72 && dpi <= 480) return (float)dpi / 96.0f;
    }
    {
        HDC dc = GetDC(hwnd);
        int d = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
        if (dc) ReleaseDC(hwnd, dc);
        if (d < 72 || d > 480) d = 96;
        return (float)d / 96.0f;
    }
}

static int create_gl_context(HWND hwnd)
{
    PIXELFORMATDESCRIPTOR pfd;
    int pf;
    HGLRC temp = NULL, ctx = NULL;
    PFNWGLCREATECONTEXTATTRIBSARBPROC create_ctx = NULL;
    int attribs_core[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
        WGL_CONTEXT_MINOR_VERSION_ARB, 3,
        WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
        0
    };
    int attribs_any[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
        WGL_CONTEXT_MINOR_VERSION_ARB, 3,
        0
    };
    const char *ver;

    g.hdc = GetDC(hwnd);
    if (!g.hdc) return 0;

    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;

    pf = ChoosePixelFormat(g.hdc, &pfd);
    if (!pf || !SetPixelFormat(g.hdc, pf, &pfd)) {
        logf_("SetPixelFormat failed (err=%lu)\n", GetLastError());
        return 0;
    }

    temp = wglCreateContext(g.hdc);
    if (!temp) {
        logf_("wglCreateContext failed (err=%lu)\n", GetLastError());
        return 0;
    }
    if (!wglMakeCurrent(g.hdc, temp)) {
        logf_("wglMakeCurrent failed\n");
        return 0;
    }
    create_ctx = (PFNWGLCREATECONTEXTATTRIBSARBPROC)(void *)wglGetProcAddress("wglCreateContextAttribsARB");
    if (create_ctx) {
        ctx = create_ctx(g.hdc, NULL, attribs_core);
        if (!ctx) {
            logf_("core 3.3 context failed, retrying without profile mask\n");
            ctx = create_ctx(g.hdc, NULL, attribs_any);
        }
    }
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(temp);

    if (!create_ctx || !ctx) {
        logf_("no OpenGL 3.3 context available\n");
        return 0;
    }
    if (!wglMakeCurrent(g.hdc, ctx)) {
        logf_("wglMakeCurrent(3.3) failed\n");
        return 0;
    }
    g.glrc = ctx;

    ver = (const char *)glGetString(GL_VERSION);
    logf_("GL_VERSION: %s\n", ver ? ver : "(null)");
    logf_("GL_RENDERER: %s\n", (const char *)glGetString(GL_RENDERER));

    if (!load_gl_functions()) return 0;
    if (wglSwapIntervalEXT_) wglSwapIntervalEXT_(1);
    return 1;
}

static void make_texture_1x1(GLuint *tex)
{
    uint32_t zero = 0;
    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_BGRA, GL_UNSIGNED_BYTE, &zero);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static void gl_init_resources(void)
{
    GLfloat tri[6] = { -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };
    GLfloat quad[12] = { 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 0, 1 };

    g_glassProg = make_program(GLASS_VS, GLASS_FS, "glass");
    g_sheetProg = make_program(GLASS_VS, SHEET_FS, "sheet");
    g_spriteProg = make_program(SPRITE_VS, SPRITE_FS, "sprite");
    g_blurProg = make_program(GLASS_VS, BLUR_FS, "blur");

    g_uBg = glGetUniformLocation(g_glassProg, "uBg");
    g_uRes = glGetUniformLocation(g_glassProg, "uRes");
    g_uCenter = glGetUniformLocation(g_glassProg, "uCenter");
    g_uR = glGetUniformLocation(g_glassProg, "uR");
    g_uRefract = glGetUniformLocation(g_glassProg, "uRefract");
    g_uDisp = glGetUniformLocation(g_glassProg, "uDisp");
    g_uFrost = glGetUniformLocation(g_glassProg, "uFrost");
    g_uEdge = glGetUniformLocation(g_glassProg, "uEdge");
    g_uDpr = glGetUniformLocation(g_glassProg, "uDpr");
    g_uBlur = glGetUniformLocation(g_glassProg, "uBlur");
    g_uFrostMix = glGetUniformLocation(g_glassProg, "uFrostMix");

    g_bTex = glGetUniformLocation(g_blurProg, "uTex");
    g_bRes = glGetUniformLocation(g_blurProg, "uRes");
    g_bStep = glGetUniformLocation(g_blurProg, "uStep");
    g_bSigma = glGetUniformLocation(g_blurProg, "uSigma");

    g_sScene = glGetUniformLocation(g_sheetProg, "uScene");
    g_sUi = glGetUniformLocation(g_sheetProg, "uUi");
    g_sRes = glGetUniformLocation(g_sheetProg, "uRes");
    g_sRect = glGetUniformLocation(g_sheetProg, "uRect");
    g_sRadius = glGetUniformLocation(g_sheetProg, "uRadius");
    g_sDpr = glGetUniformLocation(g_sheetProg, "uDpr");

    g_pTex = glGetUniformLocation(g_spriteProg, "uTex");
    g_pAlpha = glGetUniformLocation(g_spriteProg, "uAlpha");
    g_pRect = glGetUniformLocation(g_spriteProg, "uRect");
    g_pRes = glGetUniformLocation(g_spriteProg, "uRes");

    glUseProgram(g_glassProg);
    if (g_uBg >= 0) glUniform1i(g_uBg, 0);
    if (g_uBlur >= 0) glUniform1i(g_uBlur, 1);
    if (g_uFrostMix >= 0) glUniform1f(g_uFrostMix, 0.0f);
    glUseProgram(g_blurProg);
    if (g_bTex >= 0) glUniform1i(g_bTex, 0);
    glUseProgram(g_sheetProg);
    if (g_sScene >= 0) glUniform1i(g_sScene, 0);
    if (g_sUi >= 0) glUniform1i(g_sUi, 1);
    glUseProgram(g_spriteProg);
    if (g_pTex >= 0) glUniform1i(g_pTex, 0);
    glUseProgram(0);

    glGenVertexArrays(1, &g_vaoTri);
    glBindVertexArray(g_vaoTri);
    glGenBuffers(1, &g_vboTri);
    glBindBuffer(GL_ARRAY_BUFFER, g_vboTri);
    glBufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (const void *)0);

    glGenVertexArrays(1, &g_vaoQuad);
    glBindVertexArray(g_vaoQuad);
    glGenBuffers(1, &g_vboQuad);
    glBindBuffer(GL_ARRAY_BUFFER, g_vboQuad);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (const void *)0);

    glBindVertexArray(0);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    make_texture_1x1(&g_bgTex);
    make_texture_1x1(&g_uiTex);
}

/* ========================================================================== */
/*  场景 / 输出 FBO                                                            */
/* ========================================================================== */

static void fbo_target(GLuint *tex, GLuint *fbo, int *cw, int *ch, int w, int h)
{
    if (*tex && *cw == w && *ch == h) return;
    if (*tex) {
        glDeleteTextures(1, tex);
        glDeleteFramebuffers(1, fbo);
        *tex = 0;
        *fbo = 0;
    }
    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        fatal("framebuffer incomplete");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    *cw = w;
    *ch = h;
}

/* ========================================================================== */
/*  背景绘制                                                                   */
/* ========================================================================== */

static void add_blob(Canvas *c, float cx, float cy, float R, uint32_t rgb, float strength)
{
    int x0 = (int)floorf(cx - R), x1 = (int)ceilf(cx + R);
    int y0 = (int)floorf(cy - R), y1 = (int)ceilf(cy + R);
    float cr = (float)((rgb >> 16) & 255), cg = (float)((rgb >> 8) & 255), cb = (float)(rgb & 255);

    x0 = IMAX(x0, 0); y0 = IMAX(y0, 0);
    x1 = IMIN(x1, c->w); y1 = IMIN(y1, c->h);

    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            float dx = (float)x + 0.5f - cx;
            float dy = (float)y + 0.5f - cy;
            float t = sqrtf(dx * dx + dy * dy) / R;
            float f, r, g, b;
            uint32_t p, out;
            if (t >= 1.0f) continue;
            f = strength * (1.0f - t);
            p = c->px[(size_t)y * (size_t)c->w + (size_t)x];
            r = (float)((p >> 16) & 255) + cr * f;
            g = (float)((p >> 8) & 255) + cg * f;
            b = (float)(p & 255) + cb * f;
            out = 0xFF000000u |
                  ((uint32_t)CLAMPF(r, 0, 255) << 16) |
                  ((uint32_t)CLAMPF(g, 0, 255) << 8) |
                  (uint32_t)CLAMPF(b, 0, 255);
            c->px[(size_t)y * (size_t)c->w + (size_t)x] = out;
        }
    }
}

static void draw_poster(Canvas *c, float W, float H, float s)
{
    static const float blobs[5][4] = {
        { 0.18f, 0.22f, 0.55f, 0.0f },
        { 0.85f, 0.18f, 0.50f, 0.0f },
        { 0.75f, 0.78f, 0.60f, 0.0f },
        { 0.15f, 0.85f, 0.45f, 0.0f },
        { 0.50f, 0.50f, 0.35f, 0.0f },
    };
    static const uint32_t blobCols[5] = {
        0xFF5A3C, 0x7B5CFF, 0x14C8E6, 0xFFCF33, 0xFF3D9A,
    };
    static const wchar_t *lines[5] = {
        L"refraction  n = 1.52",
        L"dispersion  \u0394n = 0.012",
        L"spring  k = 320  \u03b6 = 0.18",
        L"SDF \u00b7 Catmull-Rom \u00b7 48 nodes",
        L"drag \u00b7 fling \u00b7 spring",
    };
    static const uint32_t dotCols[6] = {
        0xFF5A3C, 0xFFCF33, 0x3DDC84, 0x14C8E6, 0x7B5CFF, 0xFF3D9A,
    };
    float big, step;
    int bigPx, subPx, smallPx;

    canvas_clear(c, 0xFF0D0F1A);

    for (int i = 0; i < 5; i++) {
        float R = blobs[i][2] * FMAX(W, H);
        add_blob(c, blobs[i][0] * W, blobs[i][1] * H, R, blobCols[i], 0.80f);
    }

    /* 网格线 */
    step = 44.0f * s;
    for (float x = 0.0f; x < W; x += step)
        fill_rect(c, x, 0, x + FMAX(1.0f, 1.0f * s), H, 0xFFFFFF, 0.07f);
    for (float y = 0.0f; y < H; y += step)
        fill_rect(c, 0, y, W, y + FMAX(1.0f, 1.0f * s), 0xFFFFFF, 0.07f);

    big = FMIN(W / 4.2f, H / 4.5f);
    bigPx = IMAX(1, (int)lroundf(big));
    subPx = IMAX(1, (int)lroundf(big * 0.42f));
    smallPx = IMAX(1, (int)lroundf(13.0f * s));

    text_direct(c, L"VITREOUS", W * 0.5f,
                H * 0.40f + (float)(bigPx * 0.35f), bigPx, 900, 1, 0xFFFFFF, 0);
    text_outline(c, L"LIQUID GLASS", W * 0.5f,
                 H * 0.40f + big * 0.78f + (float)(subPx * 0.35f),
                 subPx, 800, 1, 0xFFFFFF, 0.90f, 2.0f * s, 1);

    for (int i = 0; i < 5; i++)
        text_draw(c, lines[i], 24.0f * s, 40.0f * s + i * 22.0f * s, smallPx, 500,
                  0, 0xFFFFFF, 0.55f, 1);

    for (int i = 0; i < 6; i++) {
        float r = 9.0f * s;
        fill_circle(c, W - 30.0f * s - i * 30.0f * s, 46.0f * s, r, dotCols[i], 1.0f);
    }
}

static void draw_grid(Canvas *c, float W, float H, float s)
{
    static const uint32_t hues[6] = {
        0xFF5A3C, 0xFFB020, 0x3DDC84, 0x14B8E6, 0x6D5CFF, 0xFF3D9A,
    };
    float step = 36.0f * s;
    float lw = FMAX(1.0f, 1.2f * s);
    int labelPx = IMAX(1, (int)lroundf(10.0f * s));
    int bigPx;
    wchar_t buf[16];

    canvas_clear(c, 0xFFF4F5F8);

    for (float x = 0.0f, i = 0.0f; x < W; x += step * 3.0f, i += 1.0f)
        fill_rect(c, x, 0, x + step * 1.5f, H, hues[((int)i) % 6], 0.20f);

    for (float x = 0.0f; x < W; x += step)
        fill_rect(c, x, 0, x + lw, H, 0x1B1D26, 1.0f);
    for (float y = 0.0f; y < H; y += step)
        fill_rect(c, 0, y, W, y + lw, 0x1B1D26, 1.0f);

    for (int r = 0, y = 0; (float)y < H; y += (int)(step * 2.0f), r++) {
        for (int k = 0, x = 0; (float)x < W; x += (int)(step * 2.0f), k++) {
            int asc = 0, desc = 0, hgt = 0;
            swprintf(buf, 16, L"%c%d", (wchar_t)(L'A' + (r % 26)), k);
            font_vmetrics(labelPx, 600, 1, &asc, &desc, &hgt);
            text_direct(c, buf, (float)x + 4.0f * s,
                        (float)y + 4.0f * s + (float)asc, labelPx, 600, 0, 0x1B1D26, 1);
        }
    }

    bigPx = IMAX(1, (int)lroundf(FMIN(W / 5.0f, H / 5.0f)));
    text_direct(c, L"REFRACT", W * 0.5f,
                H * 0.42f + (float)(bigPx * 0.35f), bigPx, 900, 1, 0x1B1D26, 0);
}

static void draw_image_cover(Canvas *c, float W, float H)
{
    int iw = g_img.w, ih = g_img.h;
    float sc, dw, dh, ox, oy;

    canvas_clear(c, 0xFF000000);
    if (iw <= 0 || ih <= 0) return;

    sc = FMAX(W / (float)iw, H / (float)ih);
    dw = iw * sc;
    dh = ih * sc;
    ox = (W - dw) * 0.5f;
    oy = (H - dh) * 0.5f;

    for (int y = 0; y < c->h; y++) {
        float sy = (((float)y + 0.5f) - oy) / sc - 0.5f;
        int y0 = (int)floorf(sy);
        float fy = sy - (float)y0;
        for (int x = 0; x < c->w; x++) {
            float sx = (((float)x + 0.5f) - ox) / sc - 0.5f;
            int x0 = (int)floorf(sx);
            float fx = sx - (float)x0;
            int xa = CLAMPI(x0, 0, iw - 1), xb = CLAMPI(x0 + 1, 0, iw - 1);
            int ya = CLAMPI(y0, 0, ih - 1), yb = CLAMPI(y0 + 1, 0, ih - 1);
            uint32_t p00 = g_img.px[(size_t)ya * iw + xa];
            uint32_t p10 = g_img.px[(size_t)ya * iw + xb];
            uint32_t p01 = g_img.px[(size_t)yb * iw + xa];
            uint32_t p11 = g_img.px[(size_t)yb * iw + xb];
            float r00 = (float)((p00 >> 16) & 255), g00 = (float)((p00 >> 8) & 255), b00 = (float)(p00 & 255);
            float r10 = (float)((p10 >> 16) & 255), g10 = (float)((p10 >> 8) & 255), b10 = (float)(p10 & 255);
            float r01 = (float)((p01 >> 16) & 255), g01 = (float)((p01 >> 8) & 255), b01 = (float)(p01 & 255);
            float r11 = (float)((p11 >> 16) & 255), g11 = (float)((p11 >> 8) & 255), b11 = (float)(p11 & 255);
            float r = (r00 * (1 - fx) + r10 * fx) * (1 - fy) + (r01 * (1 - fx) + r11 * fx) * fy;
            float g = (g00 * (1 - fx) + g10 * fx) * (1 - fy) + (g01 * (1 - fx) + g11 * fx) * fy;
            float b = (b00 * (1 - fx) + b10 * fx) * (1 - fy) + (b01 * (1 - fx) + b11 * fx) * fy;
            c->px[(size_t)y * (size_t)c->w + (size_t)x] = 0xFF000000u |
                ((uint32_t)CLAMPF(r, 0, 255) << 16) |
                ((uint32_t)CLAMPF(g, 0, 255) << 8) |
                (uint32_t)CLAMPF(b, 0, 255);
        }
    }
}

static void upload_bg(void)
{
    glBindTexture(GL_TEXTURE_2D, g_bgTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g_bgCanvas.w, g_bgCanvas.h, 0,
                 GL_BGRA, GL_UNSIGNED_BYTE, g_bgCanvas.px);
}

static void build_background(void)
{
    if (!g_bgCanvas.px || g_bgCanvas.w != g.W || g_bgCanvas.h != g.H)
        canvas_init(&g_bgCanvas, g.W, g.H, g.hdc);

    if (g.bgKind == BG_IMAGE && g_imgValid)
        draw_image_cover(&g_bgCanvas, (float)g.W, (float)g.H);
    else if (g.bgKind == BG_GRID)
        draw_grid(&g_bgCanvas, (float)g.W, (float)g.H, g.dpr);
    else
        draw_poster(&g_bgCanvas, (float)g.W, (float)g.H, g.dpr);

    upload_bg();
    logf_("background rebuilt: kind=%d size=%dx%d\n", g.bgKind, g.W, g.H);
}

/* ========================================================================== */
/*  提示文字纹理                                                               */
/* ========================================================================== */

static void build_hint(void)
{
    int px = IMAX(1, (int)lroundf(13.0f * g.dpr));
    int w = 0, hgt = 0, asc = 0, desc = 0;
    int blur = IMAX(1, (int)lroundf(4.0f * g.dpr));   /* 规范里的 blur-radius 8px ≈ σ4 */
    int pad = 4 + blur * 2;
    Canvas tmp;

    font_vmetrics(px, 500, 1, &asc, &desc, &hgt);
    text_measure(HINT_TEXT, px, 500, 1, &w, &hgt);
    if (w < 1) w = 1;

    canvas_init(&tmp, w + pad * 2, hgt + pad * 2, g.hdc);
    canvas_clear(&tmp, 0);

    /* text-shadow: 0 1px 8px rgba(0,0,0,.5) */
    text_draw(&tmp, HINT_TEXT, (float)pad, (float)pad + (float)asc + 1.0f * g.dpr,
              px, 500, 0, 0x000000, 0.5f, 1);
    canvas_blur_alpha(&tmp, blur);

    text_draw(&tmp, HINT_TEXT, (float)pad, (float)pad + (float)asc,
              px, 500, 0, 0xFFFFFF, 0.75f, 1);

    if (g_hintTex) {
        glDeleteTextures(1, &g_hintTex);
        g_hintTex = 0;
    }
    glGenTextures(1, &g_hintTex);
    glBindTexture(GL_TEXTURE_2D, g_hintTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tmp.w, tmp.h, 0, GL_BGRA, GL_UNSIGNED_BYTE, tmp.px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    g_hintW = tmp.w;
    g_hintH = tmp.h;
    g_hintPad = pad;
    canvas_free(&tmp);
}

/* ========================================================================== */
/*  布局                                                                      */
/* ========================================================================== */

static void layout_update(void)
{
    float S = g.dpr;
    float border = 1.0f * S;
    float padX = 14.0f * S, padTop = 12.0f * S, padBot = 14.0f * S, gap = 12.0f * S;
    float headH = 26.0f * S;
    float contentW, totalW, contentH, h, bodyH = 0.0f;

    /* 面板宽度 min(440px, 100% - 32px) 是内容盒宽度；外框还要加上
       左右 padding 和 1px 边框。 */
    contentW = FMIN(440.0f * S, (float)g.W - 32.0f * S);
    if (contentW < 180.0f * S)
        contentW = FMAX(120.0f * S, (float)g.W - 32.0f * S);

    if (!g.collapsed)
        bodyH = 35.0f * S + gap + (38.0f * S * 2.0f + 10.0f * S);

    contentH = headH + (g.collapsed ? 0.0f : gap + bodyH);
    totalW = contentW + padX * 2.0f + border * 2.0f;
    h = contentH + padTop + padBot + border * 2.0f;

    L.sheetW = totalW;
    L.sheetH = h;
    L.sheetX = ((float)g.W - totalW) * 0.5f;
    L.sheetY = (float)g.H - 14.0f * S - h;

    L.toggle.w = 66.0f * S;
    L.toggle.h = 22.0f * S;
    L.toggle.x = L.sheetX + border + padX + contentW - L.toggle.w;
    L.toggle.y = L.sheetY + border + padTop + (headH - L.toggle.h) * 0.5f;

    memset(L.seg, 0, sizeof(L.seg));
    memset(L.cells, 0, sizeof(L.cells));
    memset(L.slCell, 0, sizeof(L.slCell));
    memset(L.slTrack, 0, sizeof(L.slTrack));

    if (!g.collapsed) {
        float cx = L.sheetX + border + padX;
        float rowY = L.sheetY + border + padTop + headH + gap;
        float segGap = 8.0f * S;
        float segW = (contentW - segGap) * 0.5f;
        float bw = (segW - 10.0f * S) / 3.0f;
        float sy = rowY + 35.0f * S + gap;
        float colGap = 16.0f * S;
        float colW = (contentW - colGap) * 0.5f;

        L.seg[0].x = cx;
        L.seg[0].y = rowY;
        L.seg[0].w = segW;
        L.seg[0].h = 35.0f * S;

        L.seg[1].x = cx + segW + segGap;
        L.seg[1].y = rowY;
        L.seg[1].w = segW;
        L.seg[1].h = 35.0f * S;

        for (int k = 0; k < 3; k++) {
            L.cells[0][k].x = L.seg[0].x + 3.0f * S + (float)k * (bw + 2.0f * S);
            L.cells[0][k].y = rowY + 3.0f * S;
            L.cells[0][k].w = bw;
            L.cells[0][k].h = 29.0f * S;

            L.cells[1][k].x = L.seg[1].x + 3.0f * S + (float)k * (bw + 2.0f * S);
            L.cells[1][k].y = rowY + 3.0f * S;
            L.cells[1][k].w = bw;
            L.cells[1][k].h = 29.0f * S;
        }

        for (int i = 0; i < 4; i++) {
            int row = i / 2, col = i % 2;
            float x = cx + (float)col * (colW + colGap);
            float y = sy + (float)row * (38.0f * S + 10.0f * S);
            L.slCell[i].x = x;
            L.slCell[i].y = y;
            L.slCell[i].w = colW;
            L.slCell[i].h = 38.0f * S;
            L.slTrack[i].x = x;
            L.slTrack[i].y = y + 20.0f * S;
            L.slTrack[i].w = colW;
            L.slTrack[i].h = 18.0f * S;
        }
    }
}

/* ========================================================================== */
/*  物理                                                                      */
/* ========================================================================== */

static void shape_radii_target(void)
{
    const ShapeDef *sh = &SHAPES[g.shape];
    for (int i = 0; i < N_NODES; i++) {
        float c = fabsf(cosf(g_theta[i])) / sh->a;
        float s = fabsf(sinf(g_theta[i])) / sh->b;
        g_r0t[i] = g.baseR * powf(powf(c, sh->n) + powf(s, sh->n), -1.0f / sh->n);
    }
}

static float radius_at_js(float ang)
{
    float f = (ang / TAU_F) * (float)N_NODES;
    float ff;
    int i;
    f = fmodf(f, (float)N_NODES);
    if (f < 0.0f) f += (float)N_NODES;
    i = (int)floorf(f) % N_NODES;
    ff = f - floorf(f);
    return g_radii[i] * (1.0f - ff) + g_radii[(i + 1) % N_NODES] * ff;
}

static int blob_hit(float x, float y)
{
    float dx = x - g.st.x, dy = y - g.st.y;
    return hypotf(dx, dy) < radius_at_js(atan2f(dy, dx)) + 12.0f * g.dpr;
}

static void step_physics(float dt)
{
    float m = 1.0f - expf(-dt * 10.0f);
    float ex = 0.0f, ey = 0.0f;
    float ax, ay, aLen, aMax = 60000.0f;
    float K2, zeta, C2, couple, gain, speed, phi, stretch;
    float Ks, Cs, sT, mean = 0.0f;

    for (int i = 0; i < N_NODES; i++) g_r0[i] += (g_r0t[i] - g_r0[i]) * m;

    g.st.pvx = g.st.vx;
    g.st.pvy = g.st.vy;
    if (g.ptr.dragging) {
        float K = 520.0f, C = 2.0f * sqrtf(K) * 0.8f;
        g.st.vx += (K * (g.ptr.x + g.ptr.gx - g.st.x) - C * g.st.vx) * dt;
        g.st.vy += (K * (g.ptr.y + g.ptr.gy - g.st.y) - C * g.st.vy) * dt;
    } else {
        float f = expf(-dt * 2.2f);
        g.st.vx *= f;
        g.st.vy *= f;
    }
    g.st.x += g.st.vx * dt;
    g.st.y += g.st.vy * dt;

    for (int i = 0; i < N_NODES; i++) {
        ex = FMAX(ex, fabsf(cosf(g_theta[i])) * g_r0[i]);
        ey = FMAX(ey, fabsf(sinf(g_theta[i])) * g_r0[i]);
    }
    ex *= g.st.scale;
    ey *= g.st.scale;
    {
        const float rest = 0.55f;
        if (g.st.x < ex)      { g.st.x = ex;      if (g.st.vx < 0) g.st.vx = -g.st.vx * rest; }
        if (g.st.x > g.W - ex){ g.st.x = g.W - ex; if (g.st.vx > 0) g.st.vx = -g.st.vx * rest; }
        if (g.st.y < ey)      { g.st.y = ey;      if (g.st.vy < 0) g.st.vy = -g.st.vy * rest; }
        if (g.st.y > g.H - ey){ g.st.y = g.H - ey; if (g.st.vy > 0) g.st.vy = -g.st.vy * rest; }
    }

    ax = (g.st.vx - g.st.pvx) / dt / g.dpr;
    ay = (g.st.vy - g.st.pvy) / dt / g.dpr;
    aLen = hypotf(ax, ay);
    if (aLen > aMax) {
        ax *= aMax / aLen;
        ay *= aMax / aLen;
    }

    K2 = 260.0f + (1.0f - g.jelly) * 260.0f;
    zeta = 0.07f + (1.0f - g.jelly) * 0.45f;
    C2 = 2.0f * sqrtf(K2) * zeta;
    couple = K2 * 0.9f;
    gain = 0.0045f + g.jelly * 0.0035f;

    speed = hypotf(g.st.vx, g.st.vy) / g.dpr;
    phi = atan2f(g.st.vy, g.st.vx);
    stretch = FMIN(speed / 2600.0f, 1.0f) * (0.12f + g.jelly * 0.12f);

    for (int i = 0; i < N_NODES; i++) {
        float c = cosf(g_theta[i]), s = sinf(g_theta[i]);
        float target = stretch * cosf(2.0f * (g_theta[i] - phi));
        float lap = g_off[(i + N_NODES - 1) % N_NODES] + g_off[(i + 1) % N_NODES] - 2.0f * g_off[i];
        float inertia = -(ax * c + ay * s) * gain;
        g_tmp[i] = K2 * (target - g_off[i]) - C2 * g_offV[i] + couple * lap + inertia;
    }
    for (int i = 0; i < N_NODES; i++) {
        g_offV[i] += g_tmp[i] * dt;
        g_off[i] += g_offV[i] * dt;
        g_off[i] = FMAX(-0.38f, FMIN(0.45f, g_off[i]));
        mean += g_off[i];
    }
    mean /= (float)N_NODES;
    for (int i = 0; i < N_NODES; i++) g_off[i] -= mean;

    Ks = 420.0f;
    Cs = 2.0f * sqrtf(Ks) * (0.16f + (1.0f - g.jelly) * 0.4f);
    sT = g.ptr.dragging ? 1.07f : 1.0f;
    g.st.scaleV += (Ks * (sT - g.st.scale) - Cs * g.st.scaleV) * dt;
    g.st.scale += g.st.scaleV * dt;

    for (int i = 0; i < N_NODES; i++)
        g_radii[i] = g_r0[i] * g.st.scale * (1.0f + g_off[i]);
}

/* ========================================================================== */
/*  面板 UI 纹理                                                               */
/* ========================================================================== */

static int hit_widget(float x, float y)
{
    if (rect_contains(L.toggle, x, y)) return W_TOGGLE;
    if (!g.collapsed) {
        for (int k = 0; k < 3; k++)
            if (rect_contains(L.cells[0][k], x, y)) return W_SHAPE0 + k;
        for (int k = 0; k < 3; k++)
            if (rect_contains(L.cells[1][k], x, y)) return W_BG0 + k;
        for (int i = 0; i < 4; i++) {
            RectF r = L.slTrack[i];
            r.y -= 6.0f * g.dpr;
            r.h += 12.0f * g.dpr;
            if (rect_contains(r, x, y)) return W_SLIDER0 + i;
        }
    }
    return W_NONE;
}

static float *slider_value_ptr(int id)
{
    switch (id) {
    case W_SLIDER0: return &g.refract;
    case W_SLIDER1: return &g.disp;
    case W_SLIDER2: return &g.frost;
    case W_SLIDER3: return &g.jelly;
    default: return NULL;
    }
}

static uint32_t active_pill_color(void) { return 0xFFFFFF; }

static void ui_rebuild(void)
{
    float S = g.dpr;
    int SW, SH;
    int px13, px12, px11;
    float ox, oy;
    uint32_t ink = 0xEEF0F6, dim = 0x9AA0B4, accent = 0x5B9DFF;

    if (L.sheetW < 4.0f || L.sheetH < 4.0f) return;
    SW = (int)ceilf(L.sheetW);
    SH = (int)ceilf(L.sheetH);
    if (!g_uiCanvas.px || g_uiCanvas.w != SW || g_uiCanvas.h != SH)
        canvas_init(&g_uiCanvas, SW, SH, g.hdc);
    canvas_clear(&g_uiCanvas, 0);

    ox = L.sheetX;
    oy = L.sheetY;
    px13 = IMAX(1, (int)lroundf(13.0f * S));
    px12 = IMAX(1, (int)lroundf(12.0f * S));
    px11 = IMAX(1, (int)lroundf(11.0f * S));

    if (!g.collapsed) {
        /* 分段控件底 */
        for (int s = 0; s < 2; s++) {
            fill_round_rect(&g_uiCanvas,
                            L.seg[s].x - ox, L.seg[s].y - oy,
                            L.seg[s].x + L.seg[s].w - ox, L.seg[s].y + L.seg[s].h - oy,
                            11.0f * S, 0xFFFFFF, 0.06f);
        }

        /* 选中态 */
        for (int k = 0; k < 3; k++) {
            int active = (g.shape == k);
            if (active)
                fill_round_rect(&g_uiCanvas,
                                L.cells[0][k].x - ox, L.cells[0][k].y - oy,
                                L.cells[0][k].x + L.cells[0][k].w - ox,
                                L.cells[0][k].y + L.cells[0][k].h - oy,
                                8.0f * S, active_pill_color(), 0.14f);
            else if (g.hover == W_SHAPE0 + k)
                fill_round_rect(&g_uiCanvas,
                                L.cells[0][k].x - ox, L.cells[0][k].y - oy,
                                L.cells[0][k].x + L.cells[0][k].w - ox,
                                L.cells[0][k].y + L.cells[0][k].h - oy,
                                8.0f * S, 0xFFFFFF, 0.05f);
        }
        for (int k = 0; k < 2; k++) {
            int active = (g.bgKind == k);
            if (active)
                fill_round_rect(&g_uiCanvas,
                                L.cells[1][k].x - ox, L.cells[1][k].y - oy,
                                L.cells[1][k].x + L.cells[1][k].w - ox,
                                L.cells[1][k].y + L.cells[1][k].h - oy,
                                8.0f * S, active_pill_color(), 0.14f);
            else if (g.hover == W_BG0 + k)
                fill_round_rect(&g_uiCanvas,
                                L.cells[1][k].x - ox, L.cells[1][k].y - oy,
                                L.cells[1][k].x + L.cells[1][k].w - ox,
                                L.cells[1][k].y + L.cells[1][k].h - oy,
                                8.0f * S, 0xFFFFFF, 0.05f);
        }
        if (g.hover == W_BG2)
            fill_round_rect(&g_uiCanvas,
                            L.cells[1][2].x - ox, L.cells[1][2].y - oy,
                            L.cells[1][2].x + L.cells[1][2].w - ox,
                            L.cells[1][2].y + L.cells[1][2].h - oy,
                            8.0f * S, 0xFFFFFF, 0.05f);

        /* 分段文字 */
        {
            static const wchar_t *shapeNames[3] = {
                L"Circle", L"Pill", L"Squircle"
            };
            static const wchar_t *bgNames[3] = {
                L"Poster", L"Grid", L"Image"
            };
            for (int k = 0; k < 3; k++) {
                RectF r = L.cells[0][k];
                int active = (g.shape == k);
                float cy = r.y + r.h * 0.5f - oy;
                text_draw(&g_uiCanvas, shapeNames[k], r.x + r.w * 0.5f - ox,
                          mid_baseline(cy, px12, 400, 0), px12, 400, 1,
                          active ? ink : dim, 1.0f, 0);
            }
            for (int k = 0; k < 3; k++) {
                RectF r = L.cells[1][k];
                int active = (k < 2 && g.bgKind == k);
                float cy = r.y + r.h * 0.5f - oy;
                text_draw(&g_uiCanvas, bgNames[k], r.x + r.w * 0.5f - ox,
                          mid_baseline(cy, px12, 400, 0), px12, 400, 1,
                          active ? ink : dim, 1.0f, 0);
            }
        }

        /* 滑块 */
        {
            static const wchar_t *slNames[4] = {
                L"Refraction", L"Dispersion", L"Frost", L"Jelly"
            };
            const wchar_t *names[4] = { slNames[0], slNames[1], slNames[2], slNames[3] };
            float *vals[4] = { &g.refract, &g.disp, &g.frost, &g.jelly };
            for (int i = 0; i < 4; i++) {
                RectF cell = L.slCell[i];
                RectF tr = L.slTrack[i];
                int asc, desc, hgt;
                wchar_t buf[32];
                float x0 = tr.x - ox, x1 = tr.x + tr.w - ox;
                float cy = tr.y + tr.h * 0.5f - oy;
                float v = *vals[i];
                float knobX = x0 + (x1 - x0) * v;

                font_vmetrics(px11, 400, 0, &asc, &desc, &hgt);
                {
                    float labelBase = cell.y - oy + (16.0f * S - (float)(asc + desc)) * 0.5f + (float)asc;
                    text_draw(&g_uiCanvas, names[i], cell.x - ox,
                              labelBase, px11, 400, 0, dim, 1.0f, 0);
                    swprintf(buf, 32, L"%.2f", v);
                    text_draw(&g_uiCanvas, buf, cell.x + cell.w - ox,
                              labelBase, px11, 600, 2, ink, 1.0f, 1);
                }

                fill_round_rect(&g_uiCanvas, x0, cy - 2.0f * S, x1, cy + 2.0f * S,
                                2.0f * S, 0xFFFFFF, 0.16f);
                if (knobX > x0 + 0.5f)
                    fill_round_rect(&g_uiCanvas, x0, cy - 2.0f * S, knobX, cy + 2.0f * S,
                                    FMIN(2.0f * S, (knobX - x0) * 0.5f), accent, 1.0f);
                fill_circle(&g_uiCanvas, knobX, cy, 7.0f * S, accent, 0.30f);
                fill_circle(&g_uiCanvas, knobX, cy, 5.5f * S, 0xFFFFFF, 1.0f);
            }
        }
    }

    /* 标题 + 收起/展开 */
    {
        float headCy = L.sheetY + 13.0f * S + 13.0f * S - oy;
        text_draw(&g_uiCanvas, L"Vitreous", L.sheetX + 15.0f * S - ox,
                  mid_baseline(headCy, px13, 600, 0), px13, 600, 0, ink, 1.0f, 0);
        text_draw(&g_uiCanvas, g.collapsed ? L"Expand" : L"Collapse",
                  L.toggle.x + L.toggle.w * 0.5f - ox,
                  mid_baseline(L.toggle.y + L.toggle.h * 0.5f - oy, px12, 400, 0),
                  px12, 400, 1, dim, 1.0f, 0);
    }

    glBindTexture(GL_TEXTURE_2D, g_uiTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g_uiCanvas.w, g_uiCanvas.h, 0,
                 GL_BGRA, GL_UNSIGNED_BYTE, g_uiCanvas.px);
    g.uiDirty = 0;
}

/* ========================================================================== */
/*  交互动作                                                                   */
/* ========================================================================== */

static void pick_image(void)
{
    OPENFILENAMEW ofn;
    wchar_t file[MAX_PATH] = L"";

    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFilter =
        L"Images (*.png;*.jpg;*.jpeg;*.bmp;*.webp)\0*.png;*.jpg;*.jpeg;*.bmp;*.webp\0"
        L"All files\0*.*\0\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrTitle = L"Choose a background image";

    if (!GetOpenFileNameW(&ofn)) return;

    {
        Image img;
        if (load_image_wic(file, &img)) {
            if (g_img.px) free(g_img.px);
            g_img = img;
            g_imgValid = 1;
            g.bgKind = BG_IMAGE;
            build_background();
            g.uiDirty = 1;
            logf_("image loaded: %dx%d\n", img.w, img.h);
        } else {
            MessageBoxW(g.hwnd, L"Could not read that image.",
                        L"Vitreous", MB_ICONWARNING);
        }
    }
}

static void activate_widget(int w)
{
    switch (w) {
    case W_TOGGLE:
        g.collapsed = !g.collapsed;
        layout_update();
        g.uiDirty = 1;
        break;
    case W_SHAPE0:
    case W_SHAPE1:
    case W_SHAPE2:
        g.shape = w - W_SHAPE0;
        shape_radii_target();
        g.st.scaleV -= 1.6f;
        g.uiDirty = 1;
        break;
    case W_BG0:
        g.bgKind = BG_POSTER;
        build_background();
        g.uiDirty = 1;
        break;
    case W_BG1:
        g.bgKind = BG_GRID;
        build_background();
        g.uiDirty = 1;
        break;
    case W_BG2:
        pick_image();
        break;
    default:
        break;
    }
}

static void set_slider_from_x(int id, float x)
{
    float *v = slider_value_ptr(id);
    RectF r;
    if (!v) return;
    r = L.slTrack[id - W_SLIDER0];
    *v = CLAMPF((x - r.x) / FMAX(r.w, 1.0f), 0.0f, 1.0f);
    *v = floorf(*v * 100.0f + 0.5f) / 100.0f;
    g.uiDirty = 1;
}

/* ========================================================================== */
/*  渲染                                                                      */
/* ========================================================================== */

static void draw_hint(GLuint targetFbo)
{
    float x, y;

    if (g.hintAlpha <= 0.001f || !g_hintTex) return;

    x = ((float)g.W - (float)g_hintW) * 0.5f;
    y = 18.0f * g.dpr - 2.0f - (float)(g_hintPad - 4);

    glBindFramebuffer(GL_FRAMEBUFFER, targetFbo);
    glViewport(0, 0, g.W, g.H);
    glUseProgram(g_spriteProg);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_hintTex);
    glUniform1i(g_pTex, 0);
    glUniform1f(g_pAlpha, g.hintAlpha);
    glUniform4f(g_pRect, x, y, (float)g_hintW, (float)g_hintH);
    glUniform2f(g_pRes, (float)g.W, (float)g.H);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(g_vaoQuad);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisable(GL_BLEND);
}

static void blur_scene(float sigma)
{
    float step = FMAX(sigma * 0.45f, 0.5f);

    glUseProgram(g_blurProg);
    glBindVertexArray(g_vaoTri);
    glUniform2f(g_bRes, (float)g.W, (float)g.H);
    glUniform1f(g_bSigma, sigma);
    glActiveTexture(GL_TEXTURE0);

    glBindFramebuffer(GL_FRAMEBUFFER, g_blurFbo[0]);
    glViewport(0, 0, g.W, g.H);
    glBindTexture(GL_TEXTURE_2D, g_bgTex);
    glUniform2f(g_bStep, step, 0.0f);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindFramebuffer(GL_FRAMEBUFFER, g_blurFbo[1]);
    glBindTexture(GL_TEXTURE_2D, g_blurTex[0]);
    glUniform2f(g_bStep, 0.0f, step);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

static void render(void)
{
    GLuint target = g.shotMode ? g_outFbo : 0;
    float sigma = g.frost * 7.0f * g.dpr;
    int doBlur = sigma > 0.6f;

    if (doBlur) blur_scene(sigma);

    /* pass 1: glass -> sceneTex */
    glBindFramebuffer(GL_FRAMEBUFFER, g_sceneFbo);
    glViewport(0, 0, g.W, g.H);
    glUseProgram(g_glassProg);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, g_blurTex[1]);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_bgTex);
    glUniform1i(g_uBg, 0);
    glUniform1f(g_uFrostMix, doBlur ? FMIN(1.0f, g.frost * 25.0f) : 0.0f);
    glUniform2f(g_uRes, (float)g.W, (float)g.H);
    glUniform2f(g_uCenter, g.st.x, g.st.y);
    glUniform1fv(g_uR, N_NODES, g_radii);
    glUniform1f(g_uRefract, g.refract);
    glUniform1f(g_uDisp, g.disp);
    glUniform1f(g_uFrost, g.frost);
    {
        float minR = g_r0[0];
        for (int i = 1; i < N_NODES; i++) minR = FMIN(minR, g_r0[i]);
        glUniform1f(g_uEdge, FMAX(minR * g.st.scale * 0.55f, 1.0f));
    }
    glUniform1f(g_uDpr, g.dpr);
    glBindVertexArray(g_vaoTri);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    /* pass 2: sheet + backdrop blur -> target */
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glViewport(0, 0, g.W, g.H);
    glUseProgram(g_sheetProg);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_sceneTex);
    glUniform1i(g_sScene, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, g_uiTex);
    glUniform1i(g_sUi, 1);
    glActiveTexture(GL_TEXTURE0);
    glUniform2f(g_sRes, (float)g.W, (float)g.H);
    glUniform4f(g_sRect, L.sheetX, L.sheetY, L.sheetX + L.sheetW, L.sheetY + L.sheetH);
    glUniform1f(g_sRadius, 22.0f * g.dpr);
    glUniform1f(g_sDpr, g.dpr);
    glBindVertexArray(g_vaoTri);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    draw_hint(target);
}

/* ========================================================================== */
/*  截图输出                                                                   */
/* ========================================================================== */

static int save_bmp(const wchar_t *path, const uint8_t *bgra, int W, int H)
{
    FILE *f;
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    int rowBytes = W * 3;
    int pad = (4 - (rowBytes % 4)) % 4;
    size_t total = (size_t)(rowBytes + pad) * (size_t)H;
    uint8_t *row = (uint8_t *)malloc((size_t)rowBytes + (size_t)pad);

    if (!row) return 0;
    f = _wfopen(path, L"wb");
    if (!f) {
        free(row);
        return 0;
    }
    memset(&fh, 0, sizeof(fh));
    memset(&ih, 0, sizeof(ih));
    fh.bfType = 0x4D42;
    fh.bfOffBits = (DWORD)(sizeof(fh) + sizeof(ih));
    fh.bfSize = (DWORD)(fh.bfOffBits + total);
    ih.biSize = sizeof(ih);
    ih.biWidth = W;
    ih.biHeight = H;
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = (DWORD)total;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);

    for (int y = 0; y < H; y++) {
        const uint8_t *src = bgra + (size_t)y * (size_t)W * 4;
        for (int x = 0; x < W; x++) {
            row[x * 3 + 0] = src[x * 4 + 0];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 2];
        }
        memset(row + rowBytes, 0, (size_t)pad);
        fwrite(row, (size_t)rowBytes + (size_t)pad, 1, f);
    }
    fclose(f);
    free(row);
    return 1;
}

static void run_shot(void)
{
    uint8_t *pixels;
    int steps;
    float t;

    fbo_target(&g_outTex, &g_outFbo, &g_outW, &g_outH, g.W, g.H);

    steps = (int)((float)g.shotMs / 1000.0f / H_STEP);

    /* 合成点击 / 拖拽，用于在无人值守环境下验证交互链路 */
    if (g_testClick && g.hwnd) {
        SendMessageW(g.hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(g_tcX, g_tcY));
        SendMessageW(g.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(g_tcX, g_tcY));
        SendMessageW(g.hwnd, WM_LBUTTONUP, 0, MAKELPARAM(g_tcX, g_tcY));
        logf_("test click at (%d,%d): shape=%d bg=%d collapsed=%d\n",
              g_tcX, g_tcY, g.shape, g.bgKind, g.collapsed);
    }

    if (g_testDrag && g.hwnd) {
        int dragSteps = steps * 2 / 3;
        if (dragSteps < 1) dragSteps = 1;
        SendMessageW(g.hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(g_tdX0, g_tdY0));
        SendMessageW(g.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(g_tdX0, g_tdY0));
        for (int i = 0; i < steps; i++) {
            if (i < dragSteps) {
                float u = (float)i / (float)dragSteps;
                int x = (int)((float)g_tdX0 + (float)(g_tdX1 - g_tdX0) * u);
                int y = (int)((float)g_tdY0 + (float)(g_tdY1 - g_tdY0) * u);
                SendMessageW(g.hwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x, y));
            } else if (i == dragSteps) {
                SendMessageW(g.hwnd, WM_LBUTTONUP, 0, MAKELPARAM(g_tdX1, g_tdY1));
            }
            step_physics(H_STEP);
        }
    } else {
        for (int i = 0; i < steps; i++) step_physics(H_STEP);
    }

    {
        float rmin = g_radii[0], rmax = g_radii[0];
        for (int i = 1; i < N_NODES; i++) {
            rmin = FMIN(rmin, g_radii[i]);
            rmax = FMAX(rmax, g_radii[i]);
        }
        logf_("shot state: center=(%.1f,%.1f) scale=%.4f scaleV=%.4f rmin=%.2f rmax=%.2f baseR=%.2f dpr=%.2f shape=%d bg=%d collapsed=%d\n",
              g.st.x, g.st.y, g.st.scale, g.st.scaleV, rmin, rmax, g.baseR, g.dpr,
              g.shape, g.bgKind, g.collapsed);
    }

    if (g.uiDirty) ui_rebuild();
    render();
    glFinish();

    pixels = (uint8_t *)malloc((size_t)g.W * (size_t)g.H * 4);
    if (!pixels) fatal("out of memory");
    glBindFramebuffer(GL_FRAMEBUFFER, g_outFbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, g.W, g.H, GL_BGRA, GL_UNSIGNED_BYTE, pixels);

    if (!save_bmp(g.shotPath, pixels, g.W, g.H)) {
        logf_("save_bmp failed\n");
        MessageBoxW(g.hwnd, L"\u4fdd\u5b58 BMP \u5931\u8d25\u3002", L"Liquid Glass", MB_ICONERROR);
    } else {
        logf_("saved %ls (%dx%d)\n", g.shotPath, g.W, g.H);
    }
    free(pixels);
    t = (float)g.W / (float)g.H;
    (void)t;
}

/* ========================================================================== */
/*  重置 / 窗口尺寸                                                            */
/* ========================================================================== */

static void app_resize(int W, int H)
{
    int oldW, oldH;

    if (!g.glReady || W < 2 || H < 2) return;

    oldW = g.W;
    oldH = g.H;
    g.W = W;
    g.H = H;
    g.dpr = g.dprOverride > 0.0f ? g.dprOverride : dpi_scale(g.hwnd);
    if (g.dpr > 2.0f) g.dpr = 2.0f;
    if (g.dpr < 0.5f) g.dpr = 0.5f;

    g.baseR = FMAX(80.0f, FMIN(165.0f, FMIN((float)W / g.dpr, (float)H / g.dpr) * 0.24f)) * g.dpr;

    shape_radii_target();
    if (oldW < 2) {
        memcpy(g_r0, g_r0t, sizeof(g_r0));
        g.st.x = (float)W * 0.5f;
        g.st.y = (float)H * 0.40f;
    } else {
        g.st.x *= (float)W / (float)oldW;
        g.st.y *= (float)H / (float)oldH;
    }

    layout_update();
    build_background();
    fbo_target(&g_sceneTex, &g_sceneFbo, &g_sceneW, &g_sceneH, g.W, g.H);
    fbo_target(&g_blurTex[0], &g_blurFbo[0], &g_blurW[0], &g_blurH[0], g.W, g.H);
    fbo_target(&g_blurTex[1], &g_blurFbo[1], &g_blurW[1], &g_blurH[1], g.W, g.H);
    build_hint();
    ui_rebuild();
    glViewport(0, 0, g.W, g.H);
    logf_("resize: %dx%d dpr=%.2f baseR=%.1f sheet=%.0fx%.0f\n",
          W, H, g.dpr, g.baseR, L.sheetW, L.sheetH);
}

/* ========================================================================== */
/*  窗口过程                                                                   */
/* ========================================================================== */

static void update_cursor(void)
{
    HCURSOR cur = LoadCursorW(NULL, IDC_ARROW);
    if (g.ptr.dragging) {
        cur = LoadCursorW(NULL, IDC_HAND);
    } else if (g.hover == W_TOGGLE || (g.hover >= W_SHAPE0 && g.hover <= W_BG2)) {
        cur = LoadCursorW(NULL, IDC_HAND);
    } else if (g.hover == W_NONE && blob_hit(g.ptr.x, g.ptr.y)) {
        cur = LoadCursorW(NULL, IDC_HAND);
    }
    SetCursor(cur);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        break;

    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) {
            g.minimized = 1;
        } else {
            g.minimized = 0;
            app_resize(LOWORD(lp), HIWORD(lp));
        }
        return 0;

    case WM_DPICHANGED:
        {
            RECT *r = (RECT *)lp;
            SetWindowPos(hwnd, NULL, r->left, r->top,
                         r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
        }
        return 0;

    case WM_MOUSEMOVE:
        {
            float x = (float)GET_X_LPARAM(lp);
            float y = (float)GET_Y_LPARAM(lp);
            int newHover;

            if (!g.ptr.dragging && !g.sliderDrag) {
                TRACKMOUSEEVENT tme;
                memset(&tme, 0, sizeof(tme));
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                TrackMouseEvent(&tme);
            }

            g.ptr.x = x;
            g.ptr.y = y;

            if (g.sliderDrag && g.active >= W_SLIDER0 && g.active <= W_SLIDER3) {
                set_slider_from_x(g.active, x);
                update_cursor();
                return 0;
            }
            if (g.ptr.dragging) {
                update_cursor();
                return 0;
            }

            newHover = hit_widget(x, y);
            if (newHover != g.hover) {
                g.hover = newHover;
                g.uiDirty = 1;
            }
            update_cursor();
        }
        return 0;

    case WM_MOUSELEAVE:
        if (g.hover != W_NONE) {
            g.hover = W_NONE;
            g.uiDirty = 1;
        }
        g.ptr.x = -10000.0f;
        g.ptr.y = -10000.0f;
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            update_cursor();
            return TRUE;
        }
        break;

    case WM_LBUTTONDOWN:
        {
            float x = (float)GET_X_LPARAM(lp);
            float y = (float)GET_Y_LPARAM(lp);
            int w = hit_widget(x, y);

            SetFocus(hwnd);
            g.ptr.x = x;
            g.ptr.y = y;

            if (w != W_NONE) {
                g.active = w;
                g.hover = w;
                if (w >= W_SLIDER0 && w <= W_SLIDER3) {
                    g.sliderDrag = 1;
                    set_slider_from_x(w, x);
                }
                g.uiDirty = 1;
                SetCapture(hwnd);
                return 0;
            }

            if (blob_hit(x, y)) {
                g.ptr.dragging = 1;
                g.ptr.gx = g.st.x - x;
                g.ptr.gy = g.st.y - y;
                g.st.scaleV -= 0.6f;
                SetCapture(hwnd);
                if (g.hintVisible) {
                    g.hintVisible = 0;
                    g.hintFading = 1;
                    g.hintFadeT0 = now_sec();
                }
                update_cursor();
            }
        }
        return 0;

    case WM_LBUTTONUP:
        if (g.sliderDrag) {
            g.sliderDrag = 0;
            g.active = W_NONE;
            ReleaseCapture();
            return 0;
        }
        if (g.ptr.dragging) {
            g.ptr.dragging = 0;
            ReleaseCapture();
            update_cursor();
            return 0;
        }
        if (g.active != W_NONE) {
            int w = g.active;
            float x = (float)GET_X_LPARAM(lp);
            float y = (float)GET_Y_LPARAM(lp);
            g.active = W_NONE;
            ReleaseCapture();
            if (w == hit_widget(x, y)) activate_widget(w);
            g.uiDirty = 1;
        }
        return 0;

    case WM_CAPTURECHANGED:
        g.ptr.dragging = 0;
        g.sliderDrag = 0;
        g.active = W_NONE;
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            g.running = 0;
            DestroyWindow(hwnd);
        }
        return 0;

    case WM_CLOSE:
        g.running = 0;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ========================================================================== */
/*  参数解析                                                                   */
/* ========================================================================== */

static void set_dpi_aware(void)
{
    typedef BOOL(WINAPI * PFN_SetProcessDpiAwarenessContext)(void *);
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u) {
        PFN_SetProcessDpiAwarenessContext pfn =
            (PFN_SetProcessDpiAwarenessContext)(void *)GetProcAddress(
                u, "SetProcessDpiAwarenessContext");
        if (pfn) {
            /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = -4 */
            if (pfn((void *)(intptr_t)-4)) return;
        }
    }
    SetProcessDPIAware();
}

static void parse_values(const wchar_t *s)
{
    float v[4];
    int n = swscanf(s, L"%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3]);
    if (n >= 1) g.refract = CLAMPF(v[0], 0, 1);
    if (n >= 2) g.disp = CLAMPF(v[1], 0, 1);
    if (n >= 3) g.frost = CLAMPF(v[2], 0, 1);
    if (n >= 4) g.jelly = CLAMPF(v[3], 0, 1);
}

static void parse_args(void)
{
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    g.argW = 1180;
    g.argH = 780;
    g.shotMs = 600;

    if (!argv) return;
    for (int i = 1; i < argc; i++) {
        const wchar_t *a = argv[i];
        if (!wcscmp(a, L"--log")) {
            g.logOn = 1;
        } else if (!wcscmp(a, L"--shot") && i + 1 < argc) {
            g.shotMode = 1;
            wcsncpy(g.shotPath, argv[++i], MAX_PATH - 1);
        } else if (!wcscmp(a, L"--size") && i + 1 < argc) {
            int w = 0, h = 0;
            if (swscanf(argv[++i], L"%dx%d", &w, &h) == 2 && w > 100 && h > 100) {
                g.argW = w;
                g.argH = h;
            }
        } else if (!wcscmp(a, L"--ms") && i + 1 < argc) {
            g.shotMs = _wtoi(argv[++i]);
            if (g.shotMs < 0) g.shotMs = 0;
        } else if (!wcscmp(a, L"--dpr") && i + 1 < argc) {
            g.dprOverride = (float)_wtof(argv[++i]);
        } else if (!wcscmp(a, L"--shape") && i + 1 < argc) {
            const wchar_t *v = argv[++i];
            if (!wcscmp(v, L"pill") || !wcscmp(v, L"1")) g.shape = 1;
            else if (!wcscmp(v, L"squircle") || !wcscmp(v, L"2")) g.shape = 2;
            else g.shape = 0;
        } else if (!wcscmp(a, L"--bg") && i + 1 < argc) {
            const wchar_t *v = argv[++i];
            if (!wcscmp(v, L"grid") || !wcscmp(v, L"1")) g.bgKind = BG_GRID;
            else if (!wcscmp(v, L"image") || !wcscmp(v, L"2")) g.bgKind = BG_IMAGE;
            else g.bgKind = BG_POSTER;
        } else if (!wcscmp(a, L"--collapse")) {
            g.collapsed = 1;
        } else if (!wcscmp(a, L"--values") && i + 1 < argc) {
            parse_values(argv[++i]);
        } else if (!wcscmp(a, L"--click") && i + 1 < argc) {
            if (swscanf(argv[++i], L"%d,%d", &g_tcX, &g_tcY) == 2) g_testClick = 1;
        } else if (!wcscmp(a, L"--drag") && i + 1 < argc) {
            if (swscanf(argv[++i], L"%d,%d,%d,%d", &g_tdX0, &g_tdY0, &g_tdX1, &g_tdY1) == 4)
                g_testDrag = 1;
        } else if (!wcscmp(a, L"--image") && i + 1 < argc) {
            if (load_image_wic(argv[++i], &g_img)) {
                g_imgValid = 1;
                g.bgKind = BG_IMAGE;
            }
        } else if (!wcscmp(a, L"--help")) {
            MessageBoxW(NULL,
                        L"Vitreous [--shot out.bmp] [--size 1200x800] [--ms 600]\n"
                        L"  [--dpr 1.5] [--shape circle|pill|squircle|0|1|2]\n"
                        L"  [--bg poster|grid|image|0|1|2] [--image PATH] [--collapse]\n"
                        L"  [--values r,d,f,j] [--click x,y] [--drag x0,y0,x1,y1] [--log]",
                        L"Vitreous", MB_OK);
            ExitProcess(0);
        }
    }
    LocalFree(argv);
}

/* ========================================================================== */
/*  入口                                                                      */
/* ========================================================================== */

static int g_quit = 0;

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLine, int nShow)
{
    WNDCLASSEXW wc;
    HWND hwnd;
    RECT rc;
    int winW, winH, sx, sy;
    MSG msg;
    float last, acc = 0.0f;
    const wchar_t *title =
        L"Vitreous \u2014 C / Win32 + OpenGL 3.3";

    (void)hPrev;
    (void)cmdLine;
    (void)nShow;

    memset(&g, 0, sizeof(g));
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    g.inst = hInst;
    g.running = 1;
    g.shape = 0;
    g.bgKind = BG_POSTER;
    g.refract = 0.60f;
    g.disp = 0.35f;
    g.frost = 0.10f;
    g.jelly = 0.70f;
    g.st.scale = 0.60f;
    g.hintVisible = 1;
    g.hintAlpha = 1.0f;
    g.ptr.x = -10000.0f;
    g.ptr.y = -10000.0f;

    for (int i = 0; i < N_NODES; i++)
        g_theta[i] = (float)i / (float)N_NODES * TAU_F;

    parse_args();
    if (g.logOn) {
        g_log = fopen("vitreous.log", "w");
        logf_("Vitreous starting\n");
    }

    set_dpi_aware();

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"VitreousWnd";
    if (!RegisterClassExW(&wc)) fatal("RegisterClassExW failed");

    rc.left = 0;
    rc.top = 0;
    rc.right = g.argW;
    rc.bottom = g.argH;
    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);
    winW = rc.right - rc.left;
    winH = rc.bottom - rc.top;
    sx = (GetSystemMetrics(SM_CXSCREEN) - winW) / 2;
    sy = (GetSystemMetrics(SM_CYSCREEN) - winH) / 2;
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;

    hwnd = CreateWindowExW(0, L"VitreousWnd", title, WS_OVERLAPPEDWINDOW,
                           sx, sy, winW, winH, NULL, NULL, hInst, NULL);
    if (!hwnd) fatal("CreateWindowExW failed");
    g.hwnd = hwnd;

    if (!create_gl_context(hwnd)) {
        MessageBoxW(hwnd, L"\u65e0\u6cd5\u521b\u5efa OpenGL 3.3 \u4e0a\u4e0b\u6587\u3002\n"
                          L"\u8bf7\u786e\u8ba4\u9a71\u52a8\u7a0b\u5e8f\u652f\u6301 OpenGL 3.3\u3002",
                    L"\u6db2\u6001\u73bb\u7483", MB_ICONERROR);
        return 1;
    }
    gl_init_resources();
    g.glReady = 1;

    /* 入场：弹出 + 抖动 */
    g.st.scaleV = 4.0f;
    for (int i = 0; i < N_NODES; i++)
        g_offV[i] = cosf(g_theta[i] * 3.0f) * 1.2f;

    {
        RECT cr;
        GetClientRect(hwnd, &cr);
        app_resize(cr.right - cr.left, cr.bottom - cr.top);
    }

    if (g.shotMode) {
        run_shot();
        if (g_log) fclose(g_log);
        return 0;
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);

    last = now_sec();
    while (g.running) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g.running = 0;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g.running) break;
        if (g.minimized) {
            Sleep(16);
            last = now_sec();
            continue;
        }

        {
            float now = now_sec();
            float dt = now - last;
            last = now;
            if (dt > 0.05f) dt = 0.05f;
            if (dt < 0.0f) dt = 0.0f;
            acc += dt;
            while (acc >= H_STEP) {
                step_physics(H_STEP);
                acc -= H_STEP;
            }
        }

        if (g.hintFading) {
            float t = (now_sec() - g.hintFadeT0) / 0.6f;
            if (t >= 1.0f) {
                g.hintAlpha = 0.0f;
                g.hintFading = 0;
            } else {
                g.hintAlpha = 1.0f - t;
            }
        }

        if (g.uiDirty) ui_rebuild();

        render();
        SwapBuffers(g.hdc);
    }

    if (g.glrc) {
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(g.glrc);
        g.glrc = NULL;
    }
    if (g.hdc) {
        ReleaseDC(hwnd, g.hdc);
        g.hdc = NULL;
    }
    if (g_log) fclose(g_log);
    CoUninitialize();
    (void)g_quit;
    return 0;
}
