#include "aglib.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define SIDE_X  20
#define SIDE_W  160
#define FIELD_X 200
#define FIELD_M 20

#define IMG_LIMIT 4

static ag_window *g_win;
static ag_ui    *g_ui;
static ag_font  *g_font;
static int g_win_w = 820, g_win_h = 560;
static int g_font_px = 16;
static int g_theme_green = 1;
static int g_tab = 0;
static int g_borders = 1;
static int g_main_c[3] = { 22, 26, 36 };

static ag_widget *g_log, *g_info;
static ag_widget *g_console;
static ag_widget *g_btn_tab[4];
static ag_widget *g_btn_theme, *g_btn_clear, *g_btn_exit;
static ag_widget *g_sl_font;
static ag_widget *g_tgl_borders;
static ag_widget *g_sl_c[3];

static int g_main_x, g_main_y, g_main_w, g_main_h;  
static int g_con_y;                                  

static ag_image *g_img[IMG_LIMIT];
static const char *g_img_name[IMG_LIMIT];
static int g_img_n = 0;
static int g_img_cur = 0;

static void log_line(const char *tag, const char *msg);
static void show_tab(void);

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

static void apply_font_px(int px)
{
    if (px < 8)  px = 8;
    if (px > 48) px = 48;
    ag_font *nf = ag_font_default((unsigned)px);
    if (!nf) return;
    ag_font_close(g_font);                  
    g_font = nf;
    g_font_px = px;
    ag_window_set_font(g_win, g_font);
    ag_ui_set_font(g_ui, g_font);
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

    show_tab();
}

static void show_tab(void)
{
    const int off = -8000;
    if (g_tab == 0) {
        ag_widget_set_rect(g_log,     g_main_x, g_main_y, g_main_w, g_main_h);
        ag_widget_set_rect(g_info,    off, 0, 1, 1);
        ag_widget_set_rect(g_console, g_main_x, g_con_y, g_main_w, 26);
        ag_widget_set_rect(g_tgl_borders, off, 0, 1, 1);
        ag_widget_set_rect(g_sl_font, off, 0, 1, 1);
        ag_widget_set_rect(g_sl_c[0], off, 0, 1, 1);
        ag_widget_set_rect(g_sl_c[1], off, 0, 1, 1);
        ag_widget_set_rect(g_sl_c[2], off, 0, 1, 1);
    } else if (g_tab == 1) {
        ag_widget_set_rect(g_log,     off, 0, 1, 1);
        ag_widget_set_rect(g_info,    off, 0, 1, 1);
        ag_widget_set_rect(g_console, off, 0, 1, 1);
        ag_widget_set_rect(g_tgl_borders, off, 0, 1, 1);
        ag_widget_set_rect(g_sl_font, off, 0, 1, 1);
        ag_widget_set_rect(g_sl_c[0], off, 0, 1, 1);
        ag_widget_set_rect(g_sl_c[1], off, 0, 1, 1);
        ag_widget_set_rect(g_sl_c[2], off, 0, 1, 1);
    } else if (g_tab == 2) {
        ag_widget_set_rect(g_log,     off, 0, 1, 1);
        ag_widget_set_rect(g_info,    g_main_x, g_main_y, g_main_w, g_main_h);
        ag_widget_set_rect(g_console, off, 0, 1, 1);
        ag_widget_set_rect(g_tgl_borders, off, 0, 1, 1);
        ag_widget_set_rect(g_sl_font, off, 0, 1, 1);
        ag_widget_set_rect(g_sl_c[0], off, 0, 1, 1);
        ag_widget_set_rect(g_sl_c[1], off, 0, 1, 1);
        ag_widget_set_rect(g_sl_c[2], off, 0, 1, 1);
    } else {
        ag_widget_set_rect(g_log,     off, 0, 1, 1);
        ag_widget_set_rect(g_info,    off, 0, 1, 1);
        ag_widget_set_rect(g_console, off, 0, 1, 1);
        ag_widget_set_rect(g_tgl_borders, g_main_x, g_main_y,      g_main_w, 24);
        ag_widget_set_rect(g_sl_font,     g_main_x, g_main_y + 64, g_main_w, 24);
        ag_widget_set_rect(g_sl_c[0],     g_main_x, g_main_y + 108, g_main_w, 24);
        ag_widget_set_rect(g_sl_c[1],     g_main_x, g_main_y + 152, g_main_w, 24);
        ag_widget_set_rect(g_sl_c[2],     g_main_x, g_main_y + 196, g_main_w, 24);
    }
}

static void draw_photo(ag_window *w)
{
    if (g_img_cur >= g_img_n) return;
    ag_image *im = g_img[g_img_cur];
    int iw = 0, ih = 0;
    if (ag_image_get_size(im, &iw, &ih) || iw <= 0 || ih <= 0) return;

    int mw = g_main_w, mh = g_main_h;
    if (mw <= 0 || mh <= 0) return;

    float k = (float)mw / iw;
    float kh = (float)mh / ih;
    if (kh < k) k = kh;
    if (k > 1.0f) k = 1.0f;                 
    int dw = (int)(iw * k);
    int dh = (int)(ih * k);
    int dx = g_main_x + (mw - dw) / 2;
    int dy = g_main_y + (mh - dh) / 2;
    ag_draw_image(w, dx, dy, dw, dh, im);

    char cap[96];
    snprintf(cap, sizeof(cap), "%s  %dx%d  — клик по области — следующий",
             g_img_name[g_img_cur], iw, ih);
    int luma = (g_main_c[0] * 299 + g_main_c[1] * 587 + g_main_c[2] * 114) / 1000;
    ag_set_color(w, luma > 140 ? AG_RGB(20, 24, 32) : AG_RGB(222, 222, 222));
    ag_draw_text(w, g_main_x + 4, g_main_y + 4, cap);
}

static void log_line(const char *tag, const char *msg)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "[%s] %s\n", tag, msg);
    if (g_log) ag_textview_append(g_log, buf);
    printf("%s", buf);
    fflush(stdout);
}

static void on_tab(ag_widget *wdg, void *ud)
{
    (void)wdg;
    g_tab = (int)(intptr_t)ud;
    show_tab();
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
    if (px != g_font_px) apply_font_px(px);
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
            "AGLib — кроссплатформенная GUI-библиотека на чистом C: "
            "виджеты, TrueType, PNG/JPEG/SVG, темы. Ввод UTF-8.\n");
        return;
    }
    log_line("cmd", cmd);
    ag_textbox_set_text(wdg, "");
}

static int on_event(ag_window *w, const ag_event *e, void *ud)
{
    (void)ud;

    if (g_tab == 1 && e->type == AG_EVENT_MOUSE_DOWN &&
        e->x >= g_main_x && e->x < g_main_x + g_main_w &&
        e->y >= g_main_y && e->y < g_main_y + g_main_h) {
        if (g_img_n > 1) g_img_cur = (g_img_cur + 1) % g_img_n;
        ag_window_redraw(w);
        return 1;
    }

    if (g_ui && ag_ui_handle(g_ui, e)) {
        ag_window_redraw(w);
        return 1;
    }

    if (e->type == AG_EVENT_EXPOSE) {
        ag_begin_frame(w);
        ag_clear(w, AG_RGB(g_main_c[0], g_main_c[1], g_main_c[2]));

        int luma = (g_main_c[0] * 299 + g_main_c[1] * 587 + g_main_c[2] * 114) / 1000;
        ag_color hc = luma > 140 ? AG_RGB(20, 24, 32) : AG_RGB(222, 222, 222);
        ag_set_color(w, hc);
        ag_draw_text(w, 20, 14, "AGLib");
        char st[96];
        snprintf(st, sizeof(st), "%dx%d  %s  %dpx",
                 g_win_w, g_win_h, ag_get_platform(), g_font_px);
        ag_draw_text(w, g_win_w - 20 - ag_font_text_width(g_font, st), 14, st);

        if (g_tab == 3) {
            ag_set_color(w, hc);
            char lbl[64];
            snprintf(lbl, sizeof(lbl), "Шрифт %dpx", g_font_px);
            ag_draw_text(w, g_main_x, g_main_y + 44, lbl);
            ag_draw_text(w, g_main_x, g_main_y + 88, "Главный цвет — R");
            ag_draw_text(w, g_main_x, g_main_y + 132, "G");
            ag_draw_text(w, g_main_x, g_main_y + 176, "B");
        }

        ag_ui_draw(g_ui);

        if (g_tab == 1) draw_photo(w);

        ag_end_frame(w);
        return 1;
    }

    if (e->type == AG_EVENT_KEY_DOWN) {
        if (e->key == 27 || e->key == 0xFF1B) {
            printf("esc\n");
            ag_quit();
            return 1;
        }
        return 1;
    }
    return 0;
}

int main(void)
{
    if (ag_init() != 0) { fprintf(stderr, "init failed\n"); return 1; }

    g_win = ag_window_create("AGLib App", 820, 560);
    ag_window_set_callback(g_win, on_event, NULL);
    ag_window_show(g_win);

    apply_font_px(16);                      

    g_ui = ag_ui_create(g_win);
    apply_theme();

    g_btn_tab[0] = ag_ui_add_button(g_ui, "Лог", 0, 0, 0, 0, on_tab, (void *)(intptr_t)0);
    g_btn_tab[1] = ag_ui_add_button(g_ui, "Фото", 0, 0, 0, 0, on_tab, (void *)(intptr_t)1);
    g_btn_tab[2] = ag_ui_add_button(g_ui, "Инфо", 0, 0, 0, 0, on_tab, (void *)(intptr_t)2);
    g_btn_tab[3] = ag_ui_add_button(g_ui, "Настройки", 0, 0, 0, 0, on_tab, (void *)(intptr_t)3);
    g_btn_theme  = ag_ui_add_button(g_ui, "Тема", 0, 0, 0, 0, on_theme, NULL);
    g_btn_clear  = ag_ui_add_button(g_ui, "Очистить лог", 0, 0, 0, 0, on_clear, NULL);
    g_btn_exit   = ag_ui_add_button(g_ui, "Exit", 0, 0, 0, 0, on_exit, NULL);

    g_log = ag_ui_add_textview(g_ui, 0, 0, 0, 0);
    g_console = ag_ui_add_textbox(g_ui, 0, 0, 0, 0, on_console, NULL);

    g_info = ag_ui_add_textview(g_ui, 0, 0, 0, 0);
    ag_textview_append(g_info,
        "AGLib: каркас приложения.\n"
        "\n"
        "Вкладки слева переключают основной регион.\n"
        "«Лог» — лента событий; длинные строки переносятся по ширине окна.\n"
        "Строка под логом — команды: ввод + Enter (clear, about, exit).\n"
        "«Фото» — картинки JPEG/PNG, вписываются в область; клик — смена кадра.\n"
        "«Настройки» — тумблер рамок, слайдер шрифта и главный цвет R/G/B.\n"
        "Ввод — UTF-8, русская раскладка работает (X11 и Windows).\n"
        "Esc — выход, если фокус не в поле ввода.\n");

    g_sl_font = ag_ui_add_slider(g_ui, 10.0f, 26.0f, 16.0f,
                                 0, 0, 0, 0, on_font, NULL);
    g_tgl_borders = ag_ui_add_toggle(g_ui, "Рамки",
                                     g_borders, 0, 0, on_borders, NULL);
    g_sl_c[0] = ag_ui_add_slider(g_ui, 0.0f, 255.0f, (float)g_main_c[0],
                                 0, 0, 0, 0, on_col, (void *)(intptr_t)0);
    g_sl_c[1] = ag_ui_add_slider(g_ui, 0.0f, 255.0f, (float)g_main_c[1],
                                 0, 0, 0, 0, on_col, (void *)(intptr_t)1);
    g_sl_c[2] = ag_ui_add_slider(g_ui, 0.0f, 255.0f, (float)g_main_c[2],
                                 0, 0, 0, 0, on_col, (void *)(intptr_t)2);

    ag_ui_set_layout_cb(g_ui, on_layout, NULL);
    on_layout(g_ui, 820, 560, NULL);        

    const char *photos[] = { "test.jpg", "test.png", "test.bmp", NULL };
    for (int i = 0; photos[i]; ++i) {
        if (g_img_n >= IMG_LIMIT) break;
        ag_image *im = ag_image_load(photos[i]);
        if (im) {
            g_img[g_img_n] = im;
            g_img_name[g_img_n] = photos[i];
            ++g_img_n;
        } else {
            char m[128];
            snprintf(m, sizeof(m), "не загрузилась: %s", photos[i]);
            log_line("img", m);
        }
    }
    if (g_img_n > 0) {
        printf("loaded %d photo(s)\n", g_img_n);
        log_line("img", "кадр 1 готов, клик по вкладке «Фото»");
    } else {
        log_line("img", "нет картинок (test.jpg / test.png)");
    }

    log_line("app", "запуск");
    log_line("app", "перенос по ширине окна: растяните окно — длинные строки лягут заново.");

    ag_main_loop();

    for (int i = 0; i < g_img_n; ++i) ag_image_destroy(g_img[i]);
    ag_ui_destroy(g_ui);
    ag_window_destroy(g_win);
    ag_font_close(g_font);
    ag_shutdown();
    return 0;
}
