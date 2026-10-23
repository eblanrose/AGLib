#ifndef AGMEDIA_H
#define AGMEDIA_H

#include "aglib.h"

#include <stddef.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ag_packet {
    unsigned char *data;
    size_t  size;
    double  pts;
    int     key;
} ag_packet;

enum { AG_VCODEC_NONE = 0, AG_VCODEC_H264, AG_VCODEC_MJPEG,
       AG_VCODEC_UNSUPPORTED };
enum { AG_ACODEC_NONE = 0, AG_ACODEC_AAC, AG_ACODEC_MP3,
       AG_ACODEC_UNSUPPORTED };

#define AG_PCM_MAX 8192
#define AG_PCM_CH_MAX 8

typedef struct ag_demux ag_demux;
struct ag_demux {
    void *self;
    void (*close)(void *self);
    int    (*read)(void *self, int track, ag_packet *out);
    int    (*seek)(void *self, int track, double seconds);
    void   (*vinfo)(void *self, int *w, int *h, double *fps, double *dur);
    int    (*vcodec)(void *self);
    int    (*acodec)(void *self);
    const unsigned char *(*avcc)(void *self, size_t *sz);
    const unsigned char *(*asc)(void *self, size_t *sz);
    int    (*arate)(void *self);
    int    (*achan)(void *self);
    long   (*vcount)(void *self);
    double (*next_out_pts)(void *self);
};

ag_demux *ag_mp4_open(const char *path, char *err, int errcap);
ag_demux *ag_mkv_open(const char *path, char *err, int errcap);

static inline void ag_demux_free(ag_demux *d)
{
    if (!d) return;
    d->close(d->self);
    free(d);
}

typedef struct ag_h264 ag_h264;

ag_h264 *ag_h264_open(const unsigned char *avcc, size_t size);
void     ag_h264_close(ag_h264 *d);
int      ag_h264_decode(ag_h264 *d, const unsigned char *data, size_t size,
                        ag_image **out);
int      ag_h264_flush(ag_h264 *d, ag_image **out);
int      ag_h264_reset(ag_h264 *d);

typedef struct ag_aac ag_aac;

ag_aac *ag_aac_open(const unsigned char *asc, size_t size);
void    ag_aac_close(ag_aac *a);
void    ag_aac_reset(ag_aac *a);
int     ag_aac_decode(ag_aac *a, const unsigned char *data, size_t size,
                      short *pcm, int cap);
int     ag_aac_rate(const ag_aac *a);
int     ag_aac_channels(const ag_aac *a);

typedef struct ag_mp3 ag_mp3;

ag_mp3 *ag_mp3_open(void);
void    ag_mp3_close(ag_mp3 *m);
void    ag_mp3_reset(ag_mp3 *m);
int     ag_mp3_decode(ag_mp3 *m, const unsigned char *data, size_t size,
                      short *pcm, int cap, int *rate, int *channels);

typedef struct ag_aout ag_aout;
ag_aout *ag_aout_open(int rate, int channels);
int      ag_aout_write(ag_aout *o, const short *pcm, int frames);
void     ag_aout_drop(ag_aout *o);
void     ag_aout_set_clock(ag_aout *o, double seconds);
double   ag_aout_clock(const ag_aout *o);
void     ag_aout_close(ag_aout *o);

#ifdef __cplusplus
}
#endif

#endif /* AGMEDIA_H */
