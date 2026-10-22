
#include "aglib.h"
#include "aglib_int.h"
#include "agmedia.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static unsigned u32_(const unsigned char *p)
{
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) |
           ((unsigned)p[2] << 8) | (unsigned)p[3];
}

static unsigned u16_(const unsigned char *p)
{
    return ((unsigned)p[0] << 8) | (unsigned)p[1];
}

static unsigned long long u64_(const unsigned char *p)
{
    return ((unsigned long long)u32_(p) << 32) | (unsigned long long)u32_(p + 4);
}

static void seterr(char *err, int cap, const char *fmt, ...)
{
    if (!err || cap <= 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, (size_t)cap, fmt, ap);
    va_end(ap);
}


typedef struct {
    const unsigned char *body;
    size_t   bn;
    char     type[5];
} box_t;

static int next_box(const unsigned char **p, const unsigned char *end,
                    box_t *b)
{
    const unsigned char *start = *p;
    if (start + 8 > end) return 0;
    unsigned long long sz = u32_(start);
    const unsigned char *q = start + 8;
    if (sz == 1) {
        if (q + 8 > end) return 0;
        sz = u64_(q);
        q += 8;
    } else if (sz == 0) {
        sz = (unsigned long long)(end - start);
    }
    if (sz < (unsigned long long)(q - start) || start + sz > end) return 0;

    memcpy(b->type, start + 4, 4);
    b->type[4] = 0;
    b->body = q;
    b->bn = (size_t)((start + sz) - q);
    *p = start + (size_t)sz;
    return 1;
}

static int find_box(const unsigned char *data, size_t n, const char *type,
                    box_t *out)
{
    const unsigned char *p = data;
    const unsigned char *end = data + n;
    box_t b;
    while (next_box(&p, end, &b))
        if (memcmp(b.type, type, 4) == 0) { *out = b; return 1; }
    return 0;
}


typedef struct {
    int      present;
    int      codec;
    char     fourcc[5];
    long     ts;
    long     n;
    long     idx;
    long long *off;
    unsigned *sz;
    double   *dts;
    double   *pts;
    unsigned char *key;
    double   *out_pts;
    long      out_i;
    int      w, h;
    int      rate, channels;
    unsigned char *avcc; size_t avcc_len;
    unsigned char *asc;  size_t asc_len;
} track_t;

typedef struct ag_mp4 {
    FILE    *f;
    track_t  v, a;
    double   duration;
    unsigned char *buf[2];
    size_t   buf_cap[2];
} ag_mp4;

static void track_free(track_t *t)
{
    free(t->off); free(t->sz); free(t->dts); free(t->pts);
    free(t->key); free(t->out_pts); free(t->avcc); free(t->asc);
    memset(t, 0, sizeof(*t));
}

static const unsigned char *vble(const unsigned char *p,
                                 const unsigned char *end, size_t *out)
{
    size_t v = 0;
    for (int i = 0; i < 4 && p < end; ++i) {
        unsigned char c = *p++;
        v = (v << 7) | (size_t)(c & 0x7F);
        if (!(c & 0x80)) { *out = v; return p; }
    }
    return NULL;
}


static int parse_esds(const unsigned char *d, size_t n, track_t *t,
                      char *err, int errcap)
{
    if (n < 8) return 0;
    const unsigned char *p = d + 4;
    const unsigned char *end = d + n;

    if (p >= end || *p != 0x03) return 0;
    ++p;
    size_t len = 0;
    p = vble(p, end, &len);
    if (!p || p + 3 > end) return 0;
    p += 3;

    while (p < end) {
        int tag = *p++;
        size_t ln = 0;
        const unsigned char *body = vble(p, end, &ln);
        if (!body || body + ln > end) break;

        if (tag == 0x04 && ln >= 13) {
            unsigned ot = body[0];
            const unsigned char *q = body + 13;
            const unsigned char *bend = body + ln;

            if (ot == 0x69 || ot == 0x6B) {
                t->codec = AG_ACODEC_MP3;
                return 1;
            }
            while (q < bend) {
                int t2 = *q++;
                size_t l2 = 0;
                const unsigned char *b2 = vble(q, bend, &l2);
                if (!b2 || b2 + l2 > bend) break;
                if (t2 == 0x05) {
                    if (ot == 0x40 || ot == 0x66 || ot == 0x67 || ot == 0x68) {
                        t->codec = AG_ACODEC_AAC;
                        t->asc = (unsigned char *)malloc(l2 ? l2 : 1);
                        if (t->asc) {
                            memcpy(t->asc, b2, l2);
                            t->asc_len = l2;
                        }
                        return t->asc != NULL;
                    }
                    snprintf(t->fourcc, sizeof(t->fourcc), "ot%02x", ot);
                    t->codec = AG_ACODEC_UNSUPPORTED;
                    seterr(err, errcap,
                           "звук «%s» не поддерживается (нужен AAC или MP3)",
                           t->fourcc);
                    return 0;
                }
                q = b2 + l2;
            }
            snprintf(t->fourcc, sizeof(t->fourcc), "ot%02x", ot);
            t->codec = AG_ACODEC_UNSUPPORTED;
            seterr(err, errcap,
                   "звук «%s» не поддерживается (нужен AAC или MP3)",
                   t->fourcc);
            return 0;
        }
        p = body + ln;
    }
    return 0;
}


static int parse_video_entry(track_t *t, const unsigned char *e, size_t n,
                             char *err, int errcap)
{
    if (n < 86) return 0;
    memcpy(t->fourcc, e + 4, 4);
    t->fourcc[4] = 0;
    t->w = (int)u16_(e + 32);
    t->h = (int)u16_(e + 34);

    int is_h264 = memcmp(e + 4, "avc1", 4) == 0 ||
                  memcmp(e + 4, "avc3", 4) == 0;
    int is_mjpg = memcmp(e + 4, "jpeg", 4) == 0 ||
                  memcmp(e + 4, "mjpg", 4) == 0 ||
                  memcmp(e + 4, "mjpa", 4) == 0 ||
                  memcmp(e + 4, "mjpb", 4) == 0;

    if (is_h264) {
        t->codec = AG_VCODEC_H264;
        box_t avcc;
        if (find_box(e + 86, n - 86, "avcC", &avcc)) {
            t->avcc = (unsigned char *)malloc(avcc.bn ? avcc.bn : 1);
            if (t->avcc) {
                memcpy(t->avcc, avcc.body, avcc.bn);
                t->avcc_len = avcc.bn;
            }
        }
        if (!t->avcc || !t->avcc_len) {
            seterr(err, errcap, "в mp4 нет параметров H.264 (avcC)");
            return 0;
        }
        return 1;
    }
    if (is_mjpg) {
        t->codec = AG_VCODEC_MJPEG;
        return 1;
    }

    t->codec = AG_VCODEC_UNSUPPORTED;
    seterr(err, errcap,
           "видео-кодек «%s» не поддерживается — нужны H.264 или MJPEG",
           t->fourcc);
    return 0;
}

static int parse_audio_entry(track_t *t, const unsigned char *e, size_t n,
                             char *err, int errcap)
{
    if (n < 36) return 0;
    memcpy(t->fourcc, e + 4, 4);
    t->fourcc[4] = 0;

    t->channels = (int)u16_(e + 24);
    unsigned sr = u32_(e + 36) >> 16;
    if (sr) t->rate = (int)sr;

    unsigned version = u16_(e + 16);
    if (version > 1 || (n <= 52 && version == 1) || n <= 36) {
        seterr(err, errcap, "неподдерживаемая запись звука «%s»", t->fourcc);
        return 0;
    }
    size_t child_at = (version == 1) ? 52 : 36;

    box_t esds;
    if (!find_box(e + child_at, n - child_at, "esds", &esds)) {
        seterr(err, errcap, "у звука «%s» нет описания потока (esds)",
               t->fourcc);
        return 0;
    }
    return parse_esds(esds.body, esds.bn, t, err, errcap);
}


static int build_tables(track_t *t, const unsigned char *stbl, size_t n,
                        long ts, char *err, int errcap)
{
    box_t b;

    if (!find_box(stbl, n, "stsz", &b) || b.bn < 12) {
        seterr(err, errcap, "повреждён mp4: нет таблицы размеров (stsz)");
        return 0;
    }
    unsigned fixed = u32_(b.body + 4);
    unsigned count = u32_(b.body + 8);
    if (count == 0 || count > 100000000L) {
        seterr(err, errcap, "повреждён mp4: пустая таблица кадров");
        return 0;
    }
    if (!fixed && b.bn < 12 + (size_t)count * 4) {
        seterr(err, errcap, "повреждён mp4: обрезана таблица размеров");
        return 0;
    }

    t->n = (long)count;
    t->ts = ts;
    t->sz = (unsigned *)malloc((size_t)count * sizeof(unsigned));
    t->dts = (double *)malloc((size_t)count * sizeof(double));
    t->pts = (double *)malloc((size_t)count * sizeof(double));
    t->key = (unsigned char *)malloc((size_t)count);
    t->off = (long long *)malloc((size_t)count * sizeof(long long));
    if (!t->sz || !t->dts || !t->pts || !t->key || !t->off) {
        seterr(err, errcap, "не хватает памяти на таблицы mp4");
        return 0;
    }
    if (fixed) {
        for (unsigned i = 0; i < count; ++i) t->sz[i] = fixed;
    } else {
        const unsigned char *p = b.body + 12;
        for (unsigned i = 0; i < count; ++i, p += 4) t->sz[i] = u32_(p);
    }

    if (!find_box(stbl, n, "stts", &b) || b.bn < 8) {
        seterr(err, errcap, "повреждён mp4: нет таблицы длительностей (stts)");
        return 0;
    }
    {
        unsigned entries = u32_(b.body + 4);
        if (8 + (size_t)entries * 8 > b.bn)
            entries = (unsigned)((b.bn - 8) / 8);
        const unsigned char *p = b.body + 8;
        unsigned long long tick = 0;
        long i = 0;
        for (unsigned e = 0; e < entries && i < t->n; ++e, p += 8) {
            unsigned cnt = u32_(p);
            unsigned delta = u32_(p + 4);
            for (unsigned k = 0; k < cnt && i < t->n; ++k, ++i) {
                t->dts[i] = (double)tick / (double)ts;
                tick += delta;
            }
        }
        for (; i < t->n; ++i)
            t->dts[i] = i ? t->dts[i - 1] : 0.0;
    }

    memcpy(t->pts, t->dts, (size_t)t->n * sizeof(double));
    if (find_box(stbl, n, "ctts", &b) && b.bn >= 8) {
        unsigned ver = b.body[0];
        unsigned entries = u32_(b.body + 4);
        if (8 + (size_t)entries * 8 > b.bn)
            entries = (unsigned)((b.bn - 8) / 8);
        const unsigned char *p = b.body + 8;
        unsigned i = 0;
        for (unsigned e = 0; e < entries && i < count; ++e, p += 8) {
            unsigned cnt = u32_(p);
            long long off = ver ? (long long)(int)u32_(p + 4)
                                : (long long)u32_(p + 4);
            for (unsigned k = 0; k < cnt && i < count; ++k, ++i)
                t->pts[i] = t->dts[i] + (double)off / (double)ts;
        }
    }

    memset(t->key, 1, (size_t)t->n);
    if (find_box(stbl, n, "stss", &b) && b.bn >= 8) {
        unsigned entries = u32_(b.body + 4);
        if (8 + (size_t)entries * 4 > b.bn)
            entries = (unsigned)((b.bn - 8) / 4);
        memset(t->key, 0, (size_t)t->n);
        const unsigned char *p = b.body + 8;
        for (unsigned e = 0; e < entries; ++e, p += 4) {
            unsigned s = u32_(p);
            if (s >= 1 && s <= count) t->key[s - 1] = 1;
        }
    }

    box_t stsc, stco;
    if (!find_box(stbl, n, "stsc", &stsc) || stsc.bn < 8 ||
        (!find_box(stbl, n, "stco", &stco) &&
         !find_box(stbl, n, "co64", &stco)) || stco.bn < 8) {
        seterr(err, errcap, "повреждён mp4: нет таблицы смещений");
        return 0;
    }
    {
        unsigned sc = u32_(stsc.body + 4);
        unsigned cc = u32_(stco.body + 4);
        if (8 + (size_t)sc * 12 > stsc.bn) sc = (unsigned)((stsc.bn - 8) / 12);
        int co64 = memcmp(stco.type, "co64", 4) == 0;
        size_t crec = co64 ? 8 : 4;
        if (8 + (size_t)cc * crec > stco.bn)
            cc = (unsigned)((stco.bn - 8) / crec);

        const unsigned char *scp = stsc.body + 8;
        const unsigned char *cp = stco.body + 8;
        unsigned run = 0;
        unsigned spc = sc ? u32_(scp + 4) : 0;
        long si = 0;

        for (unsigned ci = 0; ci < cc && si < t->n; ++ci) {
            unsigned chunk_no = ci + 1;
            while (run + 1 < sc) {
                unsigned nf = u32_(scp + (size_t)(run + 1) * 12);
                if (nf <= chunk_no) {
                    ++run;
                    spc = u32_(scp + (size_t)run * 12 + 4);
                } else {
                    break;
                }
            }
            long long coff = co64 ? (long long)u64_(cp) : (long long)u32_(cp);
            cp += crec;
            for (unsigned k = 0; k < spc && si < t->n; ++k, ++si) {
                t->off[si] = coff;
                coff += t->sz[si];
            }
        }
        for (; si < t->n; ++si)
            t->off[si] = si ? t->off[si - 1] : 0;
    }
    return 1;
}


static int parse_trak(ag_mp4 *m, const unsigned char *d, size_t n,
                      int want_video, char *err, int errcap)
{
    box_t mdia, hdlr, mdhd, minf, stbl, stsd;
    if (!find_box(d, n, "mdia", &mdia)) return 0;
    if (!find_box(mdia.body, mdia.bn, "hdlr", &hdlr) || hdlr.bn < 12)
        return 0;

    int is_video = memcmp(hdlr.body + 8, "vide", 4) == 0;
    int is_audio = memcmp(hdlr.body + 8, "soun", 4) == 0;
    if (!is_video && !is_audio) return 0;
    if (want_video != is_video) return 0;

    track_t *t = want_video ? &m->v : &m->a;
    if (t->present) return 0;

    long ts = 90000;
    if (find_box(mdia.body, mdia.bn, "mdhd", &mdhd) && mdhd.bn >= 20) {
        unsigned ver = mdhd.body[0];
        ts = ver ? (long)u32_(mdhd.body + 20) : (long)u32_(mdhd.body + 12);
        if (ts <= 0) ts = 90000;
    }

    if (!find_box(mdia.body, mdia.bn, "minf", &minf) ||
        !find_box(minf.body, minf.bn, "stbl", &stbl) ||
        !find_box(stbl.body, stbl.bn, "stsd", &stsd) || stsd.bn < 16)
        return 0;

    box_t eb;
    {
        const unsigned char *p = stsd.body + 8;
        if (!next_box(&p, stsd.body + stsd.bn, &eb)) return 0;
    }
    const unsigned char *entry = eb.body - 8;
    size_t entry_n = eb.bn + 8;

    char aerr[256];
    char *e = err;
    int ecap = errcap;
    if (!is_video) { aerr[0] = 0; e = aerr; ecap = (int)sizeof(aerr); }

    track_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    int ok = is_video ? parse_video_entry(&tmp, entry, entry_n, e, ecap)
                      : parse_audio_entry(&tmp, entry, entry_n, e, ecap);
    if (!ok) {
        track_free(&tmp);
        return is_video ? -1 : 0;
    }

    if (!build_tables(&tmp, stbl.body, stbl.bn, ts, e, ecap)) {
        track_free(&tmp);
        return is_video ? -1 : 0;
    }
    if (tmp.n <= 0) {
        track_free(&tmp);
        return 0;
    }

    *t = tmp;
    t->present = 1;
    return 1;
}

static int parse_moov(ag_mp4 *m, const unsigned char *d, size_t n,
                      char *err, int errcap)
{
    box_t b;
    if (find_box(d, n, "mvhd", &b) && b.bn >= 20) {
        unsigned ver = b.body[0];
        unsigned long long ts = ver ? u32_(b.body + 20) : u32_(b.body + 12);
        unsigned long long dur = ver ? u64_(b.body + 24) : u32_(b.body + 16);
        if (ts) m->duration = (double)dur / (double)ts;
    }

    char first_err[256];
    first_err[0] = 0;
    int video_seen = 0;

    const unsigned char *p = d;
    const unsigned char *end = d + n;
    box_t trak;
    while (next_box(&p, end, &trak)) {
        if (memcmp(trak.type, "trak", 4) != 0) continue;

        box_t mdia, hdlr;
        int is_video = 0;
        if (find_box(trak.body, trak.bn, "mdia", &mdia) &&
            find_box(mdia.body, mdia.bn, "hdlr", &hdlr) && hdlr.bn >= 12)
            is_video = memcmp(hdlr.body + 8, "vide", 4) == 0;
        if (is_video) video_seen = 1;

        char *e = is_video && !first_err[0] ? first_err : NULL;
        char local[256];
        char *use = e ? local : NULL;
        if (is_video && !first_err[0]) use = first_err;
        else if (is_video) use = local;
        else use = local;

        int r = parse_trak(m, trak.body, trak.bn, is_video,
                           use, (int)sizeof(local));
        if (r < 0) continue;
    }

    if (!m->v.present) {
        if (first_err[0])
            seterr(err, errcap, "%s", first_err);
        else if (!video_seen)
            seterr(err, errcap, "в файле нет видеодорожки");
        else
            seterr(err, errcap, "не удалось разобрать видеодорожку mp4");
        return 0;
    }
    seterr(err, errcap, "");
    return 1;
}


static void mp4_close(void *self)
{
    ag_mp4 *m = (ag_mp4 *)self;
    if (!m) return;
    if (m->f) fclose(m->f);
    track_free(&m->v);
    track_free(&m->a);
    free(m->buf[0]);
    free(m->buf[1]);
    free(m);
}

static int mp4_read(void *self, int track, ag_packet *out)
{
    ag_mp4 *m = (ag_mp4 *)self;
    int k = (track == 0) ? 0 : 1;
    track_t *t = k ? &m->a : &m->v;
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

static int mp4_seek(void *self, int track, double seconds)
{
    ag_mp4 *m = (ag_mp4 *)self;
    track_t *t = (track == 0) ? &m->v : &m->a;
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

static void mp4_vinfo(void *self, int *w, int *h, double *fps, double *dur)
{
    ag_mp4 *m = (ag_mp4 *)self;
    track_t *t = &m->v;
    double span = (t->n > 1) ? (t->pts[t->n - 1] - t->pts[0]) : 0.0;
    double d = m->duration;
    if (d <= 0.0 && span > 0.0 && t->n > 1)
        d = span + span / (double)(t->n - 1);

    if (w) *w = t->w;
    if (h) *h = t->h;
    if (dur) *dur = d;

    if (fps) {
        double f = 0.0;
        if (span > 0.0 && t->n > 1)
            f = (double)(t->n - 1) / span;
        else if (d > 0.0 && t->n > 0)
            f = (double)t->n / d;
        if (f < 1.0 || f > 1000.0) f = 25.0;
        *fps = f;
    }
}

static int mp4_vcodec(void *self)  { return ((ag_mp4 *)self)->v.codec; }
static int mp4_acodec(void *self)  { return ((ag_mp4 *)self)->a.codec; }

static const unsigned char *mp4_avcc(void *self, size_t *sz)
{
    ag_mp4 *m = (ag_mp4 *)self;
    if (sz) *sz = m->v.avcc_len;
    return m->v.avcc;
}

static const unsigned char *mp4_asc(void *self, size_t *sz)
{
    ag_mp4 *m = (ag_mp4 *)self;
    if (sz) *sz = m->a.asc_len;
    return m->a.asc;
}

static int mp4_arate(void *self)  { return ((ag_mp4 *)self)->a.rate; }
static int mp4_achan(void *self)  { return ((ag_mp4 *)self)->a.channels; }

static long mp4_vcount(void *self)
{
    ag_mp4 *m = (ag_mp4 *)self;
    return m->v.present ? m->v.n : 0;
}

static double mp4_next_out_pts(void *self)
{
    ag_mp4 *m = (ag_mp4 *)self;
    track_t *t = &m->v;
    if (!t->out_pts || t->n == 0) return 0.0;
    if (t->out_i >= t->n) t->out_i = t->n - 1;
    return t->out_pts[t->out_i++];
}


static int cmp_dbl(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

ag_demux *ag_mp4_open(const char *path, char *err, int errcap)
{
    seterr(err, errcap, "");
    FILE *f = fopen(path, "rb");
    if (!f) {
        seterr(err, errcap, "не удалось открыть файл «%s»", path);
        return NULL;
    }
    setvbuf(f, NULL, _IOFBF, 256 * 1024);

    unsigned char *moov = NULL;
    size_t moov_len = 0;
    int saw_ftyp = 0;
    for (;;) {
        unsigned char hdr[8];
        if (fread(hdr, 1, 8, f) != 8) break;
        unsigned long long sz = u32_(hdr);
        if (sz == 1) {
            unsigned char ext[8];
            if (fread(ext, 1, 8, f) != 8) break;
            sz = u64_(ext);
        } else if (sz == 0) {
            long cur = ftell(f);
            if (fseek(f, 0, SEEK_END) != 0) break;
            sz = (unsigned long long)(ftell(f) - cur);
            if (fseek(f, cur, SEEK_SET) != 0) break;
        }
        if (sz < 8) break;

        if (memcmp(hdr + 4, "ftyp", 4) == 0) saw_ftyp = 1;
        if (memcmp(hdr + 4, "moov", 4) == 0) {
            size_t body = (size_t)(sz - 8);
            moov = (unsigned char *)malloc(body ? body : 1);
            if (moov && body && fread(moov, 1, body, f) == body) {
                moov_len = body;
                break;
            }
            free(moov);
            moov = NULL;
            break;
        }
        if (fseek(f, (long)(sz - 8), SEEK_CUR) != 0) break;
    }

    if (!moov) {
        fclose(f);
        seterr(err, errcap, saw_ftyp
               ? "повреждён mp4: нет таблицы треков (moov)"
               : "не похоже на mp4/mov");
        return NULL;
    }

    ag_mp4 *m = (ag_mp4 *)calloc(1, sizeof(ag_mp4));
    if (!m) { free(moov); fclose(f); return NULL; }
    m->f = f;

    int ok = parse_moov(m, moov, moov_len, err, errcap);
    free(moov);
    if (!ok) {
        if (err && errcap > 0 && !err[0])
            seterr(err, errcap, "не удалось разобрать mp4");
        mp4_close(m);
        return NULL;
    }

    track_t *t = &m->v;
    t->out_pts = (double *)malloc((size_t)t->n * sizeof(double));
    if (t->out_pts) {
        memcpy(t->out_pts, t->pts, (size_t)t->n * sizeof(double));
        qsort(t->out_pts, (size_t)t->n, sizeof(double), cmp_dbl);
    }

    ag_demux *d = (ag_demux *)calloc(1, sizeof(ag_demux));
    if (!d) { mp4_close(m); return NULL; }
    d->self = m;
    d->close = mp4_close;
    d->read = mp4_read;
    d->seek = mp4_seek;
    d->vinfo = mp4_vinfo;
    d->vcodec = mp4_vcodec;
    d->acodec = mp4_acodec;
    d->avcc = mp4_avcc;
    d->asc = mp4_asc;
    d->arate = mp4_arate;
    d->achan = mp4_achan;
    d->vcount = mp4_vcount;
    d->next_out_pts = mp4_next_out_pts;
    return d;
}
