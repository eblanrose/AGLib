
#include "aglib.h"
#include "aglib_int.h"
#include "agmedia.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <mmsystem.h>
#elif defined(AG_HAVE_ALSA)
#include <alsa/asoundlib.h>
#endif

#define AG_AOUT_BUFS 8          /* waveOut: round robin slots */
#define AG_AOUT_BUF_FRAMES 256  /* waveOut: ~6 ms steps for the clock */

struct ag_aout {
    int rate;
    int ch;
    long long submitted;
    long long in_flight;
    long long base;

#if defined(_WIN32)
    HWAVEOUT  wh;
    WAVEHDR   hdr[AG_AOUT_BUFS];
    short    *buf[AG_AOUT_BUFS];
    int       hframes[AG_AOUT_BUFS];
    int       hqueued[AG_AOUT_BUFS];
#elif defined(AG_HAVE_ALSA)
    snd_pcm_t *pcm;
#endif
};


static double aout_clock_now(const ag_aout *o)
{
    double c = (double)(o->submitted - o->in_flight - o->base) /
               (double)o->rate;
    return c > 0.0 ? c : 0.0;
}

double ag_aout_clock(const ag_aout *o)
{
    if (!o || o->rate <= 0) return 0.0;
    return aout_clock_now(o);
}

void ag_aout_set_clock(ag_aout *o, double seconds)
{
    if (!o || o->rate <= 0) return;
    o->base = o->submitted - o->in_flight -
              (long long)(seconds * (double)o->rate + 0.5);
}


#if defined(_WIN32)

ag_aout *ag_aout_open(int rate, int channels)
{
    if (rate <= 0 || channels <= 0 || channels > 8) return NULL;
    ag_aout *o = (ag_aout *)calloc(1, sizeof(ag_aout));
    if (!o) return NULL;
    o->rate = rate;
    o->ch = channels;

    WAVEFORMATEX wfx;
    memset(&wfx, 0, sizeof(wfx));
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = (WORD)channels;
    wfx.nSamplesPerSec = (DWORD)rate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = (WORD)(channels * 2);
    wfx.nAvgBytesPerSec = (DWORD)(rate * wfx.nBlockAlign);

    if (waveOutOpen(&o->wh, WAVE_MAPPER, &wfx, 0, 0, 0) != MMSYSERR_NOERROR) {
        free(o);
        return NULL;
    }

    size_t bytes = (size_t)AG_AOUT_BUF_FRAMES * (size_t)channels * 2;
    for (int i = 0; i < AG_AOUT_BUFS; ++i) {
        o->buf[i] = (short *)malloc(bytes);
        if (!o->buf[i]) { ag_aout_close(o); return NULL; }
        memset(&o->hdr[i], 0, sizeof(WAVEHDR));
        o->hdr[i].lpData = (LPSTR)o->buf[i];
        o->hdr[i].dwBufferLength = (DWORD)bytes;
        waveOutPrepareHeader(o->wh, &o->hdr[i], sizeof(WAVEHDR));
    }
    return o;
}

static void aout_retire(ag_aout *o)
{
    for (int i = 0; i < AG_AOUT_BUFS; ++i) {
        if (o->hqueued[i] && (o->hdr[i].dwFlags & WHDR_DONE)) {
            o->in_flight -= o->hframes[i];
            o->hqueued[i] = 0;
        }
    }
}

int ag_aout_write(ag_aout *o, const short *pcm, int frames)
{
    if (!o || frames < 0) return -1;
    int written = 0;

    while (written < frames) {
        aout_retire(o);
        int slot = -1;
        for (int i = 0; i < AG_AOUT_BUFS; ++i) {
            if (!o->hqueued[i]) { slot = i; break; }
        }
        if (slot < 0) { Sleep(1); continue; }

        int n = frames - written;
        if (n > AG_AOUT_BUF_FRAMES) n = AG_AOUT_BUF_FRAMES;
        memcpy(o->buf[slot], pcm + (size_t)written * (size_t)o->ch,
               (size_t)n * (size_t)o->ch * 2);

        WAVEHDR *h = &o->hdr[slot];
        h->dwBufferLength = (DWORD)((size_t)n * (size_t)o->ch * 2);
        h->dwFlags &= ~WHDR_DONE;
        if (waveOutWrite(o->wh, h, sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
            return written > 0 ? written : -1;

        o->hqueued[slot] = 1;
        o->hframes[slot] = n;
        o->submitted += n;
        o->in_flight += n;
        written += n;
    }
    return written;
}

void ag_aout_drop(ag_aout *o)
{
    if (!o) return;
    o->base += o->in_flight;
    o->in_flight = 0;
    waveOutReset(o->wh);
    for (int i = 0; i < AG_AOUT_BUFS; ++i) o->hqueued[i] = 0;
}

void ag_aout_close(ag_aout *o)
{
    if (!o) return;
    if (o->wh) {
        waveOutReset(o->wh);
        for (int i = 0; i < AG_AOUT_BUFS; ++i)
            if (o->buf[i]) waveOutUnprepareHeader(o->wh, &o->hdr[i],
                                                  sizeof(WAVEHDR));
        waveOutClose(o->wh);
    }
    for (int i = 0; i < AG_AOUT_BUFS; ++i) free(o->buf[i]);
    free(o);
}


#elif defined(AG_HAVE_ALSA)

ag_aout *ag_aout_open(int rate, int channels)
{
    if (rate <= 0 || channels <= 0 || channels > 8) return NULL;
    ag_aout *o = (ag_aout *)calloc(1, sizeof(ag_aout));
    if (!o) return NULL;
    o->rate = rate;
    o->ch = channels;

    if (snd_pcm_open(&o->pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        free(o);
        return NULL;
    }
    if (snd_pcm_set_params(o->pcm, SND_PCM_FORMAT_S16_LE,
                           SND_PCM_ACCESS_RW_INTERLEAVED,
                           (unsigned)channels, (unsigned)rate,
                           1 , 100000 ) < 0) {
        snd_pcm_close(o->pcm);
        free(o);
        return NULL;
    }
    return o;
}

static void aout_refresh_flight(ag_aout *o)
{
    snd_pcm_sframes_t delay = 0;
    if (snd_pcm_delay(o->pcm, &delay) >= 0 && delay > 0)
        o->in_flight = delay;
    else
        o->in_flight = 0;
}

int ag_aout_write(ag_aout *o, const short *pcm, int frames)
{
    if (!o || !o->pcm || frames < 0) return -1;
    int written = 0;
    int soft_err = 0;

    while (written < frames) {
        snd_pcm_sframes_t n = snd_pcm_writei(o->pcm,
                                             pcm + (size_t)written *
                                                   (size_t)o->ch,
                                             (snd_pcm_uframes_t)(frames - written));
        if (n > 0) {
            written += (int)n;
            o->submitted += n;
            aout_refresh_flight(o);
            soft_err = 0;
            continue;
        }
        if (n == -EINTR) continue;
        if (n == -EBADFD) {
            if (snd_pcm_prepare(o->pcm) < 0) return written > 0 ? written : -1;
            continue;
        }
        if (snd_pcm_recover(o->pcm, (int)n, 1) < 0)
            return written > 0 ? written : -1;
        aout_refresh_flight(o);
        if (++soft_err > 8) return written > 0 ? written : -1;
    }
    return written;
}

void ag_aout_drop(ag_aout *o)
{
    if (!o || !o->pcm) return;
    aout_refresh_flight(o);
    o->base += o->in_flight;
    o->in_flight = 0;
    snd_pcm_drop(o->pcm);
    snd_pcm_prepare(o->pcm);
}

void ag_aout_close(ag_aout *o)
{
    if (!o) return;
    if (o->pcm) {
        snd_pcm_drop(o->pcm);
        snd_pcm_close(o->pcm);
    }
    free(o);
}


#else

ag_aout *ag_aout_open(int rate, int channels)
{
    (void)rate; (void)channels;
    return NULL;
}

int  ag_aout_write(ag_aout *o, const short *pcm, int frames)
{ (void)o; (void)pcm; (void)frames; return -1; }
void ag_aout_drop(ag_aout *o) { (void)o; }
void ag_aout_set_clock(ag_aout *o, double seconds) { (void)o; (void)seconds; }
double ag_aout_clock(const ag_aout *o) { (void)o; return 0.0; }
void ag_aout_close(ag_aout *o) { (void)o; }

#endif
