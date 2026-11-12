// Window, input and OpenGL. The game speaks OpenGL ES 2; the host gives it a desktop OpenGL context and
// translates the handful of places where the two differ (shader headers, depth calls, a few queries).
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL.h>

#include "runner.h"
#include "ui.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "stb_image_write.h"

// The game draws into a picture of its own (a framebuffer of a fixed size, game_w x game_h). Each frame that
// picture is shown in the window, scaled to fit under the bar of controls, so the window can be resized or made
// full screen while the game goes on believing its screen never changes.
static SDL_Window *win;
static SDL_GLContext ctx;
static int win_w = 640, win_h = 960, game_w = 640, game_h = 960, mouse_down;
static unsigned game_fbo, game_tex, game_depth;
static int fullscreen, picture_wanted, windowed_w, windowed_h;
static unsigned long touch_down_frame;          // when the mouse button went down, to keep very quick clicks from being lost
static int paced;                               // frames are timed by the display's refresh
static uint64_t pace_since; static int pace_frames;
static char test_shot[900];
static uint64_t gl_scratch;
static unsigned bound_array, bound_elements;
static unsigned long frame_no;
static int shot_frames[64], n_shots, shot_every;
static char shot_dir[512];

static float F(int n) { return cpu_s(n); }
#define STK(n) (*(uint64_t *)g2h(sp + 8 * (n)))

static void (GLAPIENTRY *p_glClearDepth)(double);
static void (GLAPIENTRY *p_glDepthRange)(double, double);
static const unsigned char *(GLAPIENTRY *p_glGetString)(unsigned);
static void (GLAPIENTRY *p_glShaderSource)(unsigned, int, const char *const *, const int *);
static void (GLAPIENTRY *p_glVertexAttribPointer)(unsigned, int, unsigned, unsigned char, int, const void *);
static void (GLAPIENTRY *p_glDrawElements)(unsigned, int, unsigned, const void *);
static void (GLAPIENTRY *p_glGetIntegerv)(unsigned, int *);
static void (GLAPIENTRY *p_glBindBuffer)(unsigned, unsigned);
static void (GLAPIENTRY *p_glRenderbufferStorage)(unsigned, unsigned, int, int);
static void (GLAPIENTRY *p_glReadPixels)(int, int, int, int, unsigned, unsigned, void *);
static unsigned (GLAPIENTRY *p_glGetError)(void);

static uint64_t sp_glClearDepthf(const uint64_t *x, uint64_t sp) { (void)x; (void)sp; p_glClearDepth(F(0)); return 0; }
static uint64_t sp_glDepthRangef(const uint64_t *x, uint64_t sp) { (void)x; (void)sp; p_glDepthRange(F(0), F(1)); return 0; }
static uint64_t sp_glReleaseShaderCompiler(const uint64_t *x, uint64_t sp) { (void)x; (void)sp; return 0; }
static uint64_t sp_glShaderBinary(const uint64_t *x, uint64_t sp) { (void)x; (void)sp; return 0; }
static uint64_t sp_glGetVertexAttribPointerv(const uint64_t *x, uint64_t sp) { (void)sp; uint64_t *o = g2h(x[2]); if (o) *o = 0; return 0; }
static uint64_t sp_glBindBuffer(const uint64_t *x, uint64_t sp) {
    (void)sp;
    if ((unsigned)x[0] == 0x8892) bound_array = (unsigned)x[1];
    if ((unsigned)x[0] == 0x8893) bound_elements = (unsigned)x[1];
    p_glBindBuffer((unsigned)x[0], (unsigned)x[1]);
    return 0;
}
static uint64_t sp_glVertexAttribPointer(const uint64_t *x, uint64_t sp) {
    (void)sp;
    const void *p = bound_array ? (const void *)(uintptr_t)x[5] : g2h(x[5]);
    p_glVertexAttribPointer((unsigned)x[0], (int)x[1], (unsigned)x[2], (unsigned char)x[3], (int)x[4], p);
    return 0;
}
static uint64_t sp_glDrawElements(const uint64_t *x, uint64_t sp) {
    (void)sp;
    const void *p = bound_elements ? (const void *)(uintptr_t)x[3] : g2h(x[3]);
    p_glDrawElements((unsigned)x[0], (int)x[1], (unsigned)x[2], p);
    return 0;
}
static uint64_t sp_glRenderbufferStorage(const uint64_t *x, uint64_t sp) {
    (void)sp;
    unsigned fmt = (unsigned)x[1];
    if (fmt == 0x8D62) fmt = 0x8051;            // RGB565 -> RGB8
    if (fmt == 0x81A5) fmt = 0x81A6;            // DEPTH_COMPONENT16 -> 24
    p_glRenderbufferStorage((unsigned)x[0], fmt, (int)x[2], (int)x[3]);
    return 0;
}
// Framebuffer 0 is the screen to the game; here it is the game's own picture.
static uint64_t sp_glBindFramebuffer(const uint64_t *x, uint64_t sp) {
    (void)sp;
    hg.BindFramebuffer((unsigned)x[0], (unsigned)x[1] ? (unsigned)x[1] : game_fbo);
    return 0;
}
static uint64_t sp_glGetIntegerv(const uint64_t *x, uint64_t sp) {
    (void)sp;
    int *out = g2h(x[1]);
    switch ((unsigned)x[0]) {
    case 0x8CA6: p_glGetIntegerv(0x8CA6, out); if ((unsigned)*out == game_fbo) *out = 0; return 0;   // FRAMEBUFFER_BINDING
    case 0x8DFB: *out = 256; return 0;          // MAX_VERTEX_UNIFORM_VECTORS
    case 0x8DFC: *out = 15; return 0;           // MAX_VARYING_VECTORS
    case 0x8DFD: *out = 224; return 0;          // MAX_FRAGMENT_UNIFORM_VECTORS
    case 0x8B9A: *out = 0x1401; return 0;       // IMPLEMENTATION_COLOR_READ_TYPE
    case 0x8B9B: *out = 0x1908; return 0;       // IMPLEMENTATION_COLOR_READ_FORMAT
    case 0x86A2: *out = 0; return 0;            // NUM_COMPRESSED_TEXTURE_FORMATS
    case 0x86A3: return 0;
    case 0x8DF9: *out = 0; return 0;            // NUM_SHADER_BINARY_FORMATS
    }
    p_glGetIntegerv((unsigned)x[0], out);
    return 0;
}
static uint64_t sp_glGetShaderPrecisionFormat(const uint64_t *x, uint64_t sp) {
    (void)sp;
    int *range = g2h(x[2]), *prec = g2h(x[3]);
    int is_int = (unsigned)x[1] >= 0x8DF3;
    if (range) { range[0] = is_int ? 31 : 127; range[1] = is_int ? 30 : 127; }
    if (prec) *prec = is_int ? 0 : 23;
    return 0;
}
static uint64_t sp_glGetString(const uint64_t *x, uint64_t sp) {
    (void)sp;
    const char *s;
    int slot;
    switch ((unsigned)x[0]) {
    case 0x1F00: s = "FFBE desktop runner"; slot = 0; break;
    case 0x1F01: s = (const char *)p_glGetString(0x1F01); slot = 1; break;
    case 0x1F02: s = "OpenGL ES 2.0 (desktop runner)"; slot = 2; break;
    case 0x1F03: s = "GL_OES_rgb8_rgba8 GL_OES_depth24 GL_OES_packed_depth_stencil GL_OES_element_index_uint"; slot = 3; break;
    case 0x8B8C: s = "OpenGL ES GLSL ES 1.00"; slot = 4; break;
    default: s = ""; slot = 5;
    }
    if (!s) s = "";
    char *dst = (char *)g2h(gl_scratch) + slot * 512;
    snprintf(dst, 512, "%s", s);
    return gl_scratch + (uint64_t)slot * 512;
}
// GLSL ES 1.00 -> desktop GLSL 1.20: drop "precision ...;" statements and neutralise the precision words.
static uint64_t sp_glShaderSource(const uint64_t *x, uint64_t sp) {
    (void)sp;
    int count = (int)x[1];
    uint64_t *strs = g2h(x[2]);
    int *lens = g2h(x[3]);
    size_t total = 0;
    for (int i = 0; i < count; i++) total += lens && lens[i] >= 0 ? (size_t)lens[i] : strlen(gstr(strs[i]));
    char *src = malloc(total + 1), *p = src;
    for (int i = 0; i < count; i++) { size_t l = lens && lens[i] >= 0 ? (size_t)lens[i] : strlen(gstr(strs[i])); memcpy(p, gstr(strs[i]), l); p += l; }
    *p = 0;
    for (char *q = src; (q = strstr(q, "precision")) != NULL;) {
        char *semi = strchr(q, ';'), *nl = strchr(q, '\n');
        if (semi && (!nl || semi < nl) && (q == src || q[-1] == '\n' || q[-1] == ' ' || q[-1] == '\t' || q[-1] == ';')) memset(q, ' ', (size_t)(semi - q) + 1);
        else q += 9;
    }
    for (char *q = src; (q = strstr(q, "#version")) != NULL;) { char *nl = strchr(q, '\n'); memset(q, ' ', nl ? (size_t)(nl - q) : strlen(q)); }
    static const char head[] = "#version 120\n#define lowp\n#define mediump\n#define highp\n";
    const char *parts[2] = {head, src};
    p_glShaderSource((unsigned)x[0], 2, parts, NULL);
    free(src);
    return 0;
}

#include "gl_names.h"
#include "gl_gen.inc"

uint64_t gl_call(int index, const uint64_t *x, uint64_t sp) {
    if (!ctx) return 0;
    static int check = -1, reported;
    if (check < 0) check = getenv("FFBE_GL_CHECK") != NULL;
    if (!check) return gl_dispatch(index, x, sp);
    // Diagnosis: name the GL call that raised an error (slow; off unless FFBE_GL_CHECK is set).
    while (p_glGetError() != 0) {}
    uint64_t r = gl_dispatch(index, x, sp);
    unsigned err = strcmp(GL_NAMES[index], "glGetError") ? p_glGetError() : 0;
    if (err && reported++ < 40)
        rlog("[gl] %s(%#llx, %#llx, %#llx, %#llx, %#llx, %#llx, %#llx, %#llx) raised error %#x\n", GL_NAMES[index], (unsigned long long)x[0], (unsigned long long)x[1],
             (unsigned long long)x[2], (unsigned long long)x[3], (unsigned long long)x[4], (unsigned long long)x[5], (unsigned long long)x[6], (unsigned long long)x[7], err);
    return r;
}

// ---------------------------------------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------------------------------------
static void window_resized(void) {
    SDL_GL_GetDrawableSize(win, &win_w, &win_h);
    ui_layout(win_w, win_h);
    web_relayout();
}
// Window sizes: the share of the screen's usable height the window takes (small, medium, large).
static const double SIZE_SHARE[3] = {0.62, 0.78, 0.94};
static void size_for(int which, int *w, int *h) {
    SDL_Rect area = {0, 0, 1280, 1024};
    int display = win ? SDL_GetWindowDisplayIndex(win) : 0;
    SDL_GetDisplayUsableBounds(display < 0 ? 0 : display, &area);
    double total = (area.h - (ui_own_frame ? 8 : 36)) * SIZE_SHARE[which < 0 || which > 2 ? 2 : which];   // 36: the system's title bar
    double per_h = 1.0 + ui_bar_share() * 2.0 / 3.0;                                         // picture plus bar, for a 2:3 picture
    int gh = (int)(total / per_h); if (gh > 2160) gh = 2160; if (gh < 480) gh = 480;
    gh -= gh % 6;
    *w = gh * 2 / 3; *h = gh;
}
static int bar_px(int picture_w) { return ui_bar_height(picture_w); }

// With the runner's own frame the bar is the title bar: pressing on a free part of it moves the window, and the
// window's edges resize it. SDL asks this for every pointer position.
static int maximized;
static SDL_Rect restore_rect;
static SDL_HitTestResult hit_test(SDL_Window *w, const SDL_Point *p, void *data) {
    (void)data;
    if (fullscreen) return SDL_HITTEST_NORMAL;
    int ww = 0, wh = 0; SDL_GetWindowSize(w, &ww, &wh);
    if (ww <= 0 || wh <= 0) return SDL_HITTEST_NORMAL;
    if (!maximized) {
        int e = 4 + ui_strip.h / 16, top = 3;
        int l = p->x < e, r = p->x >= ww - e, t = p->y < top, b = p->y >= wh - e;
        if (t && l) return SDL_HITTEST_RESIZE_TOPLEFT;
        if (t && r) return SDL_HITTEST_RESIZE_TOPRIGHT;
        if (b && (l || p->x < e * 3)) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
        if (b && (r || p->x >= ww - e * 3)) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
        if (l) return p->y >= wh - e * 3 ? SDL_HITTEST_RESIZE_BOTTOMLEFT : SDL_HITTEST_RESIZE_LEFT;
        if (r) return p->y >= wh - e * 3 ? SDL_HITTEST_RESIZE_BOTTOMRIGHT : SDL_HITTEST_RESIZE_RIGHT;
        if (t) return SDL_HITTEST_RESIZE_TOP;
        if (b) return SDL_HITTEST_RESIZE_BOTTOM;
    }
    if (maximized) return SDL_HITTEST_NORMAL;
    return ui_draggable(p->x * win_w / ww, p->y * win_h / wh) ? SDL_HITTEST_DRAGGABLE : SDL_HITTEST_NORMAL;
}
// The window's icon: a small picture the launcher leaves in the game folder (made from the user's own game files).
// Without it the window keeps the program's own icon.
static void set_icon(void) {
    char path[1100]; snprintf(path, sizeof path, "%s/icon.rgba", g_root);
    FILE *f = os_fopen(path, "rb");
    if (!f) return;
    unsigned char head[8];
    if (fread(head, 1, 8, f) == 8 && !memcmp(head, "FFIC", 4)) {
        int w = head[4] | head[5] << 8, h = head[6] | head[7] << 8;
        if (w > 0 && h > 0 && w <= 512 && h <= 512) {
            unsigned char *px = malloc((size_t)w * h * 4);
            if (fread(px, 4, (size_t)w * h, f) == (size_t)w * h) {
                SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(px, w, h, 32, w * 4, SDL_PIXELFORMAT_RGBA32);
                if (s) { SDL_SetWindowIcon(win, s); SDL_FreeSurface(s); }
            }
            free(px);
        }
    }
    fclose(f);
}

void video_init(int w, int h, const char *title) {
    if (win) return;
    ui_own_frame = !cfg_int("system_frame", 0) && !getenv("FFBE_SYSTEM_FRAME");
#ifdef SDL_HINT_WINDOWS_DPI_AWARENESS
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");     // real pixels on scaled Windows displays, not a blurred enlargement
#endif
#ifdef SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");            // the click that brings the window forward is a click in the game too
#endif
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) fatal("cannot open a window: %s", SDL_GetError());
    // The game's picture: FFBE_WINDOW=WxH if given (tests), else as large as the "large" window on this screen, so the
    // picture is exact at that size and only ever scaled a little.
    const char *ws = getenv("FFBE_WINDOW"), *rs = getenv("FFBE_RENDER");
    int vw, vh;                                                       // the picture's size on the screen at start
    if (ws && sscanf(ws, "%dx%d", &w, &h) == 2) { vw = w; vh = h; }
    else {
        size_for(2, &w, &h);
        if (cfg_int("render_screen", 0)) {                           // sharpest in full screen: as tall as the screen allows
            SDL_DisplayMode mode;
            if (SDL_GetDesktopDisplayMode(0, &mode) == 0) { int gh = (int)(mode.h / (1.0 + ui_bar_share() * 2.0 / 3.0)); gh -= gh % 6; if (gh > h) { h = gh > 2160 ? 2160 : gh; w = h * 2 / 3; } }
        }
        size_for(cfg_int("size", 2), &vw, &vh);
    }
    if (rs) sscanf(rs, "%dx%d", &w, &h);
    game_w = w; game_h = h;
    ui_init(game_w, game_h);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, vw, vh + bar_px(vw),
                           SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | (ui_own_frame ? SDL_WINDOW_BORDERLESS : 0));
    if (!win) fatal("cannot open a window: %s", SDL_GetError());
    if (ui_own_frame && SDL_SetWindowHitTest(win, hit_test, NULL) != 0) rlog("[video] the bar cannot move the window here: %s\n", SDL_GetError());
    set_icon();
    ctx = SDL_GL_CreateContext(win);
    SDL_StartTextInput();                           // typed characters arrive as SDL_TEXTINPUT
    if (!ctx) fatal("cannot create an OpenGL context: %s", SDL_GetError());
    video_set_vsync(cfg_int("vsync", 0));
    SDL_SetWindowMinimumSize(win, 320, 480 + bar_px(320));
    gl_load();
    p_glClearDepth = (void *)SDL_GL_GetProcAddress("glClearDepth");
    p_glDepthRange = (void *)SDL_GL_GetProcAddress("glDepthRange");
    p_glGetString = (void *)SDL_GL_GetProcAddress("glGetString");
    p_glShaderSource = (void *)SDL_GL_GetProcAddress("glShaderSource");
    p_glVertexAttribPointer = (void *)SDL_GL_GetProcAddress("glVertexAttribPointer");
    p_glDrawElements = (void *)SDL_GL_GetProcAddress("glDrawElements");
    p_glGetIntegerv = (void *)SDL_GL_GetProcAddress("glGetIntegerv");
    p_glBindBuffer = (void *)SDL_GL_GetProcAddress("glBindBuffer");
    p_glRenderbufferStorage = (void *)SDL_GL_GetProcAddress("glRenderbufferStorage");
    p_glReadPixels = (void *)SDL_GL_GetProcAddress("glReadPixels");
    p_glGetError = (void *)SDL_GL_GetProcAddress("glGetError");
    if (!p_glShaderSource || !p_glBindBuffer) fatal("this graphics driver does not provide OpenGL 2.0");
    if (!hostgl_load()) fatal("this graphics driver is too old: OpenGL 2.0 with framebuffer objects is needed");
    // The game's picture and its depth and stencil.
    hg.GenTextures(1, &game_tex);
    hg.BindTexture(0x0DE1, game_tex);
    hg.TexImage2D(0x0DE1, 0, 0x8058 /* RGBA8 */, game_w, game_h, 0, 0x1908, 0x1401, NULL);
    hg.TexParameteri(0x0DE1, 0x2802, 0x812F); hg.TexParameteri(0x0DE1, 0x2803, 0x812F);
    video_set_smooth(cfg_int("smooth", 1));
    hg.BindTexture(0x0DE1, 0);
    hg.GenRenderbuffers(1, &game_depth);
    hg.BindRenderbuffer(0x8D41, game_depth);
    hg.RenderbufferStorage(0x8D41, 0x88F0 /* DEPTH24_STENCIL8 */, game_w, game_h);
    hg.BindRenderbuffer(0x8D41, 0);
    hg.GenFramebuffers(1, &game_fbo);
    hg.BindFramebuffer(0x8D40, game_fbo);
    hg.FramebufferTexture2D(0x8D40, 0x8CE0, 0x0DE1, game_tex, 0);
    hg.FramebufferRenderbuffer(0x8D40, 0x8D00, 0x8D41, game_depth);
    hg.FramebufferRenderbuffer(0x8D40, 0x8D20, 0x8D41, game_depth);
    unsigned status = hg.CheckFramebufferStatus(0x8D40);
    if (status != 0x8CD5) fatal("this graphics driver cannot make the game's picture (framebuffer status 0x%x)", status);
    hg.Viewport(0, 0, game_w, game_h);
    hg.ClearColor(0, 0, 0, 1); hg.Clear(0x4000);
    gl_scratch = guest_alloc(4096);
    window_resized();
    rlog("[video] picture %dx%d in a %dx%d window, OpenGL %s on %s\n", game_w, game_h, win_w, win_h, (const char *)p_glGetString(0x1F02), (const char *)p_glGetString(0x1F01));
    const char *sd = getenv("FFBE_SHOT_DIR"), *sf = getenv("FFBE_SHOT_FRAMES");
    if (sd) snprintf(shot_dir, sizeof shot_dir, "%s", sd);
    if (sd && sf) {
        if (!strncmp(sf, "every:", 6)) shot_every = atoi(sf + 6);
        else while (*sf && n_shots < 64) { shot_frames[n_shots++] = atoi(sf); sf = strchr(sf, ','); if (!sf) break; sf++; }
    }
    if (cfg_int("fullscreen", 0) && !ws) video_toggle_fullscreen();
}
void video_size(int *w, int *h) { if (!win) video_init(640, 960, "FINAL FANTASY BRAVE EXVIUS"); *w = game_w; *h = game_h; }

// ---- what the bar's buttons do ---------------------------------------------------------------------------
static struct HcEvent synth[64]; static unsigned long synth_at[64]; static int n_synth;
static void synth_add(int type, float x, float y, int key, unsigned long at) { if (n_synth < 64) { synth[n_synth] = (struct HcEvent){.type = type, .x = x, .y = y, .key = key}; synth_at[n_synth++] = at; } }
void video_press_back(void) { synth_add(HEV_KEY_DOWN, 0, 0, 4, frame_no + 1); synth_add(HEV_KEY_UP, 0, 0, 4, frame_no + 4); }
void video_take_picture(void) { picture_wanted = 1; }
int video_is_fullscreen(void) { return fullscreen; }
void video_toggle_fullscreen(void) {
    if (!fullscreen) SDL_GetWindowSize(win, &windowed_w, &windowed_h);
    fullscreen = !fullscreen;
    SDL_SetWindowFullscreen(win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    cfg_set("fullscreen", fullscreen);
    window_resized();
}
int video_is_maximized(void) { return maximized; }
void video_toggle_maximize(void) {
    if (fullscreen) { video_toggle_fullscreen(); return; }
    if (!ui_own_frame) {
        if (SDL_GetWindowFlags(win) & SDL_WINDOW_MAXIMIZED) SDL_RestoreWindow(win); else SDL_MaximizeWindow(win);
    } else if (!maximized) {                                        // the screen's work area: the taskbar stays in view
        SDL_Rect area; int display = SDL_GetWindowDisplayIndex(win);
        if (SDL_GetDisplayUsableBounds(display < 0 ? 0 : display, &area) != 0) return;
        SDL_GetWindowPosition(win, &restore_rect.x, &restore_rect.y);
        SDL_GetWindowSize(win, &restore_rect.w, &restore_rect.h);
        SDL_SetWindowPosition(win, area.x, area.y);
        SDL_SetWindowSize(win, area.w, area.h);
        maximized = 1;
    } else {
        SDL_SetWindowSize(win, restore_rect.w, restore_rect.h);
        SDL_SetWindowPosition(win, restore_rect.x, restore_rect.y);
        maximized = 0;
    }
    window_resized();
}
void video_minimize(void) { SDL_MinimizeWindow(win); }
void video_close(void) { SDL_Event quit; memset(&quit, 0, sizeof quit); quit.type = SDL_QUIT; SDL_PushEvent(&quit); }
int video_mouse(int *x, int *y) {
    int gx, gy, wx, wy, ww = 0, wh = 0;
    SDL_GetGlobalMouseState(&gx, &gy);
    SDL_GetWindowPosition(win, &wx, &wy);
    SDL_GetWindowSize(win, &ww, &wh);
    if (ww <= 0 || wh <= 0 || gx < wx || gy < wy || gx >= wx + ww || gy >= wy + wh) return 0;
    *x = (gx - wx) * win_w / ww; *y = (gy - wy) * win_h / wh;
    return 1;
}
void video_set_window_size(int which) {
    if (fullscreen) video_toggle_fullscreen();
    maximized = 0;
    int w, h; size_for(which, &w, &h);
    SDL_SetWindowSize(win, w, h + bar_px(w));
    SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    cfg_set("size", which);
    window_resized();
}
// Frame timing. The game's own clock shows 60 frames a second, which on a 60 Hz screen drifts against the screen's
// refresh and now and then shows a frame twice or not at all. With "match the screen" on and a screen at about 60 Hz,
// each frame instead waits for the screen's refresh. If that turns out not to hold the pace (no real display, a driver
// that ignores it), the game's clock takes over again.
void video_set_vsync(int on) {
    paced = 0;
    SDL_DisplayMode mode;
    int display = SDL_GetWindowDisplayIndex(win);
    int hz = SDL_GetCurrentDisplayMode(display < 0 ? 0 : display, &mode) == 0 ? mode.refresh_rate : 0;
    if (on && hz >= 59 && hz <= 61 && SDL_GL_SetSwapInterval(1) == 0) { paced = 1; pace_since = 0; pace_frames = 0; }
    else SDL_GL_SetSwapInterval(0);
    rlog("[video] frames timed by %s (screen at %d Hz)\n", paced ? "the screen's refresh" : "the game's clock", hz);
}
int video_paced(void) { return paced; }
static void pace_check(void) {                  // is the screen really setting the pace? Frames that come too fast say no.
    if (!paced) return;
    static int fast;
    uint64_t now = now_ns();
    if (pace_since && now - pace_since < 12000000ULL) fast++;
    pace_since = now;
    if (++pace_frames < 120) return;
    if (fast > 60) {
        paced = 0; SDL_GL_SetSwapInterval(0);
        rlog("[video] the screen's refresh is not holding the pace (%d of 120 frames came early); back to the game's clock\n", fast);
    }
    pace_frames = 0; fast = 0;
}
void video_set_smooth(int on) {
    int bound = 0;
    hg.GetIntegerv(0x8069, &bound);
    hg.BindTexture(0x0DE1, game_tex);
    hg.TexParameteri(0x0DE1, 0x2801, on ? 0x2601 : 0x2600); hg.TexParameteri(0x0DE1, 0x2800, on ? 0x2601 : 0x2600);
    hg.BindTexture(0x0DE1, (unsigned)bound);
}
const char *video_pictures_folder(void) {
    static char dir[1100];
    if (!dir[0]) {
        const char *e = getenv("FFBE_PICTURES");
        if (e && *e) snprintf(dir, sizeof dir, "%s", e); else snprintf(dir, sizeof dir, "%s/pictures", g_root);
        for (char *p = dir; *p; p++) if (*p == '\\') *p = '/';
    }
    return dir;
}
void video_open_folder(const char *path) {
    os_mkdir(path);
    char native[1100]; snprintf(native, sizeof native, "%s", path);
#ifdef _WIN32
    for (char *p = native; *p; p++) if (*p == '/') *p = '\\';
#endif
    os_open_url(native);
}

// Test control: a folder polled for a file named "cmd" with lines "tap X Y", "key back", "shot NAME", "quit" and, for
// the runner's own controls, "click X Y" (a mouse click at a window position), "wheel X Y N", "size N", "fullscreen".
static void save_shot_as(const char *path);
static void push_mouse(int type, int x, int y, int wheel) {
    SDL_Event e; SDL_zero(e);
    e.type = (Uint32)type;
    if (type == SDL_MOUSEWHEEL) { e.wheel.y = wheel; SDL_Event m; SDL_zero(m); m.type = SDL_MOUSEMOTION; m.motion.x = x; m.motion.y = y; m.motion.which = 9999; SDL_PushEvent(&m); }
    else if (type == SDL_MOUSEMOTION) { e.motion.x = x; e.motion.y = y; e.motion.which = 9999; }
    else { e.button.button = SDL_BUTTON_LEFT; e.button.x = x; e.button.y = y; e.button.which = 9999; }
    SDL_PushEvent(&e);
}
static void control_poll(void) {
    static const char *dir; static int checked;
    if (!checked) { checked = 1; dir = getenv("FFBE_CONTROL"); }
    if (!dir || frame_no % 10) return;
    char path[700], line[256];
    snprintf(path, sizeof path, "%s/cmd", dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        float x, y, x2, y2; char name[128]; int n;
        if (sscanf(line, "tap %f %f", &x, &y) == 2) { synth_add(HEV_TOUCH_DOWN, x, y, 0, frame_no + 1); synth_add(HEV_TOUCH_UP, x, y, 0, frame_no + 6); rlog("[control] tap %.0f %.0f at frame %lu\n", x, y, frame_no); }
        else if (sscanf(line, "drag %f %f %f %f", &x, &y, &x2, &y2) == 4) {            // press, slide over about a second, release
            synth_add(HEV_TOUCH_DOWN, x, y, 0, frame_no + 1);
            for (int i = 1; i <= 12; i++) synth_add(HEV_TOUCH_MOVE, x + (x2 - x) * i / 12, y + (y2 - y) * i / 12, 0, frame_no + 1 + (unsigned long)i * 5);
            synth_add(HEV_TOUCH_UP, x2, y2, 0, frame_no + 100);
            rlog("[control] drag %.0f %.0f -> %.0f %.0f at frame %lu\n", x, y, x2, y2, frame_no);
        }
        else if (!strncmp(line, "key back", 8)) { synth_add(HEV_KEY_DOWN, 0, 0, 4, frame_no + 1); synth_add(HEV_KEY_UP, 0, 0, 4, frame_no + 4); }
        else if (sscanf(line, "shot %127s", name) == 1) snprintf(test_shot, sizeof test_shot, "%s/%s.ppm", dir, name);
        else if (sscanf(line, "click %f %f", &x, &y) == 2) { push_mouse(SDL_MOUSEMOTION, (int)x, (int)y, 0); push_mouse(SDL_MOUSEBUTTONDOWN, (int)x, (int)y, 0); push_mouse(SDL_MOUSEBUTTONUP, (int)x, (int)y, 0); }
        else if (sscanf(line, "mdrag %f %f %f %f", &x, &y, &x2, &y2) == 4) {
            push_mouse(SDL_MOUSEMOTION, (int)x, (int)y, 0); push_mouse(SDL_MOUSEBUTTONDOWN, (int)x, (int)y, 0);
            for (int i = 1; i <= 8; i++) push_mouse(SDL_MOUSEMOTION, (int)(x + (x2 - x) * i / 8), (int)(y + (y2 - y) * i / 8), 0);
            push_mouse(SDL_MOUSEBUTTONUP, (int)x2, (int)y2, 0);
        }
        else if (sscanf(line, "wheel %f %f %d", &x, &y, &n) == 3) push_mouse(SDL_MOUSEWHEEL, (int)x, (int)y, n);
        else if (sscanf(line, "size %d", &n) == 1) video_set_window_size(n);
        else if (sscanf(line, "resize %f %f", &x, &y) == 2) { SDL_SetWindowSize(win, (int)x, (int)y); window_resized(); }
        else if (!strncmp(line, "fullscreen", 10)) video_toggle_fullscreen();
        else if (!strncmp(line, "maximize", 8)) video_toggle_maximize();
        else if (!strncmp(line, "minimize", 8)) video_minimize();
        else if (!strncmp(line, "picture", 7)) video_take_picture();
        else if (!strncmp(line, "quit", 4)) synth_add(HEV_QUIT, 0, 0, 0, frame_no + 1);
    }
    fclose(f);
    remove(path);
}
static void save_shot(void) {
    char path[700];
    snprintf(path, sizeof path, "%s/frame_%06lu.ppm", shot_dir, frame_no);
    save_shot_as(path);
}
static void save_shot_as(const char *path) {                       // the whole window, as the person sees it
    unsigned char *pix = malloc((size_t)win_w * win_h * 3);
    hg.PixelStorei(0x0D05, 1);
    p_glReadPixels(0, 0, win_w, win_h, 0x1907, 0x1401, pix);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", win_w, win_h);
        for (int y = win_h - 1; y >= 0; y--) fwrite(pix + (size_t)y * win_w * 3, 1, (size_t)win_w * 3, f);
        fclose(f);
    }
    free(pix);
}
static void png_to_file(void *ud, void *data, int size) { fwrite(data, 1, (size_t)size, (FILE *)ud); }
static void save_picture(void) {                                    // the game's own picture at full detail, as a PNG
    const char *dir = video_pictures_folder();
    os_mkdir(dir);
    time_t now = time(NULL); struct tm *tmv = localtime(&now);
    char name[64], path[1300];
    strftime(name, sizeof name, "FFBE %Y-%m-%d %H.%M.%S", tmv);
    snprintf(path, sizeof path, "%s/%s.png", dir, name);
    for (int n = 2; n < 100; n++) { FILE *t = os_fopen(path, "rb"); if (!t) break; fclose(t); snprintf(path, sizeof path, "%s/%s (%d).png", dir, name, n); }
    unsigned char *pix = malloc((size_t)game_w * game_h * 3);
    hg.BindFramebuffer(0x8D40, game_fbo);
    hg.PixelStorei(0x0D05, 1);
    p_glReadPixels(0, 0, game_w, game_h, 0x1907, 0x1401, pix);
    hg.BindFramebuffer(0x8D40, 0);
    FILE *f = os_fopen(path, "wb");
    if (f) {
        stbi_flip_vertically_on_write(1);
        stbi_write_png_to_func(png_to_file, f, game_w, game_h, 3, pix, game_w * 3);
        fclose(f);
        ui_toast("Picture saved");
        rlog("[video] picture saved to %s\n", path);
    } else ui_toast("The picture could not be saved");
    free(pix);
}
void video_present(void) {
    if (!win) return;
    control_poll();
    // Everything the game set is put back afterwards: it keeps its own record of the OpenGL state.
    int program = 0, array_buffer = 0, bound_fbo = 0;
    static int timing = -1; static uint64_t t_game, t_own, t_swap; static int t_n;
    if (timing < 0) timing = getenv("FFBE_TIME_PRESENT") != NULL;       // diagnosis: where the time to show a frame goes
    uint64_t t0 = timing ? now_ns() : 0;
    if (timing) { void (GLAPIENTRY *finish)(void) = (void *)SDL_GL_GetProcAddress("glFinish"); finish(); }
    uint64_t t1 = timing ? now_ns() : 0;
    hg.GetIntegerv(0x8B8D, &program); hg.GetIntegerv(0x8894, &array_buffer); hg.GetIntegerv(0x8CA6, &bound_fbo);
    hg.PushAttrib(0x000FFFFF);
    hg.PushClientAttrib(0xFFFFFFFF);
    hg.BindFramebuffer(0x8D40, 0);
    hg.UseProgram(0);
    hg.BindBuffer(0x8892, 0);
    hg.ActiveTexture(0x84C0);
    hg.Disable(0x0C11); hg.Disable(0x0B71); hg.Disable(0x0B90); hg.Disable(0x0B44); hg.Disable(0x0BC0);
    hg.Disable(0x8037); hg.Disable(0x0BD0);                         // polygon offset, dither
    hg.ColorMask(1, 1, 1, 1);
    if (picture_wanted) { picture_wanted = 0; save_picture(); }
    ui_draw(win_w, win_h, game_tex);
    int want = shot_every && frame_no % (unsigned long)shot_every == 0;
    for (int i = 0; i < n_shots; i++) if ((unsigned long)shot_frames[i] == frame_no) want = 1;
    if (want && shot_dir[0]) save_shot();
    if (test_shot[0]) { save_shot_as(test_shot); test_shot[0] = 0; }
    uint64_t t2 = timing ? now_ns() : 0;
    SDL_GL_SwapWindow(win);
    if (timing) {
        t_game += t1 - t0; t_own += t2 - t1; t_swap += now_ns() - t2;
        if (++t_n == 300) { rlog("[video] per frame: the game's drawing %.2f ms, the runner's own %.2f ms, showing it %.2f ms\n", t_game / 3e8, t_own / 3e8, t_swap / 3e8); t_game = t_own = t_swap = 0; t_n = 0; }
    }
    pace_check();
    hg.PopClientAttrib();
    hg.PopAttrib();
    hg.UseProgram((unsigned)program);
    hg.BindBuffer(0x8892, (unsigned)array_buffer);
    hg.BindFramebuffer(0x8D40, (unsigned)bound_fbo);
    ui_frame_shown();
    frame_no++;
}
// Window position -> the game's own pixels.
static void to_game(int x, int y, float *gx, float *gy) {
    *gx = (float)(x - ui_view.x) * game_w / (float)(ui_view.w > 0 ? ui_view.w : 1);
    *gy = (float)(y - ui_view.y) * game_h / (float)(ui_view.h > 0 ? ui_view.h : 1);
    if (*gx < 0) *gx = 0; if (*gx > game_w - 1) *gx = (float)(game_w - 1);
    if (*gy < 0) *gy = 0; if (*gy > game_h - 1) *gy = (float)(game_h - 1);
}
// The mouse wheel. The game only knows touches, so turning the wheel over the game becomes a finger that presses where
// the pointer is, slides, rests and lifts: lists scroll as they do under a thumb. The rest before lifting keeps the
// list from being flung. `wheel=0` in desktop.ini turns it off.
static struct { int state, hold, still; float ox, oy, x, y, px, py; unsigned long last, frame; } whl;
static int ptr_x, ptr_y;

// ---- the mouse as a finger -------------------------------------------------------------------------------
// The game expects a finger: it goes down, perhaps slides, and lifts, one thing after the other, a frame at a time.
// A mouse is quicker and sloppier than that, and the game's main thread sometimes stops for a moment (loading an
// attack's effects), after which several clicks arrive at once. Three rules keep what the game sees in order:
//  - A release that comes within three frames of its press waits until the third frame (the game's buttons ignore
//    a touch that is down for less than a frame or two). While it waits, nothing else from the mouse is looked at,
//    so a second click made in the meantime is played after the first, not into the middle of it.
//  - After a finger has lifted, the next one goes down in the next frame at the earliest.
//  - A click made while the pointer is travelling is a tap where it landed, not a slide: see tap_begin.
#define TAP_FRAMES 3
static struct { int held; float x, y; unsigned long at; } late_up;       // a quick click's release, waiting for its frame
static unsigned long up_frame = (unsigned long)-1;                       // the frame in which the game was last told of a lift
static SDL_Event deferred; static int has_deferred;                     // one event put aside to be handled first, next time

// A click made in passing. Someone who clicks units one after the other does not stop the mouse for each click:
// the button goes down and comes up some way apart, and passed on as it is the game reads a slide (in a battle:
// the pull that opens abilities or guards). A real slide starts from rest. So if the pointer was already moving
// when the button went down, its movements are kept from the game while the click could still be a click: for
// TAP_MS, unless it goes further than TAP_FAR of the picture's height. A release inside that is a tap at the
// place where the button went down. Past it, the finger is where the pointer is and slides as usual.
// `tap_guard=0` in desktop.ini turns this off.
#define TAP_MS 170
#define TAP_FAR 0.25f
#define TAP_NEAR 0.02f
static struct { int on, sx, sy; float gx, gy; uint64_t since; } tap;
static struct { uint64_t t; float step; } trail[16]; static int trail_at;  // the pointer's last movements: when, how far
static void trail_add(int x, int y) {
    static int lx, ly, have;
    float dx = (float)(x - lx), dy = (float)(y - ly);
    if (have) { trail[trail_at].t = now_ns(); trail[trail_at].step = sqrtf(dx * dx + dy * dy); trail_at = (trail_at + 1) % 16; }
    lx = x; ly = y; have = 1;
}
static int pointer_travelling(void) {                  // has it covered some ground in the last 150 ms?
    uint64_t now = now_ns(); float sum = 0;
    for (int i = 0; i < 16; i++) if (trail[i].t && now - trail[i].t < 150000000ULL) sum += trail[i].step;
    return sum > fmaxf(8, ui_view.h * 0.015f);
}
static void tap_begin(int x, int y, float gx, float gy) {
    tap.on = cfg_int("tap_guard", 1) && pointer_travelling();
    tap.sx = x; tap.sy = y; tap.gx = gx; tap.gy = gy; tap.since = now_ns();
}
static int tap_over(int x, int y) {                     // has this press turned out to be a slide after all?
    float dx = (float)(x - tap.sx), dy = (float)(y - tap.sy), d = sqrtf(dx * dx + dy * dy);
    if (d > ui_view.h * TAP_FAR) return 1;
    return now_ns() - tap.since > TAP_MS * 1000000ULL && d > fmaxf(6, ui_view.h * TAP_NEAR);
}
static void wheel_turn(float dx, float dy) {
    if (!cfg_int("wheel", 1)) return;
    if (ptr_x < ui_view.x || ptr_x >= ui_view.x + ui_view.w || ptr_y < ui_view.y || ptr_y >= ui_view.y + ui_view.h) return;
    float step = game_h * 0.085f;
    whl.px += dx * step; whl.py += dy * step;
    whl.last = frame_no;
    if (whl.state == 0) to_game(ptr_x, ptr_y, &whl.ox, &whl.oy);
}
static float wheel_axis(float *at, float *pending, float low, float high) {
    float target = *at + *pending;
    if (target < low) target = low; if (target > high) target = high;
    float room = target - *at, move = room * 0.45f;
    if (fabsf(room) < 0.5f) return 0;
    if (fabsf(move) < 7) move = room < 0 ? -fminf(7, -room) : fminf(7, room);
    *at += move; *pending -= move;
    return move;
}
static void wheel_tick(void) {
    if (whl.frame == frame_no) return;
    whl.frame = frame_no;
    if (whl.state == 0) {
        if ((fabsf(whl.px) < 0.5f && fabsf(whl.py) < 0.5f) || mouse_down || late_up.held || n_synth) { if (mouse_down || late_up.held) whl.px = whl.py = 0; return; }
        whl.x = whl.ox; whl.y = whl.oy; whl.state = 1; whl.hold = 2; whl.still = 0;
        synth_add(HEV_TOUCH_DOWN, whl.x, whl.y, 0, frame_no);
    } else if (whl.state == 1) {
        if (whl.hold > 0) { whl.hold--; return; }
        float mx = wheel_axis(&whl.x, &whl.px, 4, (float)game_w - 5), my = wheel_axis(&whl.y, &whl.py, 4, (float)game_h - 5);
        synth_add(HEV_TOUCH_MOVE, whl.x, whl.y, 0, frame_no);
        if (mx != 0 || my != 0) { whl.still = 0; return; }
        int more = fabsf(whl.px) >= 0.5f || fabsf(whl.py) >= 0.5f;       // more to go, but the finger is at the picture's edge
        if (++whl.still >= 5 && (more || frame_no - whl.last >= 9)) { synth_add(HEV_TOUCH_UP, whl.x, whl.y, 0, frame_no); whl.state = 2; whl.hold = 3; }
    } else if (--whl.hold <= 0) whl.state = 0;
}
static int poll_one(struct HcEvent *ev) {
    SDL_Event e;
    memset(ev, 0, sizeof *ev);
    wheel_tick();
    for (int i = 0; i < n_synth; i++) if (synth_at[i] <= frame_no) {
        *ev = synth[i];
        for (int k = i; k + 1 < n_synth; k++) { synth[k] = synth[k + 1]; synth_at[k] = synth_at[k + 1]; }
        n_synth--;
        if (ev->type == HEV_TOUCH_UP) up_frame = frame_no;
        return 1;
    }
    if (late_up.held) {                                 // a quick click's release: due yet? Until it is out, the mouse waits
        if (frame_no < late_up.at) return 0;
        late_up.held = 0; up_frame = frame_no;
        ev->type = HEV_TOUCH_UP; ev->x = late_up.x; ev->y = late_up.y; return 1;
    }
    if (up_frame == frame_no) return 0;                 // a finger has just lifted: the next thing happens in the next frame
    if (mouse_down && tap.on && tap_over(ptr_x, ptr_y)) {            // held too long or gone too far to be a click: it slides
        tap.on = 0;
        ev->type = HEV_TOUCH_MOVE; to_game(ptr_x, ptr_y, &ev->x, &ev->y); return 1;
    }
    for (;;) {
        int again = has_deferred;                       // an event put aside earlier comes first, as it was left
        if (again) { e = deferred; has_deferred = 0; }
        else if (!SDL_PollEvent(&e)) break;
        if (!again) {
            // Mouse positions arrive in window units; everything here works in the window's pixels.
            int ww = 0, wh = 0; SDL_GetWindowSize(win, &ww, &wh);
            if (ww > 0 && wh > 0 && (ww != win_w || wh != win_h)) {
                if (e.type == SDL_MOUSEMOTION) { e.motion.x = e.motion.x * win_w / ww; e.motion.y = e.motion.y * win_h / wh; }
                if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) { e.button.x = e.button.x * win_w / ww; e.button.y = e.button.y * win_h / wh; }
            }
            // The runner's own controls and the in-game web page come first, unless the game is in the middle of a touch.
            int mouse = e.type == SDL_MOUSEMOTION || e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP;
            if (e.type == SDL_MOUSEMOTION) { ptr_x = e.motion.x; ptr_y = e.motion.y; trail_add(ptr_x, ptr_y); }
            if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) { ptr_x = e.button.x; ptr_y = e.button.y; }
            // The pointer moves with the button up while we still hold a finger down: the release went missing (it
            // happened while another program had the keyboard, say). Lift the finger here. (9999: the test channel.)
            if (e.type == SDL_MOUSEMOTION && mouse_down && !(e.motion.state & SDL_BUTTON_LMASK) && e.motion.which != 9999) {
                int x = e.motion.x, y = e.motion.y;
                SDL_zero(e); e.type = SDL_MOUSEBUTTONUP; e.button.button = SDL_BUTTON_LEFT; e.button.x = x; e.button.y = y;
            }
            if (!(mouse && mouse_down) && ui_event(&e)) continue;
        }
        switch (e.type) {
        case SDL_QUIT: ev->type = HEV_QUIT; return 1;
        case SDL_MOUSEBUTTONDOWN:
            if (e.button.button != SDL_BUTTON_LEFT || mouse_down) break;
            if (e.button.x < ui_view.x || e.button.x >= ui_view.x + ui_view.w || e.button.y < ui_view.y || e.button.y >= ui_view.y + ui_view.h) break;
            if (whl.state == 1) {                       // the wheel's finger is still down: lift it, then press in the next frame
                whl.state = 0; whl.px = whl.py = 0; up_frame = frame_no;
                deferred = e; has_deferred = 1;
                ev->type = HEV_TOUCH_UP; ev->x = whl.x; ev->y = whl.y; return 1;
            }
            mouse_down = 1; touch_down_frame = frame_no; SDL_CaptureMouse(SDL_TRUE);     // a drag may leave the window
            ev->type = HEV_TOUCH_DOWN; to_game(e.button.x, e.button.y, &ev->x, &ev->y);
            tap_begin(e.button.x, e.button.y, ev->x, ev->y);
            return 1;
        case SDL_MOUSEBUTTONUP: {
            if (e.button.button != SDL_BUTTON_LEFT || !mouse_down) break;
            float gx, gy; to_game(e.button.x, e.button.y, &gx, &gy);
            if (tap.on) {
                tap.on = 0;
                if (tap_over(e.button.x, e.button.y)) {  // it did slide, and stopped before we looked: the finger goes there, then lifts
                    deferred = e; has_deferred = 1;
                    ev->type = HEV_TOUCH_MOVE; ev->x = gx; ev->y = gy; return 1;
                }
                gx = tap.gx; gy = tap.gy;               // a click: the finger lifts where it went down
            }
            mouse_down = 0; SDL_CaptureMouse(SDL_FALSE);
            // A very quick click (a touchpad tap, a mouse macro, a click made while the game was busy) can press and let
            // go within one frame, and the game then misses it: its buttons want to see the finger down for a frame or
            // two. Such a release waits, and so does everything the mouse does after it.
            if (frame_no < touch_down_frame + TAP_FRAMES) { late_up.held = 1; late_up.x = gx; late_up.y = gy; late_up.at = touch_down_frame + TAP_FRAMES; return 0; }
            up_frame = frame_no;
            ev->type = HEV_TOUCH_UP; ev->x = gx; ev->y = gy; return 1;
        }
        case SDL_MOUSEWHEEL: {
            float dx = (float)e.wheel.x, dy = (float)e.wheel.y;
#if SDL_VERSION_ATLEAST(2, 0, 18)
            if (e.wheel.preciseX != 0 || e.wheel.preciseY != 0) { dx = e.wheel.preciseX; dy = e.wheel.preciseY; }
#endif
            if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) { dx = -dx; dy = -dy; }
            if (SDL_GetModState() & KMOD_SHIFT) { dx = dy; dy = 0; }      // Shift turns the wheel sideways
            wheel_turn(-dx, dy);
            break;
        }
        case SDL_MOUSEMOTION:
            if (!mouse_down) break;
            if (tap.on) { if (!tap_over(e.motion.x, e.motion.y)) break; tap.on = 0; }
            ev->type = HEV_TOUCH_MOVE; to_game(e.motion.x, e.motion.y, &ev->x, &ev->y); return 1;
        case SDL_KEYDOWN: case SDL_KEYUP: {
            int sym = e.key.keysym.sym, code = sym == SDLK_ESCAPE ? 4 : sym == SDLK_BACKSPACE ? 67 : (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) ? 66 : 0;
            if (code) { ev->type = e.type == SDL_KEYDOWN ? HEV_KEY_DOWN : HEV_KEY_UP; ev->key = code; return 1; }
            break;
        }
        case SDL_TEXTINPUT: {                         // typed text, one character per event to the game
            const unsigned char *s = (const unsigned char *)e.text.text;
            for (int n = 0; *s && n_synth < 60; n++) {
                int c = *s, len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
                if (len == 2) c = (c & 0x1F) << 6 | (s[1] & 0x3F);
                else if (len == 3) c = (c & 0x0F) << 12 | (s[1] & 0x3F) << 6 | (s[2] & 0x3F);
                else if (len == 4) c = (c & 0x07) << 18 | (s[1] & 0x3F) << 12 | (s[2] & 0x3F) << 6 | (s[3] & 0x3F);
                synth_add(HEV_TEXT, 0, 0, c, frame_no);
                for (int k = 0; k < len && *s; k++) s++;
            }
            break;
        }
        case SDL_WINDOWEVENT:
            if (getenv("FFBE_WINDOW_LOG")) rlog("[video] window event %d (flags %#x) frame %lu\n", e.window.event, SDL_GetWindowFlags(win), frame_no);
            if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) window_resized();
#ifdef SDL_WINDOWEVENT_DISPLAY_CHANGED
            if (e.window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED) video_set_vsync(cfg_int("vsync", 0));   // another screen, another refresh rate
#endif
            if (e.window.event == SDL_WINDOWEVENT_MINIMIZED) { ev->type = HEV_PAUSE; return 1; }
            if (e.window.event == SDL_WINDOWEVENT_RESTORED) { ev->type = HEV_RESUME; return 1; }
            break;
        }
    }
    return 0;
}
int video_poll(struct HcEvent *ev) {
    static int tell = -1;
    if (tell < 0) tell = getenv("FFBE_TOUCH_LOG") != NULL;         // diagnosis: every touch the game is given, with its frame
    int got = poll_one(ev);
    if (tell && got && (ev->type == HEV_TOUCH_DOWN || ev->type == HEV_TOUCH_UP || ev->type == HEV_TOUCH_MOVE))
        rlog("[touch] %s %.0f %.0f frame %lu\n", ev->type == HEV_TOUCH_DOWN ? "down" : ev->type == HEV_TOUCH_UP ? "up" : "move", ev->x, ev->y, frame_no);
    return got;
}
void video_shutdown(void) { if (ctx) SDL_GL_DeleteContext(ctx); if (win) SDL_DestroyWindow(win); SDL_Quit(); }
