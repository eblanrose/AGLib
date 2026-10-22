/* AGLib — AAC decoding on top of the bundled fdk-aac (Fraunhofer FDK
 * license, see third_party/fdk-aac/NOTICE). Raw access units from mp4/mkv,
 * configured with the AudioSpecificConfig of the container. */

#include "aglib.h"
#include "aglib_int.h"
#include "agmedia.h"

#include "aacdecoder_lib.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct ag_aac {
    HANDLE_AACDECODER h;
    int rate;
    int channels;
};

ag_aac *ag_aac_open(const unsigned char *asc, size_t size)
{
    if (!asc || !size) return NULL;
    ag_aac *a = (ag_aac *)calloc(1, sizeof(ag_aac));
    if (!a) return NULL;

    a->h = aacDecoder_Open(TT_MP4_RAW, 1);
    if (!a->h) { free(a); return NULL; }

    UCHAR *p = (UCHAR *)(uintptr_t)asc;
    UINT len = (UINT)size;
    if (aacDecoder_ConfigRaw(a->h, &p, &len) != AAC_DEC_OK) {
        aacDecoder_Close(a->h);
        free(a);
        return NULL;
    }
    a->rate = 48000;
    a->channels = 2;
    return a;
}

void ag_aac_close(ag_aac *a)
{
    if (!a) return;
    if (a->h) aacDecoder_Close(a->h);
    free(a);
}

void ag_aac_reset(ag_aac *a)
{
    if (!a || !a->h) return;
    INT_PCM sink[256];
    aacDecoder_DecodeFrame(a->h, sink, 256, AACDEC_CLRHIST);
}

int ag_aac_decode(ag_aac *a, const unsigned char *data, size_t size,
                  short *pcm, int cap)
{
    if (!a || !a->h || !data || !size || !pcm || cap <= 0) return -1;

    UCHAR *ptr = (UCHAR *)(uintptr_t)data;
    UINT total = (UINT)size;
    UINT valid = (UINT)size;
    if (aacDecoder_Fill(a->h, &ptr, &total, &valid) != AAC_DEC_OK)
        return 0;

    INT_PCM *dst = (INT_PCM *)pcm;
    AAC_DECODER_ERROR err = aacDecoder_DecodeFrame(a->h, dst, cap, 0);
    if (err == AAC_DEC_NOT_ENOUGH_BITS) return 0;
    if (err != AAC_DEC_OK) return 0;

    CStreamInfo *si = aacDecoder_GetStreamInfo(a->h);
    if (!si || si->numChannels <= 0 || si->sampleRate <= 0 || si->frameSize <= 0)
        return 0;
    a->rate = si->sampleRate;
    a->channels = si->numChannels;
    if ((long)si->frameSize * si->numChannels > cap) return 0;
    return si->frameSize;
}

int ag_aac_rate(const ag_aac *a)      { return a ? a->rate : 0; }
int ag_aac_channels(const ag_aac *a)  { return a ? a->channels : 0; }
