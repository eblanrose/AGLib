# Another GUI Lib
> AGLib

**it's a Lightweight**
> 9K SLOC! + 10 MiB! and just a few libs

**It's Cross-platform**
> Can work on **UNIX And NT!**


***It's SIMPLE**

> all what you need to compile is a C/C++ Compiler and GNU Make!

# Building

```sh
make            # builds `app` (X11) and `app.exe` (Windows)
make app        # builds `app` (X11)
make app.exe    # builds the Windows exe (mingw)
make wayland    # builds `app-wayland`
make test       # builds and runs the feature smoke test
```
# Examples

``` c
#include "aglib.h"

static int on_event(ag_window *w, const ag_event *e, void *ud)
{
    (void)ud;
    if (e->type == AG_EVENT_EXPOSE) {
        ag_clear(w, AG_RGB(30, 30, 40));

        ag_set_color(w, AG_RGB(86, 156, 214));
        ag_fill_round_rect(w, 40, 40, 200, 60, 8);

        ag_set_color(w, AG_WHITE);
        ag_set_font_size(w, 20);
        ag_draw_text(w, 60, 60, "Hello, AGLib!");
    }
    if (e->type == AG_EVENT_CLOSE) return 0; /* allow close */
    return 0;
}

int main(void)
{
    ag_init();
    ag_window *win = ag_window_create("AGLib Demo", 400, 300);
    ag_window_set_callback(win, on_event, NULL);
    ag_window_show(win);

    while (ag_poll_events()) {
        ag_begin_frame(win);
        ag_event e = { .type = AG_EVENT_EXPOSE };
        on_event(win, &e, NULL);
        ag_end_frame(win);
        ag_sleep_ms(16);
    }

    ag_window_destroy(win);
    ag_shutdown();
    return 0;
} // its example
```
or...
``` c
#include "aglib.h"

static ag_ui *g_ui;

static void on_click(ag_widget *w, void *ud)
{
    (void)w; (void)ud;
    printf("Button clicked!\n");
}

static void on_text(ag_widget *w, void *ud)
{
    (void)ud;
    printf("Text: %s\n", ag_textbox_text(w));
}

static void on_resize(ag_ui *ui, int w, int h, void *ud)
{
    (void)ui; (void)h; (void)ud;
    printf("Resized to %d\n", w);
}

static int on_event(ag_window *win, const ag_event *e, void *ud)
{
    (void)ud;
    if (ag_ui_handle(g_ui, e)) return 0;

    if (e->type == AG_EVENT_EXPOSE) {
        ag_clear(win, AG_RGB(36, 36, 40));
        ag_ui_draw(g_ui);
    }
    return 0;
}

int main(void)
{
    ag_init();
    ag_window *win = ag_window_create("Widgets", 500, 300);
    g_ui = ag_ui_create(win);

    ag_ui_add_button(g_ui, "Click Me", 20, 20, 140, 36, on_click, NULL);
    ag_ui_add_toggle(g_ui, "Enable", 1, 20, 70, NULL, NULL);
    ag_ui_add_slider(g_ui, 0, 100, 50, 20, 110, 200, 24, NULL, NULL);
    ag_ui_add_textbox(g_ui, 20, 150, 200, 30, on_text, NULL);
    ag_widget *log = ag_ui_add_textview(g_ui, 240, 20, 240, 200);
    ag_textview_append(log, "Log line 1\nLog line 2\n");

    ag_ui_set_layout_cb(g_ui, on_resize, NULL);
    ag_window_set_callback(win, on_event, NULL);
    ag_window_show(win);

    ag_main_loop();

    ag_ui_destroy(g_ui);
    ag_window_destroy(win);
    ag_shutdown();
    return 0;
}
```

# API

### Core / Init

`int ag_init(void)`

Initialize library (Win32 class / X11 locale)

`void ag_shutdown(void)`

Cleanup

`const char *ag_get_platform(void)`

Returns `"win32"`, `"x11"` or `"wayland"`

`void ag_sleep_ms(unsigned int ms)`

Sleep milliseconds

`unsigned long ag_time_ms(void)`

Monotonic clock in milliseconds — use it for animations and playback

### Window

`ag_window *ag_window_create(title, w, h)`

Create window

`void ag_window_destroy(w)`

Destroy window

`void ag_window_set_title(w, title)`

Set window title

`void ag_window_set_size(w, w, h)`

Resize window

`void ag_window_get_size(w, *w, *h)`

Query size

`void ag_window_set_callback(w, cb, ud)`

Set event callback

`void ag_window_show(w)` / `ag_window_hide(w)`

Show / hide

`void ag_window_set_font(w, font)`

Set window font

`void ag_window_redraw(w)`

Request redraw (triggers EXPOSE)

### Event loop

`int ag_poll_events(void)`

Process pending events Returns 0 when quitting

`void ag_main_loop(void)`

Blocking loop until quit

`void ag_quit(void)`

Stop main loop

### Frame

`void ag_begin_frame(w)`

Begin drawing (no-op currently)

`void ag_end_frame(w)`

Blit backbuffer to screen

### Drawing

`ag_clear(w, color)`

Fill window with color

`ag_set_color(w, c)`

Set current draw color

`ag_set_font_size(w, px)`

Set fallback font size

`ag_draw_pixel(w, x, y)`

Draw pixel

`ag_draw_line(w, x0,y0,x1,y1)`

Bresenham line

`ag_draw_rect(w, x,y,w,h)`

Outline rect

`ag_fill_rect(w, x,y,w,h)`

Filled rect

`ag_fill_round_rect(w, x,y,w,h,r)`

Filled rounded rect

`ag_draw_round_rect(w, x,y,w,h,r)`

Rounded rect outline

`ag_draw_text(w, x,y, utf8)`

Draw UTF-8 text

`ag_draw_image(w, x,y,w,h, img)`

Draw image (with alpha, bilinear scaling)

### Fonts
`ag_font *ag_font_open(name_or_path, px)`

Load font (Win32: family name, X11: TTF path)

`ag_font *ag_font_open_file(path, px)`

Load a font **from a file** (TTF/OTF). The family name is read from the
file itself, so it also works on Windows where fonts normally come from
the system.

`ag_font *ag_font_default(px)`

Load default font — the copy in `assets/fonts/` first, then the usual
system locations. The environment variable `AG_FONT` overrides the path.

`void ag_font_close(f)`

Free font

`int ag_font_ascent(f)` / `descent(f)` / `height(f)`

Metrics

`int ag_font_text_width(f, utf8)`

Measure text width

`const char *ag_font_family(f)`

Name of the loaded family (e.g. `DejaVu Sans`)

`int ag_font_set_size(f, px)`

Rescale the font in place — much cheaper than reloading the file

### Images
`ag_image *ag_image_load(path)`

Load PNG / JPEG / SVG / BMP

`ag_image *ag_image_load_svg(path, scale)`

Load SVG at scale

`ag_image *ag_image_create(w, h)`

Create blank image

`ag_image *ag_image_clone(img)`

Copy an image

`void ag_image_destroy(img)`

Free image

`int ag_image_get_size(img, *w, *h)`

Query size

`ag_u32 ag_image_pixel(img, x, y)`

Read one pixel (0xAARRGGBB)

`int ag_image_save_png(img, path)`

Write the image as PNG

`int ag_image_blur_dual_kawase(img, radius, passes)`

Dual Kawase blur in place

`int ag_image_blur_box(img, radius, passes)`

Separable box blur (two passes with a running sum), so the cost does
not depend on the radius. This is the cpu fallback of the photo cards.

`void ag_draw_image_blur(w, x, y, w, h, img, radius, passes)`

Scale the image into a rect, blur it and draw it there (frosted card)

### UI — Creation
`ag_ui *ag_ui_create(win)`

Create UI bound to window

`void ag_ui_destroy(ui)`

Free UI + widgets

`void ag_ui_set_font(ui, font)`

Set UI font

`void ag_ui_set_theme(ui, theme)`

Apply custom theme

`void ag_ui_set_layout_cb(ui, cb, ud)`

Resize callback

`const ag_ui_theme *ag_ui_default_theme(void)`

Get built-in dark theme

`void ag_ui_draw(ui)`

Render all widgets

`int ag_ui_handle(ui, ev)`

Route event to widgets. Returns 1 if consumed

### UI — Widgets
`ag_ui_add_button(ui, label, x,y,w,h, cb, ud)`

Push button

`ag_ui_add_slider(ui, min,max,val, x,y,w,h, cb, ud)`

Slider

`ag_ui_add_toggle(ui, label, checked, x,y, cb, ud)`

Checkbox

`ag_ui_add_textbox(ui, x,y,w,h, cb, ud)`

Single-line text input

`ag_ui_add_textview(ui, x,y,w,h)`

Scrollable log view

`ag_ui_add_image(ui, img, x,y,w,h, cb, ud)`

Image card. The scaled copy is cached and the blur is calculated in a
background thread, so the card never blocks the interface — it shows
the sharp picture until the blurred one is ready.

`ag_ui_add_video(ui, path, x,y,w,h, cb, ud)`

Video player widget: draws the frame, a play/pause button, a seek bar and
the time. Click the button to play/pause, drag the bar to seek.

`void ag_widget_set_blur(wdg, radius, passes)`

Blur the image card with the dual Kawase filter

`void ag_widget_set_caption(wdg, text)`

Caption drawn over the bottom of the card

`void ag_widget_set_image(wdg, img)`

Replace the picture of an image card

`void ag_widget_prepare(wdg)`

Pre-render expensive content (blurred cards) so the first frame is fast

### Animation
`void ag_ui_set_transition(ui, type, duration_ms, ease)`

Set the transition used when the page changes. Types: `AG_ANIM_FADE`,
`AG_ANIM_SLIDE_LEFT/RIGHT/UP/DOWN`, `AG_ANIM_SCALE`, `AG_ANIM_FLIP`.
Easings: `AG_EASE_LINEAR`, `AG_EASE_IN_QUAD`, `AG_EASE_OUT_QUAD`,
`AG_EASE_IN_OUT_QUAD`, `AG_EASE_OUT_CUBIC`, `AG_EASE_IN_OUT_CUBIC`,
`AG_EASE_OUT_BACK`, `AG_EASE_OUT_ELASTIC`, `AG_EASE_OUT_BOUNCE`.

`void ag_ui_set_page(ui, page)`

Switch to another page — starts the transition

`int ag_ui_page(ui)` / `int ag_ui_animating(ui)`

Current page / is a transition running

`void ag_ui_update(ui)`

Advance the animation and the video widgets. The event loop calls it
automatically, so a normal application does not have to care — call it
yourself only when you drive the loop by hand.

`void ag_widget_set_page(wdg, page)`

Put a widget on a page. Use `AG_PAGE_GLOBAL` (-1) for widgets that stay
visible on every page (a sidebar, for example).

`void ag_widget_set_anim_delay(wdg, ms)`

Delay the widget inside the transition (staggered animations)

`void ag_ui_set_page_draw_cb(ui, cb, ud)`

Called while a page is rendered — the application can draw its own
content (labels, images, ...) into the page, so that it takes part in
the transition instead of popping up.

`float ag_ease_apply(ease, t)`

Apply an easing function to a value in 0..1

### Video
`ag_video *ag_video_open(path)` / `ag_video_open_ex(path, backend)`

Open a video. `AG_VIDEO_BACKEND_AUTO` detects the container by its
signature (AVI, raw MJPEG stream, MP4, MKV) and picks the built in
decoder. Everything happens in process — no external players or tools.

* Containers: MP4/MOV, MKV/WebM, AVI, raw MJPEG
* Video codecs: H.264 (any profile, B frames), MJPEG
* Audio codecs: AAC, MP3 — the sound plays with the video
* All decoders are linked into the binary: nothing to install, the same
  on Linux and Windows
* HEVC/VP9/AV1 video → open fails with the codec name in the error;
  Vorbis/Opus audio → the file plays without sound

`void ag_video_close(v)`

Close

`int ag_video_get_size(v, *w, *h)` / `double ag_video_duration(v)` / `double ag_video_fps(v)`

Properties

`double ag_video_time(v)` / `int ag_video_frame_count(v)`

Playback position / number of frames

`int ag_video_seek(v, seconds)`

Jump to a position

`int ag_video_next_frame(v, &image)`

Decode the next frame. The image belongs to the video and stays valid
until the next call.

`const char *ag_video_error(v)` / `ag_video_last_error()`

Error messages

`const char *ag_video_backend_name(v)`

Which backend is in use

`void ag_video_widget_play(wdg, play)` / `ag_video_widget_seek(wdg, sec)`

Control the player widget

`int ag_video_widget_playing(wdg)` / `double ag_video_widget_time(wdg)`

Player state

### UI — Widget ops
`ag_widget_remove(ui, wdg)`

Mark widget for removal

`ag_widget_move(wdg, x, y)`

Move widget

`ag_widget_set_rect(wdg, x,y,w,h)`

Set position & size

`ag_widget_get_rect(wdg, *x,*y,*w,*h)`

Query rect

`ag_widget_set_callback(wdg, cb, ud)`

Change callback

### UI — Widget accessors
`const char *ag_button_label(wdg)`

Button label

`const char *ag_textbox_text(wdg)`

Textbox content

`void ag_textbox_set_text(wdg, text)`

Set textbox text

`int ag_textbox_submitted(wdg)`

Check & clear Enter flag

`float ag_slider_value(wdg)`

Slider value

`int ag_toggle_checked(wdg)`

Toggle state

`void ag_textview_append(wdg, text)`

Append line(s) to log

`const char *ag_textview_text(wdg)`

Get all log text

`void ag_textview_clear(wdg)`

Clear log

`void ag_textview_set_scroll(wdg, line)`

Set scroll (-1 = pin to bottom)

# Licenses

Every dependency is a git submodule under `third_party/` and links
statically — no system packages, no runtime loading. Each keeps its own
license text next to the sources:

* OpenH264 — BSD-2-Clause, `third_party/openh264/LICENSE`
* FDK AAC — Fraunhofer FDK AAC license, `third_party/fdk-aac/NOTICE`;
  that text has to travel with binary redistributions, as it requires
* minimp3 — CC0; zlib / libpng / libjpeg / nanosvg — permissive, see
  `third_party/<name>/LICENSE*`
