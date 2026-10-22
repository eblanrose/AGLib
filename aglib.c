#include "aglib.h"
#include "aglib_int.h"
#include "agthread.h"
#include "aggl.h"

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <locale.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <windowsx.h>
#  ifndef DT_NOEXPAND
#    define DT_NOEXPAND 0x00001000u
#  endif
#else
#  include <ft2build.h>
#  define FT_FREETYPE_H <freetype/freetype.h>
#  include FT_FREETYPE_H
#  ifdef AG_USE_WAYLAND
#    include <wayland-client.h>
#    include <xkbcommon/xkbcommon.h>
#    include <sys/mman.h>
#    include <sys/stat.h>
#    include <fcntl.h>
#    include <unistd.h>
#    include <poll.h>
#    include <errno.h>
#    include "xdg-shell-client-protocol.h"
#    include "xdg-decoration-unstable-v1-client-protocol.h"
#  else
#    include <X11/Xlib.h>
#    include <X11/Xutil.h>
#    include <X11/keysym.h>
#    include <X11/Xlocale.h>
#  endif
#endif

#include <png.h>
#include <jpeglib.h>
#include <setjmp.h>

#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include <nanosvgrast.h>

struct ag_window {
    int width, height;
    char title[256];

    ag_color current_color;
    int      font_size;

    int      clip_on, clip_x, clip_y, clip_w, clip_h;

    int      alpha;
    int      ox, oy;

    ag_u32  *rt;
    int      rt_w, rt_h;

    ag_gl_layer layers[2];
    int         nlayers;

    ag_u32 *backbuf;

    ag_event_cb cb;
    void       *userdata;

    int  should_close;
    int  redraw_pending;           

    ag_font *font;
    struct ag_ui *ui;

#ifdef _WIN32
    HWND      hwnd;
    HDC       memdc;
    HBITMAP   membmp;
    HBITMAP   oldbmp;
    int       pending_high;      
#elif defined(AG_USE_WAYLAND)
    struct wl_surface   *wl_surface;
    struct xdg_surface  *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
    struct zxdg_toplevel_decoration_v1 *xdg_decoration;
    struct wl_buffer    *buf[2];
    void                *shm_data[2];
    size_t               shm_size[2];
    int                  buf_busy[2];
    int                  cur_buf;
    int                  configured;
    int                  pending_w, pending_h;
#else
    Display  *dpy;
    Window    win;
    GC        gc;
    XImage   *img;
    Atom      wm_delete;
    int       screen;
    XIM       xim;                
    XIC       xic;
    void     *gl_visual;
    Colormap  gl_cmap;
    ag_gl_present *present;
#endif

    struct ag_window *next;
};

#define AG_GLYPH_CACHE 512

struct ag_glyph {
    ag_u32 cp;
    int xoff, yoff;
    int w, h;
    int adv;
    ag_u8 *bits;
};

static inline int ag_glyph_slot(ag_u32 cp)
{
    return (int)((cp * 2654435761u) >> 22) & (AG_GLYPH_CACHE - 1);
}

static ag_u32 utf8_decode(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    if (!p[0]) return 0;
    ag_u32 cp;
    if (p[0] < 0x80) { cp = p[0]; *s += 1; }
    else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); *s += 2;
    }
    else if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); *s += 3;
    }
    else if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 &&
             (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        cp = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) |
             ((p[2] & 0x3F) << 6)  | (p[3] & 0x3F); *s += 4;
    } else { cp = 0xFFFD; *s += 1; }
    return cp;
}

static int utf8_encode(char out[4], ag_u32 cp)
{
    if (cp < 0x80)       { out[0] = (char)cp; return 1; }
    if (cp < 0x800)      { out[0] = 0xC0 | (cp >> 6);  out[1] = 0x80 | (cp & 0x3F); return 2; }
    if (cp < 0x10000)    { out[0] = 0xE0 | (cp >> 12); out[1] = 0x80 | ((cp >> 6) & 0x3F); out[2] = 0x80 | (cp & 0x3F); return 3; }
    out[0] = 0xF0 | (cp >> 18); out[1] = 0x80 | ((cp >> 12) & 0x3F);
    out[2] = 0x80 | ((cp >> 6) & 0x3F); out[3] = 0x80 | (cp & 0x3F);
    return 4;
}

struct ag_font {
    unsigned int px;
    int ascent, descent;
    struct ag_glyph cache[AG_GLYPH_CACHE];
    char family[64];
#ifdef _WIN32
    HFONT hfont;
    HDC   dc;
#else
    FT_Face face;
#endif
};

static const char *ag_font_paths[] = {
    "assets/fonts/DejaVuSans.ttf",
    "assets/fonts/DejaVuSans-Bold.ttf",
    "DejaVuSans.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
    NULL
};

static void ag_font_clear_cache(ag_font *f)
{
    if (!f) return;
    for (int i = 0; i < AG_GLYPH_CACHE; ++i) {
        free(f->cache[i].bits);
        memset(&f->cache[i], 0, sizeof(f->cache[i]));
    }
}

static int ag_ttf_family(const char *path, char *out, int cap)
{
    if (!path || !out || cap < 2) return 0;
    out[0] = 0;

    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    unsigned char hdr[12];
    if (fread(hdr, 1, 12, f) != 12) { fclose(f); return 0; }

    if (memcmp(hdr, "ttcf", 4) == 0) {
        unsigned char b4[4];
        if (fseek(f, 12, SEEK_SET) != 0) { fclose(f); return 0; }
        if (fread(b4, 1, 4, f) != 4) { fclose(f); return 0; }
        long base = (long)(((unsigned)b4[0] << 24) | ((unsigned)b4[1] << 16) |
                           ((unsigned)b4[2] << 8) | b4[3]);
        if (fseek(f, base, SEEK_SET) != 0) { fclose(f); return 0; }
        if (fread(hdr, 1, 12, f) != 12) { fclose(f); return 0; }
    }

    int num = (hdr[4] << 8) | hdr[5];
    unsigned int name_off = 0, name_len = 0;
    for (int i = 0; i < num; ++i) {
        unsigned char rec[16];
        if (fread(rec, 1, 16, f) != 16) break;
        if (memcmp(rec, "name", 4) == 0) {
            name_off = ((unsigned)rec[8] << 24) | ((unsigned)rec[9] << 16) |
                        ((unsigned)rec[10] << 8) | rec[11];
            name_len = ((unsigned)rec[12] << 24) | ((unsigned)rec[13] << 16) |
                       ((unsigned)rec[14] << 8) | rec[15];
            break;
        }
    }
    if (!name_off || !name_len) { fclose(f); return 0; }

    if (fseek(f, (long)name_off, SEEK_SET) != 0) { fclose(f); return 0; }
    unsigned char nh[6];
    if (fread(nh, 1, 6, f) != 6) { fclose(f); return 0; }
    int count = (nh[2] << 8) | nh[3];
    int str_off = (nh[4] << 8) | nh[5];

    for (int i = 0; i < count; ++i) {
        unsigned char rec[12];
        if (fread(rec, 1, 12, f) != 12) break;
        int platform = (rec[0] << 8) | rec[1];
        int nameid   = (rec[6] << 8) | rec[7];
        int len      = (rec[8] << 8) | rec[9];
        int off      = (rec[10] << 8) | rec[11];
        if (nameid != 1) continue;
        if (len <= 0 || len > 1024) continue;
        if (fseek(f, (long)name_off + str_off + off, SEEK_SET) != 0) continue;
        char raw[1024];
        if (fread(raw, 1, (size_t)len, f) != (size_t)len) continue;

        int o = 0;
        if (platform == 0 || platform == 3) {
            for (int k = 0; k + 1 < len && o < cap - 4; k += 2) {
                unsigned cp = ((unsigned)(unsigned char)raw[k] << 8) |
                              (unsigned char)raw[k + 1];
                if (!cp) break;
                o += utf8_encode(out + o, cp);
            }
        } else {
            for (int k = 0; k < len && o < cap - 1; ++k) {
                if (!raw[k]) break;
                out[o++] = raw[k];
            }
        }
        out[o] = 0;
        if (o > 0) { fclose(f); return 1; }
    }
    fclose(f);
    return 0;
}

#ifdef _WIN32
static int ag_is_font_file(const char *name)
{
    if (!name) return 0;
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    if (strcmp(dot, ".ttf") && strcmp(dot, ".TTF") &&
        strcmp(dot, ".otf") && strcmp(dot, ".OTF") &&
        strcmp(dot, ".ttc") && strcmp(dot, ".TTC")) return 0;
    return 1;
}
#endif

static ag_font *ag_font_default_common(unsigned int px)
{
    const char *env = getenv("AG_FONT");
    if (env && *env) {
        ag_font *f = ag_font_open_file(env, px);
        if (f) return f;
    }
    for (int i = 0; ag_font_paths[i]; ++i) {
        ag_font *f = ag_font_open_file(ag_font_paths[i], px);
        if (f) return f;
    }
    return NULL;
}

static inline int font_height(const ag_font *f)
{
#ifdef _WIN32
    return f->ascent + f->descent;
#else
    return f->ascent - f->descent;
#endif
}

static ag_window *g_windows = NULL;
static int        g_running = 0;

unsigned long ag_time_ms(void)
{
#ifdef _WIN32
    return (unsigned long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)(ts.tv_sec * 1000UL + ts.tv_nsec / 1000000UL);
#endif
}

static void ag_quit_windows(void)
{
    for (ag_window *w = g_windows; w; w = w->next)
        w->should_close = 1;
}

void ag_sleep_ms(unsigned int ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

static void *ag_mem_alloc(size_t sz) { return calloc(1, sz); }
static void  ag_mem_free(void *p)    { free(p); }

static void backbuf_resize(ag_window *w, int new_w, int new_h)
{
    if (new_w <= 0) new_w = 1;
    if (new_h <= 0) new_h = 1;

    ag_u32 *nb = (ag_u32 *)realloc(w->backbuf, sizeof(ag_u32) * new_w * new_h);
    if (!nb) return;
    w->backbuf = nb;
    w->width   = new_w;
    w->height  = new_h;

    for (int i = 0; i < new_w * new_h; ++i)
        w->backbuf[i] = 0xFF000000u;
}

static inline void blend_px(ag_window *w, int x, int y, ag_u32 rgb, int alpha);

static inline void rt_blend(ag_window *w, int x, int y, ag_u32 rgb, int alpha)
{
    int sa = alpha * w->alpha / 255;
    if (sa <= 0) return;
    if (sa > 255) sa = 255;
    if (w->clip_on &&
        (x < w->clip_x || y < w->clip_y ||
         x >= w->clip_x + w->clip_w || y >= w->clip_y + w->clip_h))
        return;
    if (x < 0 || y < 0 || x >= w->rt_w || y >= w->rt_h) return;

    ag_u32 *d = &w->rt[y * w->rt_w + x];
    unsigned da = (*d >> 24) & 0xFF;
    unsigned dr = (*d >> 16) & 0xFF;
    unsigned dg = (*d >> 8) & 0xFF;
    unsigned db = (*d) & 0xFF;
    unsigned sr = (rgb >> 16) & 0xFF;
    unsigned sg = (rgb >> 8) & 0xFF;
    unsigned sb = (rgb) & 0xFF;
    unsigned inv = 255u - (unsigned)sa;
    unsigned na = (unsigned)sa + da * inv / 255u;
    unsigned nr = (sr * (unsigned)sa + dr * inv) / 255u;
    unsigned ng = (sg * (unsigned)sa + dg * inv) / 255u;
    unsigned nb = (sb * (unsigned)sa + db * inv) / 255u;
    *d = (na << 24) | (nr << 16) | (ng << 8) | nb;
}

static inline void put_pixel(ag_window *w, int x, int y, ag_u32 argb)
{
    x += w->ox;
    y += w->oy;
    if (w->rt) { rt_blend(w, x, y, argb & 0x00FFFFFFu, 255); return; }
    if (w->clip_on &&
        (x < w->clip_x || y < w->clip_y ||
         x >= w->clip_x + w->clip_w || y >= w->clip_y + w->clip_h))
        return;
    if (x < 0 || y < 0 || x >= w->width || y >= w->height) return;
    if (w->alpha < 255) {
        blend_px(w, x, y, argb & 0x00FFFFFFu, 255);
        return;
    }
    w->backbuf[y * w->width + x] = argb;
}

static inline ag_u32 color_to_argb(ag_color c)
{
    return 0xFF000000u | (c & 0x00FFFFFFu);
}

static inline void blend_px(ag_window *w, int x, int y, ag_u32 rgb, int alpha)
{
    if (w->rt) { rt_blend(w, x, y, rgb, alpha); return; }
    if (w->clip_on &&
        (x < w->clip_x || y < w->clip_y ||
         x >= w->clip_x + w->clip_w || y >= w->clip_y + w->clip_h))
        return;
    if (x < 0 || y < 0 || x >= w->width || y >= w->height || alpha <= 0) return;
    alpha = alpha * w->alpha / 255;
    if (alpha <= 0) return;
    if (alpha >= 255) {
        w->backbuf[y * w->width + x] = 0xFF000000u | (rgb & 0x00FFFFFFu);
        return;
    }
    ag_u32 dst = w->backbuf[y * w->width + x];
    int r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    int dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
    r = (r * alpha + dr * (255 - alpha)) / 255;
    g = (g * alpha + dg * (255 - alpha)) / 255;
    b = (b * alpha + db * (255 - alpha)) / 255;
    w->backbuf[y * w->width + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
}

static inline void blend_argb(ag_window *w, int x, int y, ag_u32 rgb, int alpha)
{
    blend_px(w, x + w->ox, y + w->oy, rgb, alpha);
}

static void ag_clip_set(ag_window *w, int x, int y, int cw, int ch)
{
    if (!w) return;
    w->clip_on = 1;
    w->clip_x = x; w->clip_y = y;
    w->clip_w = cw; w->clip_h = ch;
}

static void ag_clip_clear(ag_window *w)
{
    if (!w) return;
    w->clip_on = 0;
}

static const ag_u8 ag_font8x8[96][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},{0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00},
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00},{0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00},
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00},{0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00},
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00},{0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00},
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00},{0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00},
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00},{0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06},{0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00},{0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00},
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00},{0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00},
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00},{0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00},
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00},{0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00},
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00},{0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00},
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00},{0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00},
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00},{0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06},
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00},{0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00},
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00},{0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00},
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00},{0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00},
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00},{0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00},
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00},{0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00},
    {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00},{0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00},
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00},{0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00},{0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00},
    {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00},{0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00},
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00},{0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00},
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00},{0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00},
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00},{0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00},
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},{0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00},
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00},{0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00},
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00},{0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00},
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00},{0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00},
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00},{0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00},
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00},{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF},
    {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00},
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00},{0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00},
    {0x38,0x30,0x30,0x3e,0x33,0x33,0x6E,0x00},{0x00,0x00,0x1E,0x33,0x3f,0x03,0x1E,0x00},
    {0x1C,0x36,0x06,0x0f,0x06,0x06,0x0F,0x00},{0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F},
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00},{0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00},
    {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E},{0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00},
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},{0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00},
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00},{0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00},
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F},{0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78},
    {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00},{0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00},
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00},{0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00},
    {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00},{0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00},
    {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00},{0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F},
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00},{0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00},
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00},{0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00},
    {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}
};

static void draw_char8x8(ag_window *w, int x, int y, ag_u32 cp_argb)
{
    if (cp_argb < 0x20) return;
    int scale = w->font_size < 8 ? 1 : w->font_size / 8;
    if (cp_argb > 0x7F) cp_argb = '?';
    const ag_u8 *glyph = ag_font8x8[cp_argb - 0x20];
    for (int row = 0; row < 8; ++row) {
        ag_u8 bits = glyph[row];
        for (int col = 0; col < 8; ++col) {
            if (!(bits & (0x01u << col))) continue;
            for (int dy = 0; dy < scale; ++dy)
                for (int dx = 0; dx < scale; ++dx)
                    put_pixel(w, x + col * scale + dx, y + row * scale + dy,
                              color_to_argb(w->current_color));
        }
    }
}

static void ag_draw_text8x8(ag_window *w, int x, int y, const char *text)
{
    if (!w || !text) return;
    int scale = w->font_size < 8 ? 1 : w->font_size / 8;
    int cx = x, cy = y;
    const char *p = text;
    while (*p) {
        ag_u32 cp = utf8_decode(&p);
        if (cp == '\n') { cy += scale * 8; cx = x; continue; }
        if (cp == '\r') continue;
        draw_char8x8(w, cx, cy, cp);
        cx += scale * 8;
    }
}

typedef enum {
    AGK_NONE = 0, AGK_BACKSPACE, AGK_DELETE, AGK_LEFT, AGK_RIGHT,
    AGK_HOME, AGK_END, AGK_RETURN, AGK_TAB, AGK_UP, AGK_DOWN, AGK_ESC
} ag_keyctl;

static int         ag_backend_keyctl(int raw);
static int         ag_font_char_width(const ag_font *f, ag_u32 cp);
static void        ag_draw_text_font(ag_window *w, int x, int y,
                                     ag_font *font, const char *text);

void ag_clear(ag_window *w, ag_color c)
{
    if (!w) return;
    ag_u32 argb = color_to_argb(c);
    int n = w->width * w->height;
    for (int i = 0; i < n; ++i) w->backbuf[i] = argb;
}

void ag_set_color(ag_window *w, ag_color c) { if (w) w->current_color = c; }
void ag_set_font_size(ag_window *w, int px) { if (w) w->font_size = px; }

void ag_draw_pixel(ag_window *w, int x, int y)
{
    if (w) put_pixel(w, x, y, color_to_argb(w->current_color));
}

void ag_draw_line(ag_window *w, int x0, int y0, int x1, int y1)
{
    if (!w) return;
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    ag_u32 c = color_to_argb(w->current_color);
    for (;;) {
        put_pixel(w, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void ag_draw_rect(ag_window *w, int x, int y, int ww, int hh)
{
    ag_draw_line(w, x, y, x + ww - 1, y);
    ag_draw_line(w, x, y + hh - 1, x + ww - 1, y + hh - 1);
    ag_draw_line(w, x, y, x, y + hh - 1);
    ag_draw_line(w, x + ww - 1, y, x + ww - 1, y + hh - 1);
}

void ag_fill_rect(ag_window *w, int x, int y, int ww, int hh)
{
    if (!w) return;
    ag_u32 c = color_to_argb(w->current_color);
    for (int j = 0; j < hh; ++j)
        for (int i = 0; i < ww; ++i)
            put_pixel(w, x + i, y + j, c);
}

void ag_fill_round_rect(ag_window *w, int x, int y, int ww, int hh, int rad)
{
    if (!w) return;
    if (rad <= 0) { ag_fill_rect(w, x, y, ww, hh); return; }
    if (rad * 2 > ww) rad = ww / 2;
    if (rad * 2 > hh) rad = hh / 2;
    if (rad < 1) rad = 1;
    ag_u32 c = color_to_argb(w->current_color);
    int x0 = x + rad, x1 = x + ww - rad - 1;
    int y0 = y + rad, y1 = y + hh - rad - 1;
    int rad2 = rad * rad;
    for (int py = y; py < y + hh; ++py) {
        for (int px = x; px < x + ww; ++px) {
            int cx = px < x0 ? x0 : (px > x1 ? x1 : px);
            int cy = py < y0 ? y0 : (py > y1 ? y1 : py);
            int dx = px - cx, dy = py - cy;
            if (dx * dx + dy * dy <= rad2) put_pixel(w, px, py, c);
        }
    }
}

static int in_round_rect(int px, int py, int x0, int x1, int y0, int y1, int rad2)
{
    int cx = px < x0 ? x0 : (px > x1 ? x1 : px);
    int cy = py < y0 ? y0 : (py > y1 ? y1 : py);
    int dx = px - cx, dy = py - cy;
    return dx * dx + dy * dy <= rad2;
}

void ag_draw_round_rect(ag_window *w, int x, int y, int ww, int hh, int rad)
{
    if (!w) return;
    if (rad <= 0) { ag_draw_rect(w, x, y, ww, hh); return; }
    if (rad * 2 > ww) rad = ww / 2;
    if (rad * 2 > hh) rad = hh / 2;
    if (rad < 1) rad = 1;
    ag_u32 c = color_to_argb(w->current_color);
    int x0 = x + rad, x1 = x + ww - rad - 1;
    int y0 = y + rad, y1 = y + hh - rad - 1;
    int rad2 = rad * rad;
    for (int py = y; py < y + hh; ++py)
        for (int px = x; px < x + ww; ++px)
            if (in_round_rect(px, py, x0, x1, y0, y1, rad2) &&
                !in_round_rect(px - 1, py, x0, x1, y0, y1, rad2) &&
                !in_round_rect(px + 1, py, x0, x1, y0, y1, rad2) &&
                !in_round_rect(px, py - 1, x0, x1, y0, y1, rad2) &&
                !in_round_rect(px, py + 1, x0, x1, y0, y1, rad2))
                put_pixel(w, px, py, c);
}

void ag_draw_text(ag_window *w, int x, int y, const char *text)
{
    if (!w || !text) return;
    ag_draw_text_font(w, x, y, w->font, text);
}

int ag_font_text_width(const ag_font *f, const char *text)
{
    if (!f || !text) return 0;
    int wsum = 0;
    const char *p = text;
    while (*p) {
        ag_u32 cp = utf8_decode(&p);
        if (!cp) break;
        wsum += ag_font_char_width(f, cp);
    }
    return wsum;
}

void ag_window_set_font(ag_window *w, ag_font *font)
{
    if (w) w->font = font;
}

void ag_window_redraw(ag_window *w)
{
    if (!w) return;
    w->redraw_pending = 1;
}

static int ag_backend_glyph(ag_font *f, ag_u32 cp, struct ag_glyph *g);

static const struct ag_glyph *ag_font_glyph(ag_font *f, ag_u32 cp)
{
    if (!cp) return NULL;
    struct ag_glyph *slot = &f->cache[ag_glyph_slot(cp)];
    if (slot->cp == cp && slot->bits) return slot;

    if (slot->bits) { free(slot->bits); memset(slot, 0, sizeof(*slot)); }
    slot->cp = cp;
    if (ag_backend_glyph(f, cp, slot) != 0) {
        slot->cp = 0;
        return NULL;
    }
    return slot;
}

static void draw_glyph_bits(ag_window *w, int penx, int baseline,
                            const struct ag_glyph *g, ag_u32 rgb)
{
    if (!g || !g->bits || g->w <= 0) return;
    for (int row = 0; row < g->h; ++row)
        for (int col = 0; col < g->w; ++col) {
            int a = g->bits[row * g->w + col];
            if (a) blend_argb(w, penx + g->xoff + col, baseline + g->yoff + row, rgb, a);
        }
}

ag_image *ag_image_create(int w, int h)
{
    if (w <= 0 || h <= 0) return NULL;
    ag_image *im = (ag_image *)calloc(1, sizeof(ag_image));
    if (!im) return NULL;
    im->w = w; im->h = h;
    im->pixels = (ag_u32 *)calloc((size_t)w * h, sizeof(ag_u32));
    if (!im->pixels) { free(im); return NULL; }
    return im;
}

ag_image *ag_image_clone(const ag_image *img)
{
    if (!img || !img->pixels) return NULL;
    ag_image *im = ag_image_create(img->w, img->h);
    if (!im) return NULL;
    memcpy(im->pixels, img->pixels, (size_t)img->w * img->h * sizeof(ag_u32));
    return im;
}

void ag_image_destroy(ag_image *img)
{
    if (!img) return;
    free(img->pixels);
    free(img);
}

int ag_image_get_size(const ag_image *img, int *w, int *h)
{
    if (!img) return -1;
    if (w) *w = img->w;
    if (h) *h = img->h;
    return 0;
}

ag_u32 ag_image_pixel(const ag_image *img, int x, int y)
{
    if (!img || !img->pixels) return 0;
    if (x < 0 || y < 0 || x >= img->w || y >= img->h) return 0;
    return img->pixels[y * img->w + x];
}

int ag_image_save_png(const ag_image *img, const char *path)
{
    if (!img || !img->pixels || !path) return 0;
    FILE *f = fopen(path, "wb");
    if (!f) return 0;

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) { fclose(f); return 0; }
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_write_struct(&png, NULL); fclose(f); return 0; }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        fclose(f);
        return 0;
    }

    png_init_io(png, f);
    png_set_IHDR(png, info, (png_uint_32)img->w, (png_uint_32)img->h, 8,
                 PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    png_bytep row = (png_bytep)malloc((size_t)img->w * 4);
    if (!row) { png_destroy_write_struct(&png, &info); fclose(f); return 0; }
    for (int y = 0; y < img->h; ++y) {
        for (int x = 0; x < img->w; ++x) {
            ag_u32 p = img->pixels[y * img->w + x];
            row[x * 4 + 0] = (png_byte)((p >> 16) & 0xFF);
            row[x * 4 + 1] = (png_byte)((p >> 8) & 0xFF);
            row[x * 4 + 2] = (png_byte)(p & 0xFF);
            row[x * 4 + 3] = (png_byte)((p >> 24) & 0xFF);
        }
        png_write_row(png, row);
    }
    free(row);
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    fclose(f);
    return 1;
}

static ag_image *ag_image_from_bmp(const unsigned char *d, size_t sz)
{
    if (sz < 54 || d[0] != 'B' || d[1] != 'M') return NULL;

    ag_u32 offbits = d[10] | (d[11] << 8) | (d[12] << 16) | ((ag_u32)d[13] << 24);
    ag_u32 biSize  = d[14] | (d[15] << 8) | (d[16] << 16) | ((ag_u32)d[17] << 24);

    if (biSize < 40) return NULL;              
    if (offbits + 4 > sz) return NULL;

    int w   = (int)(d[18] | (d[19] << 8) | (d[20] << 16) | ((ag_u32)d[21] << 24));
    int hraw= (int)(d[22] | (d[23] << 8) | (d[24] << 16) | ((ag_u32)d[25] << 24));
    int planes = d[26] | (d[27] << 8);
    int bpp = d[28] | (d[29] << 8);
    ag_u32 comp = d[30] | (d[31] << 8) | (d[32] << 16) | ((ag_u32)d[33] << 24);

    if (planes != 1 || (bpp != 24 && bpp != 32) || comp != 0) return NULL;
    if (w <= 0 || hraw == 0 || w > 16384 || (hraw > 16384 || hraw < -16384)) return NULL;

    int flip = hraw < 0;                        
    int h = hraw < 0 ? -hraw : hraw;

    ag_image *im = ag_image_create(w, h);
    if (!im) return NULL;

    int pitch = ((bpp * w + 31) / 32) * 4;
    int bppb = bpp / 8;

    for (int y = 0; y < h; ++y) {
        int sy = flip ? y : (h - 1 - y);
        size_t base = offbits + (size_t)sy * pitch;
        if (base + (size_t)w * bppb > sz) break;
        const unsigned char *row = d + base;
        for (int x = 0; x < w; ++x) {
            unsigned char b = row[x * bppb];
            unsigned char g = row[x * bppb + 1];
            unsigned char r = row[x * bppb + 2];
            im->pixels[y * w + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
    return im;
}

typedef struct { const unsigned char *p; size_t sz, pos; } ag_png_stream;

static void ag_png_read_fn(png_structp png, png_bytep out, png_size_t n)
{
    ag_png_stream *s = (ag_png_stream *)png_get_io_ptr(png);
    if (s->pos + n > s->sz) { png_error(png, "truncated png"); return; }
    memcpy(out, s->p + s->pos, n);
    s->pos += n;
}

static ag_image *ag_image_from_png(const unsigned char *d, size_t sz)
{
    ag_png_stream st = { d, sz, 0 };
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) return NULL;
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, NULL, NULL); return NULL; }

    ag_image *im = NULL;
    if (setjmp(png_jmpbuf(png))) {
        if (im) ag_image_destroy(im);
        png_destroy_read_struct(&png, &info, NULL);
        return NULL;
    }

    png_set_read_fn(png, &st, ag_png_read_fn);
    png_read_info(png, info);

    png_uint_32 w, h;
    int bit_depth, color_type;
    png_get_IHDR(png, info, &w, &h, &bit_depth, &color_type, NULL, NULL, NULL);

    if (w == 0 || h == 0 || w > 16384 || h > 16384) png_error(png, "bad size");

    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (bit_depth == 16) png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    if (!(color_type & PNG_COLOR_MASK_ALPHA))
        png_set_add_alpha(png, 0xFF, PNG_FILLER_AFTER);

    png_read_update_info(png, info);
    png_uint_32 rw, rh;
    png_get_IHDR(png, info, &rw, &rh, NULL, NULL, NULL, NULL, NULL);
    png_size_t rowb = png_get_rowbytes(png, info);

    im = ag_image_create((int)rw, (int)rh);
    if (!im) png_error(png, "oom");

    png_bytep *rows = (png_bytep *)malloc(sizeof(png_bytep) * rh);
    if (!rows) png_error(png, "oom");
    for (png_uint_32 y = 0; y < rh; ++y) {
        rows[y] = (png_bytep)malloc(rowb);
        if (!rows[y]) { for (png_uint_32 k = 0; k < y; ++k) free(rows[k]); free(rows); png_error(png, "oom"); }
    }
    png_read_image(png, rows);
    png_read_end(png, NULL);

    for (png_uint_32 y = 0; y < rh; ++y) {
        const png_bytep row = rows[y];
        for (png_uint_32 x = 0; x < rw; ++x) {
            png_byte r = row[x * 4], g = row[x * 4 + 1], b = row[x * 4 + 2];
            png_byte a = row[x * 4 + 3];
            im->pixels[y * rw + x] = ((ag_u32)a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    for (png_uint_32 y = 0; y < rh; ++y) free(rows[y]);
    free(rows);
    png_destroy_read_struct(&png, &info, NULL);
    return im;
}

typedef struct {
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
} ag_jpg_err;

static void ag_jpg_error_exit(j_common_ptr cinfo)
{
    ag_jpg_err *err = (ag_jpg_err *)cinfo->err;
    (*cinfo->err->output_message)(cinfo);
    longjmp(err->setjmp_buffer, 1);
}

ag_image *ag_jpeg_decode_mem(const unsigned char *d, size_t sz)
{
    if (sz < 2 || d[0] != 0xFF || d[1] != 0xD8) return NULL;

    struct jpeg_decompress_struct cinfo;
    ag_jpg_err jerr;
    ag_image *im = NULL;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = ag_jpg_error_exit;

    if (setjmp(jerr.setjmp_buffer)) {
        jpeg_destroy_decompress(&cinfo);
        ag_image_destroy(im);
        return NULL;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, d, sz);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    int w = (int)cinfo.output_width, h = (int)cinfo.output_height;
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) longjmp(jerr.setjmp_buffer, 1);

    im = ag_image_create(w, h);
    if (!im) longjmp(jerr.setjmp_buffer, 1);

    int row_stride = w * cinfo.output_components;
    JSAMPARRAY row = (*cinfo.mem->alloc_sarray)((j_common_ptr)&cinfo,
                                                JPOOL_IMAGE, row_stride, 1);

    while (cinfo.output_scanline < (JDIMENSION)h) {
        jpeg_read_scanlines(&cinfo, row, 1);
        const unsigned char *p = row[0];
        int y = (int)cinfo.output_scanline - 1;
        for (int x = 0; x < w; ++x) {
            unsigned char r = p[x * 3], g = p[x * 3 + 1], b = p[x * 3 + 2];
            im->pixels[y * w + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return im;
}


static int ag_is_xml_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

typedef void (*ag_attr_cb)(void *ud, const char *name, size_t nlen,
                           const char *value, size_t vlen);

static void ag_svg_iterate_attrs(const char *b, int bsz, int *pos,
                                 ag_attr_cb cb, void *ud)
{
    int i = *pos;
    for (;;) {
        while (i < bsz && ag_is_xml_ws(b[i])) ++i;
        if (i >= bsz) break;
        if (b[i] == '>') { ++i; break; }
        if (b[i] == '/') {              
            while (i < bsz && b[i] != '>') ++i;
            if (i < bsz) ++i;
            break;
        }
        const char *nm = b + i;
        while (i < bsz && b[i] != '=' && b[i] != '>' && b[i] != '/' && !ag_is_xml_ws(b[i])) ++i;
        size_t nlen = (size_t)(b + i - nm);
        while (i < bsz && ag_is_xml_ws(b[i])) ++i;
        if (i < bsz && b[i] == '=') {
            ++i;
            while (i < bsz && ag_is_xml_ws(b[i])) ++i;
            if (i < bsz && (b[i] == '"' || b[i] == '\'')) {
                char q = b[i];
                int vs = ++i;
                while (i < bsz && b[i] != q) ++i;
                if (cb) cb(ud, nm, nlen, b + vs, (size_t)(b + i - vs));
                if (i < bsz) ++i;
            } else {
                int vs = i;
                while (i < bsz && !ag_is_xml_ws(b[i]) && b[i] != '>') ++i;
                if (cb) cb(ud, nm, nlen, b + vs, (size_t)(b + i - vs));
            }
        }
    }
    *pos = i;
}

static size_t ag_xml_unescape(const char *in, size_t len, char *out)
{
    size_t o = 0, i = 0;
    while (i < len) {
        if (in[i] == '&' && i + 2 < len && in[i + 1] == '#') {
            size_t j = i + 2;
            unsigned v = 0;
            if (j < len && (in[j] == 'x' || in[j] == 'X')) {
                ++j;
                while (j < len) {
                    char c = in[j];
                    int d;
                    if (c >= '0' && c <= '9') d = c - '0';
                    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                    else break;
                    v = v * 16 + (unsigned)d;
                    ++j;
                }
            } else {
                while (j < len && in[j] >= '0' && in[j] <= '9') {
                    v = v * 10 + (unsigned)(in[j] - '0');
                    ++j;
                }
            }
            if (j < len && in[j] == ';') {
                if (v == 9 || v == 10 || v == 13) out[o++] = ' ';
                i = j + 1;
                continue;
            }
        }
        out[o++] = in[i];
        ++i;
    }
    return o;
}

static int ag_b64_val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static size_t ag_base64_decode(const char *s, size_t len, unsigned char *out)
{
    size_t o = 0;
    unsigned acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; ++i) {
        int v = ag_b64_val((unsigned char)s[i]);
        if (v < 0) continue;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (unsigned char)((acc >> bits) & 0xFF);
        }
    }
    return o;
}

static int ag_parse_int(const char *s, size_t len, int *out)
{
    size_t i = 0;
    while (i < len && ag_is_xml_ws(s[i])) ++i;
    long long v = 0;
    int neg = 0;
    if (i < len && s[i] == '-') { neg = 1; ++i; }
    int any = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); ++i; any = 1; }
    if (!any) return -1;
    if (out) *out = (int)(neg ? -v : v);
    return 0;
}

struct ag_svg_image_ent { int x, y, w, h; ag_image *raster; };

static void ag_svg_scale_nearest(ag_image *dst, int dx, int dy, int dw, int dh,
                                 const ag_image *src)
{
    if (!dst || !src || dw <= 0 || dh <= 0) return;
    for (int j = 0; j < dh; ++j) {
        int sy = src->h > 1 ? j * (src->h - 1) / (dh - 1) : 0;
        for (int i = 0; i < dw; ++i) {
            int sx = src->w > 1 ? i * (src->w - 1) / (dw - 1) : 0;
            ag_u32 px = src->pixels[sy * src->w + sx];
            int a = (int)((px >> 24) & 0xFF);
            if (a <= 0) continue;
            int X = dx + i, Y = dy + j;
            if (X < 0 || Y < 0 || X >= dst->w || Y >= dst->h) continue;
            ag_u32 *dp = &dst->pixels[Y * dst->w + X];
            if (a >= 255) { *dp = px; continue; }
            int pr = (px >> 16) & 0xFF, pg = (px >> 8) & 0xFF, pb = px & 0xFF;
            int dr = (*dp >> 16) & 0xFF, dg = (*dp >> 8) & 0xFF, db = *dp & 0xFF;
            pr = (pr * a + dr * (255 - a)) / 255;
            pg = (pg * a + dg * (255 - a)) / 255;
            pb = (pb * a + db * (255 - a)) / 255;
            *dp = 0xFF000000u | ((ag_u32)pr << 16) | ((ag_u32)pg << 8) | pb;
        }
    }
}

struct ag_svg_collect { struct ag_svg_image_ent ents[16]; int n; };

static void ag_svg_collect_attr(void *ud, const char *name, size_t nlen,
                                const char *value, size_t vlen)
{
    struct ag_svg_collect *c = (struct ag_svg_collect *)ud;
    struct ag_svg_image_ent *e = &c->ents[0];
    static const char data_pfx[] = "data:image/";
    size_t data_pfx_len = sizeof(data_pfx) - 1;
    if (vlen > data_pfx_len && strncmp(value, data_pfx, data_pfx_len) == 0) {
        const char *base = NULL;
        for (size_t i = data_pfx_len; i + 8 <= vlen; ++i)
            if (strncmp(value + i, ";base64,", 8) == 0) { base = value + i + 8; break; }
        if (!base) return;
        size_t bl = (size_t)(value + vlen - base);
        char fmt = (vlen > 15) ? value[12] : 0;   
        char *clean = (char *)malloc(bl + 1);
        if (!clean) return;
        size_t cl = ag_xml_unescape(base, bl, clean);
        clean[cl] = '\0';
        size_t cap = (cl * 3 / 4) + 8;
        unsigned char *raw = (unsigned char *)malloc(cap);
        if (raw) {
            size_t rl = ag_base64_decode(clean, cl, raw);
            ag_image *im = NULL;
            if (fmt == 'p')      im = ag_image_from_png(raw, rl);
            else if (fmt == 'j') im = ag_jpeg_decode_mem(raw, rl);
            if (im) {
                if (e->raster) ag_image_destroy(e->raster);
                e->raster = im;
                if (e->w <= 0) e->w = im->w;
                if (e->h <= 0) e->h = im->h;
                c->n = 1;
            }
            free(raw);
        }
        free(clean);
    } else if ((nlen == 1 && (*name == 'x' || *name == 'y')) ||
               (nlen == 5 && strncmp(name, "width", 5) == 0) ||
               (nlen == 6 && strncmp(name, "height", 6) == 0)) {
        int v = 0;
        if (ag_parse_int(value, vlen, &v) == 0) {
            if (*name == 'x') e->x = v;
            else if (*name == 'y') e->y = v;
            else if (nlen == 5) e->w = v > 0 ? v : 0;
            else e->h = v > 0 ? v : 0;
        }
    }
}

struct ag_svg_root { int cw, ch; int vminx, vminy, vw, vh; };

static void ag_svg_root_attr(void *ud, const char *name, size_t nlen,
                             const char *value, size_t vlen)
{
    struct ag_svg_root *r = (struct ag_svg_root *)ud;
    if (nlen == 5 && strncmp(name, "width", 5) == 0)  ag_parse_int(value, vlen, &r->cw);
    if (nlen == 6 && strncmp(name, "height", 6) == 0) ag_parse_int(value, vlen, &r->ch);
    if (nlen == 7 && strncmp(name, "viewBox", 7) == 0) {
        int n[4] = {0,0,0,0};
        int idx = 0, i = 0;
        while (i < (int)vlen && idx < 4) {
            while (i < (int)vlen && ag_is_xml_ws(value[i])) ++i;
            int st = i;
            while (i < (int)vlen && (value[i] == '-' || (value[i] >= '0' && value[i] <= '9'))) ++i;
            if (i > st) ag_parse_int(value + st, (size_t)(i - st), &n[idx++]);
        }
        r->vminx = n[0]; r->vminy = n[1]; r->vw = n[2]; r->vh = n[3];
    }
}

static ag_image *ag_svg_embedded(const char *b, int bsz, float scale)
{
    struct ag_svg_root root = {0, 0, 0, 0, 0, 0};
    struct ag_svg_collect col; memset(&col, 0, sizeof(col));

    for (int i = 0; i < bsz - 6; ++i) {
        if (b[i] == '<' && i + 4 < bsz &&
            b[i+1] == 's' && b[i+2] == 'v' && b[i+3] == 'g' &&
            (i+4 >= bsz || ag_is_xml_ws(b[i+4]) || b[i+4] == '>' || b[i+4] == '/')) {
            int pos = i + 4;
            ag_svg_iterate_attrs(b, bsz, &pos, ag_svg_root_attr, &root);
            break;
        }
        if (strncmp(b + i, "<image", 6) == 0 &&
            (i + 6 >= bsz || ag_is_xml_ws(b[i + 6]) || b[i + 6] == '>' || b[i + 6] == '/')) {
            int pos = i + 6;
            struct ag_svg_collect tmp; memset(&tmp, 0, sizeof(tmp));
            ag_svg_iterate_attrs(b, bsz, &pos, ag_svg_collect_attr, &tmp);
            if (tmp.n > 0 && col.n < 16) {
                col.ents[col.n++] = tmp.ents[0];
            }
            i = pos - 1;
        }
    }

    if (col.n == 0) return NULL;

    int cw = root.cw, ch = root.ch;
    if (cw <= 0) cw = root.vw;
    if (ch <= 0) ch = root.vh;
    if (cw <= 0 || ch <= 0) {
        for (int k = 0; k < col.n; ++k) {
            int r = col.ents[k].x + col.ents[k].w;
            int b = col.ents[k].y + col.ents[k].h;
            if (r > cw) cw = r;
            if (b > ch) ch = b;
        }
    }
    float sx = (root.vw > 0) ? (float)cw / root.vw : 1.0f;
    float sy = (root.vh > 0) ? (float)ch / root.vh : 1.0f;

    int ow = (int)(cw * scale + 0.5f);
    int oh = (int)(ch * scale + 0.5f);
    if (ow <= 0) ow = 1;
    if (oh <= 0) oh = 1;
    if (ow > 16384 || oh > 16384) {
        for (int k = 0; k < col.n; ++k) ag_image_destroy(col.ents[k].raster);
        return NULL;
    }
    ag_image *out = ag_image_create(ow, oh);
    if (!out) {
        for (int k = 0; k < col.n; ++k) ag_image_destroy(col.ents[k].raster);
        return NULL;
    }

    for (int k = 0; k < col.n; ++k) {
        const struct ag_svg_image_ent *e = &col.ents[k];
        float dx = (float)(e->x - root.vminx) * sx * scale;
        float dy = (float)(e->y - root.vminy) * sy * scale;
        float dw = (float)e->w * sx * scale;
        float dh = (float)e->h * sy * scale;
        ag_svg_scale_nearest(out, (int)(dx + 0.5f), (int)(dy + 0.5f),
                             (int)(dw + 0.5f), (int)(dh + 0.5f), e->raster);
        ag_image_destroy(e->raster);
    }
    return out;
}

static ag_image *ag_image_from_svg_mem(const unsigned char *d, size_t sz, float scale)
{
    if (!d || sz == 0) return NULL;
    if ((int)sz > 64 * 1024 * 1024) return NULL;

    char *buf = (char *)malloc(sz + 1);
    if (!buf) return NULL;
    memcpy(buf, d, sz);
    buf[sz] = '\0';

    ag_image *embedded = ag_svg_embedded(buf, (int)sz, scale);

    NSVGimage *svg = nsvgParse(buf, "px", 96.0f);
    free(buf);
    if (!svg || svg->width <= 0.0f || svg->height <= 0.0f) {
        nsvgDelete(svg);
        return embedded;   
    }

    int w = (int)(svg->width * scale + 0.5f);
    int h = (int)(svg->height * scale + 0.5f);
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
    if (w > 16384 || h > 16384) {
        nsvgDelete(svg);
        if (embedded) { ag_image_destroy(embedded); embedded = NULL; }
        return NULL;
    }

    if (!embedded) {
        ag_image *im = ag_image_create(w, h);
        if (!im) { nsvgDelete(svg); return NULL; }
        unsigned char *rgba = (unsigned char *)malloc((size_t)w * h * 4);
        if (!rgba) { ag_image_destroy(im); nsvgDelete(svg); return NULL; }
        memset(rgba, 0, (size_t)w * h * 4);
        NSVGrasterizer *rast = nsvgCreateRasterizer();
        nsvgRasterize(rast, svg, 0.0f, 0.0f, scale, rgba, w, h, w * 4);
        nsvgDeleteRasterizer(rast);
        nsvgDelete(svg);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const unsigned char *px = rgba + (y * w + x) * 4;
                im->pixels[y * w + x] =
                    ((ag_u32)px[3] << 24) | (px[0] << 16) | (px[1] << 8) | px[2];
            }
        free(rgba);
        return im;
    }

    unsigned char *rgba = (unsigned char *)malloc((size_t)w * h * 4);
    if (!rgba) { ag_image_destroy(embedded); nsvgDelete(svg); return NULL; }
    memset(rgba, 0, (size_t)w * h * 4);
    NSVGrasterizer *rast = nsvgCreateRasterizer();
    nsvgRasterize(rast, svg, 0.0f, 0.0f, scale, rgba, w, h, w * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(svg);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const unsigned char *px = rgba + (y * w + x) * 4;
            ag_u32 v = ((ag_u32)px[3] << 24) | (px[0] << 16) | (px[1] << 8) | px[2];
            if (px[3]) embedded->pixels[y * w + x] = v;
        }
    free(rgba);
    return embedded;
}

ag_image *ag_image_load_svg(const char *path, float scale)
{
    if (!path) return NULL;
    if (scale < 0.0f) scale = 1.0f;
    if (scale > 16.0f) scale = 16.0f;

    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    size_t cap = 1 << 20, sz = 0;
    unsigned char *buf = (unsigned char *)malloc(cap);
    if (!buf) { fclose(fp); return NULL; }
    for (;;) {
        size_t got = fread(buf + sz, 1, cap - sz, fp);
        sz += got;
        if (got < cap - sz) break;
        cap *= 2;
        unsigned char *nb = (unsigned char *)realloc(buf, cap);
        if (!nb) { free(buf); fclose(fp); return NULL; }
        buf = nb;
    }
    fclose(fp);
    ag_image *im = ag_image_from_svg_mem(buf, sz, scale);
    free(buf);
    return im;
}

ag_image *ag_image_load(const char *path)
{
    if (!path) return NULL;
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    size_t cap = 1 << 20, sz = 0;
    unsigned char *buf = (unsigned char *)malloc(cap);
    if (!buf) { fclose(fp); return NULL; }
    for (;;) {
        size_t got = fread(buf + sz, 1, cap - sz, fp);
        sz += got;
        if (got < cap - sz) break;
        cap *= 2;
        unsigned char *nb = (unsigned char *)realloc(buf, cap);
        if (!nb) { free(buf); fclose(fp); return NULL; }
        buf = nb;
    }
    fclose(fp);

    ag_image *im = NULL;
    if (sz >= 8 && buf[0] == 0x89 && buf[1] == 'P' && buf[2] == 'N' && buf[3] == 'G')
        im = ag_image_from_png(buf, sz);
    else if (sz >= 2 && buf[0] == 0xFF && buf[1] == 0xD8)
        im = ag_jpeg_decode_mem(buf, sz);
    else if (sz > 0 && buf[0] == '<')
        im = ag_image_from_svg_mem(buf, sz, 1.0f);
    else
        im = ag_image_from_bmp(buf, sz);
    free(buf);
    return im;
}

static inline void image_blend_px(ag_window *w, int x, int y, ag_u32 argb)
{
    int a = (argb >> 24) & 0xFF;
    if (a == 0) return;
    if (a == 255) { put_pixel(w, x, y, argb); return; }
    ag_u32 rgb = argb & 0x00FFFFFFu;
    blend_argb(w, x, y, rgb, a);
}

void ag_draw_image(ag_window *w, int x, int y, int wc, int hc, const ag_image *img)
{
    if (!w || !img || !img->pixels) return;
    if (wc <= 0) wc = img->w;
    if (hc <= 0) hc = img->h;
    if (wc == 0 || hc == 0) return;

    if (wc == img->w && hc == img->h) {
        int opaque = 1;
        for (int i = 0; i < wc * hc && opaque; ++i)
            if ((img->pixels[i] >> 24) != 0xFF) opaque = 0;
        if (opaque) {
            for (int j = 0; j < hc; ++j) {
                int dy = y + j;
                if (dy < 0 || dy >= w->height) continue;
                int dx0 = x < 0 ? -x : 0;
                int dx1 = x + wc > w->width ? w->width - x : wc;
                if (dx0 >= dx1) continue;
                if (w->clip_on) {
                    if (dx0 + x < w->clip_x) dx0 = w->clip_x - x;
                    if (dx1 + x > w->clip_x + w->clip_w) dx1 = w->clip_x + w->clip_w - x;
                    if (dx0 >= dx1) continue;
                }
                memcpy(w->backbuf + (size_t)dy * w->width + x + dx0,
                       img->pixels + (size_t)j * img->w + dx0,
                       (size_t)(dx1 - dx0) * sizeof(ag_u32));
            }
            return;
        }
        for (int j = 0; j < hc; ++j)
            for (int i = 0; i < wc; ++i)
                image_blend_px(w, x + i, y + j, img->pixels[j * img->w + i]);
        return;
    }

    for (int j = 0; j < hc; ++j) {
        for (int i = 0; i < wc; ++i) {
            float u = img->w > 1 ? (float)i * (img->w - 1) / (wc - 1) : 0.0f;
            float v = img->h > 1 ? (float)j * (img->h - 1) / (hc - 1) : 0.0f;
            int x0 = (int)u, y0 = (int)v;
            int x1 = x0 + 1 < img->w ? x0 + 1 : x0;
            int y1 = y0 + 1 < img->h ? y0 + 1 : y0;
            float fx = u - x0, fy = v - y0;

            ag_u32 p00 = img->pixels[y0 * img->w + x0];
            ag_u32 p10 = img->pixels[y0 * img->w + x1];
            ag_u32 p01 = img->pixels[y1 * img->w + x0];
            ag_u32 p11 = img->pixels[y1 * img->w + x1];

            ag_u32 out = 0;
            for (int ch = 0; ch < 4; ++ch) {
                int shift = 24 - ch * 8;
                int c = (int)(((p00 >> shift) & 0xFF) * (1 - fx) * (1 - fy) +
                              ((p10 >> shift) & 0xFF) * fx * (1 - fy) +
                              ((p01 >> shift) & 0xFF) * (1 - fx) * fy +
                              ((p11 >> shift) & 0xFF) * fx * fy);
                out |= (ag_u32)(c & 0xFF) << shift;
            }
            image_blend_px(w, x + i, y + j, out);
        }
    }
}


static inline ag_u32 ag_pm(ag_u32 p)
{
    unsigned a = p >> 24;
    if (a == 255) return p;
    unsigned r = ((p >> 16) & 0xFF) * a / 255u;
    unsigned g = ((p >> 8)  & 0xFF) * a / 255u;
    unsigned b = ( p        & 0xFF) * a / 255u;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static inline ag_u32 ag_unpm(ag_u32 p)
{
    unsigned a = p >> 24;
    if (a == 0) return 0;
    if (a == 255) return p;
    unsigned r = ((p >> 16) & 0xFF) * 255u / a;
    unsigned g = ((p >> 8)  & 0xFF) * 255u / a;
    unsigned b = ( p        & 0xFF) * 255u / a;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static inline int ag_clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void kawase_down_rows(const ag_u32 *src, int sw, int sh,
                             ag_u32 *dst, int dw, int dh, int off,
                             int ry0, int ry1)
{
    (void)dh;
    for (int y = ry0; y < ry1; ++y) {
        int cy = y * 2;
        int y0 = ag_clampi(cy - off, 0, sh - 1);
        int y1 = ag_clampi(cy + off, 0, sh - 1);
        const ag_u32 *rowc = src + (size_t)cy * sw;
        const ag_u32 *row0 = src + (size_t)y0 * sw;
        const ag_u32 *row1 = src + (size_t)y1 * sw;
        ag_u32 *drow = dst + (size_t)y * dw;
        for (int x = 0; x < dw; ++x) {
            int cx = x * 2;
            int x0 = ag_clampi(cx - off, 0, sw - 1);
            int x1 = ag_clampi(cx + off, 0, sw - 1);
            const ag_u32 *c = &rowc[cx];
            const ag_u32 *a = &row0[x0];
            const ag_u32 *b = &row0[x1];
            const ag_u32 *d = &row1[x0];
            const ag_u32 *e = &row1[x1];
            ag_u32 out = 0;
            for (int ch = 0; ch < 4; ++ch) {
                int shift = 24 - ch * 8;
                int sum = (int)((c[0] >> shift) & 0xFF) * 4 +
                          (int)((a[0] >> shift) & 0xFF) +
                          (int)((b[0] >> shift) & 0xFF) +
                          (int)((d[0] >> shift) & 0xFF) +
                          (int)((e[0] >> shift) & 0xFF);
                out |= (ag_u32)((sum / 8) & 0xFF) << shift;
            }
            drow[x] = out;
        }
    }
}

static inline ag_u32 kawase_sample(const ag_u32 *src, int sw, int sh, int fx, int fy)
{
    int x0 = fx >> 16, y0 = fy >> 16;
    int tx = fx & 0xFFFF, ty = fy & 0xFFFF;
    if (x0 < 0) { x0 = 0; tx = 0; }
    if (y0 < 0) { y0 = 0; ty = 0; }
    if (x0 > sw - 1) { x0 = sw - 1; tx = 0; }
    if (y0 > sh - 1) { y0 = sh - 1; ty = 0; }
    int x1 = x0 + 1 < sw ? x0 + 1 : x0;
    int y1 = y0 + 1 < sh ? y0 + 1 : y0;

    const ag_u32 *p00 = &src[(size_t)y0 * sw + x0];
    const ag_u32 *p10 = &src[(size_t)y0 * sw + x1];
    const ag_u32 *p01 = &src[(size_t)y1 * sw + x0];
    const ag_u32 *p11 = &src[(size_t)y1 * sw + x1];

    int h0[4], h1[4];
    for (int c = 0; c < 4; ++c) {
        int sh2 = 24 - c * 8;
        int a = (int)((p00[0] >> sh2) & 0xFF);
        int b = (int)((p10[0] >> sh2) & 0xFF);
        h0[c] = a + (((b - a) * tx) >> 16);
        a = (int)((p01[0] >> sh2) & 0xFF);
        b = (int)((p11[0] >> sh2) & 0xFF);
        h1[c] = a + (((b - a) * tx) >> 16);
    }
    ag_u32 out = 0;
    for (int c = 0; c < 4; ++c) {
        int v = h0[c] + (((h1[c] - h0[c]) * ty) >> 16);
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        out |= (ag_u32)v << (24 - c * 8);
    }
    return out;
}

static void kawase_up_rows(const ag_u32 *src, int sw, int sh,
                           ag_u32 *dst, int dw, int dh, int off,
                           int ry0, int ry1)
{
    static const int tap_dx[8] = { -2, -1,  0,  1,  2,  1,  0, -1 };
    static const int tap_dy[8] = {  0, -1, -2, -1,  0,  1,  2,  1 };
    static const int tap_w[8]  = {  1,  2,  1,  2,  1,  2,  1,  2 };

    for (int y = ry0; y < ry1; ++y) {
        int sy = (int)((((float)y + 0.5f) * (float)sh / (float)dh - 0.5f) * 65536.0f);
        ag_u32 *drow = dst + (size_t)y * dw;
        for (int x = 0; x < dw; ++x) {
            int sx = (int)((((float)x + 0.5f) * (float)sw / (float)dw - 0.5f) * 65536.0f);
            int r = 0, g = 0, b = 0, a = 0;
            for (int t = 0; t < 8; ++t) {
                ag_u32 s = kawase_sample(src, sw, sh,
                                         sx + tap_dx[t] * off * 65536,
                                         sy + tap_dy[t] * off * 65536);
                int w = tap_w[t];
                r += (int)((s >> 16) & 0xFF) * w;
                g += (int)((s >> 8) & 0xFF) * w;
                b += (int)(s & 0xFF) * w;
                a += (int)(s >> 24) * w;
            }
            ag_u32 out = 0;
            out |= (ag_u32)ag_clampi(a / 12, 0, 255) << 24;
            out |= (ag_u32)ag_clampi(r / 12, 0, 255) << 16;
            out |= (ag_u32)ag_clampi(g / 12, 0, 255) << 8;
            out |= (ag_u32)ag_clampi(b / 12, 0, 255);
            drow[x] = out;
        }
    }
}


typedef struct {
    const ag_u32 *src;
    int   sw, sh;
    ag_u32 *dst;
    int   dw, dh, off;
    int   rows;
} kawase_job;

static void kawase_down_work(void *ud, int index, int count)
{
    kawase_job *j = (kawase_job *)ud;
    int rows = (j->dh + count - 1) / count;
    int y0 = index * rows, y1 = y0 + rows;
    if (y1 > j->dh) y1 = j->dh;
    kawase_down_rows(j->src, j->sw, j->sh, j->dst, j->dw, j->dh, j->off, y0, y1);
}

static void kawase_up_work(void *ud, int index, int count)
{
    kawase_job *j = (kawase_job *)ud;
    int rows = (j->dh + count - 1) / count;
    int y0 = index * rows, y1 = y0 + rows;
    if (y1 > j->dh) y1 = j->dh;
    kawase_up_rows(j->src, j->sw, j->sh, j->dst, j->dw, j->dh, j->off, y0, y1);
}

typedef struct {
    ag_u32 *px;
    size_t  n;
    int     mul;
} ag_alpha_job;

static void ag_alpha_work(void *ud, int index, int count)
{
    ag_alpha_job *j = (ag_alpha_job *)ud;
    size_t chunk = (j->n + (size_t)count - 1) / (size_t)count;
    size_t first = (size_t)index * chunk;
    size_t last = first + chunk;
    if (last > j->n) last = j->n;
    if (j->mul) {
        for (size_t i = first; i < last; ++i) j->px[i] = ag_pm(j->px[i]);
    } else {
        for (size_t i = first; i < last; ++i) j->px[i] = ag_unpm(j->px[i]);
    }
}

int ag_image_blur_dual_kawase(ag_image *img, int radius, int passes)
{
    if (!img || !img->pixels || img->w < 2 || img->h < 2) return 0;
    if (radius < 1) radius = 1;
    if (radius > 64) radius = 64;
    if (passes < 1) passes = 1;
    if (passes > 4) passes = 4;

    int levels = (radius + 1) / 2;
    if (levels < 1) levels = 1;
    if (levels > 8) levels = 8;

    size_t n = (size_t)img->w * img->h;
    ag_u32 *work = (ag_u32 *)malloc(n * sizeof(ag_u32));
    if (!work) return 0;
    memcpy(work, img->pixels, n * sizeof(ag_u32));

    ag_alpha_job aj;
    aj.px = work;
    aj.n = n;
    aj.mul = 1;
    ag_parallel_for((int)((n + 63) / 64), ag_alpha_work, &aj);

    int lw[9], lh[9];
    lw[0] = img->w; lh[0] = img->h;
    for (int i = 1; i <= levels; ++i) {
        lw[i] = (lw[i - 1] + 1) / 2;
        lh[i] = (lh[i - 1] + 1) / 2;
    }

    kawase_job job;
    for (int p = 0; p < passes; ++p) {
        ag_u32 *cur = work;
        int cw = lw[0], ch = lh[0], owned = 0, built = 0;

        for (int i = 1; i <= levels; ++i) {
            ag_u32 *dn = (ag_u32 *)malloc((size_t)lw[i] * lh[i] * sizeof(ag_u32));
            if (!dn) break;
            job.src = cur; job.sw = cw; job.sh = ch;
            job.dst = dn; job.dw = lw[i]; job.dh = lh[i]; job.off = 1;
            ag_parallel_for(lh[i], kawase_down_work, &job);
            if (owned) free(cur);
            cur = dn; cw = lw[i]; ch = lh[i]; owned = 1; built = i;
        }
        for (int i = built; i >= 1; --i) {
            ag_u32 *up = (ag_u32 *)malloc((size_t)lw[i - 1] * lh[i - 1] * sizeof(ag_u32));
            if (!up) break;
            job.src = cur; job.sw = cw; job.sh = ch;
            job.dst = up; job.dw = lw[i - 1]; job.dh = lh[i - 1]; job.off = 1;
            ag_parallel_for(lh[i - 1], kawase_up_work, &job);
            if (owned) free(cur);
            cur = up; cw = lw[i - 1]; ch = lh[i - 1]; owned = 1;
        }
        if (owned) {
            memcpy(work, cur, n * sizeof(ag_u32));
            free(cur);
        }
    }

    aj.mul = 0;
    ag_parallel_for((int)((n + 63) / 64), ag_alpha_work, &aj);

    for (size_t i = 0; i < n; ++i) img->pixels[i] = work[i];
    free(work);
    return 1;
}


static void ag_box_h(ag_u32 *px, int w, int h, int r)
{
    if (r < 1) return;
    int win = r * 2 + 1;
    int inv = (65536 + win / 2) / win;
    for (int y = 0; y < h; ++y) {
        ag_u32 *row = px + (size_t)y * w;
        long a[4] = { 0, 0, 0, 0 };
        int first = 0;
        for (int i = -r; i <= r; ++i) {
            int j = i < 0 ? 0 : (i >= w ? w - 1 : i);
            ag_u32 p = row[j];
            a[0] += (p >> 16) & 0xFF;
            a[1] += (p >> 8) & 0xFF;
            a[2] += p & 0xFF;
            a[3] += (p >> 24) & 0xFF;
        }
        for (int x = 0; x < w; ++x) {
            row[x] = (ag_u32)(((a[3] * inv) >> 16) << 24) |
                     (ag_u32)(((a[0] * inv) >> 16) << 16) |
                     (ag_u32)(((a[1] * inv) >> 16) << 8) |
                     (ag_u32)((a[2] * inv) >> 16);
            int out = first - r < 0 ? 0 : (first - r >= w ? w - 1 : first - r);
            int in  = first + r + 1 >= w ? w - 1 : first + r + 1;
            ag_u32 po = row[out], pi = row[in];
            a[0] += (int)((pi >> 16) & 0xFF) - (int)((po >> 16) & 0xFF);
            a[1] += (int)((pi >> 8) & 0xFF) - (int)((po >> 8) & 0xFF);
            a[2] += (int)(pi & 0xFF) - (int)(po & 0xFF);
            a[3] += (int)((pi >> 24) & 0xFF) - (int)((po >> 24) & 0xFF);
            ++first;
        }
    }
}

static void ag_box_v(ag_u32 *px, int w, int h, int r)
{
    if (r < 1) return;
    int win = r * 2 + 1;
    int inv = (65536 + win / 2) / win;
    for (int x = 0; x < w; ++x) {
        long a[4] = { 0, 0, 0, 0 };
        int first = 0;
        for (int i = -r; i <= r; ++i) {
            int j = i < 0 ? 0 : (i >= h ? h - 1 : i);
            ag_u32 p = px[(size_t)j * w + x];
            a[0] += (p >> 16) & 0xFF;
            a[1] += (p >> 8) & 0xFF;
            a[2] += p & 0xFF;
            a[3] += (p >> 24) & 0xFF;
        }
        for (int y = 0; y < h; ++y) {
            ag_u32 *dst = px + (size_t)y * w + x;
            *dst = (ag_u32)(((a[3] * inv) >> 16) << 24) |
                  (ag_u32)(((a[0] * inv) >> 16) << 16) |
                  (ag_u32)(((a[1] * inv) >> 16) << 8) |
                  (ag_u32)((a[2] * inv) >> 16);
            int out = first - r < 0 ? 0 : (first - r >= h ? h - 1 : first - r);
            int in  = first + r + 1 >= h ? h - 1 : first + r + 1;
            ag_u32 po = px[(size_t)out * w + x], pi = px[(size_t)in * w + x];
            a[0] += (int)((pi >> 16) & 0xFF) - (int)((po >> 16) & 0xFF);
            a[1] += (int)((pi >> 8) & 0xFF) - (int)((po >> 8) & 0xFF);
            a[2] += (int)(pi & 0xFF) - (int)(po & 0xFF);
            a[3] += (int)((pi >> 24) & 0xFF) - (int)((po >> 24) & 0xFF);
            ++first;
        }
    }
}

int ag_image_blur_box(ag_image *img, int radius, int passes)
{
    if (!img || !img->pixels) return 0;
    if (radius < 1) radius = 1;
    if (radius > 128) radius = 128;
    if (passes < 1) passes = 1;
    if (passes > 4) passes = 4;

    ag_u32 *tmp = (ag_u32 *)malloc((size_t)img->w * img->h * sizeof(ag_u32));
    if (!tmp) return 0;
    for (int p = 0; p < passes; ++p) {
        memcpy(tmp, img->pixels, (size_t)img->w * img->h * sizeof(ag_u32));
        ag_box_h(tmp, img->w, img->h, radius);
        ag_box_v(tmp, img->w, img->h, radius);
        memcpy(img->pixels, tmp, (size_t)img->w * img->h * sizeof(ag_u32));
    }
    free(tmp);
    return 1;
}

static void ag_image_scale_row(ag_image *dst, const ag_image *src, int j)
{
    float v = (src->h > 1 && dst->h > 1)
                  ? (float)j * (float)(src->h - 1) / (float)(dst->h - 1)
                  : 0.0f;
    int y0 = ag_clampi((int)v, 0, src->h - 1);
    int y1 = ag_clampi(y0 + 1, 0, src->h - 1);
    float fy = v - (float)y0;
    ag_u32 *drow = dst->pixels + (size_t)j * dst->w;
    for (int i = 0; i < dst->w; ++i) {
        float u = (src->w > 1 && dst->w > 1)
                      ? (float)i * (float)(src->w - 1) / (float)(dst->w - 1)
                      : 0.0f;
        int x0 = ag_clampi((int)u, 0, src->w - 1);
        int x1 = ag_clampi(x0 + 1, 0, src->w - 1);
        float fx = u - (float)x0;
        ag_u32 p00 = src->pixels[(size_t)y0 * src->w + x0];
        ag_u32 p10 = src->pixels[(size_t)y0 * src->w + x1];
        ag_u32 p01 = src->pixels[(size_t)y1 * src->w + x0];
        ag_u32 p11 = src->pixels[(size_t)y1 * src->w + x1];
        ag_u32 out = 0;
        for (int ch = 0; ch < 4; ++ch) {
            int shift = 24 - ch * 8;
            int c = (int)(((p00 >> shift) & 0xFF) * (1 - fx) * (1 - fy) +
                          ((p10 >> shift) & 0xFF) * fx * (1 - fy) +
                          ((p01 >> shift) & 0xFF) * (1 - fx) * fy +
                          ((p11 >> shift) & 0xFF) * fx * fy);
            out |= (ag_u32)ag_clampi(c, 0, 255) << shift;
        }
        drow[i] = out;
    }
}

typedef struct {
    ag_image *dst;
    const ag_image *src;
} ag_scale_job;

static void ag_scale_work(void *ud, int index, int count)
{
    ag_scale_job *j = (ag_scale_job *)ud;
    int rows = (j->dst->h + count - 1) / count;
    int y0 = index * rows, y1 = y0 + rows;
    if (y1 > j->dst->h) y1 = j->dst->h;
    for (int y = y0; y < y1; ++y)
        ag_image_scale_row(j->dst, j->src, y);
}

static void ag_image_scale_into(ag_image *dst, const ag_image *src)
{
    ag_scale_job job;
    job.dst = dst;
    job.src = src;
    ag_parallel_for(dst->h, ag_scale_work, &job);
}

void ag_draw_image_blur(ag_window *w, int x, int y, int wc, int hc,
                        const ag_image *img, int radius, int passes)
{
    if (!w || !img || !img->pixels) return;
    if (wc <= 0) wc = img->w;
    if (hc <= 0) hc = img->h;
    if (wc <= 0 || hc <= 0) return;
    if (radius <= 0) { ag_draw_image(w, x, y, wc, hc, img); return; }

    ag_image *tmp = ag_image_create(wc, hc);
    if (!tmp) { ag_draw_image(w, x, y, wc, hc, img); return; }
    ag_image_scale_into(tmp, img);
    ag_image_blur_dual_kawase(tmp, radius, passes);
    ag_draw_image(w, x, y, wc, hc, tmp);
    ag_image_destroy(tmp);
}

static const ag_ui_theme ag_default_theme = {
    AG_RGB(36,36,40),
    AG_RGB(46,46,52),
    AG_RGB(92,92,102),
    AG_RGB(86,156,214),
    AG_RGB(60,120,170),
    AG_RGB(222,222,222),
    AG_RGB(56,56,64),
    AG_RGB(24,24,30),
    AG_RGB(60,60,70),
    AG_RGB(150,150,160),
    AG_RGB(70,70,80),
    AG_RGB(94,94,106),
    4,
    1
};

typedef enum {
    AG_WIDGET_BUTTON_T = 0,
    AG_WIDGET_SLIDER_T,
    AG_WIDGET_TOGGLE_T,
    AG_WIDGET_TEXTBOX_T,
    AG_WIDGET_TEXTVIEW_T,
    AG_WIDGET_IMAGE_T,
    AG_WIDGET_VIDEO_T
} ag_widget_kind;

#define AG_WIDGET_SB_W 12

typedef struct ag_blur_job {
    ag_image *scaled;
    const ag_image *src;
    int  sw, sh;
    int  dw, dh;
    int  radius, passes;
    int  gen;
    int  done;
    ag_image *result;
} ag_blur_job;

#if defined(_WIN32)
#  define AG_TLS __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#  define AG_TLS __thread
#else
#  define AG_TLS
#endif
static AG_TLS ag_gl *g_blur_gl = NULL;
static AG_TLS int g_blur_gl_tried = 0;
static int g_gpu_blur_state = 0;

static ag_gl *ag_blur_gl_get(void)
{
    if (g_blur_gl_tried) return g_blur_gl;
    g_blur_gl_tried = 1;
    g_blur_gl = ag_gl_create();
    if (!g_blur_gl) g_gpu_blur_state = -1;
    return g_blur_gl;
}

struct ag_widget {
    ag_widget_kind kind;
    int x, y, w, h;
    ag_widget_cb cb;
    void *ud;
    struct ag_widget *next;
    int hovered, pressed, focused;
    int removed;
    int page;
    int anim_delay;
    union {
        struct { char *label; } button;
        struct { float vmin, vmax, val; } slider;
        struct { char *label; int on; } toggle;
        struct { char *buf; int cap, len, cursor, submit; } textbox;
        struct { char *buf; int cap, len;
                 int scroll;
                 int max_scroll;
                 int sb_drag;
                 int pinned;
        } textview;
        struct {
            const ag_image *img;
            char *caption;
            int   blur_radius;
            int   blur_passes;
            ag_image *cache;
            ag_image *cache_blur;
            int   cache_w, cache_h;
            int   cache_radius;
            int   blur_gen;
            const ag_image *cache_src;
            int   gen;
            struct ag_blur_job *job;
            ag_thread *worker;
        } image;
        struct {
            ag_video *video;
            ag_image *frame;
            char     *err;
            int       playing;
            int       seek_drag;
            double    acc;
            unsigned long last_tick;
        } video;
    } u;
};

struct ag_ui {
    ag_window *win;
    ag_font *font;
    int font_owned;
    ag_ui_theme th;
    struct ag_widget *head, **tail;
    struct ag_widget *focused;
    struct ag_widget *pressed;
    ag_layout_cb resize_cb;
    void *resize_ud;

    int  page;
    int  page_from;
    int  anim_type, anim_dur, anim_ease;
    unsigned long anim_start;
    int  anim_active;
    ag_u32 *page_buf;
    ag_u32 *page_buf_out;
    int  page_buf_w, page_buf_h;
    int  out_valid;
    int  in_valid;
    ag_page_draw_cb page_draw_cb;
    void *page_draw_ud;
};

static int ui_text_height(ag_ui *ui)
{
    if (ui->font) return ag_font_height(ui->font);
    return ui->win->font_size;
}

static void ui_draw_text(ag_ui *ui, int x, int y, const char *t)
{
    ag_draw_text_font(ui->win, x, y, ui->font, t);
}

static int ui_text_width(ag_ui *ui, const char *t)
{
    if (ui->font) return ag_font_text_width(ui->font, t);
    int scale = ui->win->font_size < 8 ? 1 : ui->win->font_size / 8;
    return (int)strlen(t) * 8 * scale;
}

static struct ag_widget *ag_ui_widget_at(ag_ui *ui, int x, int y)
{
    for (struct ag_widget *wdg = ui->head; wdg; wdg = wdg->next) {
        if (wdg->removed) continue;
        if (wdg->page != ui->page && wdg->page != AG_PAGE_GLOBAL) continue;
        if (x >= wdg->x && y >= wdg->y &&
            x < wdg->x + wdg->w && y < wdg->y + wdg->h)
            return wdg;
    }
    return NULL;
}

static void ag_ui_gc(ag_ui *ui)
{
    struct ag_widget **pp = &ui->head;
    while (*pp) {
        struct ag_widget *cur = *pp;
        if (cur->removed) {
            *pp = cur->next;
            switch (cur->kind) {
            case AG_WIDGET_BUTTON_T: free(cur->u.button.label); break;
            case AG_WIDGET_TOGGLE_T: free(cur->u.toggle.label); break;
            case AG_WIDGET_TEXTBOX_T: free(cur->u.textbox.buf); break;
            case AG_WIDGET_TEXTVIEW_T: free(cur->u.textview.buf); break;
            case AG_WIDGET_IMAGE_T: {
                free(cur->u.image.caption);
                ag_image_destroy(cur->u.image.cache);
                ag_image_destroy(cur->u.image.cache_blur);
                if (cur->u.image.worker) {
                    ag_thread_join(cur->u.image.worker);
                    cur->u.image.worker = NULL;
                }
                if (cur->u.image.job) {
                    ag_image_destroy(cur->u.image.job->scaled);
                    ag_image_destroy(cur->u.image.job->result);
                    free(cur->u.image.job);
                    cur->u.image.job = NULL;
                }
                break;
            }
            case AG_WIDGET_VIDEO_T:
                free(cur->u.video.err);
                if (cur->u.video.video) ag_video_close(cur->u.video.video);
                break;
            default: break;
            }
            if (ui->focused == cur) ui->focused = NULL;
            if (ui->pressed == cur) ui->pressed = NULL;
            free(cur);
        } else {
            pp = &cur->next;
        }
    }
}

ag_ui *ag_ui_create(ag_window *win)
{
    if (!win) return NULL;
    ag_ui *ui = (ag_ui *)ag_mem_alloc(sizeof(ag_ui));
    if (!ui) return NULL;
    ui->win = win;
    ui->head = NULL;
    ui->tail = &ui->head;
    ui->font = ag_font_default(16);
    ui->font_owned = ui->font != NULL;
    ui->th = ag_default_theme;
    win->ui = ui;
    return ui;
}

void ag_ui_destroy(ag_ui *ui)
{
    if (!ui) return;
    ui->focused = NULL;
    ui->pressed = NULL;
    while (ui->head) {
        struct ag_widget *n = ui->head->next;
        switch (ui->head->kind) {
        case AG_WIDGET_BUTTON_T: free(ui->head->u.button.label); break;
        case AG_WIDGET_TOGGLE_T: free(ui->head->u.toggle.label); break;
        case AG_WIDGET_TEXTBOX_T: free(ui->head->u.textbox.buf); break;
        case AG_WIDGET_TEXTVIEW_T: free(ui->head->u.textview.buf); break;
        case AG_WIDGET_IMAGE_T: {
            free(ui->head->u.image.caption);
            ag_image_destroy(ui->head->u.image.cache);
            ag_image_destroy(ui->head->u.image.cache_blur);
            if (ui->head->u.image.worker) {
                ag_thread_join(ui->head->u.image.worker);
                ui->head->u.image.worker = NULL;
            }
            if (ui->head->u.image.job) {
                ag_image_destroy(ui->head->u.image.job->scaled);
                ag_image_destroy(ui->head->u.image.job->result);
                free(ui->head->u.image.job);
                ui->head->u.image.job = NULL;
            }
            break;
        }
        case AG_WIDGET_VIDEO_T:
            free(ui->head->u.video.err);
            if (ui->head->u.video.video) ag_video_close(ui->head->u.video.video);
            break;
        default: break;
        }
        free(ui->head);
        ui->head = n;
    }
    ui->tail = &ui->head;
    if (ui->win) ui->win->ui = NULL;
    free(ui->page_buf);
    free(ui->page_buf_out);
    ui->page_buf = NULL;
    ui->page_buf_out = NULL;
    if (ui->font && ui->font_owned) ag_font_close(ui->font);
    free(ui);
}

void ag_ui_set_font(ag_ui *ui, ag_font *font)
{
    if (!ui) return;
    if (ui->font && ui->font_owned) ag_font_close(ui->font);
    ui->font = font;
    ui->font_owned = 0;
}

void ag_ui_set_theme(ag_ui *ui, const ag_ui_theme *th)
{
    if (!ui) return;
    if (th) ui->th = *th;
}

void ag_ui_set_layout_cb(ag_ui *ui, ag_layout_cb cb, void *ud)
{
    if (!ui) return;
    ui->resize_cb = cb;
    ui->resize_ud = ud;
}


float ag_ease_apply(ag_ease ease, float t)
{
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    switch (ease) {
    case AG_EASE_LINEAR:
        return t;
    case AG_EASE_IN_QUAD:
        return t * t;
    case AG_EASE_OUT_QUAD:
        return t * (2.0f - t);
    case AG_EASE_IN_OUT_QUAD:
        return t < 0.5f ? 2.0f * t * t : -1.0f + (4.0f - 2.0f * t) * t;
    case AG_EASE_OUT_CUBIC: {
        float f = t - 1.0f;
        return f * f * f + 1.0f;
    }
    case AG_EASE_IN_OUT_CUBIC:
        if (t < 0.5f) return 4.0f * t * t * t;
        { float f = 2.0f * t - 2.0f; return f * f * f + 1.0f; }
    case AG_EASE_OUT_BACK: {
        const float c1 = 1.70158f, c3 = c1 + 1.0f;
        float f = t - 1.0f;
        return 1.0f + c3 * f * f * f + c1 * f * f;
    }
    case AG_EASE_OUT_ELASTIC: {
        const float c4 = 6.2831853f / 3.0f;
        if (t == 0.0f) return 0.0f;
        if (t == 1.0f) return 1.0f;
        return (float)(pow(2.0, -10.0 * t) * sin((t * 10.0 - 0.75) * c4)) + 1.0f;
    }
    case AG_EASE_OUT_BOUNCE: {
        const float n1 = 7.5625f, d1 = 2.75f;
        if (t < 1.0f / d1) return n1 * t * t;
        if (t < 2.0f / d1) { t -= 1.5f / d1; return n1 * t * t + 0.75f; }
        if (t < 2.5f / d1) { t -= 2.25f / d1; return n1 * t * t + 0.9375f; }
        t -= 2.625f / d1;
        return n1 * t * t + 0.984375f;
    }
    }
    return t;
}

void ag_ui_set_transition(ag_ui *ui, ag_anim_type type, int duration_ms, ag_ease ease)
{
    if (!ui) return;
    ui->anim_type = (int)type;
    ui->anim_dur  = duration_ms > 0 ? duration_ms : 220;
    ui->anim_ease = (int)ease;
}

void ag_ui_set_page(ag_ui *ui, int page)
{
    if (!ui) return;
    if (page == ui->page) return;
    ui->page_from = ui->page;
    ui->page      = page;
    ui->anim_start = ag_time_ms();
    ui->anim_active = 1;
    ui->out_valid = 0;
    ui->in_valid = 0;
    if (ui->win) ui->win->nlayers = 0;
    ag_window_redraw(ui->win);
}

void ag_ui_set_page_draw_cb(ag_ui *ui, ag_page_draw_cb cb, void *ud)
{
    if (!ui) return;
    ui->page_draw_cb = cb;
    ui->page_draw_ud = ud;
}

int ag_ui_page(const ag_ui *ui) { return ui ? ui->page : 0; }

int ag_ui_animating(const ag_ui *ui) { return ui ? ui->anim_active : 0; }

static void ui_update_images(ag_ui *ui);

static void ui_update_videos(ag_ui *ui)
{
    for (struct ag_widget *wdg = ui->head; wdg; wdg = wdg->next) {
        if (wdg->removed || wdg->kind != AG_WIDGET_VIDEO_T) continue;
        if (!wdg->u.video.video || !wdg->u.video.playing) continue;

        unsigned long now = ag_time_ms();
        if (wdg->u.video.last_tick == 0) { wdg->u.video.last_tick = now; continue; }
        double dt = (double)(now - wdg->u.video.last_tick) / 1000.0;
        wdg->u.video.last_tick = now;
        if (dt < 0.0) dt = 0.0;
        if (dt > 0.25) dt = 0.25;
        wdg->u.video.acc += dt;

        double fps = ag_video_fps(wdg->u.video.video);
        if (fps <= 0.0) fps = 25.0;
        double spf = 1.0 / fps;
        int advanced = 0;

        while (wdg->u.video.acc >= spf) {
            wdg->u.video.acc -= spf;
            if (ag_video_next_frame(wdg->u.video.video, &wdg->u.video.frame) == 0) {
                ag_video_seek(wdg->u.video.video, 0.0);
                if (ag_video_next_frame(wdg->u.video.video, &wdg->u.video.frame) == 0) {
                    wdg->u.video.playing = 0;
                    break;
                }
            }
            advanced = 1;
        }
        if (advanced) ag_window_redraw(ui->win);
    }
}

void ag_ui_update(ag_ui *ui)
{
    if (!ui) return;
    if (ui->anim_active) {
        unsigned long now = ag_time_ms();
        unsigned long el = now - ui->anim_start;
        if (el >= (unsigned long)ui->anim_dur) {
            ui->anim_active  = 0;
            ui->page_from    = ui->page;
        }
        ag_window_redraw(ui->win);
    }
    ui_update_videos(ui);
    ui_update_images(ui);
}

void ag_widget_set_page(ag_widget *wdg, int page)
{
    if (wdg) wdg->page = page;
}

int ag_widget_page(const ag_widget *wdg) { return wdg ? wdg->page : 0; }

void ag_widget_set_anim_delay(ag_widget *wdg, int ms)
{
    if (wdg) wdg->anim_delay = ms > 0 ? ms : 0;
}

static float ui_anim_progress(const ag_ui *ui)
{
    if (!ui->anim_active) return 1.0f;
    unsigned long el = ag_time_ms() - ui->anim_start;
    if (ui->anim_dur <= 0) return 1.0f;
    float t = (float)el / (float)ui->anim_dur;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return ag_ease_apply((ag_ease)ui->anim_ease, t);
}

const ag_ui_theme *ag_ui_default_theme(void)
{
    return &ag_default_theme;
}

static void ag_widget_add(ag_ui *ui, struct ag_widget *wdg)
{
    *ui->tail = wdg;
    ui->tail = &wdg->next;
    wdg->next = NULL;
}

ag_widget *ag_ui_add_button(ag_ui *ui, const char *label,
                            int x, int y, int w, int h,
                            ag_widget_cb cb, void *ud)
{
    if (!ui) return NULL;
    struct ag_widget *wdg = (struct ag_widget *)ag_mem_alloc(sizeof(*wdg));
    if (!wdg) return NULL;
    wdg->kind = AG_WIDGET_BUTTON_T;
    wdg->x = x; wdg->y = y; wdg->w = w; wdg->h = h;
    wdg->cb = cb; wdg->ud = ud;
    wdg->u.button.label = label ? strdup(label) : strdup("");
    ag_widget_add(ui, wdg);
    return wdg;
}

ag_widget *ag_ui_add_slider(ag_ui *ui, float vmin, float vmax, float value,
                            int x, int y, int w, int h,
                            ag_widget_cb cb, void *ud)
{
    if (!ui) return NULL;
    struct ag_widget *wdg = (struct ag_widget *)ag_mem_alloc(sizeof(*wdg));
    if (!wdg) return NULL;
    wdg->kind = AG_WIDGET_SLIDER_T;
    wdg->x = x; wdg->y = y; wdg->w = w; wdg->h = h;
    wdg->cb = cb; wdg->ud = ud;
    if (value < vmin) value = vmin;
    if (value > vmax) value = vmax;
    wdg->u.slider.vmin = vmin; wdg->u.slider.vmax = vmax;
    wdg->u.slider.val = value;
    ag_widget_add(ui, wdg);
    return wdg;
}

ag_widget *ag_ui_add_toggle(ag_ui *ui, const char *label, int checked,
                            int x, int y, ag_widget_cb cb, void *ud)
{
    if (!ui) return NULL;
    struct ag_widget *wdg = (struct ag_widget *)ag_mem_alloc(sizeof(*wdg));
    if (!wdg) return NULL;
    wdg->kind = AG_WIDGET_TOGGLE_T;
    wdg->x = x; wdg->y = y;
    wdg->w = 20 + (int)strlen(label) * 9;
    wdg->h = 24;
    wdg->cb = cb; wdg->ud = ud;
    wdg->u.toggle.on = checked ? 1 : 0;
    wdg->u.toggle.label = label ? strdup(label) : strdup("");
    ag_widget_add(ui, wdg);
    return wdg;
}

ag_widget *ag_ui_add_textbox(ag_ui *ui, int x, int y, int w, int h,
                             ag_widget_cb cb, void *ud)
{
    if (!ui) return NULL;
    struct ag_widget *wdg = (struct ag_widget *)ag_mem_alloc(sizeof(*wdg));
    if (!wdg) return NULL;
    wdg->kind = AG_WIDGET_TEXTBOX_T;
    wdg->x = x; wdg->y = y; wdg->w = w; wdg->h = h;
    wdg->cb = cb; wdg->ud = ud;
    wdg->u.textbox.cap = 64;
    wdg->u.textbox.len = 0;
    wdg->u.textbox.cursor = 0;
    wdg->u.textbox.buf = (char *)malloc(wdg->u.textbox.cap);
    wdg->u.textbox.buf[0] = '\0';
    ag_widget_add(ui, wdg);
    return wdg;
}

ag_widget *ag_ui_add_textview(ag_ui *ui, int x, int y, int w, int h)
{
    if (!ui) return NULL;
    struct ag_widget *wdg = (struct ag_widget *)ag_mem_alloc(sizeof(*wdg));
    if (!wdg) return NULL;
    wdg->kind = AG_WIDGET_TEXTVIEW_T;
    wdg->x = x; wdg->y = y; wdg->w = w; wdg->h = h;
    wdg->cb = NULL; wdg->ud = NULL;
    wdg->u.textview.cap = 256;
    wdg->u.textview.len = 0;
    wdg->u.textview.buf = (char *)malloc(wdg->u.textview.cap);
    if (!wdg->u.textview.buf) { free(wdg); return NULL; }
    wdg->u.textview.buf[0] = '\0';
    wdg->u.textview.scroll = 0;
    wdg->u.textview.max_scroll = 0;
    wdg->u.textview.sb_drag = 0;
    wdg->u.textview.pinned = 1;
    ag_widget_add(ui, wdg);
    return wdg;
}

ag_widget *ag_ui_add_image(ag_ui *ui, const ag_image *img, int x, int y, int w, int h,
                            ag_widget_cb cb, void *ud)
{
    if (!ui) return NULL;
    struct ag_widget *wdg = (struct ag_widget *)ag_mem_alloc(sizeof(*wdg));
    if (!wdg) return NULL;
    wdg->kind = AG_WIDGET_IMAGE_T;
    wdg->x = x; wdg->y = y; wdg->w = w; wdg->h = h;
    wdg->cb = cb; wdg->ud = ud;
    wdg->u.image.img = img;
    wdg->u.image.caption = NULL;
    wdg->u.image.blur_radius = 0;
    wdg->u.image.blur_passes = 1;
    ag_widget_add(ui, wdg);
    return wdg;
}

ag_widget *ag_ui_add_video(ag_ui *ui, const char *path, int x, int y, int w, int h,
                            ag_widget_cb cb, void *ud)
{
    if (!ui) return NULL;
    struct ag_widget *wdg = (struct ag_widget *)ag_mem_alloc(sizeof(*wdg));
    if (!wdg) return NULL;
    wdg->kind = AG_WIDGET_VIDEO_T;
    wdg->x = x; wdg->y = y; wdg->w = w; wdg->h = h;
    wdg->cb = cb; wdg->ud = ud;
    wdg->u.video.video = path ? ag_video_open(path) : NULL;
    wdg->u.video.frame = NULL;
    wdg->u.video.err = NULL;
    wdg->u.video.playing = 0;
    wdg->u.video.seek_drag = 0;
    wdg->u.video.acc = 0.0;
    wdg->u.video.last_tick = 0;
    if (!wdg->u.video.video) {
        const char *e = ag_video_last_error();
        wdg->u.video.err = strdup(e && *e ? e : "видео недоступно");
    } else {
        ag_video_next_frame(wdg->u.video.video, &wdg->u.video.frame);
    }
    ag_widget_add(ui, wdg);
    return wdg;
}

void ag_widget_set_blur(ag_widget *wdg, int radius, int passes)
{
    if (!wdg || wdg->kind != AG_WIDGET_IMAGE_T) return;
    wdg->u.image.blur_radius = radius > 0 ? radius : 0;
    wdg->u.image.blur_passes = passes > 0 ? passes : 1;
    wdg->u.image.gen++;
}

void ag_widget_set_image(ag_widget *wdg, const ag_image *img)
{
    if (!wdg || wdg->kind != AG_WIDGET_IMAGE_T) return;
    wdg->u.image.img = img;
    wdg->u.image.gen++;
}

void ag_widget_set_caption(ag_widget *wdg, const char *text)
{
    if (!wdg || wdg->kind != AG_WIDGET_IMAGE_T) return;
    free(wdg->u.image.caption);
    wdg->u.image.caption = text ? strdup(text) : NULL;
}

int  ag_video_widget_playing(ag_widget *wdg)
{
    if (!wdg || wdg->kind != AG_WIDGET_VIDEO_T) return 0;
    return wdg->u.video.playing;
}

void ag_video_widget_play(ag_widget *wdg, int play)
{
    if (!wdg || wdg->kind != AG_WIDGET_VIDEO_T) return;
    wdg->u.video.playing = play ? 1 : 0;
    wdg->u.video.acc = 0.0;
    wdg->u.video.last_tick = ag_time_ms();
    if (wdg->u.video.video)
        ag_video_set_playing(wdg->u.video.video, play);
}

void ag_video_widget_seek(ag_widget *wdg, double seconds)
{
    if (!wdg || wdg->kind != AG_WIDGET_VIDEO_T) return;
    if (!wdg->u.video.video) return;
    ag_video_seek(wdg->u.video.video, seconds);
    ag_video_next_frame(wdg->u.video.video, &wdg->u.video.frame);
    wdg->u.video.acc = 0.0;
}

double ag_video_widget_time(const ag_widget *wdg)
{
    if (!wdg || wdg->kind != AG_WIDGET_VIDEO_T) return 0.0;
    return ag_video_time(wdg->u.video.video);
}

void ag_widget_remove(ag_ui *ui, ag_widget *wdg)
{
    if (!ui || !wdg) return;
    wdg->removed = 1;
}

static void ag_widget_invalidate(ag_widget *wdg)
{
    if (!wdg) return;
    if (wdg->kind == AG_WIDGET_IMAGE_T) wdg->u.image.gen++;
}

void ag_widget_move(ag_widget *wdg, int x, int y)
{
    if (!wdg) return;
    wdg->x = x; wdg->y = y;
    ag_widget_invalidate(wdg);
}

void ag_widget_set_rect(ag_widget *wdg, int x, int y, int w, int h)
{
    if (!wdg) return;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    wdg->x = x; wdg->y = y; wdg->w = w; wdg->h = h;
    ag_widget_invalidate(wdg);
}

void ag_widget_get_rect(const ag_widget *wdg, int *x, int *y, int *w, int *h)
{
    if (!wdg) return;
    if (x) *x = wdg->x;
    if (y) *y = wdg->y;
    if (w) *w = wdg->w;
    if (h) *h = wdg->h;
}

void ag_widget_set_callback(ag_widget *wdg, ag_widget_cb cb, void *ud)
{
    if (!wdg) return;
    wdg->cb = cb; wdg->ud = ud;
}

static int utf8_prev_boundary(const char *s, int idx)
{
    int i = idx - 1;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) --i;
    return i < 0 ? 0 : i;
}

static int utf8_next_boundary(const char *s, int idx, int max)
{
    int i = idx + 1;
    while (i < max && ((unsigned char)s[i] & 0xC0) == 0x80) ++i;
    return i;
}

static void ag_tb_insert(ag_widget *wdg, ag_u32 cp)
{
    if (cp < 0x20 && cp != '\n') return;
    char enc[4];
    int nb = utf8_encode(enc, cp);
    int need = wdg->u.textbox.len + nb + 1;
    if (need > wdg->u.textbox.cap) {
        int ncap = wdg->u.textbox.cap * 2;
        char *nbuf = (char *)realloc(wdg->u.textbox.buf, ncap);
        if (!nbuf) return;
        wdg->u.textbox.buf = nbuf;
        wdg->u.textbox.cap = ncap;
    }
    int c = wdg->u.textbox.cursor;
    memmove(wdg->u.textbox.buf + c + nb, wdg->u.textbox.buf + c,
            wdg->u.textbox.len - c + 1);
    memcpy(wdg->u.textbox.buf + c, enc, nb);
    wdg->u.textbox.len += nb;
    wdg->u.textbox.cursor = c + nb;
}

static void ag_tb_erase(ag_widget *wdg, int back)
{
    char *buf = wdg->u.textbox.buf;
    int c = wdg->u.textbox.cursor, len = wdg->u.textbox.len;
    if (back) {
        if (c <= 0) return;
        int s = utf8_prev_boundary(buf, c);
        memmove(buf + s, buf + c, len - c + 1);
        wdg->u.textbox.len -= (c - s);
        wdg->u.textbox.cursor = s;
    } else {
        if (c >= len) return;
        int n = utf8_next_boundary(buf, c, len);
        memmove(buf + c, buf + n, len - n + 1);
        wdg->u.textbox.len -= (n - c);
    }
}

static void ag_tb_move(ag_widget *wdg, ag_keyctl kc)
{
    char *buf = wdg->u.textbox.buf;
    int c = wdg->u.textbox.cursor, len = wdg->u.textbox.len;
    switch (kc) {
    case AGK_LEFT:  wdg->u.textbox.cursor = c > 0 ? utf8_prev_boundary(buf, c) : 0; break;
    case AGK_RIGHT: wdg->u.textbox.cursor = c < len ? utf8_next_boundary(buf, c, len) : len; break;
    case AGK_HOME:  wdg->u.textbox.cursor = 0; break;
    case AGK_END:   wdg->u.textbox.cursor = len; break;
    default: break;
    }
}

const char *ag_button_label(const ag_widget *wdg)
{
    if (!wdg || wdg->kind != AG_WIDGET_BUTTON_T) return "";
    return wdg->u.button.label ? wdg->u.button.label : "";
}

const char *ag_textbox_text(const ag_widget *wdg)
{
    if (!wdg || wdg->kind != AG_WIDGET_TEXTBOX_T) return "";
    return wdg->u.textbox.buf ? wdg->u.textbox.buf : "";
}

int ag_textbox_submitted(const ag_widget *wdg)
{
    if (!wdg || wdg->kind != AG_WIDGET_TEXTBOX_T) return 0;
    struct ag_widget *w = (struct ag_widget *)wdg;
    int r = w->u.textbox.submit;
    w->u.textbox.submit = 0;
    return r;
}

void ag_textbox_set_text(ag_widget *wdg, const char *text)
{
    if (!wdg || wdg->kind != AG_WIDGET_TEXTBOX_T) return;
    if (!text) return;
    int len = (int)strlen(text);
    if (len + 1 > wdg->u.textbox.cap) {
        int ncap = len + 1;
        char *nb = (char *)realloc(wdg->u.textbox.buf, ncap);
        if (!nb) return;
        wdg->u.textbox.buf = nb;
        wdg->u.textbox.cap = ncap;
    }
    memcpy(wdg->u.textbox.buf, text, len + 1);
    wdg->u.textbox.len = len;
    wdg->u.textbox.cursor = len;
}

void ag_textview_append(ag_widget *wdg, const char *text)
{
    if (!wdg || wdg->kind != AG_WIDGET_TEXTVIEW_T || !text) return;
    int n = (int)strlen(text);
    if (n <= 0) return;
    if (wdg->u.textview.len + n + 1 > wdg->u.textview.cap) {
        int ncap = wdg->u.textview.cap;
        while (ncap < wdg->u.textview.len + n + 1) ncap *= 2;
        char *nb = (char *)realloc(wdg->u.textview.buf, ncap);
        if (!nb) return;
        wdg->u.textview.buf = nb;
        wdg->u.textview.cap = ncap;
    }
    memcpy(wdg->u.textview.buf + wdg->u.textview.len, text, n);
    wdg->u.textview.len += n;
    wdg->u.textview.buf[wdg->u.textview.len] = '\0';
    wdg->u.textview.scroll = 0x7FFFFFFF;
    wdg->u.textview.pinned = 1;
}

const char *ag_textview_text(const ag_widget *wdg)
{
    if (!wdg || wdg->kind != AG_WIDGET_TEXTVIEW_T) return "";
    return wdg->u.textview.buf ? wdg->u.textview.buf : "";
}

void ag_textview_clear(ag_widget *wdg)
{
    if (!wdg || wdg->kind != AG_WIDGET_TEXTVIEW_T) return;
    wdg->u.textview.buf[0] = '\0';
    wdg->u.textview.len = 0;
    wdg->u.textview.scroll = 0;
    wdg->u.textview.max_scroll = 0;
    wdg->u.textview.pinned = 1;
}

void ag_textview_set_scroll(ag_widget *wdg, int line)
{
    if (!wdg || wdg->kind != AG_WIDGET_TEXTVIEW_T) return;
    if (line < 0) { wdg->u.textview.scroll = 0x7FFFFFFF; wdg->u.textview.pinned = 1; }
    else          { wdg->u.textview.scroll = line;        wdg->u.textview.pinned = 0; }
}

float ag_slider_value(const ag_widget *wdg)
{
    if (!wdg || wdg->kind != AG_WIDGET_SLIDER_T) return 0.0f;
    return wdg->u.slider.val;
}

int ag_toggle_checked(const ag_widget *wdg)
{
    if (!wdg || wdg->kind != AG_WIDGET_TOGGLE_T) return 0;
    return wdg->u.toggle.on;
}

#define UI_THUMB_W 14

static void ui_draw_button(ag_ui *ui, ag_widget *wdg)
{
    ag_window *win = ui->win;
    ag_color bg = wdg->pressed ? ui->th.pressed : (wdg->hovered ? ui->th.hover : ui->th.bg);
    ag_set_color(win, bg);
    ag_fill_round_rect(win, wdg->x, wdg->y, wdg->w, wdg->h, ui->th.radius);
    if (ui->th.borders) {
        ag_set_color(win, wdg->focused ? ui->th.accent : ui->th.border);
        ag_draw_round_rect(win, wdg->x, wdg->y, wdg->w, wdg->h, ui->th.radius);
    }

    const char *lab = wdg->u.button.label;
    int tw = ui_text_width(ui, lab);
    int th = ui_text_height(ui);
    ag_set_color(win, ui->th.text);
    ui_draw_text(ui, wdg->x + (wdg->w - tw) / 2, wdg->y + (wdg->h - th) / 2, lab);
}

static void ui_draw_slider(ag_ui *ui, ag_widget *wdg)
{
    ag_window *win = ui->win;
    float t = 0.0f;
    if (wdg->u.slider.vmax > wdg->u.slider.vmin)
        t = (wdg->u.slider.val - wdg->u.slider.vmin) /
            (wdg->u.slider.vmax - wdg->u.slider.vmin);
    int tracky = wdg->y + wdg->h / 2 - 2;
    int tw = wdg->w - UI_THUMB_W;

    ag_set_color(win, ui->th.track);
    ag_fill_rect(win, wdg->x + UI_THUMB_W / 2, tracky, tw, 4);
    ag_set_color(win, ui->th.accent);
    ag_fill_rect(win, wdg->x + UI_THUMB_W / 2, tracky, (int)(tw * t), 4);

    int hx = wdg->x + (int)(tw * t);
    ag_set_color(win, wdg->hovered || wdg->pressed ? ui->th.thumb : AG_RGB(120,120,130));
    ag_fill_round_rect(win, hx, wdg->y, UI_THUMB_W, wdg->h, ui->th.radius);
    if (ui->th.borders) {
        ag_set_color(win, ui->th.border);
        ag_draw_round_rect(win, hx, wdg->y, UI_THUMB_W, wdg->h, ui->th.radius);
    }
}

static void ui_draw_toggle(ag_ui *ui, ag_widget *wdg)
{
    ag_window *win = ui->win;
    int sz = wdg->h - 4;
    int bx = wdg->x + 2, by = wdg->y + 2;

    ag_set_color(win, wdg->u.toggle.on ? ui->th.accent : ui->th.bg2);
    ag_fill_round_rect(win, bx, by, sz, sz, ui->th.radius);
    if (ui->th.borders) {
        ag_set_color(win, wdg->focused ? ui->th.accent : ui->th.border);
        ag_draw_round_rect(win, bx, by, sz, sz, ui->th.radius);
    }

    if (wdg->u.toggle.on) {
        ag_set_color(win, AG_WHITE);
        ag_draw_line(win, bx + 5, by + sz / 2, bx + sz / 2, by + sz - 5);
        ag_draw_line(win, bx + sz / 2, by + sz - 5, bx + sz - 4, by + 4);
    }

    ag_set_color(win, ui->th.text);
    ui_draw_text(ui, bx + sz + 8, by, wdg->u.toggle.label);
}

static void ui_draw_textbox(ag_ui *ui, ag_widget *wdg)
{
    ag_window *win = ui->win;
if (!ui->font) {
        ag_set_color(win, ui->th.bg2);
        ag_fill_rect(win, wdg->x, wdg->y, wdg->w, wdg->h);
        if (ui->th.borders) {
            ag_set_color(win, wdg->focused ? ui->th.accent : ui->th.border);
            ag_draw_rect(win, wdg->x, wdg->y, wdg->w, wdg->h);
        }
        ag_set_color(win, ui->th.text);
        ui_draw_text(ui, wdg->x + 4, wdg->y + 3, wdg->u.textbox.buf);
        return;
    }

    ag_set_color(win, ui->th.bg2);
    ag_fill_rect(win, wdg->x, wdg->y, wdg->w, wdg->h);
    if (ui->th.borders) {
        ag_set_color(win, wdg->focused ? ui->th.accent : ui->th.border);
        ag_draw_rect(win, wdg->x, wdg->y, wdg->w, wdg->h);
    }

    const char *txt = wdg->u.textbox.buf;
    int curs = wdg->u.textbox.cursor;
    int len  = wdg->u.textbox.len;
    int maxw = wdg->w - 8;
    int fh = ag_font_height(ui->font);

    int *wb = (int *)malloc(sizeof(int) * (len + 1));
    if (!wb) return;
    wb[0] = 0;
    {
        const char *p = txt;
        int acc = 0, b = 1;
        while (*p) {
            const char *q = p;
            ag_u32 cp = utf8_decode(&p);
            acc += ag_font_char_width(ui->font, cp);
            int nb = (int)(p - q);
            for (int k = 0; k < nb && b <= len; ++k) { wb[b] = acc; ++b; }
        }
        while (b <= len) { wb[b] = acc; ++b; }
    }
    int total = wb[len];

    int start = 0;
    if (total > maxw && wb[curs] > maxw) {
        for (int i = 0; i < curs; ++i)
            if (wb[curs] - wb[i] <= maxw) { start = i; break; }
    }
    int caret_x = wb[curs] - wb[start];

    int px = wdg->x + 4, py = wdg->y + (wdg->h - fh) / 2;
    const char *p = txt;
    while ((int)(p - txt) < start && *p) utf8_decode(&p);
    ag_set_color(win, ui->th.text);
    while (*p && (int)(p - txt) <= len) {
        const char *q = p;
        ag_u32 cp = utf8_decode(&p);
        int cw = ag_font_char_width(ui->font, cp);
        if (px + cw > wdg->x + wdg->w - 4) break;
        char tmp[5];
        int n = (int)(p - q);
        memcpy(tmp, q, n);
        tmp[n] = '\0';
        ui_draw_text(ui, px, py, tmp);
        px += cw;
    }

    if (wdg->focused) {
        int cx = wdg->x + 4 + caret_x;
        if (cx < wdg->x + wdg->w - 2) {
            ag_set_color(win, ui->th.accent);
            ag_fill_rect(win, cx, py, 1, fh);
        }
    }
    free(wb);
}

static int tv_line_rows(ag_ui *ui, const char *line, int n, int text_w)
{
    int rows = 1, w = 0, p = 0;
    if (n <= 0) return 1;
    while (p < n) {
        const char *q = line + p;
        ag_u32 cp = utf8_decode(&q);
        int cw = ui->font ? ag_font_char_width(ui->font, cp) : 0;
        if (w + cw > text_w && w > 0) { ++rows; w = 0; }
        w += cw;
        p = (int)(q - line);
    }
    return rows;
}

static int tv_visual_rows(ag_ui *ui, const char *buf, int len, int text_w)
{
    int rows = 0, p = 0;
    for (;;) {
        int end = p;
        while (end < len && buf[end] != '\n') ++end;
        rows += tv_line_rows(ui, buf + p, end - p, text_w);
        if (end >= len) break;
        p = end + 1;
    }
    return rows;
}

static int tv_scroll_max(ag_ui *ui, ag_widget *wdg)
{
    int lh = ui_text_height(ui);
    if (lh <= 1) lh = 16;
    int vis = wdg->h > lh ? wdg->h / lh : 1;
    int text_w = wdg->w - 4;
    if (text_w < 0) text_w = 0;
    int total = tv_visual_rows(ui, wdg->u.textview.buf, wdg->u.textview.len, text_w);
    if (total > vis) {
        text_w = wdg->w - 4 - (AG_WIDGET_SB_W + 2);
        if (text_w < 0) text_w = 0;
        total = tv_visual_rows(ui, wdg->u.textview.buf, wdg->u.textview.len, text_w);
    }
    return total > vis ? total - vis : 0;
}

static int tv_draw_line(ag_ui *ui, int x, int *py, int text_w,
                        int lh, const char *line, int n, int top_skip)
{
    char tmp[1024];
    int w = 0, si = 0, p = 0, row = 0, drawn = 0;
    while (p < n) {
        const char *q = line + p;
        ag_u32 cp = utf8_decode(&q);
        int cw = ui->font ? ag_font_char_width(ui->font, cp) : 0;
        if (w + cw > text_w && w > 0) {
            if (row >= top_skip) {
                int sl = p - si;
                if (sl >= (int)sizeof(tmp)) sl = (int)sizeof(tmp) - 1;
                memcpy(tmp, line + si, (size_t)sl);
                tmp[sl] = '\0';
                ui_draw_text(ui, x, *py, tmp);
                *py += lh;
                ++drawn;
            }
            ++row;
            w = 0;
            si = p;
        }
        w += cw;
        p = (int)(q - line);
    }
    if (row >= top_skip) {
        int sl = p - si;
        if (sl >= (int)sizeof(tmp)) sl = (int)sizeof(tmp) - 1;
        memcpy(tmp, line + si, (size_t)sl);
        tmp[sl] = '\0';
        ui_draw_text(ui, x, *py, tmp);
        *py += lh;
        ++drawn;
    }
    return drawn;
}

static void ui_draw_textview(ag_ui *ui, ag_widget *wdg)
{
    ag_window *win = ui->win;

    ag_set_color(win, ui->th.bg2);
    ag_fill_rect(win, wdg->x, wdg->y, wdg->w, wdg->h);
    if (ui->th.borders) {
        ag_set_color(win, wdg->focused ? ui->th.accent : ui->th.border);
        ag_draw_rect(win, wdg->x, wdg->y, wdg->w, wdg->h);
    }

    int lh = ui_text_height(ui);
    if (lh <= 1) lh = 16;
    int vis = wdg->h > lh ? wdg->h / lh : 1;

    const char *buf = wdg->u.textview.buf;
    int sb_w = 0;
    int text_w = wdg->w - 4;
    if (text_w < 0) text_w = 0;
    int total = tv_visual_rows(ui, buf, wdg->u.textview.len, text_w);
    if (total > vis) {
        sb_w = AG_WIDGET_SB_W;
        text_w = wdg->w - 4 - (sb_w + 2);
        if (text_w < 0) text_w = 0;
        total = tv_visual_rows(ui, buf, wdg->u.textview.len, text_w);
    }
    int max_scroll = total > vis ? total - vis : 0;

    int s = wdg->u.textview.scroll;
    if (s < 0) s = 0;
    if (s > max_scroll) s = max_scroll;
    wdg->u.textview.scroll = s;
    wdg->u.textview.max_scroll = max_scroll;
    wdg->u.textview.pinned = (s >= max_scroll);

    ag_clip_set(win, wdg->x + 2, wdg->y + 2, text_w, wdg->h - 4);
    ag_set_color(win, ui->th.text);

    int py = wdg->y + 2;
    int row = 0, p = 0;
    for (;;) {
        int end = p;
        while (end < wdg->u.textview.len && buf[end] != '\n') ++end;
        int n = end - p;
        int r = tv_line_rows(ui, buf + p, n, text_w);
        if (row + r > s) {
            int skip = s > row ? s - row : 0;
            tv_draw_line(ui, wdg->x + 2, &py, text_w, lh, buf + p, n, skip);
            if (py > wdg->y + wdg->h - 2) break;
        }
        row += r;
        if (end >= wdg->u.textview.len) break;
        p = end + 1;
    }

    ag_clip_clear(win);

    if (sb_w) {
        int sbx = wdg->x + wdg->w - sb_w;
        int sby = wdg->y + 1, sbh = wdg->h - 2;
        ag_color sb_col = (wdg->hovered || wdg->u.textview.sb_drag)
                            ? ui->th.scrollbar_hover : ui->th.scrollbar;
        ag_set_color(win, sb_col);
        ag_fill_rect(win, sbx, sby, sb_w, sbh);

        int th2 = sbh * vis / total;
        if (th2 < 8) th2 = sbh > 8 ? 8 : sbh;
        int ty = sby + (max_scroll > 0 ? (sbh - th2) * s / max_scroll : 0);
        ag_set_color(win, ui->th.thumb);
        ag_fill_rect(win, sbx, ty, sb_w, th2);
    }
}


static void ag_blur_worker(void *ud)
{
    ag_blur_job *job = (ag_blur_job *)ud;
    ag_image *res = NULL;

    ag_gl *gl = ag_blur_gl_get();
    if (gl && job->src && job->src->pixels) {
        int bw = job->dw / 2, bh = job->dh / 2;
        if (bw < 1) bw = 1;
        if (bh < 1) bh = 1;
        unsigned char *rgba = (unsigned char *)malloc((size_t)bw * bh * 4);
        if (rgba) {
            if (ag_gl_blur(gl, job->src->pixels, job->sw, job->sh,
                           bw, bh, job->radius, rgba)) {
                res = ag_image_create(bw, bh);
                if (res) {
                    for (int y = 0; y < bh; ++y) {
                        const unsigned char *row = rgba + (size_t)y * bw * 4;
                        ag_u32 *dst = res->pixels + (size_t)y * bw;
                        for (int x = 0; x < bw; ++x) {
                            dst[x] = ((ag_u32)row[x * 4 + 3] << 24) |
                                     ((ag_u32)row[x * 4 + 0] << 16) |
                                     ((ag_u32)row[x * 4 + 1] << 8) |
                                     (ag_u32)row[x * 4 + 2];
                        }
                    }
                }
            }
            free(rgba);
        }
        if (res) g_gpu_blur_state = 1;
    }

    if (!res && job->scaled) {
        res = ag_image_clone(job->scaled);
        if (res) ag_image_blur_box(res, job->radius, 2);
    }
    job->result = res;
    job->done = 1;
}

static const ag_image *ui_image_cache(ag_widget *wdg)
{
    const ag_image *im = wdg->u.image.img;
    if (!im || !im->pixels) return NULL;
    if (g_gpu_blur_state > 0) return NULL;
    if (wdg->u.image.cache && wdg->u.image.cache_src == im &&
        wdg->u.image.cache_w == wdg->w && wdg->u.image.cache_h == wdg->h)
        return wdg->u.image.cache;

    ag_image *c = ag_image_create(wdg->w, wdg->h);
    if (!c) return NULL;
    ag_image_scale_into(c, im);
    ag_image_destroy(wdg->u.image.cache);
    wdg->u.image.cache = c;
    wdg->u.image.cache_src = im;
    wdg->u.image.cache_w = wdg->w;
    wdg->u.image.cache_h = wdg->h;
    wdg->u.image.gen++;
    return c;
}

static const ag_image *ui_image_blur(ag_widget *wdg)
{
    if (wdg->u.image.blur_radius <= 0) return NULL;
    return wdg->u.image.cache_blur;
}

static void ui_update_images(ag_ui *ui)
{
    int changed = 0;

    for (struct ag_widget *wdg = ui->head; wdg; wdg = wdg->next) {
        if (wdg->removed || wdg->kind != AG_WIDGET_IMAGE_T) continue;
        if (wdg->page != ui->page && wdg->page != AG_PAGE_GLOBAL) continue;

        ag_blur_job *job = wdg->u.image.job;
        if (job) {
            if (!job->done) continue;
            if (wdg->u.image.worker) {
                ag_thread_join(wdg->u.image.worker);
                wdg->u.image.worker = NULL;
            }
            if (job->gen == wdg->u.image.gen && job->result) {
                ag_image_destroy(wdg->u.image.cache_blur);
                wdg->u.image.cache_blur = job->result;
                job->result = NULL;
                wdg->u.image.cache_radius = job->radius;
                wdg->u.image.blur_gen = job->gen;
                changed = 1;
            }
            ag_image_destroy(job->scaled);
            free(job);
            wdg->u.image.job = NULL;
        }

        if (wdg->u.image.blur_radius > 0 && !wdg->u.image.job &&
            wdg->u.image.blur_gen != wdg->u.image.gen) {
            const ag_image *src = wdg->u.image.img;
            const ag_image *scaled = ui_image_cache(wdg);
            if (src && src->pixels) {
                ag_blur_job *nj = (ag_blur_job *)calloc(1, sizeof(ag_blur_job));
                if (nj) {
                    nj->src    = src;
                    nj->sw     = src->w;
                    nj->sh     = src->h;
                    nj->dw     = wdg->w;
                    nj->dh     = wdg->h;
                    nj->scaled = scaled ? ag_image_clone(scaled) : NULL;
                    nj->radius = wdg->u.image.blur_radius;
                    nj->passes = wdg->u.image.blur_passes;
                    nj->gen    = wdg->u.image.gen;
                    wdg->u.image.worker = ag_thread_start(ag_blur_worker, nj);
                    if (wdg->u.image.worker) {
                        wdg->u.image.job = nj;
                    } else {
                        ag_image_destroy(nj->scaled);
                        free(nj);
                    }
                }
            }
        }
    }
    if (changed) ag_window_redraw(ui->win);
}

static void ui_draw_image_card(ag_ui *ui, struct ag_widget *wdg)
{
    ag_window *win = ui->win;
    const ag_image *im = wdg->u.image.img;

    if (im && im->pixels) {
        const ag_image *pic = ui_image_blur(wdg);
        if (!pic) pic = im;
        if (pic) ag_draw_image(win, wdg->x, wdg->y, wdg->w, wdg->h, pic);
    } else {
        ag_set_color(win, ui->th.bg2);
        ag_fill_rect(win, wdg->x, wdg->y, wdg->w, wdg->h);
    }

    if (wdg->u.image.caption && *wdg->u.image.caption) {
        int fh = ui_text_height(ui);
        int strip = fh + 6;
        int sy = wdg->y + wdg->h - strip;
        if (sy < wdg->y) sy = wdg->y;
        int oa = win->alpha;
        win->alpha = oa * 3 / 4;
        ag_set_color(win, AG_RGB(0, 0, 0));
        ag_fill_rect(win, wdg->x, sy, wdg->w, strip);
        win->alpha = oa;
        ag_set_color(win, AG_WHITE);
        ui_draw_text(ui, wdg->x + 6, sy + 3, wdg->u.image.caption);
    }

    if (ui->th.borders) {
        ag_set_color(win, ui->th.border);
        ag_draw_rect(win, wdg->x, wdg->y, wdg->w, wdg->h);
    }
}

void ag_widget_prepare(ag_widget *wdg)
{
    if (!wdg) return;
    if (wdg->kind == AG_WIDGET_IMAGE_T) {
        const ag_image *scaled = ui_image_cache(wdg);
        if (scaled && wdg->u.image.blur_radius > 0) {
            ag_image *b = ag_image_clone(scaled);
            if (b) {
                ag_image_blur_box(b, wdg->u.image.blur_radius, 2);
                ag_image_destroy(wdg->u.image.cache_blur);
                wdg->u.image.cache_blur = b;
                wdg->u.image.cache_radius = wdg->u.image.blur_radius;
                wdg->u.image.blur_gen = wdg->u.image.gen;
            }
        }
    }
}

#define UI_VIDEO_BAR 28

static void ui_video_time_str(char *buf, int cap, double t)
{
    int total = (int)t;
    int m = total / 60, s = total % 60;
    int ms = (int)((t - (double)total) * 100.0);
    snprintf(buf, (size_t)cap, "%d:%02d.%02d", m, s, ms);
}

static void ui_draw_video(ag_ui *ui, struct ag_widget *wdg)
{
    ag_window *win = ui->win;
    int bar = wdg->h > UI_VIDEO_BAR * 2 ? UI_VIDEO_BAR : 0;
    int vh = wdg->h - bar;
    if (vh < 1) vh = 1;

    ag_set_color(win, AG_RGB(10, 10, 14));
    ag_fill_rect(win, wdg->x, wdg->y, wdg->w, wdg->h);

    const ag_image *fr = wdg->u.video.frame;
    if (fr && fr->pixels) {
        int iw = 0, ih = 0;
        ag_image_get_size(fr, &iw, &ih);
        if (iw > 0 && ih > 0) {
            float k = (float)wdg->w / (float)iw;
            float kh = (float)vh / (float)ih;
            if (kh < k) k = kh;
            int dw = (int)((float)iw * k), dh = (int)((float)ih * k);
            if (dw < 1) dw = 1;
            if (dh < 1) dh = 1;
            int dx = wdg->x + (wdg->w - dw) / 2;
            int dy = wdg->y + (vh - dh) / 2;
            ag_draw_image(win, dx, dy, dw, dh, fr);
        }
    } else if (wdg->u.video.err && *wdg->u.video.err) {
        ag_set_color(win, ui->th.text);
        ui_draw_text(ui, wdg->x + 8, wdg->y + 8, wdg->u.video.err);
    }

    if (bar > 0) {
        int by = wdg->y + vh;
        ag_set_color(win, ui->th.bg2);
        ag_fill_rect(win, wdg->x, by, wdg->w, bar);

        int bx = wdg->x + 6, byy = by + (bar - 18) / 2;
        ag_set_color(win, ui->th.accent);
        if (wdg->u.video.playing) {
            ag_fill_rect(win, bx, byy, 5, 18);
            ag_fill_rect(win, bx + 9, byy, 5, 18);
        } else {
            for (int i = 0; i < 9; ++i) {
                int hgt = 18 - i * 2;
                ag_fill_rect(win, bx + i, byy + (18 - hgt) / 2, 1, hgt);
            }
        }

        int sx = bx + 22;
        int sw = wdg->w - (sx - wdg->x) - 66;
        if (sw > 20) {
            int sy2 = by + bar / 2 - 2;
            ag_set_color(win, ui->th.track);
            ag_fill_rect(win, sx, sy2, sw, 4);
            double dur = ag_video_duration(wdg->u.video.video);
            double cur = ag_video_time(wdg->u.video.video);
            if (dur > 0.0) {
                int p = (int)(sw * (cur / dur));
                if (p < 0) p = 0;
                if (p > sw) p = sw;
                ag_set_color(win, ui->th.accent);
                ag_fill_rect(win, sx, sy2, p, 4);
                ag_fill_round_rect(win, sx + p - 3, by + 2, 6, bar - 4, 3);
            }
        }

        char ts[32];
        double cur = ag_video_time(wdg->u.video.video);
        double dur = ag_video_duration(wdg->u.video.video);
        ui_video_time_str(ts, sizeof(ts), cur);
        ag_set_color(win, ui->th.text);
        int tw = ui_text_width(ui, ts);
        ui_draw_text(ui, wdg->x + wdg->w - tw - 8, by + (bar - ui_text_height(ui)) / 2, ts);
        (void)dur;
    }

    if (ui->th.borders) {
        ag_set_color(win, ui->th.border);
        ag_draw_rect(win, wdg->x, wdg->y, wdg->w, wdg->h);
    }
}

static void ui_video_seek_at(ag_widget *wdg, int x)
{
    if (!wdg || wdg->kind != AG_WIDGET_VIDEO_T || !wdg->u.video.video) return;
    if (wdg->h <= UI_VIDEO_BAR * 2) return;
    int sx = wdg->x + 6 + 22;
    int sw = wdg->w - (sx - wdg->x) - 66;
    if (sw <= 20) return;
    double t = (double)(x - sx) / (double)sw;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    ag_video_widget_seek(wdg, ag_video_duration(wdg->u.video.video) * t);
}


static void ui_draw_widget(ag_ui *ui, struct ag_widget *wdg)
{
    switch (wdg->kind) {
    case AG_WIDGET_BUTTON_T:  ui_draw_button(ui, wdg); break;
    case AG_WIDGET_SLIDER_T:  ui_draw_slider(ui, wdg); break;
    case AG_WIDGET_TOGGLE_T:  ui_draw_toggle(ui, wdg); break;
    case AG_WIDGET_TEXTBOX_T: ui_draw_textbox(ui, wdg); break;
    case AG_WIDGET_TEXTVIEW_T:ui_draw_textview(ui, wdg); break;
    case AG_WIDGET_IMAGE_T:   ui_draw_image_card(ui, wdg); break;
    case AG_WIDGET_VIDEO_T:   ui_draw_video(ui, wdg); break;
    }
}

static void ui_render_page(ag_ui *ui, int page, int incoming, float prog)
{
    ag_window *win = ui->win;
    int oa = win->alpha;

    for (struct ag_widget *wdg = ui->head; wdg; wdg = wdg->next) {
        if (wdg->removed || wdg->page != page) continue;

        float p = prog;
        if (wdg->anim_delay > 0 && ui->anim_dur > 0) {
            float d = (float)wdg->anim_delay / (float)ui->anim_dur;
            if (d > 0.99f) d = 0.99f;
            p = (p - d) / (1.0f - d);
            if (p < 0.0f) p = 0.0f;
            if (p > 1.0f) p = 1.0f;
        }
        float a = incoming ? p : (1.0f - p);
        if (a <= 0.002f) continue;

        win->alpha = (int)(a * 255.0f + 0.5f);
        ui_draw_widget(ui, wdg);
    }
    win->alpha = oa;

    if (ui->page_draw_cb) ui->page_draw_cb(ui, page, ui->page_draw_ud);
}

static int ui_page_buf_ensure(ag_ui *ui, int w, int h)
{
    if (ui->page_buf && ui->page_buf_w == w && ui->page_buf_h == h) return 1;
    ag_u32 *nb = (ag_u32 *)realloc(ui->page_buf, (size_t)w * h * sizeof(ag_u32));
    if (!nb) { ui->page_buf = NULL; ui->page_buf_w = ui->page_buf_h = 0; return 0; }
    ui->page_buf = nb;
    ag_u32 *ob = (ag_u32 *)realloc(ui->page_buf_out, (size_t)w * h * sizeof(ag_u32));
    if (!ob) { ui->page_buf_out = NULL; ui->out_valid = 0; }
    else ui->page_buf_out = ob;
    ui->page_buf_w = w;
    ui->page_buf_h = h;
    ui->out_valid = 0;
    ui->in_valid = 0;
    memset(nb, 0, (size_t)w * h * sizeof(ag_u32));
    if (ob) memset(ob, 0, (size_t)w * h * sizeof(ag_u32));
    return 1;
}

static void ui_blend_premul(ag_window *w, int x, int y, ag_u32 p, int ga)
{
    x += w->ox;
    y += w->oy;
    if (w->clip_on &&
        (x < w->clip_x || y < w->clip_y ||
         x >= w->clip_x + w->clip_w || y >= w->clip_y + w->clip_h))
        return;
    if (x < 0 || y < 0 || x >= w->width || y >= w->height) return;

    unsigned pa = (p >> 24) & 0xFF;
    int fa = (int)((unsigned)pa * (unsigned)ga / 255u);
    if (fa <= 0) return;

    ag_u32 *d = &w->backbuf[y * w->width + x];
    int dr = (int)((*d >> 16) & 0xFF);
    int dg = (int)((*d >> 8) & 0xFF);
    int db = (int)((*d) & 0xFF);
    int pr = (int)((p >> 16) & 0xFF);
    int pg = (int)((p >> 8) & 0xFF);
    int pb = (int)((p) & 0xFF);
    int r = (int)((unsigned)pr * (unsigned)ga / 255u) + dr * (255 - fa) / 255;
    int g = (int)((unsigned)pg * (unsigned)ga / 255u) + dg * (255 - fa) / 255;
    int b = (int)((unsigned)pb * (unsigned)ga / 255u) + db * (255 - fa) / 255;
    *d = 0xFF000000u | ((ag_u32)ag_clampi(r, 0, 255) << 16) |
         ((ag_u32)ag_clampi(g, 0, 255) << 8) | (ag_u32)ag_clampi(b, 0, 255);
}

static void ui_page_composite(ag_window *w, const ag_u32 *buf, int bw, int bh,
                              float alpha, float sx, float sy, int ox, int oy)
{
    int ga = (int)(alpha * 255.0f + 0.5f);
    if (ga <= 0) return;
    if (ga > 255) ga = 255;
    if (sx < 0.001f || sy < 0.001f) return;
    float cx = (float)bw * 0.5f, cy = (float)bh * 0.5f;

    if (sx == 1.0f && sy == 1.0f) {
        for (int y = 0; y < bh; ++y) {
            for (int x = 0; x < bw; ++x) {
                ag_u32 p = buf[(size_t)y * bw + x];
                if (!((p >> 24) & 0xFF)) continue;
                ui_blend_premul(w, x + ox, y + oy, p, ga);
            }
        }
        return;
    }

    int fdx = (int)(65536.0f / sx);
    int fdy = (int)(65536.0f / sy);
    int fx0 = (int)(((-(float)ox - cx) / sx + cx) * 65536.0f);
    int fy0 = (int)(((-(float)oy - cy) / sy + cy) * 65536.0f);
    int maxx = bw << 16, maxy = bh << 16;

    for (int y = 0; y < w->height; ++y) {
        int fy = fy0 + y * fdy;
        if (fy < 0 || fy > maxy) continue;
        int fx = fx0;
        for (int x = 0; x < w->width; ++x) {
            if (fx >= 0 && fx <= maxx) {
                int nx = fx >> 16, ny = fy >> 16;
                ag_u32 p = buf[(size_t)ny * bw + nx];
                if ((p >> 24) & 0xFF)
                    ui_blend_premul(w, x, y, p, ga);
            }
            fx += fdx;
        }
    }
}

static int ag_window_gpu_layers(ag_window *w)
{
#if defined(_WIN32) || defined(AG_USE_WAYLAND)
    (void)w;
    return 0;
#else
    return w && w->present != NULL;
#endif
}

static void ui_layer_params(ag_ui *ui, int incoming, float p,
                            float *alpha, float *sx, float *sy,
                            int *ox, int *oy)
{
    int bw = ui->page_buf_w, bh = ui->page_buf_h;

    float vis = incoming ? p : (1.0f - p);
    *alpha = vis;
    *sx = 1.0f;
    *sy = 1.0f;
    *ox = 0;
    *oy = 0;

    switch (ui->anim_type) {
    case AG_ANIM_FADE:
        break;
    case AG_ANIM_SLIDE_LEFT:
        *ox = incoming ? (int)((1.0f - p) * (float)bw)
                       : -(int)(p * (float)bw);
        break;
    case AG_ANIM_SLIDE_RIGHT:
        *ox = incoming ? -(int)((1.0f - p) * (float)bw)
                       : (int)(p * (float)bw);
        break;
    case AG_ANIM_SLIDE_UP:
        *oy = incoming ? (int)((1.0f - p) * (float)bh)
                       : -(int)(p * (float)bh);
        break;
    case AG_ANIM_SLIDE_DOWN:
        *oy = incoming ? -(int)((1.0f - p) * (float)bh)
                       : (int)(p * (float)bh);
        break;
    case AG_ANIM_SCALE:
        *sx = *sy = incoming ? (0.92f + 0.08f * p) : (1.0f + 0.08f * p);
        break;
    case AG_ANIM_FLIP:
        *sx = incoming ? p : (1.0f - p);
        *sy = 1.0f;
        break;
    }
}

static void ui_composite_buf(ag_ui *ui, const ag_u32 *buf, int incoming, float p)
{
    ag_window *win = ui->win;
    int bw = ui->page_buf_w, bh = ui->page_buf_h;
    if (!buf || bw <= 0 || bh <= 0) return;

    float alpha, sx, sy;
    int ox, oy;
    ui_layer_params(ui, incoming, p, &alpha, &sx, &sy, &ox, &oy);
    ui_page_composite(win, buf, bw, bh, alpha, sx, sy, ox, oy);
}

void ag_ui_draw(ag_ui *ui)
{
    if (!ui || !ui->win) return;
    ag_window *win = ui->win;
    ag_color oc = win->current_color;
    int ofs = win->font_size;
    int oa = win->alpha;

    ag_ui_gc(ui);

    if (ui->anim_active && win->width > 0 && win->height > 0 &&
        ui_page_buf_ensure(ui, win->width, win->height) && ui->page_buf_out) {
        float p = ui_anim_progress(ui);

        if (!ui->out_valid) {
            memset(ui->page_buf_out, 0,
                   (size_t)ui->page_buf_w * ui->page_buf_h * sizeof(ag_u32));
            win->rt = ui->page_buf_out;
            win->rt_w = ui->page_buf_w;
            win->rt_h = ui->page_buf_h;
            ui_render_page(ui, ui->page_from, 1, 1.0f);
            win->rt = NULL;
            ui->out_valid = 1;
        }
        int gpu = ag_window_gpu_layers(win);
        ag_gl_layer layers[2];
        float a0, sx0, sy0, a1, sx1, sy1;
        int ox0, oy0, ox1, oy1;
        ui_layer_params(ui, 0, p, &a0, &sx0, &sy0, &ox0, &oy0);
        ui_layer_params(ui, 1, p, &a1, &sx1, &sy1, &ox1, &oy1);

        if (gpu) {
            layers[0].pixels = ui->page_buf_out;
            layers[0].w = ui->page_buf_w;
            layers[0].h = ui->page_buf_h;
            layers[0].alpha = a0;
            layers[0].scale = sx0;
            layers[0].ox = ox0;
            layers[0].oy = oy0;
            layers[0].dirty = !ui->out_valid;
        } else {
            ui_composite_buf(ui, ui->page_buf_out, 0, p);
        }

        int stagger = 0;
        for (struct ag_widget *wg = ui->head; wg; wg = wg->next)
            if (!wg->removed && wg->page == ui->page && wg->anim_delay > 0) stagger = 1;

        if (stagger || !ui->in_valid) {
            memset(ui->page_buf, 0,
                   (size_t)ui->page_buf_w * ui->page_buf_h * sizeof(ag_u32));
            win->rt = ui->page_buf;
            win->rt_w = ui->page_buf_w;
            win->rt_h = ui->page_buf_h;
            ui_render_page(ui, ui->page, 1, stagger ? p : 1.0f);
            win->rt = NULL;
            ui->in_valid = !stagger;
        }

        if (gpu) {
            layers[1].pixels = ui->page_buf;
            layers[1].w = ui->page_buf_w;
            layers[1].h = ui->page_buf_h;
            layers[1].alpha = a1;
            layers[1].scale = sx1;
            layers[1].ox = ox1;
            layers[1].oy = oy1;
            layers[1].dirty = 1;
            win->layers[0] = layers[0];
            win->layers[1] = layers[1];
            win->nlayers = 2;
        } else {
            ui_composite_buf(ui, ui->page_buf, 1, p);
        }

        for (struct ag_widget *wdg = ui->head; wdg; wdg = wdg->next) {
            if (wdg->removed || wdg->page != AG_PAGE_GLOBAL) continue;
            ui_draw_widget(ui, wdg);
        }
    } else {
        for (struct ag_widget *wdg = ui->head; wdg; wdg = wdg->next) {
            if (wdg->removed) continue;
            if (wdg->page != ui->page && wdg->page != AG_PAGE_GLOBAL) continue;
            ui_draw_widget(ui, wdg);
        }
        if (ui->page_draw_cb) ui->page_draw_cb(ui, ui->page, ui->page_draw_ud);
    }

    win->current_color = oc;
    win->font_size = ofs;
    win->alpha = oa;
}

int ag_ui_handle(ag_ui *ui, const ag_event *ev)
{
    if (!ui || !ev) return 0;

    if (ev->type == AG_EVENT_MOUSE_MOVE) {
        struct ag_widget *h = ag_ui_widget_at(ui, ev->x, ev->y);
        for (struct ag_widget *w = ui->head; w; w = w->next)
            w->hovered = (w == h);

        if (ui->pressed && ui->pressed->kind == AG_WIDGET_VIDEO_T &&
            ui->pressed->u.video.seek_drag && !ui->pressed->removed) {
            ui_video_seek_at(ui->pressed, ev->x);
            return 1;
        }

        if (ui->pressed && ui->pressed->kind == AG_WIDGET_SLIDER_T &&
            !ui->pressed->removed) {
            struct ag_widget *s = ui->pressed;
            int tw = s->w - UI_THUMB_W;
            float t = tw > 0 ? (float)(ev->x - s->x) / tw : 0.0f;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            s->u.slider.val = s->u.slider.vmin +
                              t * (s->u.slider.vmax - s->u.slider.vmin);
            if (s->cb) s->cb(s, s->ud);
            return 1;
        }

        if (ui->pressed && ui->pressed->kind == AG_WIDGET_TEXTVIEW_T &&
            ui->pressed->u.textview.sb_drag && !ui->pressed->removed) {
            ag_widget *tv = ui->pressed;
            int ms = tv_scroll_max(ui, tv);
            if (ms > 0) {
                float t = (float)(ev->y - tv->y) / tv->h;
                if (t < 0.0f) t = 0.0f;
                if (t > 1.0f) t = 1.0f;
                tv->u.textview.scroll = (int)(t * ms + 0.5f);
                tv->u.textview.pinned = (tv->u.textview.scroll >= ms);
                return 1;
            }
        }
        return h != NULL;
    }

    if (ev->type == AG_EVENT_MOUSE_WHEEL) {
        struct ag_widget *h = ag_ui_widget_at(ui, ev->x, ev->y);
        if (h && h->kind == AG_WIDGET_TEXTVIEW_T) {
            int ms = tv_scroll_max(ui, h);
            h->u.textview.scroll -= ev->wheel_delta;
            if (h->u.textview.scroll < 0) h->u.textview.scroll = 0;
            if (h->u.textview.scroll > ms) h->u.textview.scroll = ms;
            h->u.textview.pinned = (h->u.textview.scroll >= ms);
            return 1;
        }
        return 0;
    }

    if (ev->type == AG_EVENT_MOUSE_DOWN) {
        struct ag_widget *h = ag_ui_widget_at(ui, ev->x, ev->y);
        if (ui->focused && ui->focused != h) ui->focused->focused = 0;
        if (!h) { ui->focused = NULL; ui->pressed = NULL; return 0; }
        ui->focused = h;
        ui->pressed = h;
        h->focused = 1;
        h->pressed = 1;
        if (h->kind == AG_WIDGET_TEXTVIEW_T &&
            ev->x >= h->x + h->w - AG_WIDGET_SB_W) {
            h->u.textview.sb_drag = 1;
            int ms = tv_scroll_max(ui, h);
            if (ms > 0) {
                float t = (float)(ev->y - h->y) / h->h;
                if (t < 0.0f) t = 0.0f;
                if (t > 1.0f) t = 1.0f;
                h->u.textview.scroll = (int)(t * ms + 0.5f);
                h->u.textview.pinned = (h->u.textview.scroll >= ms);
            }
        }
        if (h->kind == AG_WIDGET_VIDEO_T) {
            int bar = h->h > UI_VIDEO_BAR * 2 ? UI_VIDEO_BAR : 0;
            int by = h->y + h->h - bar;
            int bx = h->x + 6;
            if (bar > 0 && ev->y >= by) {
                if (ev->x >= bx && ev->x < bx + 22) {
                    ag_video_widget_play(h, !ag_video_widget_playing(h));
                    if (h->cb) h->cb(h, h->ud);
                } else {
                    h->u.video.seek_drag = 1;
                    ui_video_seek_at(h, ev->x);
                }
            }
            return 1;
        }
        return 1;
    }

    if (ev->type == AG_EVENT_MOUSE_UP) {
        struct ag_widget *p = ui->pressed;
        if (!p) return 0;
        ui->pressed = NULL;
        if (p->removed) return 1;
        p->pressed = 0;
        if (p->kind == AG_WIDGET_TEXTVIEW_T) p->u.textview.sb_drag = 0;
        if (p->kind == AG_WIDGET_VIDEO_T)  p->u.video.seek_drag = 0;
        if (ag_ui_widget_at(ui, ev->x, ev->y) == p) {
            switch (p->kind) {
            case AG_WIDGET_BUTTON_T:
                if (p->cb) p->cb(p, p->ud);
                break;
            case AG_WIDGET_TOGGLE_T:
                p->u.toggle.on = !p->u.toggle.on;
                if (p->cb) p->cb(p, p->ud);
                break;
            default:
                if (p->cb && (p->kind == AG_WIDGET_SLIDER_T  ||
                              p->kind == AG_WIDGET_IMAGE_T   ||
                              p->kind == AG_WIDGET_VIDEO_T))
                    p->cb(p, p->ud);
                break;
            }
            return 1;
        }
        return 1;
    }

    if (ev->type == AG_EVENT_KEY_DOWN) {
        ag_keyctl kc = ag_backend_keyctl(ev->key);
        struct ag_widget *f = ui->focused;

        if (kc == AGK_TAB) {
            struct ag_widget *w = f ? f->next : ui->head;
            if (!w) w = ui->head;
            if (w) {
                if (f) f->focused = 0;
                ui->focused = w;
                w->focused = 1;
            }
            return 1;
        }

        if (f && !f->removed && f->kind == AG_WIDGET_TEXTBOX_T) {
            switch (kc) {
            case AGK_BACKSPACE: ag_tb_erase(f, 1); if (f->cb) f->cb(f, f->ud); return 1;
            case AGK_DELETE:    ag_tb_erase(f, 0); if (f->cb) f->cb(f, f->ud); return 1;
            case AGK_LEFT: case AGK_RIGHT: case AGK_HOME: case AGK_END:
                ag_tb_move(f, kc); return 1;
            case AGK_RETURN:
            f->u.textbox.submit = 1;
            if (f->cb) f->cb(f, f->ud);
            return 1;
            case AGK_ESC:       f->focused = 0; ui->focused = NULL; return 1;
            default: break;
            }
        }
        return f != NULL && f == ui->focused ? (kc != AGK_NONE) : 0;
    }

    if (ev->type == AG_EVENT_CHAR) {
        struct ag_widget *f = ui->focused;
        if (f && !f->removed && f->kind == AG_WIDGET_TEXTBOX_T) {
            ag_u32 cp = (ag_u32)ev->key;
            ag_tb_insert(f, cp);
            if (f->cb) f->cb(f, f->ud);
            return 1;
        }
        return 0;
    }

    if (ev->type == AG_EVENT_RESIZE) {
        if (ui->resize_cb)
            ui->resize_cb(ui, ev->width, ev->height, ui->resize_ud);
        return 1;
    }

    return 0;
}

#ifdef _WIN32

static int ag_font_char_width(const ag_font *f, ag_u32 cp)
{
    if (!f) return 0;
    const struct ag_glyph *g = ag_font_glyph((ag_font *)f, cp);
    if (!g) return font_height(f) / 2;
    return g->adv;
}

static int ag_backend_glyph(ag_font *f, ag_u32 cp, struct ag_glyph *g)
{
    if (!f || !f->dc || !f->hfont || cp > 0xFFFF) return -1;
    if (cp < 0x20) return -1;
    MAT2 mat = {{0,1},{0,0},{0,0},{0,1}};
    GLYPHMETRICS gm;
    SelectObject(f->dc, f->hfont);
    DWORD sz = GetGlyphOutlineW(f->dc, (UINT)cp, GGO_GRAY8_BITMAP, &gm, 0, NULL, &mat);
    if (sz == GDI_ERROR || sz == 0) return -1;
    unsigned char *buf = (unsigned char *)malloc(sz);
    if (!buf) return -1;
    if (GetGlyphOutlineW(f->dc, (UINT)cp, GGO_GRAY8_BITMAP, &gm, sz, buf, &mat) == GDI_ERROR) {
        free(buf);
        return -1;
    }
    g->adv  = gm.gmCellIncX;
    g->xoff = gm.gmptGlyphOrigin.x;
    g->yoff = -gm.gmptGlyphOrigin.y;
    g->w    = (int)gm.gmBlackBoxX;
    g->h    = (int)gm.gmBlackBoxY;
    if (g->w <= 0 || g->h <= 0) { free(buf); g->bits = NULL; return 0; }
    int pitch = ((g->w + 3) / 4) * 4;
    g->bits = (ag_u8 *)malloc(g->w * g->h);
    if (!g->bits) { free(buf); return -1; }
    for (int y = 0; y < g->h; ++y)
        for (int x = 0; x < g->w; ++x)
            g->bits[y * g->w + x] = (ag_u8)((buf[y * pitch + x] * 255) / 64);
    free(buf);
    return 0;
}

static void ag_draw_text_font(ag_window *w, int x, int y, ag_font *f, const char *text)
{
    if (!w || !text) return;
    if (!f) { ag_draw_text8x8(w, x, y, text); return; }

    ag_u32 rgb = w->current_color;
    int baseline = y + f->ascent;
    int penx = x;
    const char *p = text;
    while (*p) {
        ag_u32 cp = utf8_decode(&p);
        if (!cp) break;
        if (cp == '\n') { baseline += font_height(f); penx = x; continue; }
        const struct ag_glyph *g = ag_font_glyph(f, cp);
        if (!g) { penx += font_height(f) / 2; continue; }
        draw_glyph_bits(w, penx, baseline, g, rgb);
        penx += g->adv;
    }
}

static void ag_utf8_to_wide(const char *s, wchar_t *out, int cap)
{
    int o = 0;
    if (!s) { if (cap > 0) out[0] = 0; return; }
    while (*s && o < cap - 1) {
        ag_u32 cp = utf8_decode(&s);
        if (cp < 0x10000) out[o++] = (wchar_t)cp;
    }
    out[o] = 0;
}

static int ag_win32_make_font(ag_font *f, const wchar_t *family, int px)
{
    HFONT hf = CreateFontW(-(int)px, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, family);
    if (!hf) return 0;
    if (!f->dc) {
        f->dc = CreateCompatibleDC(NULL);
        if (!f->dc) { DeleteObject(hf); return 0; }
    }
    SelectObject(f->dc, hf);
    if (f->hfont) DeleteObject(f->hfont);
    f->hfont = hf;
    TEXTMETRICW tm;
    GetTextMetricsW(f->dc, &tm);
    f->ascent  = tm.tmAscent;
    f->descent = tm.tmDescent;
    return 1;
}

ag_font *ag_font_open(const char *name, unsigned int px)
{
    if (!name || !px) return NULL;
    if (ag_is_font_file(name)) return ag_font_open_file(name, px);

    ag_font *f = (ag_font *)ag_mem_alloc(sizeof(ag_font));
    if (!f) return NULL;
    f->px = px;

    wchar_t wname[256] = L"";
    ag_utf8_to_wide(name, wname, 256);

    if (!ag_win32_make_font(f, wname, px)) { ag_mem_free(f); return NULL; }
    snprintf(f->family, sizeof(f->family), "%s", name);
    return f;
}

ag_font *ag_font_open_file(const char *path, unsigned int px)
{
    if (!path || !px) return NULL;

    char family[64];
    if (!ag_ttf_family(path, family, sizeof(family))) return NULL;

    wchar_t wpath[1024] = L"";
    ag_utf8_to_wide(path, wpath, 1024);
    AddFontResourceExW(wpath, FR_PRIVATE, NULL);

    ag_font *f = (ag_font *)ag_mem_alloc(sizeof(ag_font));
    if (!f) return NULL;
    f->px = px;

    wchar_t wfam[256] = L"";
    ag_utf8_to_wide(family, wfam, 256);
    if (!ag_win32_make_font(f, wfam, px)) { ag_mem_free(f); return NULL; }
    snprintf(f->family, sizeof(f->family), "%s", family);
    return f;
}

ag_font *ag_font_default(unsigned int px)
{
    ag_font *f = ag_font_default_common(px);
    if (f) return f;
    f = ag_font_open("Segoe UI", px);
    if (!f) f = ag_font_open("Arial", px);
    return f;
}

const char *ag_font_family(const ag_font *f)
{
    return f ? f->family : "";
}

int ag_font_set_size(ag_font *f, unsigned int px)
{
    if (!f || !px) return 0;
    if (f->px == px) return 1;
    wchar_t wfam[256] = L"";
    ag_utf8_to_wide(f->family, wfam, 256);
    if (!ag_win32_make_font(f, wfam, (int)px)) return 0;
    f->px = px;
    ag_font_clear_cache(f);
    return 1;
}

void ag_font_close(ag_font *f)
{
    if (!f) return;
    for (int i = 0; i < AG_GLYPH_CACHE; ++i)
        free(f->cache[i].bits);
    if (f->dc)  DeleteDC(f->dc);
    if (f->hfont) DeleteObject(f->hfont);
    ag_mem_free(f);
}

int ag_font_ascent(const ag_font *f)  { return f ? f->ascent : 0; }
int ag_font_descent(const ag_font *f) { return f ? f->descent : 0; }
int ag_font_height(const ag_font *f)  { return f ? (f->ascent + f->descent) : 0; }

static int ag_backend_keyctl(int raw)
{
    switch (raw) {
    case VK_BACK:    return AGK_BACKSPACE;
    case VK_DELETE:  return AGK_DELETE;
    case VK_LEFT:    return AGK_LEFT;
    case VK_RIGHT:   return AGK_RIGHT;
    case VK_HOME:    return AGK_HOME;
    case VK_END:     return AGK_END;
    case VK_RETURN:  return AGK_RETURN;
    case VK_TAB:     return AGK_TAB;
    case VK_UP:      return AGK_UP;
    case VK_DOWN:    return AGK_DOWN;
    case VK_ESCAPE:  return AGK_ESC;
    default:         return AGK_NONE;
    }
}

static LRESULT CALLBACK ag_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    ag_window *w = (ag_window *)GetWindowLongPtr(hwnd, GWLP_USERDATA);

    if (msg == WM_NCCREATE) {
        CREATESTRUCT *cs = (CREATESTRUCT *)lp;
        w = (ag_window *)cs->lpCreateParams;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)w);
        w->hwnd = hwnd;
        return 1;
    }
    if (!w) return DefWindowProc(hwnd, msg, wp, lp);

    ag_event ev; memset(&ev, 0, sizeof(ev));

    switch (msg) {
    case WM_CLOSE:
        ev.type = AG_EVENT_CLOSE;
        if (!w->cb || w->cb(w, &ev, w->userdata) != 1) {
            w->should_close = 1;
            PostQuitMessage(0);
        }
        return 0;

    case WM_SIZE: {
        int nw = LOWORD(lp), nh = HIWORD(lp);
        ev.type = AG_EVENT_RESIZE;
        ev.width  = nw;
        ev.height = nh;
        backbuf_resize(w, nw, nh);

        if (w->memdc) {
            SelectObject(w->memdc, w->oldbmp);
            DeleteObject(w->membmp);
            DeleteDC(w->memdc);
            w->memdc = NULL;
        }
        if (nw > 0 && nh > 0) {
            HDC hdc = GetDC(hwnd);
            w->memdc = CreateCompatibleDC(hdc);
            if (w->memdc) {
                w->membmp = CreateCompatibleBitmap(hdc, nw, nh);
                w->oldbmp = (HBITMAP)SelectObject(w->memdc, w->membmp);
            }
            ReleaseDC(hwnd, hdc);
        }
        if (w->cb) w->cb(w, &ev, w->userdata);
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (w->memdc) {
            BitBlt(hdc, 0, 0, w->width, w->height, w->memdc, 0, 0, SRCCOPY);
        }
        EndPaint(hwnd, &ps);
        ev.type = AG_EVENT_EXPOSE;
        if (w->cb) w->cb(w, &ev, w->userdata);
        return 0;
    }

    case WM_LBUTTONDOWN: case WM_MBUTTONDOWN: case WM_RBUTTONDOWN:
        ev.type = AG_EVENT_MOUSE_DOWN;
        ev.x = ev.mouse_x = GET_X_LPARAM(lp);
        ev.y = ev.mouse_y = GET_Y_LPARAM(lp);
        ev.button = (msg == WM_LBUTTONDOWN) ? 1 :
                    (msg == WM_MBUTTONDOWN) ? 2 : 3;
        if (w->cb) w->cb(w, &ev, w->userdata);
        return 0;

    case WM_LBUTTONUP: case WM_MBUTTONUP: case WM_RBUTTONUP:
        ev.type = AG_EVENT_MOUSE_UP;
        ev.x = ev.mouse_x = GET_X_LPARAM(lp);
        ev.y = ev.mouse_y = GET_Y_LPARAM(lp);
        ev.button = (msg == WM_LBUTTONUP) ? 1 :
                    (msg == WM_MBUTTONUP) ? 2 : 3;
        if (w->cb) w->cb(w, &ev, w->userdata);
        return 0;

    case WM_MOUSEMOVE:
        ev.type = AG_EVENT_MOUSE_MOVE;
        ev.x = ev.mouse_x = GET_X_LPARAM(lp);
        ev.y = ev.mouse_y = GET_Y_LPARAM(lp);
        if (w->cb) w->cb(w, &ev, w->userdata);
        return 0;

    case WM_MOUSEWHEEL: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &pt);
        int delta = GET_WHEEL_DELTA_WPARAM(wp);   
        ev.type = AG_EVENT_MOUSE_WHEEL;
        ev.x = ev.mouse_x = pt.x;
        ev.y = ev.mouse_y = pt.y;
        ev.wheel_delta = delta >= 0 ? 1 : -1;
        if (w->cb) w->cb(w, &ev, w->userdata);
        return 0;
    }

    case WM_KEYDOWN:
        ev.type = AG_EVENT_KEY_DOWN;
        ev.key  = (int)wp;
        if (w->cb) w->cb(w, &ev, w->userdata);
        return 0;

    case WM_KEYUP:
        ev.type = AG_EVENT_KEY_UP;
        ev.key  = (int)wp;
        if (w->cb) w->cb(w, &ev, w->userdata);
        return 0;

    case WM_CHAR: {
        int u16 = (int)wp;
        ev.type = AG_EVENT_CHAR;
        if (u16 >= 0x80 && u16 <= 0xFF) {
            char b = (char)u16;
            wchar_t wc = 0;
            if (MultiByteToWideChar(CP_ACP, MB_PRECOMPOSED, &b, 1, &wc, 1) > 0)
                u16 = (int)wc;
        }
        if (u16 >= 0xD800 && u16 <= 0xDBFF) {    
            w->pending_high = u16;
            return 0;
        }
        if (u16 >= 0xDC00 && u16 <= 0xDFFF) {    
            if (w->pending_high) {
                int hi = w->pending_high;
                w->pending_high = 0;
                ev.key = 0x10000 + ((hi - 0xD800) << 10) + (u16 - 0xDC00);
            } else ev.key = 0xFFFD;
        } else {
            w->pending_high = 0;
            ev.key = u16;
        }
        if (w->cb) w->cb(w, &ev, w->userdata);
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int ag_init(void)
{
    setlocale(LC_ALL, "");      
    WNDCLASSA wc; memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = ag_wndproc;
    wc.hInstance     = GetModuleHandle(NULL);
    wc.lpszClassName = "AGLibWindowClass";
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    if (!RegisterClassA(&wc)) return -1;
    return 0;
}

void ag_shutdown(void) {  }

const char *ag_get_platform(void) { return "win32"; }

ag_window *ag_window_create(const char *title, int width, int height)
{
    ag_window *w = (ag_window *)ag_mem_alloc(sizeof(ag_window));
    if (!w) return NULL;

    w->width = width; w->height = height;
    w->current_color = AG_WHITE;
    w->font_size = 16;
    w->alpha     = 255;
    w->clip_on = 0;
    strncpy(w->title, title ? title : "AGLib", sizeof(w->title) - 1);
    w->title[sizeof(w->title) - 1] = '\0';

    DWORD style = WS_OVERLAPPEDWINDOW;
    RECT r = {0, 0, width, height};
    AdjustWindowRect(&r, style, FALSE);

    w->hwnd = CreateWindowExA(0, "AGLibWindowClass", w->title, style,
                              CW_USEDEFAULT, CW_USEDEFAULT,
                              r.right - r.left, r.bottom - r.top,
                              NULL, NULL, GetModuleHandle(NULL), w);
    if (!w->hwnd) { ag_mem_free(w); return NULL; }

    backbuf_resize(w, width, height);

    HDC hdc = GetDC(w->hwnd);
    w->memdc = CreateCompatibleDC(hdc);
    w->membmp = CreateCompatibleBitmap(hdc, width, height);
    w->oldbmp = (HBITMAP)SelectObject(w->memdc, w->membmp);
    ReleaseDC(w->hwnd, hdc);

    w->next = g_windows;
    g_windows = w;
    return w;
}

void ag_window_destroy(ag_window *w)
{
    if (!w) return;

    ag_window **pp = &g_windows;
    while (*pp && *pp != w) pp = &(*pp)->next;
    if (*pp) *pp = w->next;

    if (w->memdc) {
        SelectObject(w->memdc, w->oldbmp);
        DeleteDC(w->memdc);
        DeleteObject(w->membmp);
    }
    if (w->hwnd) DestroyWindow(w->hwnd);
    free(w->backbuf);
    ag_mem_free(w);
}

void ag_window_set_title(ag_window *w, const char *title)
{
    if (!w || !title) return;
    strncpy(w->title, title, sizeof(w->title) - 1);
    w->title[sizeof(w->title) - 1] = '\0';
    SetWindowTextA(w->hwnd, title);
}

void ag_window_set_size(ag_window *w, int width, int height)
{
    if (!w) return;
    SetWindowPos(w->hwnd, NULL, 0, 0, width, height,
                 SWP_NOMOVE | SWP_NOZORDER);
}

void ag_window_get_size(ag_window *w, int *width, int *height)
{
    if (!w) return;
    if (width)  *width  = w->width;
    if (height) *height = w->height;
}

void ag_window_set_callback(ag_window *w, ag_event_cb cb, void *userdata)
{
    if (!w) return;
    w->cb = cb; w->userdata = userdata;
}

void ag_window_show(ag_window *w) { if (w) ShowWindow(w->hwnd, SW_SHOW); }
void ag_window_hide(ag_window *w) { if (w) ShowWindow(w->hwnd, SW_HIDE); }

int ag_poll_events(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return 0;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    for (ag_window *w = g_windows; w; w = w->next) {
        if (w->ui) ag_ui_update(w->ui);
        if (w->redraw_pending) {     
            w->redraw_pending = 0;
            ag_event ev; memset(&ev, 0, sizeof(ev));
            ev.type = AG_EVENT_EXPOSE;
            if (w->cb) w->cb(w, &ev, w->userdata);
        }
    }
    return 1;
}

void ag_main_loop(void)
{
    g_running = 1;
    while (g_running) {
        if (!ag_poll_events()) break;
        ag_sleep_ms(1);
    }
}

void ag_quit(void) { g_running = 0; ag_quit_windows(); PostQuitMessage(0); }

void ag_begin_frame(ag_window *w) { (void)w; }

void ag_end_frame(ag_window *w)
{
    if (!w || !w->memdc) return;

    BITMAPINFO bi; memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w->width;
    bi.bmiHeader.biHeight      = -w->height; 
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    SetDIBits(w->memdc, w->membmp, 0, w->height, w->backbuf, &bi, DIB_RGB_COLORS);

    HDC hdc = GetDC(w->hwnd);
    BitBlt(hdc, 0, 0, w->width, w->height, w->memdc, 0, 0, SRCCOPY);
    ReleaseDC(w->hwnd, hdc);
}

#else

static FT_Library g_ft_lib = NULL;

ag_font *ag_font_open(const char *path, unsigned int px)
{
    if (!path || !px) return NULL;
    if (!g_ft_lib) {
        if (FT_Init_FreeType(&g_ft_lib)) return NULL;
    }
    ag_font *f = (ag_font *)ag_mem_alloc(sizeof(ag_font));
    if (!f) return NULL;
    f->px = px;
    if (FT_New_Face(g_ft_lib, path, 0, &f->face)) { ag_mem_free(f); return NULL; }
    FT_Set_Pixel_Sizes(f->face, 0, px);

    f->ascent  = (int)((FT_MulFix(f->face->ascender, f->face->size->metrics.y_scale) + 32) / 64);
    int th      = (int)((FT_MulFix(f->face->height,  f->face->size->metrics.y_scale) + 32) / 64);
    f->descent = f->ascent - th;
    ag_ttf_family(path, f->family, (int)sizeof(f->family));
    if (!f->family[0]) {
        const char *sl = strrchr(path, '/');
        snprintf(f->family, sizeof(f->family), "%s", sl ? sl + 1 : path);
    }
    return f;
}

ag_font *ag_font_open_file(const char *path, unsigned int px)
{
    return ag_font_open(path, px);
}

ag_font *ag_font_default(unsigned int px)
{
    return ag_font_default_common(px);
}

const char *ag_font_family(const ag_font *f)
{
    return f ? f->family : "";
}

int ag_font_set_size(ag_font *f, unsigned int px)
{
    if (!f || !px) return 0;
    if (f->px == px) return 1;
    if (FT_Set_Pixel_Sizes(f->face, 0, px)) return 0;
    f->px = px;
    f->ascent  = (int)((FT_MulFix(f->face->ascender, f->face->size->metrics.y_scale) + 32) / 64);
    int th      = (int)((FT_MulFix(f->face->height,  f->face->size->metrics.y_scale) + 32) / 64);
    f->descent = f->ascent - th;
    ag_font_clear_cache(f);
    return 1;
}

void ag_font_close(ag_font *f)
{
    if (!f) return;
    for (int i = 0; i < AG_GLYPH_CACHE; ++i)
        free(f->cache[i].bits);
    FT_Done_Face(f->face);
    ag_mem_free(f);
}

int ag_font_ascent(const ag_font *f)  { return f ? f->ascent : 0; }
int ag_font_descent(const ag_font *f) { return f ? f->descent : 0; }
int ag_font_height(const ag_font *f)  { return f ? (f->ascent - f->descent) : 0; }

static int ag_backend_glyph(ag_font *f, ag_u32 cp, struct ag_glyph *g)
{
    FT_UInt idx = FT_Get_Char_Index(f->face, cp);
    if (!idx) return -1;
    if (FT_Load_Glyph(f->face, idx, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL)) return -1;
    FT_GlyphSlot s = f->face->glyph;
    if (s->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY &&
        s->bitmap.pixel_mode != FT_PIXEL_MODE_MONO) return -1;

    g->adv  = (int)s->advance.x / 64;
    g->xoff = s->bitmap_left;
    g->yoff = -s->bitmap_top;
    g->w    = s->bitmap.width;
    g->h    = s->bitmap.rows;
    if (g->w <= 0 || g->h <= 0) { g->bits = NULL; return 0; }

    g->bits = (ag_u8 *)malloc(g->w * g->h);
    if (!g->bits) return -1;
    if (s->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
        for (int y = 0; y < g->h; ++y)
            memcpy(g->bits + y * g->w, s->bitmap.buffer + y * s->bitmap.pitch, g->w);
    } else {
        for (int y = 0; y < g->h; ++y)
            for (int x = 0; x < g->w; ++x) {
                unsigned char byte = s->bitmap.buffer[y * s->bitmap.pitch + x / 8];
                g->bits[y * g->w + x] = (byte & (0x80 >> (x & 7))) ? 255 : 0;
            }
    }
    return 0;
}

static int ag_font_char_width(const ag_font *f, ag_u32 cp)
{
    if (!f) return 0;
    const struct ag_glyph *g = ag_font_glyph((ag_font *)f, cp);
    if (!g) return font_height(f) / 2;
    return g->adv;
}

static void ag_draw_text_font(ag_window *w, int x, int y, ag_font *f, const char *text)
{
    if (!w || !text) return;
    if (!f) { ag_draw_text8x8(w, x, y, text); return; }

    ag_u32 rgb = w->current_color;
    int baseline = y + f->ascent;
    int penx = x;
    const char *p = text;
    while (*p) {
        ag_u32 cp = utf8_decode(&p);
        if (!cp) break;
        if (cp == '\n') { baseline += f->ascent - f->descent; penx = x; continue; }
        const struct ag_glyph *g = ag_font_glyph(f, cp);
        if (!g) { penx += font_height(f) / 2; continue; }
        draw_glyph_bits(w, penx, baseline, g, rgb);
        penx += g->adv;
    }
}

#ifndef AG_USE_WAYLAND

static int ag_backend_keyctl(int raw)
{
    switch (raw) {
    case XK_BackSpace:  return AGK_BACKSPACE;
    case XK_Delete:     return AGK_DELETE;
    case XK_Left:       return AGK_LEFT;
    case XK_Right:      return AGK_RIGHT;
    case XK_Home:       return AGK_HOME;
    case XK_End:        return AGK_END;
    case XK_Return:     return AGK_RETURN;
    case XK_Tab:        return AGK_TAB;
    case XK_Up:         return AGK_UP;
    case XK_Down:       return AGK_DOWN;
    case XK_Escape:     return AGK_ESC;
    default:            return AGK_NONE;
    }
}

int ag_init(void)
{
    setlocale(LC_ALL, "");
    if (XSupportsLocale()) {
        XSetLocaleModifiers("");
    }
    return 0;
}

void ag_shutdown(void) { }

const char *ag_get_platform(void) { return "x11"; }

ag_window *ag_window_create(const char *title, int width, int height)
{
    ag_window *w = (ag_window *)ag_mem_alloc(sizeof(ag_window));
    if (!w) return NULL;

    w->dpy = XOpenDisplay(NULL);
    if (!w->dpy) { ag_mem_free(w); return NULL; }

    w->screen = DefaultScreen(w->dpy);
    w->width = width; w->height = height;
    w->current_color = AG_WHITE;
    w->font_size = 16;
    w->alpha     = 255;
    w->clip_on = 0;
    strncpy(w->title, title ? title : "AGLib", sizeof(w->title) - 1);
    w->title[sizeof(w->title) - 1] = '\0';

    void *visual = NULL;
    int depth = 0;
    if (ag_gl_x11_visual(w->dpy, w->screen, &visual, &depth)) {
        XVisualInfo *vi = (XVisualInfo *)visual;
        Colormap cmap = XCreateColormap(w->dpy, RootWindow(w->dpy, w->screen),
                                        vi->visual, AllocNone);
        XSetWindowAttributes swa;
        memset(&swa, 0, sizeof(swa));
        swa.colormap = cmap;
        swa.border_pixel = 0;
        swa.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask |
                         ButtonPressMask | ButtonReleaseMask |
                         PointerMotionMask | StructureNotifyMask;
        w->win = XCreateWindow(w->dpy, RootWindow(w->dpy, w->screen),
                               0, 0, width, height, 0, vi->depth,
                               InputOutput, vi->visual,
                               CWColormap | CWBorderPixel | CWEventMask, &swa);
        w->gl_visual = vi;
        w->gl_cmap = cmap;
    }
    if (!w->win) {
        w->win = XCreateSimpleWindow(w->dpy, RootWindow(w->dpy, w->screen),
                                     0, 0, width, height, 0,
                                     BlackPixel(w->dpy, w->screen),
                                     BlackPixel(w->dpy, w->screen));
    }

    XStoreName(w->dpy, w->win, w->title);

    XSelectInput(w->dpy, w->win,
                 ExposureMask | KeyPressMask | KeyReleaseMask |
                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                 StructureNotifyMask);

    w->wm_delete = XInternAtom(w->dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(w->dpy, w->win, &w->wm_delete, 1);

    w->gc = XCreateGC(w->dpy, w->win, 0, NULL);

    backbuf_resize(w, width, height);
    w->img = XCreateImage(w->dpy, DefaultVisual(w->dpy, w->screen),
                          DefaultDepth(w->dpy, w->screen),
                          ZPixmap, 0, (char *)w->backbuf,
                          width, height, 32, 0);
    if (!w->img) { XCloseDisplay(w->dpy); ag_mem_free(w); return NULL; }
    w->img->byte_order       = LSBFirst;
    w->img->bitmap_bit_order = LSBFirst;

    w->xim = NULL;
    w->xic = NULL;
    if (XSupportsLocale()) {
        w->xim = XOpenIM(w->dpy, NULL, NULL, NULL);
        if (w->xim) {
            w->xic = XCreateIC(w->xim,
                               XNInputStyle,
                               XIMPreeditNothing | XIMStatusNothing,
                               XNClientWindow, w->win,
                               NULL);
        }
    }

    w->next = g_windows;
    g_windows = w;
    return w;
}

void ag_window_destroy(ag_window *w)
{
    if (!w) return;

    ag_window **pp = &g_windows;
    while (*pp && *pp != w) pp = &(*pp)->next;
    if (*pp) *pp = w->next;

    if (w->img) {
        w->img->data = NULL;
        XDestroyImage(w->img);
    }
    if (w->xic) XDestroyIC(w->xic);
    if (w->xim) XCloseIM(w->xim);
    if (w->gc)  XFreeGC(w->dpy, w->gc);
    if (w->win) XDestroyWindow(w->dpy, w->win);
    if (w->dpy) XCloseDisplay(w->dpy);
    free(w->backbuf);
    ag_mem_free(w);
}

void ag_window_set_title(ag_window *w, const char *title)
{
    if (!w || !title) return;
    strncpy(w->title, title, sizeof(w->title) - 1);
    w->title[sizeof(w->title) - 1] = '\0';
    XStoreName(w->dpy, w->win, title);
}

void ag_window_set_size(ag_window *w, int width, int height)
{
    if (!w) return;
    XResizeWindow(w->dpy, w->win, width, height);
}

void ag_window_get_size(ag_window *w, int *width, int *height)
{
    if (!w) return;
    if (width)  *width  = w->width;
    if (height) *height = w->height;
}

void ag_window_set_callback(ag_window *w, ag_event_cb cb, void *userdata)
{
    if (!w) return;
    w->cb = cb; w->userdata = userdata;
}

void ag_window_show(ag_window *w) { if (w) XMapWindow(w->dpy, w->win); }
void ag_window_hide(ag_window *w) { if (w) XUnmapWindow(w->dpy, w->win); }

static ag_window *find_window_by_xid(Window xid)
{
    for (ag_window *w = g_windows; w; w = w->next)
        if (w->win == xid) return w;
    return NULL;
}

static void ag_recreate_image(ag_window *w)
{
    if (w->img) {
        w->img->data = NULL;
        XDestroyImage(w->img);
    }
    w->img = XCreateImage(w->dpy, DefaultVisual(w->dpy, w->screen),
                          DefaultDepth(w->dpy, w->screen),
                          ZPixmap, 0, (char *)w->backbuf,
                          w->width, w->height, 32, 0);
    if (w->img) {
        w->img->byte_order       = LSBFirst;
        w->img->bitmap_bit_order = LSBFirst;
    }
}

int ag_poll_events(void)
{
    for (ag_window *w = g_windows; w; ) {
        ag_window *next = w->next;
        Display *dpy = w->dpy;

        while (XPending(dpy)) {
            XEvent xe;
            XNextEvent(dpy, &xe);

            ag_window *tw = find_window_by_xid(xe.xany.window);
            if (!tw) continue;

            ag_event ev; memset(&ev, 0, sizeof(ev));

            switch (xe.type) {
            case Expose:
                ev.type = AG_EVENT_EXPOSE;
                if (tw->cb) tw->cb(tw, &ev, tw->userdata);
                break;

            case ConfigureNotify:
                if (xe.xconfigure.width != tw->width ||
                    xe.xconfigure.height != tw->height) {
                    ev.type  = AG_EVENT_RESIZE;
                    ev.width = xe.xconfigure.width;
                    ev.height = xe.xconfigure.height;
                    backbuf_resize(tw, ev.width, ev.height);
                    ag_recreate_image(tw);
                    if (tw->cb) tw->cb(tw, &ev, tw->userdata);
                }
                break;

            case ClientMessage:
                if ((Atom)xe.xclient.data.l[0] == tw->wm_delete) {
                    ev.type = AG_EVENT_CLOSE;
                    if (!tw->cb || tw->cb(tw, &ev, tw->userdata) != 1)
                        tw->should_close = 1;
                }
                break;

            case ButtonPress:
                if (xe.xbutton.button == 4 || xe.xbutton.button == 5) {
                    ev.type = AG_EVENT_MOUSE_WHEEL;
                    ev.x = ev.mouse_x = xe.xbutton.x;
                    ev.y = ev.mouse_y = xe.xbutton.y;
                    ev.wheel_delta = (xe.xbutton.button == 4) ? 1 : -1;
                    if (tw->cb) tw->cb(tw, &ev, tw->userdata);
                    break;
                }
                ev.type   = AG_EVENT_MOUSE_DOWN;
                ev.x = ev.mouse_x = xe.xbutton.x;
                ev.y = ev.mouse_y = xe.xbutton.y;
                ev.button = xe.xbutton.button;
                if (tw->cb) tw->cb(tw, &ev, tw->userdata);
                break;

            case ButtonRelease:
                if (xe.xbutton.button == 4 || xe.xbutton.button == 5) break;
                ev.type   = AG_EVENT_MOUSE_UP;
                ev.x = ev.mouse_x = xe.xbutton.x;
                ev.y = ev.mouse_y = xe.xbutton.y;
                ev.button = xe.xbutton.button;
                if (tw->cb) tw->cb(tw, &ev, tw->userdata);
                break;

            case MotionNotify:
                ev.type   = AG_EVENT_MOUSE_MOVE;
                ev.x = ev.mouse_x = xe.xmotion.x;
                ev.y = ev.mouse_y = xe.xmotion.y;
                if (tw->cb) tw->cb(tw, &ev, tw->userdata);
                break;

            case KeyPress: {
                KeySym ks = XLookupKeysym(&xe.xkey, 0);
                ev.type = AG_EVENT_KEY_DOWN;
                ev.key  = (int)ks;
                if (tw->cb) tw->cb(tw, &ev, tw->userdata);

                ag_u32 cp = 0;
                {
                    char raw[64];
                    KeySym ksy = NoSymbol;
                    int n = 0;
                    if (w->xic)
                        n = Xutf8LookupString(w->xic, &xe.xkey, raw,
                                              (int)sizeof(raw) - 1, &ksy, NULL);
                    if (n <= 0) {
                        KeySym ksy2 = NoSymbol;
                        n = XLookupString(&xe.xkey, raw, (int)sizeof(raw) - 1,
                                          &ksy2, NULL);
                    }
                    if (n > 0) {
                        raw[n] = '\0';
                        const char *p = raw;
                        cp = utf8_decode(&p);
                        if (cp == 0xFFFD || cp < 0x20)
                            cp = (ag_u32)(unsigned char)raw[0];
                    }
                }
                if (cp) {
                    ag_event cev; memset(&cev, 0, sizeof(cev));
                    cev.type = AG_EVENT_CHAR;
                    cev.key  = (int)cp;
                    if (tw->cb) tw->cb(tw, &cev, tw->userdata);
                }
                break;
            }

            case KeyRelease: {
                KeySym ks = XLookupKeysym(&xe.xkey, 0);
                ev.type = AG_EVENT_KEY_UP;
                ev.key  = (int)ks;
                if (tw->cb) tw->cb(tw, &ev, tw->userdata);
                break;
            }

            default:
                break;
            }
        }
        if (w->ui) ag_ui_update(w->ui);
        if (w->redraw_pending) {
            w->redraw_pending = 0;
            ag_event ev; memset(&ev, 0, sizeof(ev));
            ev.type = AG_EVENT_EXPOSE;
            if (w->cb) w->cb(w, &ev, w->userdata);
        }
        w = next;
    }

    for (ag_window *w = g_windows; w; w = w->next)
        if (!w->should_close) return 1;

    return 0;
}

void ag_main_loop(void)
{
    g_running = 1;
    while (g_running) {
        if (!ag_poll_events()) break;
        ag_sleep_ms(1);
    }
}

void ag_quit(void) { g_running = 0; ag_quit_windows(); }

void ag_begin_frame(ag_window *w) { (void)w; }

void ag_end_frame(ag_window *w)
{
    if (!w) return;

    if (w->gl_visual) {
        if (!w->present)
            w->present = ag_gl_present_x11(w->dpy, w->win, w->gl_visual);
        if (w->present) {
            ag_gl_present_frame(w->present, w->backbuf, w->width, w->height,
                                w->layers, w->nlayers);
            w->nlayers = 0;
            return;
        }
    }

    if (!w->img) return;
    XPutImage(w->dpy, w->win, w->gc, w->img, 0, 0, 0, 0, w->width, w->height);
    XFlush(w->dpy);
}

#endif /* !AG_USE_WAYLAND */


#ifdef AG_USE_WAYLAND


static struct wl_display  *g_wl_display   = NULL;
static struct wl_registry *g_wl_registry  = NULL;
static struct wl_compositor *g_wl_compositor = NULL;
static struct wl_shm      *g_wl_shm       = NULL;
static struct xdg_wm_base *g_wl_wm_base   = NULL;
static struct zxdg_decoration_manager_v1 *g_wl_decoration = NULL;
static struct wl_seat     *g_wl_seat      = NULL;
static struct wl_pointer  *g_wl_pointer   = NULL;
static struct wl_keyboard *g_wl_keyboard  = NULL;

static struct xkb_context *g_xkb_ctx    = NULL;
static struct xkb_keymap  *g_xkb_keymap = NULL;
static struct xkb_state   *g_xkb_state  = NULL;

static ag_window *g_pointer_window  = NULL;
static ag_window *g_keyboard_window = NULL;
static int        g_pointer_x = 0, g_pointer_y = 0;

static int create_shm_fd(size_t size)
{
    char name[64];
    static unsigned counter = 0;
    int fd = -1;

    for (int tries = 0; tries < 64; ++tries) {
        snprintf(name, sizeof(name), "/aglib-%d-%u", (int)getpid(), counter++);
        fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) { shm_unlink(name); break; }
        if (errno != EEXIST) return -1;
    }
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)size) < 0) { close(fd); return -1; }
    return fd;
}

static void buffer_release(void *data, struct wl_buffer *buffer)
{
    ag_window *w = (ag_window *)data;
    for (int i = 0; i < 2; ++i)
        if (w->buf[i] == buffer) w->buf_busy[i] = 0;
}

static const struct wl_buffer_listener buffer_listener = {
    buffer_release
};

static int wayland_buffer_create(ag_window *w, int idx)
{
    int width  = w->width  > 0 ? w->width  : 1;
    int height = w->height > 0 ? w->height : 1;
    int stride = width * 4;
    size_t size = (size_t)stride * (size_t)height;

    int fd = create_shm_fd(size);
    if (fd < 0) return -1;

    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) { close(fd); return -1; }

    struct wl_shm_pool *pool = wl_shm_create_pool(g_wl_shm, fd, (int)size);
    w->buf[idx] = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
                                            WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    if (!w->buf[idx]) { munmap(data, size); return -1; }

    wl_buffer_add_listener(w->buf[idx], &buffer_listener, w);
    w->shm_data[idx] = data;
    w->shm_size[idx] = size;
    w->buf_busy[idx] = 0;
    return 0;
}

static void wayland_buffers_destroy(ag_window *w)
{
    for (int i = 0; i < 2; ++i) {
        if (w->buf[i]) { wl_buffer_destroy(w->buf[i]); w->buf[i] = NULL; }
        if (w->shm_data[i]) { munmap(w->shm_data[i], w->shm_size[i]); w->shm_data[i] = NULL; }
        w->shm_size[i] = 0;
        w->buf_busy[i] = 0;
    }
}

static void wayland_buffers_recreate(ag_window *w)
{
    wayland_buffers_destroy(w);
    w->cur_buf = 0;
    if (w->width > 0 && w->height > 0)
        for (int i = 0; i < 2; ++i)
            wayland_buffer_create(w, i);
}

static void wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    wm_base_ping
};

static void xdg_surface_configure(void *data, struct xdg_surface *surface,
                                  uint32_t serial)
{
    ag_window *w = (ag_window *)data;
    xdg_surface_ack_configure(surface, serial);

    if (w->pending_w > 0 && w->pending_h > 0 &&
        (w->pending_w != w->width || w->pending_h != w->height)) {
        int nw = w->pending_w, nh = w->pending_h;
        backbuf_resize(w, nw, nh);
        wayland_buffers_recreate(w);
        ag_event ev; memset(&ev, 0, sizeof(ev));
        ev.type = AG_EVENT_RESIZE;
        ev.width = nw; ev.height = nh;
        if (w->cb) w->cb(w, &ev, w->userdata);
    }
    w->pending_w = w->pending_h = 0;

    if (!w->configured) w->configured = 1;
    w->redraw_pending = 1;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    xdg_surface_configure
};

static void xdg_toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                                   int32_t width, int32_t height,
                                   struct wl_array *states)
{
    (void)toplevel; (void)states;
    ag_window *w = (ag_window *)data;
    if (width > 0 && height > 0) {
        w->pending_w = width;
        w->pending_h = height;
    }
}

static void xdg_toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    (void)toplevel;
    ag_window *w = (ag_window *)data;
    ag_event ev; memset(&ev, 0, sizeof(ev));
    ev.type = AG_EVENT_CLOSE;
    if (!w->cb || w->cb(w, &ev, w->userdata) != 1)
        w->should_close = 1;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_configure,
    .close     = xdg_toplevel_close,
};

static void decoration_configure(void *data,
                                 struct zxdg_toplevel_decoration_v1 *dec,
                                 uint32_t mode)
{
    (void)data; (void)dec; (void)mode;
}

static const struct zxdg_toplevel_decoration_v1_listener decoration_listener = {
    .configure = decoration_configure,
};

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy)
{
    (void)data; (void)pointer; (void)serial;
    g_pointer_window = (ag_window *)wl_proxy_get_user_data((struct wl_proxy *)surface);
    g_pointer_x = wl_fixed_to_int(sx);
    g_pointer_y = wl_fixed_to_int(sy);
}

static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface)
{
    (void)data; (void)pointer; (void)serial; (void)surface;
    g_pointer_window = NULL;
}

static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
                           wl_fixed_t sx, wl_fixed_t sy)
{
    (void)data; (void)pointer; (void)time;
    ag_window *w = g_pointer_window;
    g_pointer_x = wl_fixed_to_int(sx);
    g_pointer_y = wl_fixed_to_int(sy);
    if (!w || !w->cb) return;
    ag_event ev; memset(&ev, 0, sizeof(ev));
    ev.type = AG_EVENT_MOUSE_MOVE;
    ev.x = ev.mouse_x = g_pointer_x;
    ev.y = ev.mouse_y = g_pointer_y;
    w->cb(w, &ev, w->userdata);
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
                           uint32_t time, uint32_t button, uint32_t state)
{
    (void)data; (void)pointer; (void)serial; (void)time;
    ag_window *w = g_pointer_window;
    if (!w || !w->cb) return;
    ag_event ev; memset(&ev, 0, sizeof(ev));
    ev.type = (state == WL_POINTER_BUTTON_STATE_PRESSED)
              ? AG_EVENT_MOUSE_DOWN : AG_EVENT_MOUSE_UP;
    ev.x = ev.mouse_x = g_pointer_x;
    ev.y = ev.mouse_y = g_pointer_y;
    switch (button) {
    case 0x110: ev.button = 1; break;
    case 0x111: ev.button = 3; break;
    case 0x112: ev.button = 2; break;
    default:    ev.button = (int)button; break;
    }
    w->cb(w, &ev, w->userdata);
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
                         uint32_t axis, wl_fixed_t value)
{
    (void)data; (void)pointer; (void)time;
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) return;
    ag_window *w = g_pointer_window;
    if (!w || !w->cb) return;
    ag_event ev; memset(&ev, 0, sizeof(ev));
    ev.type = AG_EVENT_MOUSE_WHEEL;
    ev.x = ev.mouse_x = g_pointer_x;
    ev.y = ev.mouse_y = g_pointer_y;
    ev.wheel_delta = (value < 0) ? 1 : -1;
    w->cb(w, &ev, w->userdata);
}

static const struct wl_pointer_listener pointer_listener = {
    .enter  = pointer_enter,
    .leave  = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis   = pointer_axis,
};

static void keyboard_keymap(void *data, struct wl_keyboard *kb, uint32_t format,
                            int32_t fd, uint32_t size)
{
    (void)data; (void)kb;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }

    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return;
    if (!g_xkb_ctx) { munmap(map, size); return; }

    struct xkb_keymap *km = xkb_keymap_new_from_string(
        g_xkb_ctx, map, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    if (!km) return;

    struct xkb_state *st = xkb_state_new(km);
    if (!st) { xkb_keymap_unref(km); return; }

    if (g_xkb_state)  xkb_state_unref(g_xkb_state);
    if (g_xkb_keymap) xkb_keymap_unref(g_xkb_keymap);
    g_xkb_keymap = km;
    g_xkb_state  = st;
}

static void keyboard_enter(void *data, struct wl_keyboard *kb, uint32_t serial,
                           struct wl_surface *surface, struct wl_array *keys)
{
    (void)data; (void)kb; (void)serial; (void)keys;
    g_keyboard_window = (ag_window *)wl_proxy_get_user_data((struct wl_proxy *)surface);
}

static void keyboard_leave(void *data, struct wl_keyboard *kb, uint32_t serial,
                           struct wl_surface *surface)
{
    (void)data; (void)kb; (void)serial; (void)surface;
    g_keyboard_window = NULL;
}

static void keyboard_key(void *data, struct wl_keyboard *kb, uint32_t serial,
                         uint32_t time, uint32_t key, uint32_t state)
{
    (void)data; (void)kb; (void)serial; (void)time;
    ag_window *w = g_keyboard_window;
    if (!w || !w->cb || !g_xkb_state) return;

    uint32_t keycode = key + 8;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(g_xkb_state, keycode);

    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        ag_event ev; memset(&ev, 0, sizeof(ev));
        ev.type = AG_EVENT_KEY_DOWN;
        ev.key  = (int)sym;
        w->cb(w, &ev, w->userdata);

        char buf[32];
        int n = xkb_state_key_get_utf8(g_xkb_state, keycode, buf, sizeof(buf));
        if (n > 0) {
            const char *p = buf;
            ag_u32 cp = utf8_decode(&p);
            if (cp) {
                ag_event cev; memset(&cev, 0, sizeof(cev));
                cev.type = AG_EVENT_CHAR;
                cev.key  = (int)cp;
                w->cb(w, &cev, w->userdata);
            }
        }
    } else {
        ag_event ev; memset(&ev, 0, sizeof(ev));
        ev.type = AG_EVENT_KEY_UP;
        ev.key  = (int)sym;
        w->cb(w, &ev, w->userdata);
    }
}

static void keyboard_modifiers(void *data, struct wl_keyboard *kb, uint32_t serial,
                               uint32_t depressed, uint32_t latched,
                               uint32_t locked, uint32_t group)
{
    (void)data; (void)kb; (void)serial;
    if (g_xkb_state)
        xkb_state_update_mask(g_xkb_state, depressed, latched, locked, 0, 0, group);
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *kb,
                                 int32_t rate, int32_t delay)
{
    (void)data; (void)kb; (void)rate; (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap      = keyboard_keymap,
    .enter       = keyboard_enter,
    .leave       = keyboard_leave,
    .key         = keyboard_key,
    .modifiers   = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
    (void)data;

    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !g_wl_pointer) {
        g_wl_pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(g_wl_pointer, &pointer_listener, NULL);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && g_wl_pointer) {
        wl_pointer_destroy(g_wl_pointer);
        g_wl_pointer = NULL;
        g_pointer_window = NULL;
    }

    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !g_wl_keyboard) {
        g_wl_keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(g_wl_keyboard, &keyboard_listener, NULL);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && g_wl_keyboard) {
        wl_keyboard_destroy(g_wl_keyboard);
        g_wl_keyboard = NULL;
        g_keyboard_window = NULL;
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
    (void)data; (void)seat; (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name         = seat_name,
};

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface, uint32_t version)
{
    (void)data; (void)version;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        g_wl_compositor = (struct wl_compositor *)wl_registry_bind(
            registry, name, &wl_compositor_interface, 1);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        g_wl_shm = (struct wl_shm *)wl_registry_bind(
            registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        g_wl_wm_base = (struct xdg_wm_base *)wl_registry_bind(
            registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(g_wl_wm_base, &wm_base_listener, NULL);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        g_wl_seat = (struct wl_seat *)wl_registry_bind(
            registry, name, &wl_seat_interface, 1);
        wl_seat_add_listener(g_wl_seat, &seat_listener, NULL);
    } else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
        g_wl_decoration = (struct zxdg_decoration_manager_v1 *)wl_registry_bind(
            registry, name, &zxdg_decoration_manager_v1_interface, 1);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name)
{
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

static int ag_backend_keyctl(int raw)
{
    switch (raw) {
    case XKB_KEY_BackSpace: return AGK_BACKSPACE;
    case XKB_KEY_Delete:    return AGK_DELETE;
    case XKB_KEY_Left:      return AGK_LEFT;
    case XKB_KEY_Right:     return AGK_RIGHT;
    case XKB_KEY_Home:      return AGK_HOME;
    case XKB_KEY_End:       return AGK_END;
    case XKB_KEY_Return:    return AGK_RETURN;
    case XKB_KEY_KP_Enter:  return AGK_RETURN;
    case XKB_KEY_Tab:       return AGK_TAB;
    case XKB_KEY_Up:        return AGK_UP;
    case XKB_KEY_Down:      return AGK_DOWN;
    case XKB_KEY_Escape:    return AGK_ESC;
    default:                return AGK_NONE;
    }
}

int ag_init(void)
{
    setlocale(LC_ALL, "");

    g_wl_display = wl_display_connect(NULL);
    if (!g_wl_display) return -1;

    g_xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!g_xkb_ctx) { wl_display_disconnect(g_wl_display); g_wl_display = NULL; return -1; }

    g_wl_registry = wl_display_get_registry(g_wl_display);
    wl_registry_add_listener(g_wl_registry, &registry_listener, NULL);

    wl_display_roundtrip(g_wl_display);
    wl_display_roundtrip(g_wl_display);

    if (!g_wl_compositor || !g_wl_shm || !g_wl_wm_base) {
        wl_display_disconnect(g_wl_display);
        g_wl_display = NULL;
        return -1;
    }
    return 0;
}

void ag_shutdown(void)
{
    if (g_wl_pointer)  { wl_pointer_destroy(g_wl_pointer);  g_wl_pointer = NULL; }
    if (g_wl_keyboard) { wl_keyboard_destroy(g_wl_keyboard); g_wl_keyboard = NULL; }
    if (g_wl_seat)     wl_seat_destroy(g_wl_seat);
    if (g_wl_decoration) zxdg_decoration_manager_v1_destroy(g_wl_decoration);
    if (g_wl_wm_base)  xdg_wm_base_destroy(g_wl_wm_base);
    if (g_wl_shm)      wl_shm_destroy(g_wl_shm);
    if (g_wl_compositor) wl_compositor_destroy(g_wl_compositor);
    if (g_wl_registry) wl_registry_destroy(g_wl_registry);
    if (g_xkb_state)  xkb_state_unref(g_xkb_state);
    if (g_xkb_keymap) xkb_keymap_unref(g_xkb_keymap);
    if (g_xkb_ctx)    xkb_context_unref(g_xkb_ctx);
    g_xkb_state = NULL; g_xkb_keymap = NULL; g_xkb_ctx = NULL;
    if (g_wl_display) { wl_display_disconnect(g_wl_display); g_wl_display = NULL; }
}

const char *ag_get_platform(void) { return "wayland"; }

ag_window *ag_window_create(const char *title, int width, int height)
{
    if (!g_wl_display || !g_wl_compositor || !g_wl_shm || !g_wl_wm_base)
        return NULL;
    if (width  <= 0) width  = 1;
    if (height <= 0) height = 1;

    ag_window *w = (ag_window *)ag_mem_alloc(sizeof(ag_window));
    if (!w) return NULL;

    w->width = width; w->height = height;
    w->current_color = AG_WHITE;
    w->font_size = 16;
    w->alpha     = 255;
    w->clip_on = 0;
    strncpy(w->title, title ? title : "AGLib", sizeof(w->title) - 1);
    w->title[sizeof(w->title) - 1] = '\0';

    backbuf_resize(w, width, height);

    w->wl_surface = wl_compositor_create_surface(g_wl_compositor);
    if (!w->wl_surface) { free(w->backbuf); ag_mem_free(w); return NULL; }
    wl_proxy_set_user_data((struct wl_proxy *)w->wl_surface, w);

    w->xdg_surface = xdg_wm_base_get_xdg_surface(g_wl_wm_base, w->wl_surface);
    if (!w->xdg_surface) {
        wl_surface_destroy(w->wl_surface);
        free(w->backbuf); ag_mem_free(w); return NULL;
    }
    xdg_surface_add_listener(w->xdg_surface, &xdg_surface_listener, w);

    w->xdg_toplevel = xdg_surface_get_toplevel(w->xdg_surface);
    xdg_toplevel_add_listener(w->xdg_toplevel, &xdg_toplevel_listener, w);
    xdg_toplevel_set_title(w->xdg_toplevel, w->title);
    xdg_toplevel_set_app_id(w->xdg_toplevel, "aglib");

    if (g_wl_decoration) {
        w->xdg_decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(
            g_wl_decoration, w->xdg_toplevel);
        if (w->xdg_decoration) {
            zxdg_toplevel_decoration_v1_add_listener(
                w->xdg_decoration, &decoration_listener, w);
            zxdg_toplevel_decoration_v1_set_mode(
                w->xdg_decoration,
                ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        }
    }

    wayland_buffers_recreate(w);

    w->next = g_windows;
    g_windows = w;

    wl_surface_commit(w->wl_surface);
    wl_display_flush(g_wl_display);
    return w;
}

void ag_window_destroy(ag_window *w)
{
    if (!w) return;

    ag_window **pp = &g_windows;
    while (*pp && *pp != w) pp = &(*pp)->next;
    if (*pp) *pp = w->next;

    if (g_pointer_window == w)  g_pointer_window = NULL;
    if (g_keyboard_window == w) g_keyboard_window = NULL;

    wayland_buffers_destroy(w);
    if (w->xdg_decoration) zxdg_toplevel_decoration_v1_destroy(w->xdg_decoration);
    if (w->xdg_toplevel) xdg_toplevel_destroy(w->xdg_toplevel);
    if (w->xdg_surface)  xdg_surface_destroy(w->xdg_surface);
    if (w->wl_surface)   wl_surface_destroy(w->wl_surface);
    if (g_wl_display)    wl_display_flush(g_wl_display);
    free(w->backbuf);
    ag_mem_free(w);
}

void ag_window_set_title(ag_window *w, const char *title)
{
    if (!w || !title) return;
    strncpy(w->title, title, sizeof(w->title) - 1);
    w->title[sizeof(w->title) - 1] = '\0';
    if (w->xdg_toplevel) xdg_toplevel_set_title(w->xdg_toplevel, w->title);
}

void ag_window_set_size(ag_window *w, int width, int height)
{
    if (!w) return;
    if (width  <= 0) width  = 1;
    if (height <= 0) height = 1;
    if (width == w->width && height == w->height) return;
    backbuf_resize(w, width, height);
    wayland_buffers_recreate(w);
    w->redraw_pending = 1;
}

void ag_window_get_size(ag_window *w, int *width, int *height)
{
    if (!w) return;
    if (width)  *width  = w->width;
    if (height) *height = w->height;
}

void ag_window_set_callback(ag_window *w, ag_event_cb cb, void *userdata)
{
    if (!w) return;
    w->cb = cb; w->userdata = userdata;
}

void ag_window_show(ag_window *w)
{
    if (!w || !w->wl_surface) return;
    wl_surface_commit(w->wl_surface);
    if (g_wl_display) wl_display_flush(g_wl_display);
}

void ag_window_hide(ag_window *w)
{
    if (!w || !w->wl_surface) return;
    wl_surface_attach(w->wl_surface, NULL, 0, 0);
    wl_surface_commit(w->wl_surface);
    if (g_wl_display) wl_display_flush(g_wl_display);
}

int ag_poll_events(void)
{
    if (g_wl_display) {
        while (wl_display_prepare_read(g_wl_display) != 0)
            wl_display_dispatch_pending(g_wl_display);

        if (wl_display_flush(g_wl_display) < 0 && errno != EAGAIN &&
            errno != EINTR) {
            wl_display_cancel_read(g_wl_display);
        } else {
            struct pollfd pfd;
            pfd.fd = wl_display_get_fd(g_wl_display);
            pfd.events = POLLIN;
            pfd.revents = 0;
            if (poll(&pfd, 1, 0) > 0) {
                if (wl_display_read_events(g_wl_display) == 0)
                    wl_display_dispatch_pending(g_wl_display);
            } else {
                wl_display_cancel_read(g_wl_display);
            }
        }
    }

    for (ag_window *w = g_windows; w; w = w->next) {
        if (w->should_close || !w->configured) continue;
        if (w->ui) ag_ui_update(w->ui);
        if (w->redraw_pending) {
            w->redraw_pending = 0;
            ag_event ev; memset(&ev, 0, sizeof(ev));
            ev.type = AG_EVENT_EXPOSE;
            if (w->cb) w->cb(w, &ev, w->userdata);
        }
    }

    for (ag_window *w = g_windows; w; w = w->next)
        if (!w->should_close) return 1;
    return 0;
}

void ag_main_loop(void)
{
    g_running = 1;
    while (g_running) {
        if (!ag_poll_events()) break;
        ag_sleep_ms(1);
    }
}

void ag_quit(void) { g_running = 0; ag_quit_windows(); }

void ag_begin_frame(ag_window *w) { (void)w; }

void ag_end_frame(ag_window *w)
{
    if (!w || !w->wl_surface || !w->configured) return;

    int idx = -1;
    for (int i = 0; i < 2; ++i)
        if (w->buf[i] && !w->buf_busy[i]) { idx = i; break; }
    if (idx < 0) idx = w->cur_buf;
    if (idx < 0 || !w->buf[idx] || !w->shm_data[idx]) return;

    size_t bytes = (size_t)w->width * (size_t)w->height * 4u;
    if (bytes > w->shm_size[idx]) bytes = w->shm_size[idx];
    memcpy(w->shm_data[idx], w->backbuf, bytes);

    wl_surface_attach(w->wl_surface, w->buf[idx], 0, 0);
    wl_surface_damage(w->wl_surface, 0, 0, w->width, w->height);
    wl_surface_commit(w->wl_surface);
    w->buf_busy[idx] = 1;
    w->cur_buf = idx;

    if (g_wl_display) wl_display_flush(g_wl_display);
}

#endif /* AG_USE_WAYLAND */

#endif
