#include "aglib.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define SIDE_X   20
#define SIDE_W   160
#define FIELD_X  200
#define FIELD_M  20

#define PAGE_LOG    0
#define PAGE_PHOTOS 1
#define PAGE_VIDEO  2
#define PAGE_SETUP  3

#define PHOTO_CARDS 3

static ag_window *g_win;
static ag_ui    *g_ui;
static ag_font  *g_font;
static int g_win_w = 900, g_win_h = 600;
static int g_font_px = 16;
static int g_theme_green = 1;
static int g_borders = 1;
static int g_main_c[3] = { 22, 26, 36 };
static int g_blur = 10;
static int g_page = PAGE_LOG;

static ag_widget *g_log, *g_info, *g_console;
static ag_widget *g_btn_tab[4];
static ag_widget *g_btn_theme, *g_btn_clear, *g_btn_exit;
static ag_widget *g_sl_font, *g_tgl_borders, *g_sl_c[3];
static ag_widget *g_sl_blur, *g_btn_anim, *g_slb_anim;
static ag_widget *g_sl_eq[3], *g_tgl_vblur;
static ag_widget *g_vis;
static ag_widget *g_card[PHOTO_CARDS];
static ag_widget *g_video;
static ag_widget *g_video_mp4;

static ag_image *g_img[PHOTO_CARDS];
static const char *g_img_name[PHOTO_CARDS] = { "assets/test.jpg", "assets/test.png", "assets/test.bmp" };
static int g_card_sharp[PHOTO_CARDS];

static int    g_main_x, g_main_y, g_main_w, g_main_h;
static int    g_con_y;

static const ag_anim_type g_anim_types[] = {
    AG_ANIM_FADE, AG_ANIM_SLIDE_LEFT, AG_ANIM_SLIDE_RIGHT,
    AG_ANIM_SLIDE_UP, AG_ANIM_SLIDE_DOWN, AG_ANIM_SCALE, AG_ANIM_FLIP
};
static const char *g_anim_names[] = {
    "затухание", "сдвиг влево", "сдвиг вправо",
    "сдвиг снизу", "сдвиг сверху", "масштаб", "флип"
};
static int g_anim_idx = 1;
static int g_anim_ms = 320;

static void log_line(const char *tag, const char *msg);

static void apply_theme(void)
{
    ag_ui_theme th = *ag_ui_default_theme();
    th.radius = 6;
    th.borders = g_borders;
    if (g_theme_green) {
        th.accent          = AG_RGB(88, 188, 120);
        th.accent_d        = AG_RGB(56, 128, 82);
        th.scrollbar       = AG_RGB(46, 74, 54);
        th.scrollbar_hover = AG_RGB(72, 110, 82);
    }
    int r = g_main_c[0], g = g_main_c[1], b = g_main_c[2];
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    th.bg      = AG_RGB(r + 24 > 255 ? 255 : r + 24,
                        g + 24 > 255 ? 255 : g + 24,
                        b + 24 > 255 ? 255 : b + 24);
    th.bg2     = AG_RGB(r + 14 > 255 ? 255 : r + 14,
                        g + 14 > 255 ? 255 : g + 14,
                        b + 14 > 255 ? 255 : b + 14);
    th.hover   = AG_RGB(r + 40 > 255 ? 255 : r + 40,
                        g + 40 > 255 ? 255 : g + 40,
                        b + 40 > 255 ? 255 : b + 40);
    th.pressed = AG_RGB(r - 6 < 0 ? 0 : r - 6,
                        g - 6 < 0 ? 0 : g - 6,
                        b - 6 < 0 ? 0 : b - 6);
    th.border  = AG_RGB(r + 55 > 255 ? 255 : r + 55,
                        g + 55 > 255 ? 255 : g + 55,
                        b + 55 > 255 ? 255 : b + 55);
    th.text    = (r * 299 + g * 587 + b * 114) > 140000
                 ? AG_RGB(20, 24, 32) : AG_RGB(222, 222, 222);
    ag_ui_set_theme(g_ui, &th);
}

static void apply_exit_style(void)
{
    ag_ui_theme t = *ag_ui_get_theme(g_ui);
    t.accent   = AG_RGB(224, 82, 82);
    t.accent_d = AG_RGB(170, 52, 52);
    t.border   = AG_RGB(150, 74, 84);
    ag_widget_set_theme(g_btn_exit, &t);
}

static int apply_font_px(int px)
{
    if (px < 8)  px = 8;
    if (px > 48) px = 48;
    if (!g_font) {
        g_font = ag_font_open_file("assets/fonts/DejaVuSans.ttf", (unsigned)px);
        if (!g_font) g_font = ag_font_default((unsigned)px);
        if (!g_font) return 0;
        ag_window_set_font(g_win, g_font);
        ag_ui_set_font(g_ui, g_font);
    } else if (!ag_font_set_size(g_font, (unsigned)px)) {
        return 0;
    }
    g_font_px = px;
    return 1;
}

static void apply_blur(void)
{
    for (int i = 0; i < PHOTO_CARDS; ++i) {
        if (!g_card[i]) continue;
        ag_widget_set_blur(g_card[i], g_card_sharp[i] ? 0 : g_blur, 1);
    }
}

static void on_layout(ag_ui *ui, int w, int h, void *ud)
{
    (void)ui; (void)ud;
    g_win_w = w;
    g_win_h = h;

    int mw = w - FIELD_X - FIELD_M;
    if (mw < 120) mw = 120;

    ag_widget_set_rect(g_btn_tab[0], SIDE_X, 60,  SIDE_W, 30);
    ag_widget_set_rect(g_btn_tab[1], SIDE_X, 94,  SIDE_W, 30);
    ag_widget_set_rect(g_btn_tab[2], SIDE_X, 128, SIDE_W, 30);
    ag_widget_set_rect(g_btn_tab[3], SIDE_X, 162, SIDE_W, 30);
    ag_widget_set_rect(g_btn_theme,  SIDE_X, 200, SIDE_W, 30);
    ag_widget_set_rect(g_btn_clear,  SIDE_X, 240, SIDE_W, 30);
    ag_widget_set_rect(g_btn_exit,   SIDE_X, h - 50, SIDE_W, 30);

    g_main_x = FIELD_X;
    g_main_y = 60;
    g_con_y  = h - 8 - 26;
    g_main_h = g_con_y - g_main_y - 6;
    if (g_main_h < 30) g_main_h = 30;
    g_main_w = mw;

    ag_widget_set_rect(g_log,     g_main_x, g_main_y, g_main_w, g_main_h);
    ag_widget_set_rect(g_console, g_main_x, g_con_y, g_main_w, 26);

    int gap = 12;
    int cw = (g_main_w - gap * (PHOTO_CARDS - 1)) / PHOTO_CARDS;
    if (cw < 40) cw = 40;
    for (int i = 0; i < PHOTO_CARDS; ++i) {
        ag_widget_set_rect(g_card[i], g_main_x + i * (cw + gap), g_main_y, cw, g_main_h);
    }

    int vh = (g_main_h - 12) / 2;
    if (vh < 60) vh = 60;
    ag_widget_set_rect(g_video, g_main_x, g_main_y, g_main_w, vh);
    if (g_video_mp4)
        ag_widget_set_rect(g_video_mp4, g_main_x, g_main_y + vh + 12, g_main_w, vh);

    int sw = g_main_w - 170;
    if (sw < 80) sw = 80;
    ag_widget_set_rect(g_tgl_borders, g_main_x, g_main_y,      g_main_w, 24);
    ag_widget_set_rect(g_sl_font,     g_main_x, g_main_y + 40, sw, 24);
    ag_widget_set_rect(g_sl_blur,     g_main_x, g_main_y + 80, sw, 24);
    ag_widget_set_rect(g_btn_anim,    g_main_x, g_main_y + 120, g_main_w, 30);
    ag_widget_set_rect(g_sl_c[0],     g_main_x, g_main_y + 170, sw, 24);
    ag_widget_set_rect(g_sl_c[1],     g_main_x, g_main_y + 210, sw, 24);
    ag_widget_set_rect(g_sl_c[2],     g_main_x, g_main_y + 250, sw, 24);
    ag_widget_set_rect(g_slb_anim,    g_main_x, g_main_y + 300, g_main_w, 32);
    ag_widget_set_rect(g_tgl_vblur,   g_main_x, g_main_y + 340, g_main_w, 24);
    ag_widget_set_rect(g_vis,         g_main_x, g_main_y + 380, g_main_w, 120);
}

static void log_line(const char *tag, const char *msg)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "[%s] %s\n", tag, msg);
    if (g_log) ag_textview_append(g_log, buf);
    printf("%s", buf);
    fflush(stdout);
}

static void switch_page(int page)
{
    if (page == g_page) return;
    g_page = page;
    ag_ui_set_page(g_ui, page);
    const char *names[] = { "лог", "фото", "видео", "настройки" };
    char m[128];
    snprintf(m, sizeof(m), "страница: %s (анимация: %s)",
             names[page], g_anim_names[g_anim_idx]);
    log_line("ui", m);
}

static void on_tab(ag_widget *wdg, void *ud)
{
    (void)wdg;
    switch_page((int)(intptr_t)ud);
}

static void on_theme(ag_widget *wdg, void *ud)
{
    (void)wdg; (void)ud;
    g_theme_green = !g_theme_green;
    apply_theme();
    log_line("ui", g_theme_green ? "тема: зелёная" : "тема: стандартная");
}

static void on_borders(ag_widget *wdg, void *ud)
{
    (void)ud;
    g_borders = ag_toggle_checked(wdg);
    apply_theme();
    log_line("ui", g_borders ? "рамки: вкл" : "рамки: выкл");
}

static void on_col(ag_widget *wdg, void *ud)
{
    int idx = (int)(intptr_t)ud;
    g_main_c[idx] = (int)ag_slider_value(wdg);
    apply_theme();
}

static void on_clear(ag_widget *wdg, void *ud)
{
    (void)wdg; (void)ud;
    ag_textview_clear(g_log);
}

static void on_exit(ag_widget *wdg, void *ud)
{
    (void)wdg; (void)ud;
    ag_quit();
}

static void on_font(ag_widget *wdg, void *ud)
{
    (void)ud;
    int px = (int)ag_slider_value(wdg);
    if (px != g_font_px && apply_font_px(px))
        log_line("font", ag_font_family(g_font));
}

static void on_blur(ag_widget *wdg, void *ud)
{
    (void)ud;
    g_blur = (int)ag_slider_value(wdg);
    apply_blur();
    char m[64];
    snprintf(m, sizeof(m), "радиус блюра карточек: %d px", g_blur);
    log_line("blur", m);
}

static void on_anim(ag_widget *wdg, void *ud)
{
    (void)wdg; (void)ud;
    g_anim_idx = (g_anim_idx + 1) % (int)(sizeof(g_anim_types) / sizeof(g_anim_types[0]));
    ag_ui_set_transition(g_ui, g_anim_types[g_anim_idx], g_anim_ms, AG_EASE_IN_OUT_CUBIC);
    char m[96];
    snprintf(m, sizeof(m), "анимация: %s", g_anim_names[g_anim_idx]);
    log_line("ui", m);
}

static void on_anim_ms(ag_widget *wdg, void *ud)
{
    (void)ud;
    g_anim_ms = (int)ag_slider_value(wdg);
    ag_ui_set_transition(g_ui, g_anim_types[g_anim_idx], g_anim_ms, AG_EASE_IN_OUT_CUBIC);
}

static void on_vblur(ag_widget *wdg, void *ud)
{
    (void)ud;
    if (!g_video) return;
    if (ag_toggle_checked(wdg))
        ag_widget_set_blur(g_video, 6, 1);
    else
        ag_widget_set_blur(g_video, 0, 1);
}

static void on_card(ag_widget *wdg, void *ud)
{
    (void)wdg;
    int idx = (int)(intptr_t)ud;
    g_card_sharp[idx] = !g_card_sharp[idx];
    apply_blur();
    char m[96];
    snprintf(m, sizeof(m), "карточка %s: %s", g_img_name[idx],
             g_card_sharp[idx] ? "резкая" : "размытая (dual kawase)");
    log_line("blur", m);
}

static void on_console(ag_widget *wdg, void *ud)
{
    (void)ud;
    if (!ag_textbox_submitted(wdg)) return;
    const char *cmd = ag_textbox_text(wdg);
    if (!cmd || !*cmd) return;
    if (strcmp(cmd, "exit")  == 0) { ag_quit(); return; }
    if (strcmp(cmd, "clear") == 0) { ag_textview_clear(g_log); return; }
    if (strcmp(cmd, "about") == 0) {
        ag_textview_append(g_log,
            "AGLib — кроссплатформенная GUI-библиотека на чистом C.\n"
            "Шрифт грузится из файла, фото-карточки размываются dual kawase,\n"
            "переключение страниц анимируется, видео (mp4/mkv, AVI) играется\n"
            "со звуком — всё встроено, ничего устанавливать не нужно.\n");
        return;
    }
    log_line("cmd", cmd);
    ag_textbox_set_text(wdg, "");
}

static void draw_page(ag_ui *ui, int page, void *ud)
{
    (void)ui; (void)ud;
    ag_window *w = g_win;

    int luma = (g_main_c[0] * 299 + g_main_c[1] * 587 + g_main_c[2] * 114) / 1000;
    ag_color hc = luma > 140 ? AG_RGB(20, 24, 32) : AG_RGB(222, 222, 222);
    ag_set_color(w, hc);
    ag_draw_text(w, 20, 14, "AGLib");
    char st[160];
    snprintf(st, sizeof(st), "%dx%d  %s  %s %dpx",
             g_win_w, g_win_h, ag_get_platform(),
             g_font ? ag_font_family(g_font) : "bitmap", g_font_px);
    ag_draw_text(w, g_win_w - 20 - ag_font_text_width(g_font, st), 14, st);

    if (page == PAGE_SETUP) {
        char lbl[64];
        int lx = g_main_x + g_main_w - 150;
        snprintf(lbl, sizeof(lbl), "Шрифт %dpx", g_font_px);
        ag_draw_text(w, lx, g_main_y + 44, lbl);
        snprintf(lbl, sizeof(lbl), "Блюр %dpx", g_blur);
        ag_draw_text(w, lx, g_main_y + 84, lbl);
        ag_draw_text(w, lx, g_main_y + 174, "цвет R");
        ag_draw_text(w, lx, g_main_y + 214, "G");
        ag_draw_text(w, lx, g_main_y + 254, "B");
        ag_draw_text(w, lx, g_main_y + 384, "визуализатор");
    }
}

static int on_event(ag_window *w, const ag_event *e, void *ud)
{
    (void)ud;

    if (g_ui && ag_ui_handle(g_ui, e)) {
        ag_window_redraw(w);
        return 1;
    }

    if (e->type == AG_EVENT_EXPOSE) {
        ag_begin_frame(w);
        ag_clear(w, AG_RGB(g_main_c[0], g_main_c[1], g_main_c[2]));
        ag_ui_draw(g_ui);
        ag_end_frame(w);
        return 1;
    }

    if (e->type == AG_EVENT_KEY_DOWN) {
        if (e->key == 27 || e->key == 0xFF1B) { ag_quit(); return 1; }
        if (e->key == '1' || e->key == 0xFF1B) { switch_page(PAGE_LOG); return 1; }
        if (e->key == '2') { switch_page(PAGE_PHOTOS); return 1; }
        if (e->key == '3') { switch_page(PAGE_VIDEO); return 1; }
        if (e->key == '4') { switch_page(PAGE_SETUP); return 1; }
        if (e->key == ' ') {
            if (g_video) ag_video_widget_play(g_video, !ag_video_widget_playing(g_video));
            return 1;
        }
        return 1;
    }
    return 0;
}

int main(void)
{
    if (ag_init() != 0) { fprintf(stderr, "init failed\n"); return 1; }

    g_win = ag_window_create("AGLib App", g_win_w, g_win_h);
    ag_window_set_callback(g_win, on_event, NULL);
    ag_window_show(g_win);

    apply_font_px(16);

    g_ui = ag_ui_create(g_win);
    apply_theme();
    ag_ui_set_transition(g_ui, AG_ANIM_SLIDE_LEFT, g_anim_ms, AG_EASE_IN_OUT_CUBIC);
    ag_ui_set_page_draw_cb(g_ui, draw_page, NULL);

    g_btn_tab[0] = ag_ui_add_button(g_ui, "Лог", 0, 0, 0, 0, on_tab, (void *)(intptr_t)PAGE_LOG);
    g_btn_tab[1] = ag_ui_add_button(g_ui, "Фото", 0, 0, 0, 0, on_tab, (void *)(intptr_t)PAGE_PHOTOS);
    g_btn_tab[2] = ag_ui_add_button(g_ui, "Видео", 0, 0, 0, 0, on_tab, (void *)(intptr_t)PAGE_VIDEO);
    g_btn_tab[3] = ag_ui_add_button(g_ui, "Настройки", 0, 0, 0, 0, on_tab, (void *)(intptr_t)PAGE_SETUP);
    g_btn_theme  = ag_ui_add_button(g_ui, "Тема", 0, 0, 0, 0, on_theme, NULL);
    g_btn_clear  = ag_ui_add_button(g_ui, "Очистить лог", 0, 0, 0, 0, on_clear, NULL);
    g_btn_exit   = ag_ui_add_button(g_ui, "Exit", 0, 0, 0, 0, on_exit, NULL);

    g_log = ag_ui_add_textview(g_ui, 0, 0, 0, 0);
    g_console = ag_ui_add_textbox(g_ui, 0, 0, 0, 0, on_console, NULL);

    g_info = ag_ui_add_textview(g_ui, 0, 0, 0, 0);
    ag_textview_append(g_info,
        "AGLib: каркас приложения.\n"
        "\n"
        "Вкладки слева переключают страницы с анимацией.\n"
        "«Лог» — лента событий; длинные строки переносятся по ширине окна.\n"
        "Строка под логом — команды: ввод + Enter (clear, about, exit).\n"
        "«Фото» — карточки JPEG/PNG/BMP с размытием dual kawase;\n"
        "клик по карточке делает её резкой.\n"
        "«Видео» — mp4/mkv со звуком (H.264 + AAC/MP3), AVI/MJPEG.\n"
        "«Настройки» — анимация, шрифт из файла, радиус блюра, тема.\n"
        "Клавиши 1..4 — страницы, пробел — пауза видео, Esc — выход.\n");

    for (int i = 0; i < PHOTO_CARDS; ++i) {
        g_img[i] = ag_image_load(g_img_name[i]);
        if (!g_img[i]) {
            char m[128];
            snprintf(m, sizeof(m), "не загрузилась: %s", g_img_name[i]);
            log_line("img", m);
        }
        g_card[i] = ag_ui_add_image(g_ui, g_img[i], 0, 0, 0, 0, on_card,
                                    (void *)(intptr_t)i);
        char cap[64];
        snprintf(cap, sizeof(cap), "%s", g_img_name[i]);
        ag_widget_set_caption(g_card[i], cap);
        ag_widget_set_blur(g_card[i], g_blur, 1);
        ag_widget_prepare(g_card[i]);
    }

    g_video = ag_ui_add_video(g_ui, "assets/test.mp4", 0, 0, 0, 0, NULL, NULL);

    g_sl_font = ag_ui_add_slider(g_ui, 10.0f, 26.0f, 16.0f,
                                 0, 0, 0, 0, on_font, NULL);
    g_tgl_borders = ag_ui_add_toggle(g_ui, "Рамки",
                                     g_borders, 0, 0, on_borders, NULL);
    g_sl_blur = ag_ui_add_slider(g_ui, 0.0f, 24.0f, (float)g_blur,
                                 0, 0, 0, 0, on_blur, NULL);
    g_btn_anim = ag_ui_add_button(g_ui, "Анимация", 0, 0, 0, 0, on_anim, NULL);
    g_sl_c[0] = ag_ui_add_slider(g_ui, 0.0f, 255.0f, (float)g_main_c[0],
                                 0, 0, 0, 0, on_col, (void *)(intptr_t)0);
    g_sl_c[1] = ag_ui_add_slider(g_ui, 0.0f, 255.0f, (float)g_main_c[1],
                                 0, 0, 0, 0, on_col, (void *)(intptr_t)1);
    g_sl_c[2] = ag_ui_add_slider(g_ui, 0.0f, 255.0f, (float)g_main_c[2],
                                 0, 0, 0, 0, on_col, (void *)(intptr_t)2);
    g_slb_anim = ag_ui_add_button_slider(g_ui, "Длительность анимации",
                                         100.0f, 800.0f, (float)g_anim_ms,
                                         0, 0, 0, 0, on_anim_ms, NULL);
    g_tgl_vblur = ag_ui_add_toggle(g_ui, "Блюр видео", 0, 0, 0, on_vblur, NULL);
    g_vis = ag_ui_add_visualizer(g_ui, 0, 0, 0, 0);
    if (g_video) ag_visualizer_set_video(g_vis, ag_video_widget_video(g_video));

    ag_widget_set_page(g_btn_tab[0], -1);
    ag_widget_set_page(g_btn_tab[1], -1);
    ag_widget_set_page(g_btn_tab[2], -1);
    ag_widget_set_page(g_btn_tab[3], -1);
    ag_widget_set_page(g_btn_theme, -1);
    ag_widget_set_page(g_btn_clear, -1);
    ag_widget_set_page(g_btn_exit, -1);

    ag_widget_set_page(g_log, PAGE_LOG);
    ag_widget_set_page(g_console, PAGE_LOG);
    ag_widget_set_page(g_info, PAGE_LOG);

    for (int i = 0; i < PHOTO_CARDS; ++i)
        ag_widget_set_page(g_card[i], PAGE_PHOTOS);
    ag_widget_set_page(g_video, PAGE_VIDEO);

    ag_widget_set_page(g_tgl_borders, PAGE_SETUP);
    ag_widget_set_page(g_sl_font, PAGE_SETUP);
    ag_widget_set_page(g_sl_blur, PAGE_SETUP);
    ag_widget_set_page(g_btn_anim, PAGE_SETUP);
    ag_widget_set_page(g_sl_c[0], PAGE_SETUP);
    ag_widget_set_page(g_sl_c[1], PAGE_SETUP);
    ag_widget_set_page(g_sl_c[2], PAGE_SETUP);
    ag_widget_set_page(g_slb_anim, PAGE_SETUP);
    ag_widget_set_page(g_tgl_vblur, PAGE_SETUP);
    ag_widget_set_page(g_vis, PAGE_SETUP);

    apply_exit_style();

    ag_ui_set_layout_cb(g_ui, on_layout, NULL);
    on_layout(g_ui, g_win_w, g_win_h, NULL);

    log_line("app", "запуск");
    {
        const char *gpu = ag_gpu_renderer();
        char m[128];
        if (gpu && *gpu)
            snprintf(m, sizeof(m), "gpu: %s", gpu);
        else
            snprintf(m, sizeof(m), "gpu: нет (только cpu)");
        log_line("app", m);
    }
    log_line("font", g_font ? ag_font_family(g_font) : "встроенный 8x8");
    log_line("app", "переключение страниц анимировано (см. «Настройки»)");

    for (;;) {
        ag_ui_update(g_ui);
        if (!ag_poll_events()) break;
        ag_sleep_ms(8);
    }

    for (int i = 0; i < PHOTO_CARDS; ++i) ag_image_destroy(g_img[i]);
    ag_ui_destroy(g_ui);
    ag_window_destroy(g_win);
    ag_font_close(g_font);
    ag_shutdown();
    return 0;
}
