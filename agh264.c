
#include "aglib.h"
#include "aglib_int.h"
#include "agmedia.h"

#include "wels/codec_api.h"

#include <stdlib.h>
#include <string.h>

struct ag_h264 {
    ISVCDecoder   *dec;
    unsigned char  *buf;
    size_t          buf_cap;
    unsigned char  *sets;
    size_t          sets_len;
    int             fed;
    int             eos;
    int             w, h;
    int             nalsz;
};


static int parse_avcc(ag_h264 *d, const unsigned char *p, size_t size)
{
    if (size < 7) return 0;
    if (p[0] != 1) return 0;
    d->nalsz = (int)(p[4] & 3) + 1;
    if (d->nalsz > 4) d->nalsz = 4;

    size_t i = 6;
    unsigned n_sps = p[5] & 0x1F;
    if (n_sps == 0) return 0;

    size_t cap = size * 2 + 16;
    unsigned char *sets = (unsigned char *)malloc(cap);
    if (!sets) return 0;
    size_t len = 0;

    for (unsigned n = 0; n < n_sps; ++n) {
        if (i + 2 > size) goto bad;
        size_t l = ((size_t)p[i] << 8) | p[i + 1];
        i += 2;
        if (i + l > size || l == 0) goto bad;
        if (len + 4 + l > cap) goto bad;
        sets[len++] = 0; sets[len++] = 0; sets[len++] = 0; sets[len++] = 1;
        memcpy(sets + len, p + i, l);
        len += l;
        i += l;
    }
    if (i >= size) goto bad;
    unsigned n_pps = p[i++];
    for (unsigned n = 0; n < n_pps; ++n) {
        if (i + 2 > size) goto bad;
        size_t l = ((size_t)p[i] << 8) | p[i + 1];
        i += 2;
        if (i + l > size || l == 0) goto bad;
        if (len + 4 + l > cap) goto bad;
        sets[len++] = 0; sets[len++] = 0; sets[len++] = 0; sets[len++] = 1;
        memcpy(sets + len, p + i, l);
        len += l;
        i += l;
    }

    d->sets = sets;
    d->sets_len = len;
    return 1;
bad:
    free(sets);
    return 0;
}


static ag_image *image_from_i420(const unsigned char *y, int y_stride,
                                 const unsigned char *u, const unsigned char *v,
                                 int uv_stride, int w, int h)
{
    ag_image *im = (ag_image *)malloc(sizeof(ag_image));
    if (!im) return NULL;
    im->w = w;
    im->h = h;
    im->pixels = (ag_u32 *)malloc((size_t)w * (size_t)h * 4);
    if (!im->pixels) { free(im); return NULL; }

    for (int row = 0; row < h; ++row) {
        const unsigned char *yp = y + (size_t)row * (size_t)y_stride;
        const unsigned char *up = u + (size_t)(row >> 1) * (size_t)uv_stride;
        const unsigned char *vp = v + (size_t)(row >> 1) * (size_t)uv_stride;
        ag_u32 *out = im->pixels + (size_t)row * (size_t)w;
        for (int col = 0; col < w; ++col) {
            int c = (int)yp[col] - 16;
            int d = (int)up[col >> 1] - 128;
            int e = (int)vp[col >> 1] - 128;
            if (c < 0) c = 0;
            int r = (298 * c + 409 * e + 128) >> 8;
            int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
            int b = (298 * c + 516 * d + 128) >> 8;
            if (r > 255) r = 255; else if (r < 0) r = 0;
            if (g > 255) g = 255; else if (g < 0) g = 0;
            if (b > 255) b = 255; else if (b < 0) b = 0;
            out[col] = 0xFF000000u | ((ag_u32)r << 16) | ((ag_u32)g << 8)
                     | (ag_u32)b;
        }
    }
    return im;
}


ag_h264 *ag_h264_open(const unsigned char *avcc, size_t size)
{
    if (!avcc || !size) return NULL;
    ag_h264 *d = (ag_h264 *)calloc(1, sizeof(ag_h264));
    if (!d) return NULL;
    if (!parse_avcc(d, avcc, size)) { free(d); return NULL; }

    if (WelsCreateDecoder(&d->dec) != 0 || !d->dec) {
        ag_h264_close(d);
        return NULL;
    }
    SDecodingParam param;
    memset(&param, 0, sizeof(param));
    param.uiTargetDqLayer = (unsigned char)-1;
    param.eEcActiveIdc = ERROR_CON_SLICE_COPY;
    param.bParseOnly = false;
    param.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
    if ((*d->dec)->Initialize(d->dec, &param) != 0) {
        ag_h264_close(d);
        return NULL;
    }

    unsigned char *pdst[4] = { NULL, NULL, NULL, NULL };
    SBufferInfo info;
    memset(&info, 0, sizeof(info));
    (*d->dec)->DecodeFrame2(d->dec, d->sets, (int)d->sets_len, pdst, &info);
    d->fed = 1;
    return d;
}

void ag_h264_close(ag_h264 *d)
{
    if (!d) return;
    if (d->dec) {
        (*d->dec)->Uninitialize(d->dec);
        WelsDestroyDecoder(d->dec);
    }
    free(d->buf);
    free(d->sets);
    free(d);
}

int ag_h264_reset(ag_h264 *d)
{
    if (!d) return 0;
    if (d->dec) {
        (*d->dec)->Uninitialize(d->dec);
        SDecodingParam param;
        memset(&param, 0, sizeof(param));
        param.uiTargetDqLayer = (unsigned char)-1;
        param.eEcActiveIdc = ERROR_CON_SLICE_COPY;
        param.bParseOnly = false;
        param.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
        if ((*d->dec)->Initialize(d->dec, &param) != 0) {
            (*d->dec)->Uninitialize(d->dec);
            WelsDestroyDecoder(d->dec);
            d->dec = NULL;
            if (WelsCreateDecoder(&d->dec) != 0 || !d->dec) return 0;
            if ((*d->dec)->Initialize(d->dec, &param) != 0) return 0;
        }
    } else {
        if (WelsCreateDecoder(&d->dec) != 0 || !d->dec) return 0;
        SDecodingParam param;
        memset(&param, 0, sizeof(param));
        param.uiTargetDqLayer = (unsigned char)-1;
        param.eEcActiveIdc = ERROR_CON_SLICE_COPY;
        param.bParseOnly = false;
        param.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
        if ((*d->dec)->Initialize(d->dec, &param) != 0) return 0;
    }
    if (d->sets && d->sets_len > 0) {
        unsigned char *pdst[4] = { NULL, NULL, NULL, NULL };
        SBufferInfo info;
        memset(&info, 0, sizeof(info));
        (*d->dec)->DecodeFrame2(d->dec, d->sets, (int)d->sets_len, pdst, &info);
    }
    d->fed = 1;
    d->eos = 0;
    d->w = 0;
    d->h = 0;
    return 1;
}

static const unsigned char *to_annexb(ag_h264 *d, const unsigned char *data,
                                      size_t size, size_t *out_len)
{
    int n = d->nalsz;

    if (size > (size_t)n) {
        size_t cap = size + size / 8 + 16;
        if (d->buf_cap < cap) {
            unsigned char *nb = (unsigned char *)realloc(d->buf, cap);
            if (!nb) return NULL;
            d->buf = nb;
            d->buf_cap = cap;
        }

        unsigned char *out = d->buf;
        size_t olen = 0, i = 0;
        int ok = 1;
        while (i + (size_t)n <= size) {
            size_t l = 0;
            for (int k = 0; k < n; ++k) l = (l << 8) | data[i + (size_t)k];
            i += (size_t)n;
            if (l == 0 || i + l > size || (data[i] & 0x80) != 0) {
                ok = 0;
                break;
            }
            out[olen++] = 0; out[olen++] = 0; out[olen++] = 0; out[olen++] = 1;
            memcpy(out + olen, data + i, l);
            olen += l;
            i += l;
        }
        if (ok && i == size && olen > 0) {
            *out_len = olen;
            return out;
        }
    }

    if (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 &&
        data[3] == 1) {
        *out_len = size;
        return data;
    }
    if (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) {
        *out_len = size;
        return data;
    }
    return NULL;
}

static int take_output(ag_h264 *d, SBufferInfo *info, ag_image **out)
{
    if (info->iBufferStatus != 1 || !info->pDst[0]) return 0;
    int w = info->UsrData.sSystemBuffer.iWidth;
    int h = info->UsrData.sSystemBuffer.iHeight;
    int ys = info->UsrData.sSystemBuffer.iStride[0];
    int uvs = info->UsrData.sSystemBuffer.iStride[1];
    if (w <= 0 || h <= 0 || !info->pDst[1] || !info->pDst[2]) return 0;

    ag_image *im = image_from_i420(info->pDst[0], ys,
                                   info->pDst[1], info->pDst[2], uvs, w, h);
    if (!im) return -1;
    d->w = w;
    d->h = h;
    *out = im;
    return 1;
}

int ag_h264_decode(ag_h264 *d, const unsigned char *data, size_t size,
                   ag_image **out)
{
    if (!d || !data || !size || !out) return -1;
    *out = NULL;

    size_t blen = 0;
    const unsigned char *buf = to_annexb(d, data, size, &blen);
    if (!buf || blen == 0) return 0;

    unsigned char *pdst[4] = { NULL, NULL, NULL, NULL };
    SBufferInfo info;
    memset(&info, 0, sizeof(info));
    d->eos = 0;
    (*d->dec)->DecodeFrameNoDelay(d->dec, buf, (int)blen, pdst, &info);
    return take_output(d, &info, out);
}

int ag_h264_flush(ag_h264 *d, ag_image **out)
{
    if (!d || !out) return -1;
    *out = NULL;

    unsigned char *pdst[4] = { NULL, NULL, NULL, NULL };
    SBufferInfo info;

    if (!d->eos) {
        d->eos = 1;
        memset(&info, 0, sizeof(info));
        (*d->dec)->DecodeFrame2(d->dec, NULL, 0, pdst, &info);
        if (take_output(d, &info, out) > 0) return 1;
    }
    memset(&info, 0, sizeof(info));
    (*d->dec)->FlushFrame(d->dec, pdst, &info);
    return take_output(d, &info, out);
}
