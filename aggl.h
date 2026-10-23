#ifndef AGLGL_H
#define AGLGL_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ag_gl ag_gl;

ag_gl *ag_gl_create(void);
void   ag_gl_destroy(ag_gl *g);

int ag_gl_blur(ag_gl *g, const void *src, int sw, int sh,
               int dw, int dh, int radius, void *dst);

const char *ag_gpu_renderer(void);
int ag_gl_is_software(void);

#ifdef __cplusplus
}
#endif

#endif /* AGLGL_H */
