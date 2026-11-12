// What the runner draws itself, on top of and around the game's picture: the bar of controls above the game,
// its menu, short messages, and the in-game web pages. host_gl.c owns the window; ui.c and host_web.c draw.
#pragma once
#include <stdint.h>
#include <SDL.h>

// ---- OpenGL entry points the runner's own drawing uses (desktop OpenGL, loaded at start-up) -----------------
#ifndef GLAPIENTRY
#ifdef _WIN32
#define GLAPIENTRY __stdcall
#else
#define GLAPIENTRY
#endif
#endif
struct HostGL {
    void (GLAPIENTRY *Viewport)(int, int, int, int);
    void (GLAPIENTRY *Scissor)(int, int, int, int);
    void (GLAPIENTRY *Enable)(unsigned);
    void (GLAPIENTRY *Disable)(unsigned);
    void (GLAPIENTRY *BlendFunc)(unsigned, unsigned);
    void (GLAPIENTRY *BlendEquation)(unsigned);
    void (GLAPIENTRY *MatrixMode)(unsigned);
    void (GLAPIENTRY *LoadIdentity)(void);
    void (GLAPIENTRY *Ortho)(double, double, double, double, double, double);
    void (GLAPIENTRY *Begin)(unsigned);
    void (GLAPIENTRY *End)(void);
    void (GLAPIENTRY *Vertex2f)(float, float);
    void (GLAPIENTRY *TexCoord2f)(float, float);
    void (GLAPIENTRY *Color4ub)(unsigned char, unsigned char, unsigned char, unsigned char);
    void (GLAPIENTRY *BindTexture)(unsigned, unsigned);
    void (GLAPIENTRY *GenTextures)(int, unsigned *);
    void (GLAPIENTRY *DeleteTextures)(int, const unsigned *);
    void (GLAPIENTRY *TexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
    void (GLAPIENTRY *TexSubImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
    void (GLAPIENTRY *TexParameteri)(unsigned, unsigned, int);
    void (GLAPIENTRY *TexEnvi)(unsigned, unsigned, int);
    void (GLAPIENTRY *PixelStorei)(unsigned, int);
    void (GLAPIENTRY *PushAttrib)(unsigned);
    void (GLAPIENTRY *PopAttrib)(void);
    void (GLAPIENTRY *PushClientAttrib)(unsigned);
    void (GLAPIENTRY *PopClientAttrib)(void);
    void (GLAPIENTRY *ClearColor)(float, float, float, float);
    void (GLAPIENTRY *Clear)(unsigned);
    void (GLAPIENTRY *GetIntegerv)(unsigned, int *);
    void (GLAPIENTRY *ReadPixels)(int, int, int, int, unsigned, unsigned, void *);
    void (GLAPIENTRY *ColorMask)(unsigned char, unsigned char, unsigned char, unsigned char);
    void (GLAPIENTRY *DepthMask)(unsigned char);
    void (GLAPIENTRY *UseProgram)(unsigned);
    void (GLAPIENTRY *ActiveTexture)(unsigned);
    void (GLAPIENTRY *BindBuffer)(unsigned, unsigned);
    void (GLAPIENTRY *BindFramebuffer)(unsigned, unsigned);
    void (GLAPIENTRY *GenFramebuffers)(int, unsigned *);
    void (GLAPIENTRY *FramebufferTexture2D)(unsigned, unsigned, unsigned, unsigned, int);
    void (GLAPIENTRY *GenRenderbuffers)(int, unsigned *);
    void (GLAPIENTRY *BindRenderbuffer)(unsigned, unsigned);
    void (GLAPIENTRY *RenderbufferStorage)(unsigned, unsigned, int, int);
    void (GLAPIENTRY *FramebufferRenderbuffer)(unsigned, unsigned, unsigned, unsigned);
    unsigned (GLAPIENTRY *CheckFramebufferStatus)(unsigned);
};
extern struct HostGL hg;
int hostgl_load(void);                       // 0 when a function the runner needs is missing

// ---- layout ---------------------------------------------------------------------------------------------
typedef struct { int x, y, w, h; } UiRect;   // window pixels, y from the top
extern UiRect ui_view;                       // where the game's picture is shown
extern UiRect ui_bar;                        // the bar of controls above it
extern UiRect ui_strip;                      // the bar's background: the window's whole width when the runner draws its own frame
extern int ui_own_frame;                     // 1: no frame from the system; the bar is the title bar and has the window's buttons
int ui_draggable(int x, int y);              // 1 where pressing on the bar moves the window
extern int ui_game_w, ui_game_h;             // size of the game's own picture
void ui_layout(int win_w, int win_h);        // works out ui_view and ui_bar for this window size
double ui_bar_share(void);                   // bar height as a share of the picture's width
int ui_bar_height(int picture_w);            // bar height in pixels for a picture this wide

// ---- settings kept in <game folder>/desktop.ini -------------------------------------------------------
int cfg_int(const char *name, int fallback);
void cfg_set(const char *name, int value);

// ---- drawing (inside ui_draw / web_draw only) -----------------------------------------------------------
void ui_init(int game_w, int game_h);
void ui_draw(int win_w, int win_h, unsigned game_texture);
int ui_event(const SDL_Event *e);            // 1 when the runner's own controls took the event
void ui_toast(const char *fmt, ...);
void ui_frame_shown(void);                   // counts frames for the frame-rate readout
void ui_fill(float x, float y, float w, float h, uint32_t top_rgba, uint32_t bottom_rgba);
void ui_image(unsigned tex, float x, float y, float w, float h, float u0, float v0, float u1, float v1, uint32_t rgba);
float ui_text(float x, float y, float px, uint32_t rgba, const char *text);    // y is the top of the line; returns the width
float ui_text_width(float px, const char *text);
void ui_clip(float x, float y, float w, float h);
void ui_unclip(void);
unsigned ui_texture(const void *pixels, int w, int h, unsigned format, int smooth);

// ---- the in-game web page (host_web.c) ------------------------------------------------------------------
void web_show(const char *url, const char *post, int x, int y, int w, int h);
void web_set_visible(int on);
int web_state(void);
int web_take_param(char *out, size_t cap);
void web_draw(void);
int web_event(const SDL_Event *e, int mouse_x, int mouse_y);   // 1 when the page took the event
void web_relayout(void);                     // the window changed size

// ---- what the controls ask of the window (host_gl.c) and the sound (host_audio.c) -----------------------
void video_press_back(void);
void video_take_picture(void);
void video_toggle_fullscreen(void);
int video_is_fullscreen(void);
void video_set_window_size(int which);       // 0 small, 1 medium, 2 large
void video_minimize(void);
void video_toggle_maximize(void);            // fill the screen's work area, or go back
int video_is_maximized(void);
void video_close(void);
int video_mouse(int *x, int *y);             // where the pointer is, in window pixels; 0 when it is outside the window
void video_set_smooth(int on);
void video_set_vsync(int on);                // time frames by the screen's refresh when it is at about 60 Hz
void video_open_folder(const char *path);
const char *video_pictures_folder(void);
int audio_master(void);
void audio_set_master(int percent);
void audio_set_muted(int on);
int audio_muted(void);
int http_fetch(const char *url, const char *method, const char *headers, const void *body, size_t body_len, uint8_t **out, size_t *out_len);  // -> HTTP status, 0 if unreachable
