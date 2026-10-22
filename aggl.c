
#include "aggl.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if !defined(_WIN32)

#  define GL_GLEXT_PROTOTYPES 1
#  ifdef AG_USE_WAYLAND
#    include <EGL/egl.h>
#  else
#    include <GL/glx.h>
#  endif
#  include <GL/gl.h>
#  include <GL/glext.h>

#endif /* !_WIN32 */


static const char *ag_vs_full =
    "#version 120\n"
    "attribute vec2 a_pos;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    v_uv = a_pos * 0.5 + 0.5;\n"
    "    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

static const char *ag_fs_box =
    "#version 120\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec2  u_dir;\n"
    "uniform float u_step;\n"
    "uniform float u_taps;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    vec4 sum = vec4(0.0);\n"
    "    float off = (u_taps - 1.0) * 0.5;\n"
    "    for (int i = 0; i < 16; ++i) {\n"
    "        if (float(i) >= u_taps) break;\n"
    "        vec2 uv = v_uv + u_dir * u_step * (float(i) - off);\n"
    "        sum += texture2D(u_tex, uv);\n"
    "    }\n"
    "    gl_FragColor = sum / u_taps;\n"
    "}\n";

static const char *ag_fs_layers =
    "#version 120\n"
    "uniform sampler2D u_base;\n"
    "uniform sampler2D u_l0;\n"
    "uniform sampler2D u_l1;\n"
    "uniform vec4  u_p0;\n"
    "uniform vec4  u_p1;\n"
    "uniform vec2  u_size;\n"
    "uniform vec2  u_s0;\n"
    "uniform vec2  u_s1;\n"
    "varying vec2 v_uv;\n"
    "vec4 over(vec4 dst, vec4 src, float a) {\n"
    "    src *= a;\n"
    "    return src + dst * (1.0 - src.a);\n"
    "}\n"
    "void main() {\n"
    "    vec2 uv = vec2(v_uv.x, 1.0 - v_uv.y);\n"
    "    vec4 c = texture2D(u_base, uv);\n"
    "    vec2 px = uv * u_size;\n"
    "    vec2 ctr = u_size * 0.5;\n"
    "    vec2 s0 = (px - u_p0.zw - ctr) / max(u_p0.y, 0.0001) + ctr;\n"
    "    vec2 uv0 = s0 / u_s0;\n"
    "    if (uv0.x >= 0.0 && uv0.y >= 0.0 && uv0.x <= 1.0 && uv0.y <= 1.0)\n"
    "        c = over(c, texture2D(u_l0, uv0), u_p0.x);\n"
    "    vec2 s1 = (px - u_p1.zw - ctr) / max(u_p1.y, 0.0001) + ctr;\n"
    "    vec2 uv1 = s1 / u_s1;\n"
    "    if (uv1.x >= 0.0 && uv1.y >= 0.0 && uv1.x <= 1.0 && uv1.y <= 1.0)\n"
    "        c = over(c, texture2D(u_l1, uv1), u_p1.x);\n"
    "    gl_FragColor = c;\n"
    "}\n";

#if !defined(_WIN32)

static int ag_gl_query_software(void)
{
    const char *r = (const char *)glGetString(GL_RENDERER);
    if (!r) return 0;
    if (strstr(r, "llvmpipe") || strstr(r, "softpipe") ||
        strstr(r, "swrast") || strstr(r, "Software") ||
        strstr(r, "Microsoft Basic")) return 1;
    return 0;
}

static GLuint ag_gl_compile(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    if (!sh) return 0;
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        GLsizei n = 0;
        glGetShaderInfoLog(sh, (GLsizei)sizeof(log), &n, log);
        log[n < (GLsizei)sizeof(log) ? n : (GLsizei)sizeof(log) - 1] = 0;
        fprintf(stderr, "AGLib: shader error: %s\n", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint ag_gl_program(const char *vs, const char *fs)
{
    GLuint v = ag_gl_compile(GL_VERTEX_SHADER, vs);
    if (!v) return 0;
    GLuint f = ag_gl_compile(GL_FRAGMENT_SHADER, fs);
    if (!f) { glDeleteShader(v); return 0; }
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glBindAttribLocation(p, 0, "a_pos");
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        fprintf(stderr, "AGLib: program link failed\n");
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

static void ag_gl_quad(void)
{
    static const GLfloat verts[] = { -1.f, -1.f, 3.f, -1.f, -1.f, 3.f };
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, verts);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

static void ag_gl_make_tex(GLuint *tex, int w, int h)
{
    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static GLuint ag_gl_make_fbo(GLuint tex)
{
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return fbo;
}

#endif /* !_WIN32 */


static char g_gl_renderer[128] = "";
static int  g_gl_software = 0;

const char *ag_gpu_renderer(void) { return g_gl_renderer; }
int ag_gl_is_software(void) { return g_gl_software; }


struct ag_gl {
    void  *display;
    void  *pbuffer;
    void  *context;
    unsigned int blur_prog;
    unsigned int tex_src, tex_a, tex_b;
    unsigned int fbo_a, fbo_b;
    int    fb_w, fb_h;
    int    ok;
};

#if defined(_WIN32)

ag_gl *ag_gl_create(void) { return NULL; }
void   ag_gl_destroy(ag_gl *g) { (void)g; }
int    ag_gl_x11_visual(void *dpy, int screen, void **visual_out, int *depth_out)
{ (void)dpy; (void)screen; (void)visual_out; (void)depth_out; return 0; }
ag_gl_present *ag_gl_present_x11(void *dpy, unsigned long win, void *visual)
{ (void)dpy; (void)win; (void)visual; return NULL; }
void ag_gl_present_destroy(ag_gl_present *p) { (void)p; }
void ag_gl_present_frame(ag_gl_present *p, const void *backbuf, int w, int h,
                         const ag_gl_layer *layers, int n_layers)
{ (void)p; (void)backbuf; (void)w; (void)h; (void)layers; (void)n_layers; }

#elif defined(AG_USE_WAYLAND)

ag_gl *ag_gl_create(void)
{
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY) return NULL;
    EGLint maj, min;
    if (!eglInitialize(dpy, &maj, &min)) return NULL;
    eglBindAPI(EGL_OPENGL_API);
    EGLint attrs[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                       EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                       EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                       EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, attrs, &cfg, 1, &n) || n < 1) return NULL;
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, NULL);
    if (ctx == EGL_NO_CONTEXT) return NULL;
    EGLint pb[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb);
    if (!eglMakeCurrent(dpy, surf, surf, ctx)) return NULL;

    ag_gl *g = (ag_gl *)calloc(1, sizeof(ag_gl));
    if (!g) return NULL;
    g->display = (void *)dpy;
    g->pbuffer = (void *)surf;
    g->context = (void *)ctx;
    g->blur_prog = ag_gl_program(ag_vs_full, ag_fs_box);
    if (!g->blur_prog) { free(g); return NULL; }
    {
        const char *r = (const char *)glGetString(GL_RENDERER);
        if (r) snprintf(g_gl_renderer, sizeof(g_gl_renderer), "%s", r);
        g_gl_software = ag_gl_query_software();
    }
    g->ok = 1;
    return g;
}

void ag_gl_destroy(ag_gl *g) { if (g) free(g); }

#else /* X11 / GLX */

ag_gl *ag_gl_create(void)
{
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return NULL;

    static int attrs[] = { GLX_RGBA, GLX_DEPTH_SIZE, 0, GLX_DOUBLEBUFFER, None };
    XVisualInfo *vi = glXChooseVisual(dpy, DefaultScreen(dpy), attrs);
    if (!vi) { XCloseDisplay(dpy); return NULL; }

    GLXContext ctx = glXCreateContext(dpy, vi, NULL, True);
    if (!ctx) { XFree(vi); XCloseDisplay(dpy); return NULL; }

    if (!glXMakeCurrent(dpy, None, ctx)) {
        glXDestroyContext(dpy, ctx);
        XFree(vi);
        XCloseDisplay(dpy);
        return NULL;
    }

    ag_gl *g = (ag_gl *)calloc(1, sizeof(ag_gl));
    if (!g) return NULL;
    g->display = (void *)dpy;
    g->pbuffer = NULL;
    g->context = (void *)ctx;
    g->blur_prog = ag_gl_program(ag_vs_full, ag_fs_box);
    if (!g->blur_prog) { free(g); return NULL; }
    {
        const char *r = (const char *)glGetString(GL_RENDERER);
        if (r) snprintf(g_gl_renderer, sizeof(g_gl_renderer), "%s", r);
        g_gl_software = ag_gl_query_software();
    }
    g->ok = 1;
    return g;
}

void ag_gl_destroy(ag_gl *g)
{
    if (!g) return;
    if (g->display) {
        Display *dpy = (Display *)g->display;
        glXMakeCurrent(dpy, (GLXDrawable)g->pbuffer, (GLXContext)g->context);
        if (g->fbo_a) glDeleteFramebuffers(1, &g->fbo_a);
        if (g->fbo_b) glDeleteFramebuffers(1, &g->fbo_b);
        if (g->tex_src) glDeleteTextures(1, &g->tex_src);
        if (g->tex_a) glDeleteTextures(1, &g->tex_a);
        if (g->tex_b) glDeleteTextures(1, &g->tex_b);
        if (g->blur_prog) glDeleteProgram(g->blur_prog);
        glXMakeCurrent(dpy, None, NULL);
        glXDestroyContext(dpy, (GLXContext)g->context);
        XCloseDisplay(dpy);
    }
    free(g);
}


int ag_gl_x11_visual(void *dpy_, int screen, void **visual_out, int *depth_out)
{
    Display *dpy = (Display *)dpy_;
    static int attrs[] = { GLX_RGBA, GLX_DEPTH_SIZE, 0, GLX_DOUBLEBUFFER, None };
    XVisualInfo *vi = glXChooseVisual(dpy, screen, attrs);
    if (!vi) return 0;
    if (visual_out) *visual_out = vi;
    if (depth_out) *depth_out = vi->depth;
    return 1;
}

struct ag_gl_present {
    Display    *dpy;
    GLXContext  ctx;
    XVisualInfo *vi;
    GLXDrawable drawable;
    GLuint      prog;
    GLuint      tex_base, tex_l0, tex_l1;
    int         w, h;
    int         l0_w, l0_h, l1_w, l1_h;
    int         l0_dirty, l1_dirty;
    int         ok;
};

ag_gl_present *ag_gl_present_x11(void *dpy_, unsigned long win, void *visual)
{
    Display *dpy = (Display *)dpy_;
    XVisualInfo *vi = (XVisualInfo *)visual;
    GLXContext ctx = glXCreateContext(dpy, vi, NULL, True);
    if (!ctx) return NULL;
    if (!glXMakeCurrent(dpy, (GLXDrawable)win, ctx)) {
        glXDestroyContext(dpy, ctx);
        return NULL;
    }
    {
        const char *force = getenv("AG_GPU");
        if (ag_gl_query_software() && !(force && force[0] == '1')) {
            glXMakeCurrent(dpy, None, NULL);
            glXDestroyContext(dpy, ctx);
            return NULL;
        }
    }
    ag_gl_present *p = (ag_gl_present *)calloc(1, sizeof(ag_gl_present));
    if (!p) return NULL;
    p->dpy = dpy;
    p->ctx = ctx;
    p->vi = vi;
    p->drawable = (GLXDrawable)win;
    p->prog = ag_gl_program(ag_vs_full, ag_fs_layers);
    if (!p->prog) { free(p); return NULL; }
    glGenTextures(1, &p->tex_base);
    glGenTextures(1, &p->tex_l0);
    glGenTextures(1, &p->tex_l1);
    p->ok = 1;
    return p;
}

void ag_gl_present_destroy(ag_gl_present *p)
{
    if (!p) return;
    glXMakeCurrent(p->dpy, None, NULL);
    glXDestroyContext(p->dpy, p->ctx);
    free(p);
}

static void ag_gl_upload(GLuint tex, int w, int h, const void *rgba)
{
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void ag_gl_present_frame(ag_gl_present *p, const void *backbuf, int w, int h,
                         const ag_gl_layer *layers, int n_layers)
{
    if (!p || !p->ok) return;

    ag_gl_upload(p->tex_base, w, h, backbuf);

    const ag_gl_layer *l0 = n_layers > 0 ? &layers[0] : NULL;
    const ag_gl_layer *l1 = n_layers > 1 ? &layers[1] : NULL;
    if (l0 && (l0->dirty || l0->w != p->l0_w || l0->h != p->l0_h)) {
        ag_gl_upload(p->tex_l0, l0->w, l0->h, l0->pixels);
        p->l0_w = l0->w; p->l0_h = l0->h; p->l0_dirty = 0;
    }
    if (l1 && (l1->dirty || l1->w != p->l1_w || l1->h != p->l1_h)) {
        ag_gl_upload(p->tex_l1, l1->w, l1->h, l1->pixels);
        p->l1_w = l1->w; p->l1_h = l1->h; p->l1_dirty = 0;
    }

    glViewport(0, 0, w, h);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glUseProgram(p->prog);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, p->tex_base);
    glUniform1i(glGetUniformLocation(p->prog, "u_base"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, l0 ? p->tex_l0 : p->tex_base);
    glUniform1i(glGetUniformLocation(p->prog, "u_l0"), 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, l1 ? p->tex_l1 : p->tex_base);
    glUniform1i(glGetUniformLocation(p->prog, "u_l1"), 2);
    glActiveTexture(GL_TEXTURE0);

    glUniform2f(glGetUniformLocation(p->prog, "u_size"), (float)w, (float)h);
    if (l0) {
        glUniform4f(glGetUniformLocation(p->prog, "u_p0"), l0->alpha, l0->scale,
                    (float)l0->ox, (float)l0->oy);
        glUniform2f(glGetUniformLocation(p->prog, "u_s0"), (float)l0->w, (float)l0->h);
    } else {
        glUniform4f(glGetUniformLocation(p->prog, "u_p0"), 0.f, 1.f, 0.f, 0.f);
        glUniform2f(glGetUniformLocation(p->prog, "u_s0"), 1.f, 1.f);
    }
    if (l1) {
        glUniform4f(glGetUniformLocation(p->prog, "u_p1"), l1->alpha, l1->scale,
                    (float)l1->ox, (float)l1->oy);
        glUniform2f(glGetUniformLocation(p->prog, "u_s1"), (float)l1->w, (float)l1->h);
    } else {
        glUniform4f(glGetUniformLocation(p->prog, "u_p1"), 0.f, 1.f, 0.f, 0.f);
        glUniform2f(glGetUniformLocation(p->prog, "u_s1"), 1.f, 1.f);
    }

    ag_gl_quad();
    glFinish();
    glXSwapBuffers(p->dpy, p->drawable);
}

#endif /* platform */


int ag_gl_blur(ag_gl *g, const void *src, int sw, int sh,
               int dw, int dh, int radius, void *dst)
{
#if defined(_WIN32)
    (void)g; (void)src; (void)sw; (void)sh;
    (void)dw; (void)dh; (void)radius; (void)dst;
    return 0;
#else
    if (!g || !g->ok || !g->blur_prog) return 0;
    if (dw < 1 || dh < 1) return 0;
    if (radius < 1) radius = 1;

    glBindTexture(GL_TEXTURE_2D, g->tex_src);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, sw, sh, 0, GL_BGRA, GL_UNSIGNED_BYTE, src);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    if (g->fb_w != dw || g->fb_h != dh) {
        if (g->fbo_a) glDeleteFramebuffers(1, &g->fbo_a);
        if (g->fbo_b) glDeleteFramebuffers(1, &g->fbo_b);
        if (g->tex_a) glDeleteTextures(1, &g->tex_a);
        if (g->tex_b) glDeleteTextures(1, &g->tex_b);
        ag_gl_make_tex(&g->tex_a, dw, dh);
        ag_gl_make_tex(&g->tex_b, dw, dh);
        g->fbo_a = ag_gl_make_fbo(g->tex_a);
        g->fbo_b = ag_gl_make_fbo(g->tex_b);
        g->fb_w = dw;
        g->fb_h = dh;
    }

    glViewport(0, 0, dw, dh);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glUseProgram(g->blur_prog);

    glBindFramebuffer(GL_FRAMEBUFFER, g->fbo_a);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g->tex_src);
    glUniform1i(glGetUniformLocation(g->blur_prog, "u_tex"), 0);
    glUniform2f(glGetUniformLocation(g->blur_prog, "u_dir"), 0.f, 0.f);
    glUniform1f(glGetUniformLocation(g->blur_prog, "u_step"), 0.f);
    glUniform1f(glGetUniformLocation(g->blur_prog, "u_taps"), 1.f);
    ag_gl_quad();

    int taps = 16;
    float step = (float)radius / (float)taps;
    for (int it = 0; it < 2; ++it) {
        glBindFramebuffer(GL_FRAMEBUFFER, g->fbo_b);
        glBindTexture(GL_TEXTURE_2D, g->tex_a);
        glUniform2f(glGetUniformLocation(g->blur_prog, "u_dir"), 1.f / (float)dw, 0.f);
        glUniform1f(glGetUniformLocation(g->blur_prog, "u_step"), step);
        glUniform1f(glGetUniformLocation(g->blur_prog, "u_taps"), (float)taps);
        ag_gl_quad();

        glBindFramebuffer(GL_FRAMEBUFFER, g->fbo_a);
        glBindTexture(GL_TEXTURE_2D, g->tex_b);
        glUniform2f(glGetUniformLocation(g->blur_prog, "u_dir"), 0.f, 1.f / (float)dh);
        ag_gl_quad();
    }

    glBindFramebuffer(GL_FRAMEBUFFER, g->fbo_a);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, dw, dh, GL_RGBA, GL_UNSIGNED_BYTE, dst);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return 1;
#endif
}
