#ifndef AGLIB_INT_H
#define AGLIB_INT_H

#include "aglib.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ag_image {
    int    w, h;
    ag_u32 *pixels;
};

ag_image *ag_jpeg_decode_mem(const unsigned char *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* AGLIB_INT_H */
