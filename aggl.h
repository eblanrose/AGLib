#ifndef AGLGL_H
#define AGLGL_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ag_gl ag_gl;

typedef struct {
    const void *pixels;
    int    w, h;
    float  alpha;
    float  scale;
    int    ox, oy;
    int    dirty;
} ag_gl_layer;


ag_gl *ag_gl_create(void);
void   ag_gl_destroy(ag_gl *g);

int ag_gl_blur(ag_gl *g, const void *src, int sw, int sh,
               int dw, int dh, int radius, void *dst);

const char *ag_gpu_renderer(void);
int ag_gpu_is_software(void);


int ag_gl_x11_visual(void *dpy, int screen, void **visual_out, int *depth_out);

typedef struct ag_gl_present ag_gl_present;

ag_gl_present *ag_gl_present_x11(void *dpy, unsigned long win, void *visual);
void ag_gl_present_destroy(ag_gl_present *p);

void ag_gl_present_frame(ag_gl_present *p, const void *backbuf, int w, int h,
                         const ag_gl_layer *layers, int n_layers);

#ifdef __cplusplus
}
#endif

#endif /* AGLGL_H */
