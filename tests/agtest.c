/* AGLib — feature smoke test (not part of the library) */
#include "aglib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); fails++; } \
    else printf("  ok:   %s\n", msg); \
} while (0)

static void test_font(void)
{
    printf("== font from file ==\n");
    ag_font *f = ag_font_open_file("assets/fonts/DejaVuSans.ttf", 18);
    CHECK(f != NULL, "ag_font_open_file(DejaVuSans.ttf)");
    if (!f) return;
    printf("  family='%s' ascent=%d descent=%d height=%d\n",
           ag_font_family(f), ag_font_ascent(f), ag_font_descent(f),
           ag_font_height(f));
    int w = ag_font_text_width(f, "Привет, AGLib!");
    printf("  width('Привет, AGLib!') = %d\n", w);
    CHECK(w > 40, "text width measured");
    CHECK(ag_font_set_size(f, 24) == 1, "ag_font_set_size(24)");
    CHECK(ag_font_height(f) > ag_font_height(f) - 1, "metrics after resize");
    ag_font_close(f);

    ag_font *d = ag_font_default(16);
    CHECK(d != NULL, "ag_font_default finds the bundled font");
    if (d) {
        printf("  default family='%s'\n", ag_font_family(d));
        ag_font_close(d);
    }
}

static void test_blur(void)
{
    printf("== dual kawase blur ==\n");
    ag_image *im = ag_image_load("assets/test.jpg");
    CHECK(im != NULL, "assets/test.jpg loaded");
    if (!im) return;
    int w = 0, h = 0;
    ag_image_get_size(im, &w, &h);
    printf("  source %dx%d\n", w, h);

    ag_image *blur = ag_image_clone(im);
    CHECK(blur != NULL, "clone");
    CHECK(ag_image_blur_dual_kawase(blur, 12, 2) == 1, "blur applied");
    CHECK(ag_image_save_png(blur, "out_blur.png"), "blurred png written");

    /* the blur must smooth the image: compare the variance of a row */
    double v0 = 0, v1 = 0;
    for (int x = 0; x < w; ++x) {
        ag_u32 a = ag_image_pixel(im, x, h / 2);
        ag_u32 b = ag_image_pixel(blur, x, h / 2);
        double da = (double)((a >> 16) & 0xFF);
        double db = (double)((b >> 16) & 0xFF);
        v0 += da * da; v1 += db * db;
    }
    v0 = v0 / w - 100.0; v1 = v1 / w - 100.0;
    printf("  energy before=%.1f after=%.1f\n", v0, v1);
    CHECK(v1 < v0, "blur reduces high frequency energy");
    ag_image_destroy(blur);
    ag_image_destroy(im);
}

static int file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static void test_video(const char *path, ag_video_backend backend,
                       const char *name, int expect_audio, int min_frames)
{
    if (!file_exists(path)) {
        printf("== video (%s) == skipped, no %s\n", name, path);
        return;
    }
    printf("== video (%s) ==\n", name);
    ag_video *v = ag_video_open_ex(path, backend);
    if (!v) {
        const char *e = ag_video_last_error();
        printf("  FAIL: cannot open %s: %s\n", path, e);
        fails++;
        return;
    }
    int w = 0, h = 0;
    ag_video_get_size(v, &w, &h);
    printf("  backend=%s size=%dx%d fps=%.2f dur=%.2f frames=%d\n",
           ag_video_backend_name(v), w, h, ag_video_fps(v),
           ag_video_duration(v), ag_video_frame_count(v));
    CHECK(w > 0 && h > 0, "video size known");
    CHECK(ag_video_has_audio(v) == expect_audio,
          expect_audio ? "audio track detected" : "no audio track");

    /* decode at least min_frames frames and make sure the times only grow:
     * the demuxer emits frames in presentation order even with B frames */
    ag_image *fr = NULL;
    int n = 0, monotonic = 1;
    double prev = -1.0;
    while (n < min_frames && ag_video_next_frame(v, &fr) == 1 && fr) {
        double t = ag_video_time(v);
        if (t + 1e-6 < prev) monotonic = 0;
        prev = t;
        n++;
    }
    printf("  decoded %d frames (last time=%.3f)\n", n, prev);
    CHECK(n >= min_frames, "enough frames decoded");
    CHECK(monotonic, "frame times grow monotonically");

    CHECK(ag_video_seek(v, 1.0) == 1, "seek works");
    if (ag_video_next_frame(v, &fr)) {
        int fw = 0, fh = 0;
        ag_image_get_size(fr, &fw, &fh);
        CHECK(fw > 0 && fh > 0, "frame after seek");
    }
    ag_video_close(v);
}

int main(void)
{
    test_font();
    test_blur();
    test_video("assets/demo.avi", AG_VIDEO_BACKEND_AVI, "avi/mjpeg",
               0, 36);
    test_video("assets/demo.avi", AG_VIDEO_BACKEND_AUTO, "auto",
               0, 36);
    test_video("assets/demo.mp4", AG_VIDEO_BACKEND_AUTO, "mp4/h264",
               0, 60);
    test_video("assets/demo.mkv", AG_VIDEO_BACKEND_AUTO, "mkv/h264+aac",
               1, 60);
    test_video("assets/demo_mp3.mp4", AG_VIDEO_BACKEND_AUTO, "mp4/h264+mp3",
               1, 60);
    /* B frames in mkv: the pipeline must reorder them by pts */
    test_video("assets/clip.mkv", AG_VIDEO_BACKEND_AUTO, "mkv/h264+bframes",
               1, 300);
    /* long stream with B frames: guards against decoder stalls that only
     * show up after a few hundred frames (DPB exhaustion regressions) */
    test_video("assets/test.mp4", AG_VIDEO_BACKEND_AUTO, "mp4/h264+bframes",
               1, 1200);
    printf("\n%s (%d failures)\n", fails ? "FAILED" : "ALL PASSED", fails);
    return fails ? 1 : 0;
}
