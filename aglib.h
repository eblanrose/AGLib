#ifndef AGLIB_H
#define AGLIB_H

#ifdef __cplusplus
extern "C" {
#endif

#define AGLIB_VERSION_MAJOR 0
#define AGLIB_VERSION_MINOR 2
#define AGLIB_VERSION_PATCH 0

#if defined(_WIN32) && defined(AGLIB_BUILD_SHARED)
#  define AGAPI __declspec(dllexport)
#elif defined(_WIN32) && defined(AGLIB_USE_SHARED)
#  define AGAPI __declspec(dllimport)
#else
#  define AGAPI
#endif

typedef unsigned char  ag_u8;
typedef unsigned int   ag_u32;
typedef int            ag_i32;

typedef ag_u32 ag_color;

#define AG_RGB(r,g,b) (((ag_u32)(r)<<16) | ((ag_u32)(g)<<8) | (ag_u32)(b))
#define AG_BLACK   AG_RGB(0,0,0)
#define AG_WHITE   AG_RGB(255,255,255)
#define AG_RED     AG_RGB(255,0,0)
#define AG_GREEN   AG_RGB(0,255,0)
#define AG_BLUE    AG_RGB(0,0,255)
#define AG_GRAY    AG_RGB(128,128,128)

typedef struct ag_window  ag_window;
typedef struct ag_canvas  ag_canvas;
typedef struct ag_font    ag_font;
typedef struct ag_image   ag_image;
typedef struct ag_ui      ag_ui;
typedef struct ag_widget  ag_widget;

typedef enum {
    AG_EVENT_NONE = 0,
    AG_EVENT_CLOSE,          
    AG_EVENT_RESIZE,         
    AG_EVENT_KEY_DOWN,
    AG_EVENT_KEY_UP,
    AG_EVENT_CHAR,           
    AG_EVENT_MOUSE_DOWN,
    AG_EVENT_MOUSE_UP,
    AG_EVENT_MOUSE_MOVE,
    AG_EVENT_MOUSE_WHEEL,    
    AG_EVENT_EXPOSE          
} ag_event_type;

typedef struct {
    ag_event_type type;
    int  x, y;              
    int  width, height;     
    int  key;               
    int  button;            
    int  wheel_delta;       
    int  mouse_x, mouse_y;
} ag_event;

typedef int (*ag_event_cb)(ag_window *w, const ag_event *e, void *userdata);

AGAPI int  ag_init(void);
AGAPI void ag_shutdown(void);
AGAPI const char *ag_get_platform(void);  

AGAPI ag_window *ag_window_create(const char *title, int width, int height);
AGAPI void       ag_window_destroy(ag_window *w);
AGAPI void       ag_window_set_title(ag_window *w, const char *title);
AGAPI void       ag_window_set_size(ag_window *w, int width, int height);
AGAPI void       ag_window_get_size(ag_window *w, int *width, int *height);
AGAPI void       ag_window_set_callback(ag_window *w, ag_event_cb cb, void *userdata);
AGAPI void       ag_window_show(ag_window *w);
AGAPI void       ag_window_hide(ag_window *w);
AGAPI void       ag_window_set_font(ag_window *w, ag_font *font);
AGAPI void       ag_window_redraw(ag_window *w);  

AGAPI int  ag_poll_events(void);      
AGAPI void ag_main_loop(void);        
AGAPI void ag_quit(void);             

AGAPI ag_font *ag_font_open(const char *name, unsigned int px);
AGAPI ag_font *ag_font_open_file(const char *path, unsigned int px);
AGAPI ag_font *ag_font_default(unsigned int px);
AGAPI void     ag_font_close(ag_font *f);
AGAPI int      ag_font_ascent(const ag_font *f);
AGAPI int      ag_font_descent(const ag_font *f);
AGAPI int      ag_font_height(const ag_font *f);
AGAPI int      ag_font_text_width(const ag_font *f, const char *utf8);
AGAPI const char *ag_font_family(const ag_font *f);
AGAPI int      ag_font_set_size(ag_font *f, unsigned int px);


AGAPI void ag_begin_frame(ag_window *w);
AGAPI void ag_end_frame(ag_window *w);     

AGAPI void ag_clear(ag_window *w, ag_color c);

AGAPI void ag_set_color(ag_window *w, ag_color c);
AGAPI void ag_set_font_size(ag_window *w, int px);

AGAPI void ag_draw_pixel(ag_window *w, int x, int y);
AGAPI void ag_draw_line (ag_window *w, int x0, int y0, int x1, int y1);
AGAPI void ag_draw_rect (ag_window *w, int x, int y, int ww, int hh);
AGAPI void ag_fill_rect (ag_window *w, int x, int y, int ww, int hh);
AGAPI void ag_fill_round_rect(ag_window *w, int x, int y, int ww, int hh, int rad);
AGAPI void ag_draw_round_rect(ag_window *w, int x, int y, int ww, int hh, int rad);
AGAPI void ag_draw_text (ag_window *w, int x, int y, const char *text);

AGAPI ag_image *ag_image_load(const char *path);
AGAPI ag_image *ag_image_load_svg(const char *path, float scale);  
AGAPI ag_image *ag_image_create(int w, int h);
AGAPI ag_image *ag_image_clone(const ag_image *img);
AGAPI void      ag_image_destroy(ag_image *img);
AGAPI int       ag_image_get_size(const ag_image *img, int *w, int *h);
AGAPI ag_u32    ag_image_pixel(const ag_image *img, int x, int y);
AGAPI int       ag_image_save_png(const ag_image *img, const char *path);
AGAPI void ag_draw_image(ag_window *w, int x, int y, int wc, int hc,
                         const ag_image *img);

AGAPI int  ag_image_blur_dual_kawase(ag_image *img, int radius, int passes);
AGAPI int  ag_image_blur_box(ag_image *img, int radius, int passes);
AGAPI void ag_draw_image_blur(ag_window *w, int x, int y, int wc, int hc,
                              const ag_image *img, int radius, int passes);

typedef void (*ag_widget_cb)(ag_widget *wdg, void *userdata);

typedef void (*ag_layout_cb)(ag_ui *ui, int w, int h, void *userdata);

typedef struct {
    ag_color bg;                
    ag_color bg2;               
    ag_color border;            
    ag_color accent;            
    ag_color accent_d;          
    ag_color text;              
    ag_color hover;             
    ag_color pressed;           
    ag_color track;             
    ag_color thumb;             
    ag_color scrollbar;         
ag_color scrollbar_hover;   
    int      radius;
    int      borders;
} ag_ui_theme;

AGAPI ag_ui *ag_ui_create(ag_window *win);
AGAPI void   ag_ui_destroy(ag_ui *ui);
AGAPI void   ag_ui_set_font(ag_ui *ui, ag_font *font);
AGAPI void   ag_ui_set_theme(ag_ui *ui, const ag_ui_theme *th);
AGAPI void   ag_ui_set_layout_cb(ag_ui *ui, ag_layout_cb cb, void *ud);
AGAPI const ag_ui_theme *ag_ui_default_theme(void);
AGAPI void   ag_ui_draw(ag_ui *ui);          
AGAPI int    ag_ui_handle(ag_ui *ui, const ag_event *ev);  

AGAPI ag_widget *ag_ui_add_button (ag_ui *ui, const char *label, int x,int y,int w,int h, ag_widget_cb cb, void *ud);
AGAPI ag_widget *ag_ui_add_slider (ag_ui *ui, float vmin,float vmax,float value, int x,int y,int w,int h, ag_widget_cb cb, void *ud);
AGAPI ag_widget *ag_ui_add_toggle (ag_ui *ui, const char *label, int checked, int x,int y, ag_widget_cb cb, void *ud);
AGAPI ag_widget *ag_ui_add_textbox(ag_ui *ui, int x,int y,int w,int h, ag_widget_cb cb, void *ud);
AGAPI ag_widget *ag_ui_add_textview(ag_ui *ui, int x,int y,int w,int h);  

AGAPI void ag_widget_remove(ag_ui *ui, ag_widget *wdg);
AGAPI void ag_widget_move(ag_widget *wdg, int x, int y);
AGAPI void ag_widget_set_rect(ag_widget *wdg, int x, int y, int w, int h);  
AGAPI void ag_widget_get_rect(const ag_widget *wdg, int *x, int *y, int *w, int *h);
AGAPI void ag_widget_set_callback(ag_widget *wdg, ag_widget_cb cb, void *ud);

AGAPI const char *ag_button_label(const ag_widget *wdg);
AGAPI const char *ag_textbox_text(const ag_widget *wdg);
AGAPI void        ag_textbox_set_text(ag_widget *wdg, const char *text);
AGAPI int         ag_textbox_submitted(const ag_widget *wdg); 
AGAPI float       ag_slider_value(const ag_widget *wdg);
AGAPI int         ag_toggle_checked(const ag_widget *wdg);
AGAPI void        ag_textview_append(ag_widget *wdg, const char *text); 
AGAPI const char *ag_textview_text(const ag_widget *wdg);
AGAPI void        ag_textview_clear(ag_widget *wdg);
AGAPI void        ag_textview_set_scroll(ag_widget *wdg, int line);     


#define AG_PAGE_GLOBAL (-1)

typedef enum {
    AG_ANIM_FADE = 0,
    AG_ANIM_SLIDE_LEFT,
    AG_ANIM_SLIDE_RIGHT,
    AG_ANIM_SLIDE_UP,
    AG_ANIM_SLIDE_DOWN,
    AG_ANIM_SCALE,
    AG_ANIM_FLIP
} ag_anim_type;

typedef enum {
    AG_EASE_LINEAR = 0,
    AG_EASE_IN_QUAD,
    AG_EASE_OUT_QUAD,
    AG_EASE_IN_OUT_QUAD,
    AG_EASE_OUT_CUBIC,
    AG_EASE_IN_OUT_CUBIC,
    AG_EASE_OUT_BACK,
    AG_EASE_OUT_ELASTIC,
    AG_EASE_OUT_BOUNCE
} ag_ease;

AGAPI void ag_ui_set_transition(ag_ui *ui, ag_anim_type type,
                                int duration_ms, ag_ease ease);
typedef void (*ag_page_draw_cb)(ag_ui *ui, int page, void *userdata);
AGAPI void ag_ui_set_page_draw_cb(ag_ui *ui, ag_page_draw_cb cb, void *ud);
AGAPI void ag_ui_set_page(ag_ui *ui, int page);
AGAPI int  ag_ui_page(const ag_ui *ui);
AGAPI int  ag_ui_animating(const ag_ui *ui);
AGAPI void ag_ui_update(ag_ui *ui);
AGAPI void ag_widget_set_page(ag_widget *wdg, int page);
AGAPI int  ag_widget_page(const ag_widget *wdg);
AGAPI void ag_widget_set_anim_delay(ag_widget *wdg, int ms);
AGAPI float ag_ease_apply(ag_ease ease, float t);

typedef struct ag_video ag_video;

typedef enum {
    AG_VIDEO_BACKEND_AUTO = 0,
    AG_VIDEO_BACKEND_AVI,
    AG_VIDEO_BACKEND_MJPEG,
    AG_VIDEO_BACKEND_MP4,
    AG_VIDEO_BACKEND_MKV,
} ag_video_backend;

AGAPI ag_video   *ag_video_open(const char *path);
AGAPI ag_video   *ag_video_open_ex(const char *path, ag_video_backend backend);
AGAPI void        ag_video_close(ag_video *v);
AGAPI int         ag_video_get_size(const ag_video *v, int *w, int *h);
AGAPI double      ag_video_duration(const ag_video *v);
AGAPI double      ag_video_fps(const ag_video *v);
AGAPI double      ag_video_time(const ag_video *v);
AGAPI int         ag_video_frame_count(const ag_video *v);
AGAPI int         ag_video_seek(ag_video *v, double seconds);
AGAPI int         ag_video_next_frame(ag_video *v, ag_image **out);
AGAPI const char *ag_video_error(const ag_video *v);
AGAPI const char *ag_video_backend_name(const ag_video *v);

AGAPI void        ag_video_set_playing(ag_video *v, int play);
AGAPI double      ag_video_clock(ag_video *v);
AGAPI void        ag_video_set_volume(ag_video *v, double vol);
AGAPI int         ag_video_has_audio(const ag_video *v);

AGAPI ag_widget *ag_ui_add_video(ag_ui *ui, const char *path,
                                  int x, int y, int w, int h,
                                  ag_widget_cb cb, void *ud);
AGAPI ag_widget *ag_ui_add_image(ag_ui *ui, const ag_image *img,
                                  int x, int y, int w, int h,
                                  ag_widget_cb cb, void *ud);
AGAPI void   ag_widget_set_blur(ag_widget *wdg, int radius, int passes);
AGAPI void   ag_widget_set_image(ag_widget *wdg, const ag_image *img);
AGAPI void   ag_widget_set_caption(ag_widget *wdg, const char *text);
AGAPI void   ag_widget_prepare(ag_widget *wdg);
AGAPI int    ag_video_widget_playing(ag_widget *wdg);
AGAPI void   ag_video_widget_play(ag_widget *wdg, int play);
AGAPI void   ag_video_widget_seek(ag_widget *wdg, double seconds);
AGAPI double ag_video_widget_time(const ag_widget *wdg);
AGAPI const char *ag_video_last_error(void);

AGAPI unsigned long ag_time_ms(void);
AGAPI const char *ag_gpu_renderer(void);
AGAPI void ag_sleep_ms(unsigned int ms);

#ifdef __cplusplus
}
#endif

#endif