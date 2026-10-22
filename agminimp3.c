
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#include "aglib.h"
#include "aglib_int.h"
#include "agmedia.h"

#include <stdlib.h>
#include <string.h>

struct ag_mp3 {
    mp3dec_t dec;
};

ag_mp3 *ag_mp3_open(void)
{
    ag_mp3 *m = (ag_mp3 *)calloc(1, sizeof(ag_mp3));
    if (!m) return NULL;
    mp3dec_init(&m->dec);
    return m;
}

void ag_mp3_close(ag_mp3 *m)
{
    free(m);
}

void ag_mp3_reset(ag_mp3 *m)
{
    if (m) mp3dec_init(&m->dec);
}

int ag_mp3_decode(ag_mp3 *m, const unsigned char *data, size_t size,
                  short *pcm, int cap, int *rate, int *channels)
{
    if (!m || !data || !size || !pcm) return -1;
    if (cap < 1152 * 2) return -1;

    const unsigned char *p = data;
    size_t left = size;
    int done = 0;

    while (left >= 4) {
        mp3dec_frame_info_t info;
        int n = mp3dec_decode_frame(&m->dec, p, (int)left,
                                    pcm + (size_t)done * 2, &info);
        if (info.frame_bytes <= 0) break;
        p += (size_t)info.frame_bytes;
        left -= (size_t)info.frame_bytes;
        if (n > 0) {
            if (rate) *rate = info.hz;
            if (channels) *channels = info.channels;
            if (done + n > cap) break;
            done += n;
            break;
        }
    }
    return done;
}
