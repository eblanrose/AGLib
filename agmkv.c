
#include "aglib.h"
#include "aglib_int.h"
#include "agmedia.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define EBML_HEADER       0x1A45DFA3ULL
#define EBML_SEGMENT      0x18538067ULL
#define EBML_INFO         0x1549A966ULL
#define EBML_TIMESCALE    0x2AD7B1ULL
#define EBML_DURATION     0x4489ULL
#define EBML_TRACKS       0x1654AE6BULL
#define EBML_TRACKENTRY   0xAEULL
#define EBML_TRACKNUM     0xD7ULL
#define EBML_TRACKTYPE    0x83ULL
#define EBML_CODECID      0x86ULL
#define EBML_CODECPRIVATE 0x63A2ULL
#define EBML_VIDEO        0xE0ULL
#define EBML_AUDIO        0xE1ULL
#define EBML_PIXW         0xB0ULL
#define EBML_PIXH         0xBAULL
#define EBML_RATE         0xB5ULL
#define EBML_CHANNELS     0x9FULL
#define EBML_DEFDUR       0x23E383ULL
#define EBML_CLUSTER      0x1F43B675ULL
#define EBML_CTIME        0xE7ULL
#define EBML_SIMPLEBLOCK  0xA3ULL
#define EBML_BLOCKGROUP   0xA1ULL
#define EBML_BLOCK        0xA1ULL
#define EBML_REFBLOCK     0xFBULL


typedef struct {
    int            present;
    int            codec;
    long long      track_no;
    long long     *off;
    unsigned      *sz;
    double        *pts;
    unsigned char *key;
    long           n, cap, idx;
    double        *out_pts;
    long           out_i;
    int            w, h;
    int            rate, channels;
    double         default_dur;
    unsigned char *avcc; size_t avcc_len;
    unsigned char *asc;  size_t asc_len;
} mkv_track;

typedef struct ag_mkv {
    FILE        *f;
    mkv_track    v, a;
    double       duration;
    double       scale;
    unsigned char *buf[2];
    size_t       buf_cap[2];
} ag_mkv;

static void seterr(char *err, int cap, const char *fmt, ...)
{
    if (!err || cap <= 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, (size_t)cap, fmt, ap);
    va_end(ap);
}

static void track_free(mkv_track *t)
{
    free(t->off); free(t->sz); free(t->pts); free(t->key);
    free(t->out_pts); free(t->avcc); free(t->asc);
    memset(t, 0, sizeof(*t));
}


static int rd(FILE *f, void *buf, size_t n)
{
    return fread(buf, 1, n, f) == n;
}

static int vint_len(unsigned char b)
{
    if (b & 0x80) return 1;
    if (b & 0x40) return 2;
    if (b & 0x20) return 3;
    if (b & 0x10) return 4;
    if (b & 0x08) return 5;
    if (b & 0x04) return 6;
    if (b & 0x02) return 7;
    if (b & 0x01) return 8;
    return 0;
}

static int read_vint(FILE *f, unsigned long long *out, int keep)
{
    unsigned char b;
    if (!rd(f, &b, 1)) return 0;
    int len = vint_len(b);
    if (len == 0) return 0;

    unsigned long long v = b;
    if (!keep)
        v &= (len == 8) ? 0ULL : (unsigned long long)((1ULL << (8 - len)) - 1);
    for (int i = 1; i < len; ++i) {
        unsigned char c;
        if (!rd(f, &c, 1)) return 0;
        v = (v << 8) | c;
    }
    if (!keep) {
        unsigned long long all = (len == 8)
            ? 0x00FFFFFFFFFFFFFFULL
            : ((1ULL << (7 * len)) - 1);
        if (v == all) { *out = ~0ULL; return 1; }
    }
    *out = v;
    return 1;
}

typedef struct {
    unsigned long long id;
    unsigned long long size;
    long long body;
} el_t;

static int el_read(FILE *f, el_t *e)
{
    if (!read_vint(f, &e->id, 1)) return 0;
    if (!read_vint(f, &e->size, 0)) return 0;
    e->body = ftell(f);
    return e->body >= 0;
}

static int el_seek_end(FILE *f, const el_t *e)
{
    if (e->size == ~0ULL) return 0;
    return fseek(f, (long)e->body + (long)e->size - (long)ftell(f),
                 SEEK_CUR) == 0;
}

static int read_uint(FILE *f, unsigned long long size, unsigned long long *out)
{
    if (size == 0 || size > 8) return 0;
    unsigned char b[8] = { 0 };
    if (!rd(f, b, (size_t)size)) return 0;
    unsigned long long v = 0;
    for (unsigned long long i = 0; i < size; ++i) v = (v << 8) | b[i];
    *out = v;
    return 1;
}

static int read_float(FILE *f, unsigned long long size, double *out)
{
    unsigned char b[8] = { 0 };
    if (size != 4 && size != 8) return 0;
    if (!rd(f, b, (size_t)size)) return 0;
    if (size == 4) {
        unsigned u = ((unsigned)b[0] << 24) | ((unsigned)b[1] << 16) |
                     ((unsigned)b[2] << 8) | (unsigned)b[3];
        float fv;
        memcpy(&fv, &u, 4);
        *out = fv;
    } else {
        unsigned long long u = 0;
        for (int i = 0; i < 8; ++i) u = (u << 8) | b[i];
        double dv;
        memcpy(&dv, &u, 8);
        *out = dv;
    }
    return 1;
}


static int add_sample(mkv_track *t, long long off, unsigned sz, double pts,
                      int key)
{
    if (t->n >= t->cap) {
        long nc = t->cap ? t->cap * 2 : 1024;
        long long *no = (long long *)realloc(t->off, (size_t)nc * sizeof(long long));
        unsigned *ns = (unsigned *)realloc(t->sz, (size_t)nc * sizeof(unsigned));
        double *np = (double *)realloc(t->pts, (size_t)nc * sizeof(double));
        unsigned char *nk = (unsigned char *)realloc(t->key, (size_t)nc);
        if (!no || !ns || !np || !nk) {
            if (no) t->off = no;
            if (ns) t->sz = ns;
            if (np) t->pts = np;
            if (nk) t->key = nk;
            return 0;
        }
        t->off = no; t->sz = ns; t->pts = np; t->key = nk;
        t->cap = nc;
    }
    t->off[t->n] = off;
    t->sz[t->n] = sz;
    t->pts[t->n] = pts;
    t->key[t->n] = (unsigned char)(key ? 1 : 0);
    t->n++;
    return 1;
}


static int cmp_dbl(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static int load_private(ag_mkv *m, mkv_track *t, long long off,
                        unsigned long long size)
{
    if (size == 0 || size > 1024 * 1024) return 0;
    unsigned char *buf = (unsigned char *)malloc((size_t)size);
    if (!buf) return 0;
    if (fseek(m->f, (long)off, SEEK_SET) != 0 ||
        fread(buf, 1, (size_t)size, m->f) != (size_t)size) {
        free(buf);
        return 0;
    }
    t->avcc = buf;
    t->avcc_len = (size_t)size;
    return 1;
}

static void parse_track_entry(ag_mkv *m, unsigned long long size,
                              char *err, int errcap)
{
    FILE *f = m->f;
    long long end = ftell(f) + (long long)size;
    long long track_no = 0, priv_off = 0;
    unsigned long long track_type = 0, priv_size = 0;
    int have_priv = 0;
    char codec[64] = { 0 };

    mkv_track vt;
    mkv_track at;
    memset(&vt, 0, sizeof(vt));
    memset(&at, 0, sizeof(at));
    int have_video = 0, have_audio = 0;
    double defdur = 0.0;

    while (ftell(f) < end) {
        el_t e;
        if (!el_read(f, &e)) break;
        if (e.id == EBML_TRACKNUM) {
            unsigned long long v;
            if (read_uint(f, e.size, &v)) track_no = (long long)v;
        } else if (e.id == EBML_TRACKTYPE) {
            unsigned long long v;
            if (read_uint(f, e.size, &v)) track_type = v;
        } else if (e.id == EBML_CODECID) {
            if (e.size > 0 && e.size < sizeof(codec) - 1) {
                if (rd(f, codec, (size_t)e.size)) codec[e.size] = 0;
            }
        } else if (e.id == EBML_CODECPRIVATE) {
            priv_off = e.body;
            priv_size = e.size;
            have_priv = 1;
        } else if (e.id == EBML_VIDEO) {
            long long vend = e.body + (long long)e.size;
            while (e.size != ~0ULL && ftell(f) < vend) {
                el_t ve;
                if (!el_read(f, &ve)) break;
                if (ve.id == EBML_PIXW) {
                    unsigned long long v;
                    if (read_uint(f, ve.size, &v)) vt.w = (int)v;
                } else if (ve.id == EBML_PIXH) {
                    unsigned long long v;
                    if (read_uint(f, ve.size, &v)) vt.h = (int)v;
                }
                if (!el_seek_end(f, &ve)) break;
            }
            have_video = 1;
        } else if (e.id == EBML_AUDIO) {
            long long aend = e.body + (long long)e.size;
            while (e.size != ~0ULL && ftell(f) < aend) {
                el_t ae;
                if (!el_read(f, &ae)) break;
                if (ae.id == EBML_RATE) {
                    double d;
                    if (read_float(f, ae.size, &d) && d > 0 && d < 1000000)
                        at.rate = (int)(d + 0.5);
                } else if (ae.id == EBML_CHANNELS) {
                    unsigned long long v;
                    if (read_uint(f, ae.size, &v) && v > 0 && v <= 8)
                        at.channels = (int)v;
                }
                if (!el_seek_end(f, &ae)) break;
            }
            have_audio = 1;
        } else if (e.id == EBML_DEFDUR) {
            unsigned long long v;
            if (read_uint(f, e.size, &v) && v > 0)
                defdur = (double)v / 1e9;
        }
        if (!el_seek_end(f, &e)) break;
    }

    if (track_type == 1 && have_video && !m->v.present && track_no > 0) {
        if (strcmp(codec, "V_MPEG4/ISO/AVC") == 0) {
            vt.codec = AG_VCODEC_H264;
            if (!have_priv || !load_private(m, &vt, priv_off, priv_size)) {
                seterr(err, errcap,
                       "у видеодорожки mkv нет avcC (CodecPrivate)");
                track_free(&vt);
                return;
            }
        } else if (strcmp(codec, "V_MJPEG") == 0) {
            vt.codec = AG_VCODEC_MJPEG;
        } else {
            seterr(err, errcap,
                   "видео-кодек «%s» не поддерживается — нужны H.264 или MJPEG",
                   codec);
            return;
        }
        vt.track_no = track_no;
        vt.default_dur = defdur;
        m->v = vt;
        m->v.present = 1;
    } else if (track_type == 2 && have_audio && !m->a.present && track_no > 0) {
        if (strncmp(codec, "A_AAC", 5) == 0) {
            at.codec = AG_ACODEC_AAC;
            if (!have_priv || !load_private(m, &at, priv_off, priv_size)) {
                track_free(&at);
                return;
            }
            at.asc = at.avcc;
            at.asc_len = at.avcc_len;
            at.avcc = NULL;
            at.avcc_len = 0;
        } else if (strcmp(codec, "A_MPEG/LAYER3") == 0) {
            at.codec = AG_ACODEC_MP3;
        } else {
            return;
        }
        at.track_no = track_no;
        m->a = at;
        m->a.present = 1;
    }
}

static void parse_tracks(ag_mkv *m, unsigned long long size,
                         char *err, int errcap)
{
    FILE *f = m->f;
    long long end = ftell(f) + (long long)size;
    while (size != ~0ULL && ftell(f) < end) {
        el_t e;
        if (!el_read(f, &e)) break;
        if (e.id == EBML_TRACKENTRY)
            parse_track_entry(m, e.size, err, errcap);
        if (!el_seek_end(f, &e)) break;
    }
}


static int read_block_head(FILE *f, unsigned long long body_size,
                           long long *track_no, int *rel_ts, int *flags,
                           unsigned long long *frame_size)
{
    long long start = ftell(f);
    unsigned long long tn;
    if (!read_vint(f, &tn, 0)) return 0;
    unsigned char b[3];
    if (!rd(f, b, 3)) return 0;
    *track_no = (long long)tn;
    *rel_ts = (short)((unsigned)(b[0] << 8) | (unsigned)b[1]);
    *flags = b[2];
    long long hdr = ftell(f) - start;
    if (hdr < 3 || (unsigned long long)hdr > body_size) return 0;
    *frame_size = body_size - (unsigned long long)hdr;
    return 1;
}

static int is_cluster_child(unsigned long long id)
{
    return id == EBML_CTIME || id == EBML_SIMPLEBLOCK ||
           id == EBML_BLOCKGROUP || id == 0xA0ULL  ||
           id == 0xABULL  || id == 0x73C3ULL ;
}

static int scan_cluster(ag_mkv *m, unsigned long long size,
                        long long *next_level1)
{
    FILE *f = m->f;
    long long cend = (size == ~0ULL) ? -1 : ftell(f) + (long long)size;
    double cts = 0.0;

    while (cend < 0 || ftell(f) < cend) {
        long long estart = ftell(f);
        el_t e;
        if (!el_read(f, &e)) break;

        if (cend < 0 && !is_cluster_child(e.id)) {
            fseek(f, estart, SEEK_SET);
            if (next_level1) *next_level1 = estart;
            return 1;
        }

        if (e.id == EBML_CTIME) {
            unsigned long long v;
            if (read_uint(f, e.size, &v)) cts = (double)v;
        } else if (e.id == EBML_SIMPLEBLOCK || e.id == EBML_BLOCK) {
            long long tn = 0;
            int rel = 0, flags = 0;
            unsigned long long fsz = 0;
            if (read_block_head(f, e.size, &tn, &rel, &flags, &fsz)) {
                long long data = ftell(f);
                int mine = (tn == m->v.track_no) || (tn == m->a.track_no);
                if (mine && (flags & 0x06)) return -1;
                if (mine && fsz > 0 && fsz < 0x10000000ULL) {
                    mkv_track *t = (tn == m->v.track_no) ? &m->v : &m->a;
                    double pts = (cts + (double)rel) * m->scale;
                    int key = (e.id == EBML_SIMPLEBLOCK) && (flags & 0x80);
                    if (!add_sample(t, data, (unsigned)fsz, pts, key))
                        return 0;
                }
            }
        } else if (e.id == EBML_BLOCKGROUP) {
            long long gend = (e.size == ~0ULL)
                                 ? -1 : ftell(f) + (long long)e.size;
            long long tn = 0, data = 0;
            int rel = 0, flags = 0, have_block = 0, ref = 0;
            unsigned long long fsz = 0;
            while (gend < 0 || ftell(f) < gend) {
                el_t be;
                if (!el_read(f, &be)) break;
                if (be.id == EBML_BLOCK) {
                    if (read_block_head(f, be.size, &tn, &rel, &flags, &fsz)) {
                        data = ftell(f);
                        have_block = 1;
                    }
                } else if (be.id == EBML_REFBLOCK) {
                    ref = 1;
                }
                if (!el_seek_end(f, &be)) break;
            }
            int mine = (tn == m->v.track_no) || (tn == m->a.track_no);
            if (mine && (flags & 0x06)) return -1;
            if (have_block && mine && fsz > 0 && fsz < 0x10000000ULL) {
                mkv_track *t = (tn == m->v.track_no) ? &m->v : &m->a;
                double pts = (cts + (double)rel) * m->scale;
                if (!add_sample(t, data, (unsigned)fsz, pts, !ref)) return 0;
            }
            continue;
        }

        if (!el_seek_end(f, &e)) {
            if (cend < 0) break;
            return 0;
        }
    }
    return 1;
}


static void mkv_close(void *self)
{
    ag_mkv *m = (ag_mkv *)self;
    if (!m) return;
    if (m->f) fclose(m->f);
    track_free(&m->v);
    track_free(&m->a);
    free(m->buf[0]);
    free(m->buf[1]);
    free(m);
}

static int mkv_read(void *self, int track, ag_packet *out)
{
    ag_mkv *m = (ag_mkv *)self;
    int k = (track == 0) ? 0 : 1;
    mkv_track *t = k ? &m->a : &m->v;
    if (!t->present) return 0;

    while (t->idx < t->n) {
        long i = t->idx++;
        size_t sz = t->sz[i];
        if (sz == 0) continue;

        if (m->buf_cap[k] < sz) {
            unsigned char *nb = (unsigned char *)realloc(m->buf[k], sz + 64);
            if (!nb) return 0;
            m->buf[k] = nb;
            m->buf_cap[k] = sz + 64;
        }
        if (fseek(m->f, (long)t->off[i], SEEK_SET) != 0) return 0;
        if (fread(m->buf[k], 1, sz, m->f) != sz) return 0;

        out->data = m->buf[k];
        out->size = sz;
        out->pts = t->pts[i];
        out->key = t->key[i] ? 1 : 0;
        return 1;
    }
    return 0;
}

static int mkv_seek(void *self, int track, double seconds)
{
    ag_mkv *m = (ag_mkv *)self;
    mkv_track *t = (track == 0) ? &m->v : &m->a;
    if (!t->present || t->n <= 0) return 0;

    long best = -1;
    if (track == 0) {
        for (long i = 0; i < t->n; ++i)
            if (t->key[i] && t->pts[i] <= seconds) best = i;
        if (best < 0)
            for (long i = 0; i < t->n; ++i)
                if (t->key[i]) { best = i; break; }
    } else {
        for (long i = 0; i < t->n; ++i)
            if (t->pts[i] <= seconds) best = i;
    }
    if (best < 0) best = 0;
    t->idx = best;

    if (track == 0 && t->out_pts) {
        double kp = t->pts[best];
        long lo = 0, hi = t->n - 1, pos = 0;
        while (lo <= hi) {
            long mid = (lo + hi) / 2;
            if (t->out_pts[mid] + 1e-9 >= kp) { pos = mid; hi = mid - 1; }
            else lo = mid + 1;
        }
        t->out_i = pos;
    }
    return 1;
}

static void mkv_vinfo(void *self, int *w, int *h, double *fps, double *dur)
{
    ag_mkv *m = (ag_mkv *)self;
    mkv_track *t = &m->v;
    double span = (t->n > 1) ? (t->pts[t->n - 1] - t->pts[0]) : 0.0;
    double d = m->duration;
    if (d <= 0.0 && t->n > 0) {
        double step = t->default_dur > 0.0
                          ? t->default_dur
                          : (span > 0.0 && t->n > 1
                                 ? span / (double)(t->n - 1)
                                 : 0.04);
        d = t->pts[t->n - 1] + step;
    }

    if (w) *w = t->w;
    if (h) *h = t->h;
    if (dur) *dur = d;

    if (fps) {
        double f = 0.0;
        if (t->default_dur > 0.0)
            f = 1.0 / t->default_dur;
        else if (span > 0.0 && t->n > 1)
            f = (double)(t->n - 1) / span;
        if (f < 1.0 || f > 1000.0) f = 25.0;
        *fps = f;
    }
}

static int mkv_vcodec(void *self) { return ((ag_mkv *)self)->v.codec; }
static int mkv_acodec(void *self) { return ((ag_mkv *)self)->a.codec; }

static const unsigned char *mkv_avcc(void *self, size_t *sz)
{
    ag_mkv *m = (ag_mkv *)self;
    if (sz) *sz = m->v.avcc_len;
    return m->v.avcc;
}

static const unsigned char *mkv_asc(void *self, size_t *sz)
{
    ag_mkv *m = (ag_mkv *)self;
    if (sz) *sz = m->a.asc_len;
    return m->a.asc;
}

static int mkv_arate(void *self) { return ((ag_mkv *)self)->a.rate; }
static int mkv_achan(void *self) { return ((ag_mkv *)self)->a.channels; }

static long mkv_vcount(void *self)
{
    ag_mkv *m = (ag_mkv *)self;
    return m->v.present ? m->v.n : 0;
}

static double mkv_next_out_pts(void *self)
{
    ag_mkv *m = (ag_mkv *)self;
    mkv_track *t = &m->v;
    if (!t->out_pts || t->n == 0) return 0.0;
    if (t->out_i >= t->n) t->out_i = t->n - 1;
    return t->out_pts[t->out_i++];
}


ag_demux *ag_mkv_open(const char *path, char *err, int errcap)
{
    seterr(err, errcap, "");
    FILE *f = fopen(path, "rb");
    if (!f) {
        seterr(err, errcap, "не удалось открыть файл «%s»", path);
        return NULL;
    }
    setvbuf(f, NULL, _IOFBF, 256 * 1024);

    el_t e;
    if (!el_read(f, &e) || e.id != EBML_HEADER || !el_seek_end(f, &e)) {
        fclose(f);
        seterr(err, errcap, "не похоже на mkv/webm");
        return NULL;
    }

    if (!el_read(f, &e) || e.id != EBML_SEGMENT) {
        fclose(f);
        seterr(err, errcap, "не похоже на mkv/webm: нет сегмента");
        return NULL;
    }
    long long seg_end = (e.size == ~0ULL) ? -1
                                          : e.body + (long long)e.size;

    ag_mkv *m = (ag_mkv *)calloc(1, sizeof(ag_mkv));
    if (!m) { fclose(f); return NULL; }
    m->f = f;
    m->scale = 1e-3;

    int lacing_err = 0;

    for (;;) {
        long long cur = ftell(f);
        if (seg_end >= 0 && cur >= seg_end) break;
        if (!el_read(f, &e)) break;

        if (e.id == EBML_INFO) {
            long long iend = (e.size == ~0ULL) ? -1
                                               : e.body + (long long)e.size;
            while (iend < 0 || ftell(f) < iend) {
                el_t ie;
                if (!el_read(f, &ie)) break;
                if (ie.id == EBML_TIMESCALE) {
                    unsigned long long v;
                    if (read_uint(f, ie.size, &v) && v > 0)
                        m->scale = (double)v / 1e9;
                } else if (ie.id == EBML_DURATION) {
                    double d;
                    if (read_float(f, ie.size, &d) && d > 0)
                        m->duration = d * m->scale;
                }
                if (!el_seek_end(f, &ie)) break;
            }
            if (e.size != ~0ULL) fseek(f, (long)e.body + (long)e.size, SEEK_SET);
        } else if (e.id == EBML_TRACKS) {
            parse_tracks(m, e.size, err, errcap);
            if (e.size != ~0ULL) fseek(f, (long)e.body + (long)e.size, SEEK_SET);
            if (!m->v.present) {
                mkv_close(m);
                if (err && errcap > 0 && !err[0])
                    seterr(err, errcap,
                           "в mkv нет пригодной видеодорожки "
                           "(нужен H.264 или MJPEG)");
                return NULL;
            }
        } else if (e.id == EBML_CLUSTER) {
            long long next = -1;
            int r = scan_cluster(m, e.size, &next);
            if (r < 0) {
                lacing_err = 1;
                break;
            }
            if (r == 0) break;
            if (e.size == ~0ULL) {
                if (next > 0) fseek(f, (long)next, SEEK_SET);
                break;
            }
            fseek(f, (long)e.body + (long)e.size, SEEK_SET);
        } else {
            if (e.size == ~0ULL) break;
            fseek(f, (long)e.body + (long)e.size, SEEK_SET);
        }
    }

    if (lacing_err) {
        mkv_close(m);
        seterr(err, errcap, "mkv: блоки со сжатием (lacing) не поддерживаются");
        return NULL;
    }
    if (m->v.n <= 0) {
        mkv_close(m);
        if (err && errcap > 0 && !err[0])
            seterr(err, errcap, "в mkv нет видеокадров");
        return NULL;
    }

    if (m->duration <= 0.0) {
        double step = m->v.default_dur > 0.0
                          ? m->v.default_dur
                          : (m->v.n > 1
                                 ? (m->v.pts[m->v.n - 1] - m->v.pts[0]) /
                                       (double)(m->v.n - 1)
                                 : 0.04);
        m->duration = m->v.pts[m->v.n - 1] + step;
    }

    m->v.out_pts = (double *)malloc((size_t)m->v.n * sizeof(double));
    if (m->v.out_pts) {
        memcpy(m->v.out_pts, m->v.pts, (size_t)m->v.n * sizeof(double));
        qsort(m->v.out_pts, (size_t)m->v.n, sizeof(double), cmp_dbl);
    }

    ag_demux *d = (ag_demux *)calloc(1, sizeof(ag_demux));
    if (!d) { mkv_close(m); return NULL; }
    d->self = m;
    d->close = mkv_close;
    d->read = mkv_read;
    d->seek = mkv_seek;
    d->vinfo = mkv_vinfo;
    d->vcodec = mkv_vcodec;
    d->acodec = mkv_acodec;
    d->avcc = mkv_avcc;
    d->asc = mkv_asc;
    d->arate = mkv_arate;
    d->achan = mkv_achan;
    d->vcount = mkv_vcount;
    d->next_out_pts = mkv_next_out_pts;
    seterr(err, errcap, "");
    return d;
}
