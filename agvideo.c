
#include "aglib.h"
#include "aglib_int.h"
#include "agmedia.h"
#include "agthread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

#define AG_VPATH_MAX 512
#define AG_FQ_CAP 6                     /* video frame queue depth */

typedef struct {
    long off;
    long size;
} ag_vframe;

typedef struct {
    FILE      *fp;
    ag_vframe *frames;
    int        nframes, cap;
    int        idx;
    int        w, h;
    double     fps;
} ag_vstream;

struct ag_video {
    int      backend;
    int      w, h;
    double   fps;
    double   duration;
    int      frame_index;
    double   time;
    char     err[256];
    char     path[AG_VPATH_MAX];
    ag_image *frame;
    ag_vstream *stream;

    ag_demux *dmx;
    ag_h264  *h264;
    int       vcodec;
    int       acodec;
    long      vcnt;
    int       mjpeg;

    void     *adec;
    int       arate, achan;
    int       audio_active;
    ag_aout  *aout;
    int       aout_dropped;
    int       gain;
    short    *pcm;

    ag_mutex *lk;
    ag_thread *thr;
    volatile int run, playing, busy, eof, seek_pending;
    double    clock;
    double    wall0;
    double    seek_target;
    double    discard_until;

    struct { ag_image *img; double pts; } fq[AG_FQ_CAP];
    int fq_h, fq_n;
};

static char g_video_err[256] = "";

static void ag_video_seterr(const char *msg)
{
    snprintf(g_video_err, sizeof(g_video_err), "%s", msg ? msg : "");
}

const char *ag_video_last_error(void) { return g_video_err; }

const char *ag_video_backend_name(const ag_video *v)
{
    if (!v) return "none";
    switch (v->backend) {
    case AG_VIDEO_BACKEND_MP4:    return "mp4";
    case AG_VIDEO_BACKEND_MKV:    return "mkv";
    case AG_VIDEO_BACKEND_AVI:    return "avi";
    case AG_VIDEO_BACKEND_MJPEG:  return "mjpeg";
    default:                      return "none";
    }
}

const char *ag_video_error(const ag_video *v)
{
    if (!v) return g_video_err;
    return v->err[0] ? v->err : g_video_err;
}


static double ag_video_now(void)
{
#if defined(_WIN32)
    return (double)GetTickCount64() / 1000.0;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

static void ag_video_msleep(int ms)
{
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

static int ag_vadd_frame(ag_vstream *st, long off, long size)
{
    if (st->nframes >= st->cap) {
        int nc = st->cap ? st->cap * 2 : 64;
        ag_vframe *nf = (ag_vframe *)realloc(st->frames, (size_t)nc * sizeof(ag_vframe));
        if (!nf) return 0;
        st->frames = nf;
        st->cap = nc;
    }
    st->frames[st->nframes].off = off;
    st->frames[st->nframes].size = size;
    st->nframes++;
    return 1;
}

static void ag_vstream_free(ag_vstream *st)
{
    if (!st) return;
    if (st->fp) fclose(st->fp);
    free(st->frames);
    free(st);
}


static int ag_riff_u32(FILE *f, unsigned int *v)
{
    unsigned char b[4];
    if (fread(b, 1, 4, f) != 4) return 0;
    *v = (unsigned int)(b[0] | (b[1] << 8) | (b[2] << 16) | ((unsigned)b[3] << 24));
    return 1;
}

static void ag_avi_parse_chunks(ag_video *v, ag_vstream *st, FILE *f,
                                unsigned int size, int depth)
{
    if (depth > 4) return;
    long end = ftell(f) + (long)size;
    while (ftell(f) < end) {
        char id[5] = { 0 };
        unsigned int csz = 0;
        if (fread(id, 1, 4, f) != 4) break;
        if (!ag_riff_u32(f, &csz)) break;
        long data = ftell(f);
        long next = data + (long)csz + (long)(csz & 1);

        if (memcmp(id, "LIST", 4) == 0) {
            char lt[5] = { 0 };
            if (fread(lt, 1, 4, f) != 4) break;
            if (memcmp(lt, "movi", 4) == 0 || memcmp(lt, "rec ", 4) == 0 ||
                memcmp(lt, "strl", 4) == 0)
                ag_avi_parse_chunks(v, st, f, csz - 4, depth + 1);
        } else if (memcmp(id, "avih", 4) == 0) {
            unsigned int h10[10];
            int ok = 1;
            for (int i = 0; i < 10; ++i)
                if (!ag_riff_u32(f, &h10[i])) { ok = 0; break; }
            if (ok) {
                unsigned int us = h10[0], total = h10[4];
                unsigned int w = h10[8], h = h10[9];
                if (w && h) { st->w = (int)w; st->h = (int)h; }
                if (us > 0) st->fps = 1000000.0 / (double)us;
                if (total && st->fps > 0.0)
                    v->duration = (double)total / st->fps;
            }
        } else if (memcmp(id, "strh", 4) == 0) {
            char fcc[5] = { 0 };
            unsigned int flags = 0, scale = 0, rate = 0;
            fread(fcc, 1, 4, f);
            if (ag_riff_u32(f, &flags)) {
                fseek(f, 8, SEEK_CUR);
                if (ag_riff_u32(f, &scale) && ag_riff_u32(f, &rate)) {
                    if (scale > 0 && rate > 0)
                        st->fps = (double)rate / (double)scale;
                }
            }
        } else if (memcmp(id, "strf", 4) == 0) {
            unsigned int biSize = 0, w = 0, h = 0;
            if (ag_riff_u32(f, &biSize) && ag_riff_u32(f, &w) && ag_riff_u32(f, &h)) {
                int hh = (int)h;
                if (hh < 0) hh = -hh;
                if (w && hh) { st->w = (int)w; st->h = hh; }
            }
        } else if ((id[2] == 'd' && (id[3] == 'c' || id[3] == 'b')) && csz > 4) {
            unsigned char sig[2] = { 0, 0 };
            if (fread(sig, 1, 2, f) == 2 && sig[0] == 0xFF && sig[1] == 0xD8)
                ag_vadd_frame(st, data, (long)csz);
        }
        if (fseek(f, next, SEEK_SET) != 0) break;
    }
    fseek(f, end, SEEK_SET);
}

static int ag_avi_open(ag_video *v, const char *path, ag_vstream **out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    char id[5] = { 0 };
    unsigned int size = 0;
    if (fread(id, 1, 4, f) != 4 || memcmp(id, "RIFF", 4) != 0) { fclose(f); return 0; }
    if (!ag_riff_u32(f, &size)) { fclose(f); return 0; }
    if (fread(id, 1, 4, f) != 4 || memcmp(id, "AVI ", 4) != 0) { fclose(f); return 0; }

    ag_vstream *st = (ag_vstream *)calloc(1, sizeof(ag_vstream));
    if (!st) { fclose(f); return 0; }
    st->fp = f;
    st->fps = 25.0;

    long end = ftell(f) + (long)size;
    while (ftell(f) < end) {
        char cid[5] = { 0 };
        unsigned int csz = 0;
        if (fread(cid, 1, 4, f) != 4) break;
        if (!ag_riff_u32(f, &csz)) break;
        long data = ftell(f);
        long next = data + (long)csz + (long)(csz & 1);

        if (memcmp(cid, "LIST", 4) == 0) {
            char lt[5] = { 0 };
            if (fread(lt, 1, 4, f) != 4) break;
            if (memcmp(lt, "hdrl", 4) == 0 || memcmp(lt, "movi", 4) == 0)
                ag_avi_parse_chunks(v, st, f, csz - 4, 0);
        }
        if (fseek(f, next, SEEK_SET) != 0) break;
    }

    if (st->nframes <= 0) { ag_vstream_free(st); return 0; }
    *out = st;
    return 1;
}


static int ag_mjpeg_open(ag_video *v, const char *path, ag_vstream **out)
{
    (void)v;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    unsigned char sig[2] = { 0, 0 };
    if (fread(sig, 1, 2, f) != 2 || sig[0] != 0xFF || sig[1] != 0xD8) {
        fclose(f);
        return 0;
    }
    fseek(f, 0, SEEK_SET);

    unsigned char buf[65536];
    int state = 0;
    long frame_start = 0;
    long pos = 0;
    int prev = -1;

    ag_vstream *st = (ag_vstream *)calloc(1, sizeof(ag_vstream));
    if (!st) { fclose(f); return 0; }
    st->fp = f;
    st->fps = 25.0;

    for (;;) {
        size_t n = fread(buf, 1, sizeof(buf), f);
        if (n == 0) break;
        for (size_t i = 0; i < n; ++i) {
            int c = buf[i];
            if (state == 0) {
                if (prev == 0xFF && c == 0xD8) {
                    state = 1;
                    frame_start = pos - 1;
                }
            } else {
                if (prev == 0xFF && c == 0xD9) {
                    ag_vadd_frame(st, frame_start, pos + 1 - frame_start);
                    state = 0;
                }
            }
            prev = c;
            ++pos;
        }
    }

    if (st->nframes <= 0) { ag_vstream_free(st); return 0; }
    fseek(f, 0, SEEK_SET);
    *out = st;
    return 1;
}


static void fq_push(ag_video *v, ag_image *im, double pts)
{
    if (v->fq_n >= AG_FQ_CAP) {
        int h = v->fq_h;
        ag_image_destroy(v->fq[h].img);
        v->fq[h].img = im;
        v->fq[h].pts = pts;
        v->fq_h = (h + 1) % AG_FQ_CAP;
    } else {
        int t = (v->fq_h + v->fq_n) % AG_FQ_CAP;
        v->fq[t].img = im;
        v->fq[t].pts = pts;
        v->fq_n++;
    }
}

static void fq_flush(ag_video *v)
{
    for (int i = 0; i < AG_FQ_CAP; ++i) {
        ag_image_destroy(v->fq[i].img);
        v->fq[i].img = NULL;
    }
    v->fq_h = 0;
    v->fq_n = 0;
}

static ag_image *decode_video_pkt(ag_video *v, const ag_packet *pkt,
                                  double *pts)
{
    ag_image *im = NULL;
    if (v->mjpeg) {
        im = ag_jpeg_decode_mem(pkt->data, pkt->size);
        if (im) *pts = pkt->pts;
    } else if (v->h264) {
        if (ag_h264_decode(v->h264, pkt->data, pkt->size, &im) > 0 && im)
            *pts = v->dmx->next_out_pts(v->dmx->self);
        else
            im = NULL;
    }
    return im;
}

static void accept_video_frame(ag_video *v, ag_image *im, double pts)
{
    if (!im) return;
    if (v->discard_until >= 0.0 && pts + 1e-6 < v->discard_until) {
        ag_image_destroy(im);
        return;
    }
    v->discard_until = -1.0;
    fq_push(v, im, pts);
}

static void drain_video(ag_video *v, int discard)
{
    if (!v->h264) return;
    for (;;) {
        ag_image *im = NULL;
        if (ag_h264_flush(v->h264, &im) <= 0 || !im) break;
        double pts = v->dmx->next_out_pts(v->dmx->self);
        if (discard) ag_image_destroy(im);
        else accept_video_frame(v, im, pts);
    }
}

static void apply_seek(ag_video *v, double target)
{
    drain_video(v, 1);
    v->dmx->seek(v->dmx->self, 0, target);
    v->dmx->seek(v->dmx->self, 1, target);
    if (v->acodec == AG_ACODEC_AAC && v->adec)
        ag_aac_reset((ag_aac *)v->adec);
    else if (v->acodec == AG_ACODEC_MP3 && v->adec)
        ag_mp3_reset((ag_mp3 *)v->adec);
    fq_flush(v);
    v->clock = target;
    if (v->aout) ag_aout_set_clock(v->aout, target);
    else v->wall0 = ag_video_now() - target;
    v->discard_until = target;
    v->eof = 0;
    v->seek_pending = 0;
}

static void media_thread(void *ud)
{
    ag_video *v = (ag_video *)ud;
    ag_packet vpkt, apkt;
    int have_v = 0, have_a = 0;
    int video_done = 0;
    int audio_on = v->audio_active;
    int audio_done = !audio_on;
    double lead = (v->fps > 0.0) ? 0.5 / v->fps : 0.02;
    int gain = v->gain;

    while (1) {
        double clock_now;
        double discard;

        ag_mutex_lock(v->lk);
        if (!v->run) { ag_mutex_unlock(v->lk); break; }

        if (v->seek_pending) {
            apply_seek(v, v->seek_target);
            have_v = have_a = 0;
            video_done = 0;
            audio_done = !audio_on;
            ag_mutex_unlock(v->lk);
            continue;
        }
        if (!v->playing) {
            if (v->aout && !v->aout_dropped) {
                ag_aout_drop(v->aout);
                v->aout_dropped = 1;
            }
            ag_mutex_unlock(v->lk);
            ag_video_msleep(10);
            continue;
        }
        v->busy = 1;
        clock_now = v->clock;
        discard = v->discard_until;
        gain = v->gain;
        ag_mutex_unlock(v->lk);

        int progress = 0;

        if (audio_on && !audio_done) {
            if (!have_a) {
                if (v->dmx->read(v->dmx->self, 1, &apkt)) {
                    if (apkt.pts < discard) progress = 1;
                    else have_a = 1;
                } else {
                    audio_done = 1;
                }
            }
            if (have_a) {
                int n = 0;
                if (v->acodec == AG_ACODEC_AAC && v->adec) {
                    n = ag_aac_decode((ag_aac *)v->adec, apkt.data, apkt.size,
                                      v->pcm, AG_PCM_MAX * AG_PCM_CH_MAX);
                    if (n > 0) {
                        v->arate = ag_aac_rate((ag_aac *)v->adec);
                        v->achan = ag_aac_channels((ag_aac *)v->adec);
                    }
                } else if (v->acodec == AG_ACODEC_MP3 && v->adec) {
                    int r = 0, c = 0;
                    n = ag_mp3_decode((ag_mp3 *)v->adec, apkt.data, apkt.size,
                                      v->pcm, AG_PCM_MAX * AG_PCM_CH_MAX,
                                      &r, &c);
                    if (n > 0) { v->arate = r; v->achan = c; }
                }
                have_a = 0;
                progress = 1;

                if (n > 0 && v->arate > 0 && v->achan > 0) {
                    if (gain < 256) {
                        int total = n * v->achan;
                        for (int i = 0; i < total; ++i)
                            v->pcm[i] = (short)((v->pcm[i] * gain) >> 8);
                    }

                    ag_mutex_lock(v->lk);
                    if (!v->aout) {
                        v->aout = ag_aout_open(v->arate, v->achan);
                        v->aout_dropped = 0;
                        if (!v->aout) audio_on = 0;
                    }
                    ag_aout *ao = v->aout;
                    ag_mutex_unlock(v->lk);

                    if (ao) {
                        if (ag_aout_write(ao, v->pcm, n) < 0) {
                            ag_mutex_lock(v->lk);
                            if (v->aout) {
                                ag_aout_close(v->aout);
                                v->aout = NULL;
                            }
                            audio_on = 0;
                            audio_done = 1;
                            v->wall0 = ag_video_now() - v->clock;
                            ag_mutex_unlock(v->lk);
                        } else {
                            ag_mutex_lock(v->lk);
                            v->clock = ag_aout_clock(ao);
                            ag_mutex_unlock(v->lk);
                        }
                        clock_now = v->clock;
                    } else {
                        audio_done = 1;
                    }
                }
            }
        }

        if (!video_done) {
            if (!have_v) {
                if (v->dmx->read(v->dmx->self, 0, &vpkt)) {
                    have_v = 1;
                } else {
                    drain_video(v, 0);
                    video_done = 1;
                    ag_mutex_lock(v->lk);
                    v->eof = audio_done;
                    ag_mutex_unlock(v->lk);
                }
            }
            if (have_v && vpkt.pts <= clock_now + lead) {
                double pts = 0.0;
                ag_image *im = decode_video_pkt(v, &vpkt, &pts);
                have_v = 0;
                progress = 1;
                if (im) {
                    ag_mutex_lock(v->lk);
                    accept_video_frame(v, im, pts);
                    ag_mutex_unlock(v->lk);
                }
            }
        }

        if (!audio_on && !video_done) {
            ag_mutex_lock(v->lk);
            v->clock = ag_video_now() - v->wall0;
            clock_now = v->clock;
            ag_mutex_unlock(v->lk);
        }

        if (video_done && audio_done) {
            ag_mutex_lock(v->lk);
            v->eof = 1;
            ag_mutex_unlock(v->lk);
        }

        ag_mutex_lock(v->lk);
        v->busy = 0;
        ag_mutex_unlock(v->lk);

        if (!progress) ag_video_msleep(4);
    }

    ag_mutex_lock(v->lk);
    v->busy = 0;
    ag_mutex_unlock(v->lk);
}

static int pull_media_frame(ag_video *v, ag_image **out)
{
    for (;;) {
        ag_packet pkt;
        if (!v->dmx->read(v->dmx->self, 0, &pkt)) {
            drain_video(v, 0);
            ag_image *im = NULL;
            (void)im;
            if (v->fq_n > 0) {
                int h = v->fq_h;
                ag_image *f = v->fq[h].img;
                double pts = v->fq[h].pts;
                v->fq[h].img = NULL;
                v->fq_h = (h + 1) % AG_FQ_CAP;
                v->fq_n--;
                ag_image_destroy(v->frame);
                v->frame = f;
                v->time = pts;
                v->frame_index++;
                if (out) *out = f;
                return 1;
            }
            return 0;
        }

        double pts = 0.0;
        ag_image *im = decode_video_pkt(v, &pkt, &pts);
        if (!im) continue;
        if (v->discard_until >= 0.0 && pts + 1e-6 < v->discard_until) {
            ag_image_destroy(im);
            continue;
        }
        v->discard_until = -1.0;
        ag_image_destroy(v->frame);
        v->frame = im;
        v->time = pts;
        v->frame_index++;
        if (out) *out = im;
        return 1;
    }
}

static int open_media(ag_video *v, ag_demux *dmx, char *err, int errcap)
{
    v->dmx = dmx;
    v->gain = 256;
    v->discard_until = -1.0;

    int w = 0, h = 0;
    double fps = 0.0, dur = 0.0;
    dmx->vinfo(dmx->self, &w, &h, &fps, &dur);
    v->w = w;
    v->h = h;
    v->fps = (fps > 0.0) ? fps : 25.0;
    v->duration = dur;
    v->vcnt = dmx->vcount(dmx->self);
    v->vcodec = dmx->vcodec(dmx->self);

    if (v->vcodec == AG_VCODEC_H264) {
        size_t asz = 0;
        const unsigned char *avcc = dmx->avcc(dmx->self, &asz);
        v->h264 = ag_h264_open(avcc, asz);
        if (!v->h264) {
            snprintf(err, (size_t)errcap, "не удалось запустить декодер H.264");
            return 0;
        }
    } else if (v->vcodec == AG_VCODEC_MJPEG) {
        v->mjpeg = 1;
    } else {
        snprintf(err, (size_t)errcap,
                 "видео-кодек не поддерживается — нужны H.264 или MJPEG");
        return 0;
    }

    v->acodec = dmx->acodec(dmx->self);
    if (v->acodec == AG_ACODEC_AAC) {
        size_t asz = 0;
        const unsigned char *asc = dmx->asc(dmx->self, &asz);
        if (asc && asz) v->adec = ag_aac_open(asc, asz);
        if (!v->adec) v->acodec = AG_ACODEC_NONE;
    } else if (v->acodec == AG_ACODEC_MP3) {
        v->adec = ag_mp3_open();
        if (!v->adec) v->acodec = AG_ACODEC_NONE;
    } else {
        v->acodec = AG_ACODEC_NONE;
    }
    v->audio_active = (v->acodec != AG_ACODEC_NONE);
    v->arate = dmx->arate(dmx->self);
    v->achan = dmx->achan(dmx->self);

    if (v->audio_active) {
        v->pcm = (short *)malloc((size_t)AG_PCM_MAX * AG_PCM_CH_MAX *
                                 sizeof(short));
        if (!v->pcm) v->audio_active = 0;
    }

    v->lk = ag_mutex_new();
    if (!v->lk) {
        snprintf(err, (size_t)errcap, "не удалось создать мьютекс");
        return 0;
    }
    return 1;
}

static void close_media(ag_video *v)
{
    fq_flush(v);
    if (v->aout) ag_aout_close(v->aout);
    if (v->acodec == AG_ACODEC_AAC && v->adec)
        ag_aac_close((ag_aac *)v->adec);
    else if (v->acodec == AG_ACODEC_MP3 && v->adec)
        ag_mp3_close((ag_mp3 *)v->adec);
    if (v->h264) ag_h264_close(v->h264);
    ag_demux_free(v->dmx);
    v->dmx = NULL;
    if (v->pcm) free(v->pcm);
    if (v->lk) ag_mutex_free(v->lk);
}


static void ag_video_apply_stream(ag_video *v, ag_vstream *st)
{
    v->w = st->w;
    v->h = st->h;
    v->fps = st->fps > 0.0 ? st->fps : 25.0;
    if (v->duration <= 0.0 && st->nframes > 0)
        v->duration = (double)st->nframes / v->fps;
}

ag_video *ag_video_open_ex(const char *path, ag_video_backend backend)
{
    if (!path || !*path) { ag_video_seterr("путь к файлу не задан"); return NULL; }

    ag_video *v = (ag_video *)calloc(1, sizeof(ag_video));
    if (!v) return NULL;
    snprintf(v->path, sizeof(v->path), "%s", path);
    v->fps = 25.0;

    if (backend == AG_VIDEO_BACKEND_AUTO) {
        unsigned char sig[12] = { 0 };
        FILE *sf = fopen(path, "rb");
        size_t got = sf ? fread(sig, 1, sizeof(sig), sf) : 0;
        if (sf) fclose(sf);
        if (got >= 12 && memcmp(sig, "RIFF", 4) == 0 &&
            memcmp(sig + 8, "AVI ", 4) == 0)
            backend = AG_VIDEO_BACKEND_AVI;
        else if (got >= 8 && memcmp(sig + 4, "ftyp", 4) == 0)
            backend = AG_VIDEO_BACKEND_MP4;
        else if (got >= 4 && sig[0] == 0x1A && sig[1] == 0x45 &&
                 sig[2] == 0xDF && sig[3] == 0xA3)
            backend = AG_VIDEO_BACKEND_MKV;
        else if (got >= 2 && sig[0] == 0xFF && sig[1] == 0xD8)
            backend = AG_VIDEO_BACKEND_MJPEG;
        else {
            ag_video_seterr("неизвестный формат видео");
            free(v);
            return NULL;
        }
    }

    if (backend == AG_VIDEO_BACKEND_MP4 || backend == AG_VIDEO_BACKEND_MKV) {
        char derr[256] = { 0 };
        ag_demux *dmx = (backend == AG_VIDEO_BACKEND_MP4)
                            ? ag_mp4_open(path, derr, (int)sizeof(derr))
                            : ag_mkv_open(path, derr, (int)sizeof(derr));
        if (!dmx) {
            ag_video_seterr(derr[0] ? derr : "не удалось разобрать видео");
            free(v);
            return NULL;
        }
        v->backend = backend;
        if (!open_media(v, dmx, v->err, (int)sizeof(v->err))) {
            dmx->close(dmx->self);
            ag_video_seterr(v->err[0] ? v->err : "не удалось открыть видео");
            free(v);
            return NULL;
        }
        ag_video_seterr("");
        return v;
    }

    ag_vstream *st = NULL;

    if (backend == AG_VIDEO_BACKEND_AVI) {
        if (ag_avi_open(v, path, &st)) {
            v->backend = AG_VIDEO_BACKEND_AVI;
            v->stream = st;
            ag_video_apply_stream(v, st);
            ag_video_seterr("");
            return v;
        }
        ag_video_seterr("не удалось разобрать AVI");
        free(v);
        return NULL;
    }

    if (backend == AG_VIDEO_BACKEND_MJPEG) {
        if (ag_mjpeg_open(v, path, &st)) {
            v->backend = AG_VIDEO_BACKEND_MJPEG;
            v->stream = st;
            ag_video_apply_stream(v, st);
            ag_video_seterr("");
            return v;
        }
        ag_video_seterr("не удалось разобрать MJPEG");
        free(v);
        return NULL;
    }

    ag_video_seterr("неизвестный формат видео");
    free(v);
    return NULL;
}

ag_video *ag_video_open(const char *path)
{
    return ag_video_open_ex(path, AG_VIDEO_BACKEND_AUTO);
}

void ag_video_close(ag_video *v)
{
    if (!v) return;

    if (v->lk) {
        ag_mutex_lock(v->lk);
        v->run = 0;
        v->playing = 0;
        ag_mutex_unlock(v->lk);
        if (v->thr) {
            ag_thread_join(v->thr);
            v->thr = NULL;
        }
    }
    close_media(v);
    ag_vstream_free(v->stream);
    ag_image_destroy(v->frame);
    free(v);
}

int ag_video_get_size(const ag_video *v, int *w, int *h)
{
    if (!v) return 0;
    if (w) *w = v->w;
    if (h) *h = v->h;
    return 1;
}

double ag_video_duration(const ag_video *v) { return v ? v->duration : 0.0; }
double ag_video_fps(const ag_video *v)      { return v ? v->fps : 0.0; }

double ag_video_time(const ag_video *v)
{
    if (!v) return 0.0;
    if (v->dmx && v->lk) {
        double t;
        ag_mutex_lock(v->lk);
        t = v->playing ? v->clock : v->time;
        ag_mutex_unlock(v->lk);
        return t;
    }
    return v->time;
}

int ag_video_frame_count(const ag_video *v)
{
    if (!v) return 0;
    if (v->dmx) return (int)v->vcnt;
    return v->stream ? v->stream->nframes : 0;
}

int ag_video_has_audio(const ag_video *v)
{
    return (v && v->dmx && v->audio_active) ? 1 : 0;
}

double ag_video_clock(ag_video *v)
{
    if (!v || !v->dmx) return v ? v->time : 0.0;
    double c;
    ag_mutex_lock(v->lk);
    c = v->clock;
    ag_mutex_unlock(v->lk);
    return c;
}

void ag_video_set_volume(ag_video *v, double vol)
{
    if (!v || !v->dmx) return;
    if (vol < 0.0) vol = 0.0;
    if (vol > 1.0) vol = 1.0;
    ag_mutex_lock(v->lk);
    v->gain = (int)(vol * 256.0 + 0.5);
    ag_mutex_unlock(v->lk);
}

void ag_video_set_playing(ag_video *v, int play)
{
    if (!v || !v->dmx || !v->lk) return;

    ag_mutex_lock(v->lk);
    if (play && !v->playing) {
        v->playing = 1;
        v->aout_dropped = 0;
        if (!v->thr) {
            v->wall0 = ag_video_now() - v->clock;
            v->run = 1;
            v->thr = ag_thread_start(media_thread, v);
            if (!v->thr) { v->run = 0; v->playing = 0; }
        } else if (!v->aout) {
            v->wall0 = ag_video_now() - v->clock;
        }
    } else if (!play && v->playing) {
        v->playing = 0;
    }
    ag_mutex_unlock(v->lk);
}

int ag_video_next_frame(ag_video *v, ag_image **out)
{
    if (!v) return 0;
    if (out) *out = NULL;

    ag_vstream *st = v->stream;
    if (st) {
        if (st->idx >= st->nframes) return 0;
        for (;;) {
            if (st->idx >= st->nframes) return 0;
            ag_vframe *fr = &st->frames[st->idx];
            if (fseek(st->fp, fr->off, SEEK_SET) != 0) return 0;
            unsigned char *buf = (unsigned char *)malloc((size_t)fr->size);
            if (!buf) return 0;
            size_t got = fread(buf, 1, (size_t)fr->size, st->fp);
            if (got != (size_t)fr->size) { free(buf); st->idx++; continue; }

            ag_image *im = ag_jpeg_decode_mem(buf, got);
            free(buf);
            st->idx++;
            if (!im) continue;

            ag_image_destroy(v->frame);
            v->frame = im;
            if (v->w <= 0 || v->h <= 0) { v->w = im->w; v->h = im->h; }
            v->frame_index = st->idx - 1;
            v->time = (double)v->frame_index / (v->fps > 0.0 ? v->fps : 25.0);
            if (out) *out = im;
            return 1;
        }
    }

    if (!v->dmx) return 0;

    ag_mutex_lock(v->lk);

    if (v->thr && v->playing) {
        double lead = (v->fps > 0.0) ? 0.5 / v->fps : 0.02;
        if (v->fq_n > 0) {
            int h = v->fq_h;
            if (v->fq[h].pts <= v->clock + lead) {
                ag_image *im = v->fq[h].img;
                double pts = v->fq[h].pts;
                v->fq[h].img = NULL;
                v->fq_h = (h + 1) % AG_FQ_CAP;
                v->fq_n--;
                ag_image_destroy(v->frame);
                v->frame = im;
                v->time = pts;
                v->frame_index++;
                ag_mutex_unlock(v->lk);
                if (out) *out = im;
                return 1;
            }
            ag_mutex_unlock(v->lk);
            if (out) *out = v->frame;
            return v->frame ? 1 : 0;
        }
        if (v->eof) { ag_mutex_unlock(v->lk); return 0; }
        ag_mutex_unlock(v->lk);
        if (out) *out = v->frame;
        return v->frame ? 1 : 0;
    }

    while (v->thr && (v->busy || v->seek_pending)) {
        ag_mutex_unlock(v->lk);
        ag_video_msleep(1);
        ag_mutex_lock(v->lk);
    }
    int r = pull_media_frame(v, out);
    ag_mutex_unlock(v->lk);
    return r;
}

int ag_video_seek(ag_video *v, double seconds)
{
    if (!v) return 0;
    if (seconds < 0.0) seconds = 0.0;

    ag_vstream *st = v->stream;
    if (st) {
        double fps = v->fps > 0.0 ? v->fps : 25.0;
        int idx = (int)(seconds * fps);
        if (idx < 0) idx = 0;
        if (idx >= st->nframes) idx = st->nframes - 1;
        st->idx = idx;
        v->time = (double)idx / fps;
        v->frame_index = idx;
        return 1;
    }

    if (!v->dmx) return 0;

    ag_mutex_lock(v->lk);
    v->seek_target = seconds;
    v->seek_pending = 1;
    v->eof = 0;

    if (!v->thr) {
        apply_seek(v, seconds);
        ag_mutex_unlock(v->lk);
        return 1;
    }

    while (v->seek_pending && v->run) {
        ag_mutex_unlock(v->lk);
        ag_video_msleep(1);
        ag_mutex_lock(v->lk);
    }
    int ok = !v->seek_pending;
    ag_mutex_unlock(v->lk);
    return ok;
}
