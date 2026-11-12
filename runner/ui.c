// The runner's own controls: a bar above the game with Back, volume, picture, full screen and a small menu,
// and short messages over the game. Everything is drawn with plain desktop OpenGL after the game's frame;
// the shapes are computed here (no picture files), so the bar is sharp at any window size.
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "runner.h"
#include "ui.h"

int video_paced(void);

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

struct HostGL hg;
UiRect ui_view, ui_bar, ui_strip;
int ui_own_frame;
int ui_game_w = 640, ui_game_h = 960;

#define GL_TEXTURE_2D 0x0DE1
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_QUADS 0x0007
#define GL_RGBA 0x1908
#define GL_ALPHA 0x1906
#define GL_UNSIGNED_BYTE 0x1401
#define GL_LINEAR 0x2601
#define GL_NEAREST 0x2600
#define GL_CLAMP_TO_EDGE 0x812F

int hostgl_load(void) {
    static const char *names[] = {"glViewport", "glScissor", "glEnable", "glDisable", "glBlendFunc", "glBlendEquation", "glMatrixMode", "glLoadIdentity",
        "glOrtho", "glBegin", "glEnd", "glVertex2f", "glTexCoord2f", "glColor4ub", "glBindTexture", "glGenTextures", "glDeleteTextures", "glTexImage2D",
        "glTexSubImage2D", "glTexParameteri", "glTexEnvi", "glPixelStorei", "glPushAttrib", "glPopAttrib", "glPushClientAttrib", "glPopClientAttrib",
        "glClearColor", "glClear", "glGetIntegerv", "glReadPixels", "glColorMask", "glDepthMask", "glUseProgram", "glActiveTexture", "glBindBuffer",
        "glBindFramebuffer", "glGenFramebuffers", "glFramebufferTexture2D", "glGenRenderbuffers", "glBindRenderbuffer", "glRenderbufferStorage",
        "glFramebufferRenderbuffer", "glCheckFramebufferStatus"};
    void **slot = (void **)&hg;
    int ok = 1;
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        slot[i] = SDL_GL_GetProcAddress(names[i]);
        if (!slot[i] && i >= 35) {                                  // older drivers name the framebuffer calls ...EXT
            char ext[64]; SDL_snprintf(ext, sizeof ext, "%sEXT", names[i]);
            slot[i] = SDL_GL_GetProcAddress(ext);
        }
        if (!slot[i] && strcmp(names[i], "glBlendEquation")) { rlog("[video] this graphics driver lacks %s\n", names[i]); ok = 0; }
    }
    return ok;
}

// ---------------------------------------------------------------------------------------------------------
// Settings: a few numbers in <game folder>/desktop.ini, shared with the launcher's settings window.
// ---------------------------------------------------------------------------------------------------------
static struct { char name[32]; int value; } cfg[48];
static int n_cfg, cfg_loaded;
static void cfg_path(char *out, size_t cap) { SDL_snprintf(out, cap, "%s/desktop.ini", g_root); }
static void cfg_load(void) {
    cfg_loaded = 1;
    char p[1100]; cfg_path(p, sizeof p);
    FILE *f = os_fopen(p, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f) && n_cfg < 48) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#' || line[0] == ';') continue;
        *eq = 0;
        char *name = line; while (*name == ' ') name++;
        char *end = eq - 1; while (end > name && (*end == ' ' || *end == '\t')) *end-- = 0;
        SDL_snprintf(cfg[n_cfg].name, sizeof cfg[n_cfg].name, "%s", name);
        cfg[n_cfg++].value = atoi(eq + 1);
    }
    fclose(f);
}
int cfg_int(const char *name, int fallback) {
    if (!cfg_loaded) cfg_load();
    for (int i = 0; i < n_cfg; i++) if (!strcmp(cfg[i].name, name)) return cfg[i].value;
    return fallback;
}
void cfg_set(const char *name, int value) {
    if (!cfg_loaded) cfg_load();
    int i = 0;
    while (i < n_cfg && strcmp(cfg[i].name, name)) i++;
    if (i == n_cfg) { if (n_cfg == 48) return; SDL_snprintf(cfg[n_cfg++].name, sizeof cfg[0].name, "%s", name); }
    else if (cfg[i].value == value) return;
    cfg[i].value = value;
    char p[1100]; cfg_path(p, sizeof p);
    FILE *f = os_fopen(p, "w");
    if (!f) return;
    for (int k = 0; k < n_cfg; k++) fprintf(f, "%s=%d\n", cfg[k].name, cfg[k].value);
    fclose(f);
}

// ---------------------------------------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------------------------------------
#define BAR_SHARE 0.058
#define BAR_LEAST 26                          // pixels: below this the buttons are too small to hit
double ui_bar_share(void) { return BAR_SHARE; }
int ui_bar_height(int picture_w) { int h = (int)floor(BAR_SHARE * picture_w + 0.5); return h < BAR_LEAST ? BAR_LEAST : h; }
static int skins_for;                         // bar height the skins were made for
void ui_layout(int win_w, int win_h) {
    double s = fmin((double)win_w / ui_game_w, (double)win_h / (ui_game_h + BAR_SHARE * ui_game_w));
    if (BAR_SHARE * ui_game_w * s < BAR_LEAST) s = fmin((double)win_w / ui_game_w, (double)(win_h - BAR_LEAST) / ui_game_h);
    if (s <= 0) s = 0.01;
    int vw = (int)floor(ui_game_w * s + 0.5), vh = (int)floor(ui_game_h * s + 0.5);
    if (vw > win_w) vw = win_w;
    int bar = ui_bar_height(vw);
    if (vh + bar > win_h) vh = win_h - bar;
    if (ui_own_frame) {                       // the bar is the window's title bar: at the top, across the whole window
        ui_bar = (UiRect){(win_w - vw) / 2, 0, vw, bar};
        ui_strip = (UiRect){0, 0, win_w, bar};
        ui_view = (UiRect){ui_bar.x, bar + (win_h - vh - bar) / 2, vw, vh};
        return;
    }
    ui_bar = (UiRect){(win_w - vw) / 2, (win_h - vh - bar) / 2, vw, bar};
    ui_strip = ui_bar;
    ui_view = (UiRect){ui_bar.x, ui_bar.y + bar, vw, vh};
}

// ---------------------------------------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------------------------------------
static int tex_on = -1;
static void use_tex(unsigned tex) {
    if (tex) { if (tex_on != 1) { hg.Enable(GL_TEXTURE_2D); tex_on = 1; } hg.BindTexture(GL_TEXTURE_2D, tex); }
    else if (tex_on != 0) { hg.Disable(GL_TEXTURE_2D); tex_on = 0; }
}
static void color(uint32_t c) { hg.Color4ub((unsigned char)(c >> 24), (unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c); }
void ui_fill(float x, float y, float w, float h, uint32_t top, uint32_t bottom) {
    use_tex(0);
    hg.Begin(GL_QUADS);
    color(top); hg.Vertex2f(x, y); hg.Vertex2f(x + w, y);
    color(bottom); hg.Vertex2f(x + w, y + h); hg.Vertex2f(x, y + h);
    hg.End();
}
void ui_image(unsigned tex, float x, float y, float w, float h, float u0, float v0, float u1, float v1, uint32_t rgba) {
    use_tex(tex);
    color(rgba);
    hg.Begin(GL_QUADS);
    hg.TexCoord2f(u0, v0); hg.Vertex2f(x, y);
    hg.TexCoord2f(u1, v0); hg.Vertex2f(x + w, y);
    hg.TexCoord2f(u1, v1); hg.Vertex2f(x + w, y + h);
    hg.TexCoord2f(u0, v1); hg.Vertex2f(x, y + h);
    hg.End();
}
static int draw_win_h;
void ui_clip(float x, float y, float w, float h) { hg.Enable(GL_SCISSOR_TEST); hg.Scissor((int)x, draw_win_h - (int)(y + h), (int)w, (int)h); }
void ui_unclip(void) { hg.Disable(GL_SCISSOR_TEST); }
unsigned ui_texture(const void *pixels, int w, int h, unsigned format, int smooth) {
    unsigned t = 0;
    hg.GenTextures(1, &t);
    hg.BindTexture(GL_TEXTURE_2D, t);
    hg.PixelStorei(0x0CF5, 1);                                      // rows are not padded
    hg.TexParameteri(GL_TEXTURE_2D, 0x2801, smooth ? GL_LINEAR : GL_NEAREST);
    hg.TexParameteri(GL_TEXTURE_2D, 0x2800, smooth ? GL_LINEAR : GL_NEAREST);
    hg.TexParameteri(GL_TEXTURE_2D, 0x2802, GL_CLAMP_TO_EDGE);
    hg.TexParameteri(GL_TEXTURE_2D, 0x2803, GL_CLAMP_TO_EDGE);
    hg.TexImage2D(GL_TEXTURE_2D, 0, (int)format, w, h, 0, format, GL_UNSIGNED_BYTE, pixels);
    tex_on = -1;
    return t;
}

// ---- text ----------------------------------------------------------------------------------------------
static unsigned char *font_data;
static stbtt_bakedchar glyphs[96];
static unsigned font_tex;
static float font_px;
#define ATLAS_W 1024
#define ATLAS_H 512
static void font_bake(float px) {
    if (!font_data) {
        char p[1100]; SDL_snprintf(p, sizeof p, "%s/font.ttf", g_root);
        FILE *f = os_fopen(p, "rb");
        if (!f) { rlog("[video] %s is missing: the control bar has no text\n", p); return; }
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        font_data = malloc((size_t)n);
        if (fread(font_data, 1, (size_t)n, f) != (size_t)n) { free(font_data); font_data = NULL; }
        fclose(f);
        if (!font_data) return;
    }
    unsigned char *bitmap = calloc(ATLAS_W, ATLAS_H);
    stbtt_BakeFontBitmap(font_data, 0, px, bitmap, ATLAS_W, ATLAS_H, 32, 95, glyphs);
    if (font_tex) hg.DeleteTextures(1, &font_tex);
    font_tex = ui_texture(bitmap, ATLAS_W, ATLAS_H, GL_ALPHA, 1);
    free(bitmap);
    font_px = px;
}
float ui_text_width(float px, const char *s) {
    if (!font_tex) return 0;
    float w = 0, k = px / font_px;
    for (; *s; s++) if (*s >= 32 && *s < 127) w += glyphs[*s - 32].xadvance * k;
    return w;
}
float ui_text(float x, float y, float px, uint32_t rgba, const char *s) {
    if (!font_tex) return 0;
    float k = px / font_px, pen = 0, base = y + px * 0.80f;
    use_tex(font_tex);
    color(rgba);
    hg.Begin(GL_QUADS);
    for (; *s; s++) {
        if (*s < 32 || *s >= 127) continue;
        const stbtt_bakedchar *g = &glyphs[*s - 32];
        float x0 = x + (pen + g->xoff) * k, y0 = base + g->yoff * k, x1 = x0 + (g->x1 - g->x0) * k, y1 = y0 + (g->y1 - g->y0) * k;
        if (k == 1.0f) { float dx = floorf(x0 + 0.5f) - x0, dy = floorf(y0 + 0.5f) - y0; x0 += dx; x1 += dx; y0 += dy; y1 += dy; }
        float u0 = g->x0 / (float)ATLAS_W, v0 = g->y0 / (float)ATLAS_H, u1 = g->x1 / (float)ATLAS_W, v1 = g->y1 / (float)ATLAS_H;
        hg.TexCoord2f(u0, v0); hg.Vertex2f(x0, y0);
        hg.TexCoord2f(u1, v0); hg.Vertex2f(x1, y0);
        hg.TexCoord2f(u1, v1); hg.Vertex2f(x1, y1);
        hg.TexCoord2f(u0, v1); hg.Vertex2f(x0, y1);
        pen += g->xadvance;
    }
    hg.End();
    return pen * k;
}
static void text_shadowed(float x, float y, float px, uint32_t rgba, const char *s) {
    float d = px > 20 ? 1.5f : 1.0f;
    ui_text(x, y + d, px, 0x00000000u | ((rgba & 0xFF) * 3 / 4), s);
    ui_text(x, y, px, rgba, s);
}

// ---- shapes computed into small pictures -----------------------------------------------------------------
static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static float sd_box(float px, float py, float hw, float hh, float r) {
    float qx = fabsf(px) - hw + r, qy = fabsf(py) - hh + r, ax = qx > 0 ? qx : 0, ay = qy > 0 ? qy : 0;
    return sqrtf(ax * ax + ay * ay) + fminf(fmaxf(qx, qy), 0) - r;
}
static float sd_seg(float px, float py, float ax, float ay, float bx, float by) {
    float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay, t = clamp01((wx * vx + wy * vy) / (vx * vx + vy * vy));
    float dx = wx - vx * t, dy = wy - vy * t;
    return sqrtf(dx * dx + dy * dy);
}
static float sd_arc(float px, float py, float r, float half_angle) {      // an arc opening to the right, centred on the origin
    float a = atan2f(py, px);
    if (fabsf(a) <= half_angle) return fabsf(sqrtf(px * px + py * py) - r);
    float ex = r * cosf(half_angle), ey = r * sinf(half_angle);
    return sqrtf((px - ex) * (px - ex) + (fabsf(py) - ey) * (fabsf(py) - ey));
}
enum { IC_BACK, IC_SPEAKER, IC_MUTED, IC_CAMERA, IC_FULL, IC_WINDOWED, IC_GEAR, IC_MIN, IC_MAX, IC_RESTORE, IC_CLOSE, IC_COUNT };
// Signed distance to the icon's shape; x and y run from -1 to 1 across the button, y downwards.
static float icon_sdf(int id, float x, float y) {
    switch (id) {
    case IC_BACK: return fminf(sd_seg(x, y, 0.30f, -0.58f, -0.34f, 0), sd_seg(x, y, -0.34f, 0, 0.30f, 0.58f)) - 0.13f;
    case IC_SPEAKER: case IC_MUTED: {
        float body = sd_box(x + 0.62f, y, 0.16f, 0.24f, 0.04f);
        float cone = fmaxf(fmaxf(-(x + 0.52f), x + 0.08f), fabsf(y) - (0.24f + (x + 0.52f) * 0.86f));   // widening towards the right
        float d = fminf(body, cone);
        if (id == IC_SPEAKER) { d = fminf(d, sd_arc(x + 0.08f, y, 0.34f, 0.80f) - 0.075f); d = fminf(d, sd_arc(x + 0.08f, y, 0.66f, 0.80f) - 0.075f); }
        else d = fminf(d, fminf(sd_seg(x, y, 0.22f, -0.30f, 0.80f, 0.30f), sd_seg(x, y, 0.22f, 0.30f, 0.80f, -0.30f)) - 0.085f);
        return d;
    }
    case IC_CAMERA: {
        float body = fminf(sd_box(x, y - 0.10f, 0.78f, 0.50f, 0.14f), sd_box(x, y + 0.46f, 0.30f, 0.16f, 0.06f));
        float r = sqrtf(x * x + (y - 0.10f) * (y - 0.10f));
        float d = fmaxf(body, -(r - 0.34f));                       // the lens is a hole ...
        return fminf(d, r - 0.19f);                                // ... with a dot in it
    }
    case IC_FULL: case IC_WINDOWED: {
        float d = 9, a = 0.72f, b = id == IC_FULL ? 0.30f : 0.72f, c = id == IC_FULL ? 0.72f : 0.30f;
        for (int sx = -1; sx <= 1; sx += 2) for (int sy = -1; sy <= 1; sy += 2) {
            if (id == IC_FULL) { d = fminf(d, sd_seg(x, y, sx * a, sy * a, sx * b, sy * a)); d = fminf(d, sd_seg(x, y, sx * a, sy * a, sx * a, sy * b)); }
            else { d = fminf(d, sd_seg(x, y, sx * c, sy * c, sx * b, sy * c)); d = fminf(d, sd_seg(x, y, sx * c, sy * c, sx * c, sy * b)); }
        }
        return d - 0.10f;
    }
    case IC_GEAR: {
        float r = sqrtf(x * x + y * y), d = fabsf(r - 0.46f) - 0.17f;
        for (int k = 0; k < 8; k++) { float a = (float)k * 0.7853982f, cx = cosf(a), cy = sinf(a); d = fminf(d, sd_seg(x, y, cx * 0.56f, cy * 0.56f, cx * 0.80f, cy * 0.80f) - 0.115f); }
        return d;
    }
    case IC_MIN: return sd_seg(x, y, -0.56f, 0.52f, 0.56f, 0.52f) - 0.105f;
    case IC_MAX: return fabsf(sd_box(x, y, 0.60f, 0.60f, 0.10f)) - 0.10f;
    case IC_RESTORE: {
        float front = sd_box(x + 0.14f, y - 0.14f, 0.50f, 0.50f, 0.08f), back = sd_box(x - 0.16f, y + 0.16f, 0.50f, 0.50f, 0.08f);
        return fminf(fabsf(front) - 0.095f, fmaxf(fabsf(back) - 0.095f, -(front - 0.22f)));     // the one behind shows only beside the one in front
    }
    case IC_CLOSE: return fminf(sd_seg(x, y, -0.52f, -0.52f, 0.52f, 0.52f), sd_seg(x, y, -0.52f, 0.52f, 0.52f, -0.52f)) - 0.115f;
    }
    return 1;
}
static uint32_t mix(uint32_t a, uint32_t b, float t) {
    uint32_t out = 0;
    for (int s = 24; s >= 0; s -= 8) { float x = (float)((a >> s) & 255), y = (float)((b >> s) & 255); out |= (uint32_t)(x + (y - x) * t + 0.5f) << s; }
    return out;
}
static void put(unsigned char *px, uint32_t rgba, float alpha) { px[0] = (unsigned char)(rgba >> 24); px[1] = (unsigned char)(rgba >> 16); px[2] = (unsigned char)(rgba >> 8); px[3] = (unsigned char)((rgba & 255) * clamp01(alpha) + 0.5f); }
// A rounded panel: gradient fill, a light rim, and a soft highlight across the top like the game's own buttons.
static unsigned make_panel(int w, int h, float radius, float rim, uint32_t top, uint32_t bottom, uint32_t edge, float gloss) {
    unsigned char *px = malloc((size_t)w * h * 4);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        float d = sd_box(x + 0.5f - w * 0.5f, y + 0.5f - h * 0.5f, w * 0.5f, h * 0.5f, radius);
        float outer = clamp01(0.5f - d), inner = clamp01(0.5f - (d + rim));
        uint32_t c = mix(top, bottom, (float)y / (float)(h > 1 ? h - 1 : 1));
        if (gloss > 0 && y < h * 0.48f) c = mix(c, 0xFFFFFFFFu, gloss * (1.0f - (float)y / (h * 0.48f)) * 0.9f);
        c = mix(edge, c, inner);
        put(px + ((size_t)y * w + x) * 4, c, outer);
    }
    unsigned t = ui_texture(px, w, h, GL_RGBA, 1);
    free(px);
    return t;
}
static unsigned make_icon(int id, int size) {
    unsigned char *px = malloc((size_t)size * size * 4);
    float half = size * 0.5f * 0.62f;                               // the icon fills 62% of the button
    for (int y = 0; y < size; y++) for (int x = 0; x < size; x++) {
        float d = icon_sdf(id, (x + 0.5f - size * 0.5f) / half, (y + 0.5f - size * 0.5f) / half) * half;
        put(px + ((size_t)y * size + x) * 4, 0xFFFFFFFFu, 0.5f - d);
    }
    unsigned t = ui_texture(px, size, size, GL_RGBA, 1);
    free(px);
    return t;
}
static unsigned make_dot(int size) {
    unsigned char *px = malloc((size_t)size * size * 4);
    for (int y = 0; y < size; y++) for (int x = 0; x < size; x++) {
        float dx = x + 0.5f - size * 0.5f, dy = y + 0.5f - size * 0.5f;
        put(px + ((size_t)y * size + x) * 4, 0xFFFFFFFFu, size * 0.5f - sqrtf(dx * dx + dy * dy));
    }
    unsigned t = ui_texture(px, size, size, GL_RGBA, 1);
    free(px);
    return t;
}
static unsigned t_bar, t_button, t_button_hot, t_button_close, t_icons[IC_COUNT], t_dot, t_panel;
static int panel_w, panel_h, button_px;
static void pill(float x, float y, float w, float h, uint32_t rgba) {             // a bar with round ends
    if (w < h) w = h;
    ui_image(t_dot, x, y, h * 0.5f, h, 0, 0, 0.5f, 1, rgba);
    ui_image(t_dot, x + w - h * 0.5f, y, h * 0.5f, h, 0.5f, 0, 1, 1, rgba);
    if (w > h) { ui_image(t_dot, x + h * 0.5f, y, w - h, h, 0.5f, 0, 0.5f, 1, rgba); }
}
static void skins_build(void) {
    int H = ui_bar.h, W = ui_bar.w;
    unsigned old[] = {t_bar, t_button, t_button_hot, t_button_close, t_dot, t_panel};
    for (size_t i = 0; i < sizeof old / sizeof *old; i++) if (old[i]) hg.DeleteTextures(1, &old[i]);
    for (int i = 0; i < IC_COUNT; i++) if (t_icons[i]) hg.DeleteTextures(1, &t_icons[i]);
    t_panel = 0;
    // The bar: deep blue with a bright line under it, the look of the game's own header.
    unsigned char *px = malloc((size_t)H * 4 * 4);
    for (int y = 0; y < H; y++) {
        float t = (float)y / (float)(H - 1);
        uint32_t c = mix(0x1B2F7CFFu, 0x070D33FFu, t);
        if (y < H * 0.45f) c = mix(c, 0x6FA0FFFFu, 0.16f * (1 - t / 0.45f));
        if (y >= H - 1) c = 0x02040FFFu;
        else if (y >= H - 2 - (H > 44)) c = 0xC8B36AFFu;             // thin gold rule
        else if (y >= H - 3 - (H > 44)) c = 0x0A1238FFu;
        for (int x = 0; x < 4; x++) put(px + ((size_t)y * 4 + x) * 4, c, 1);
    }
    t_bar = ui_texture(px, 4, H, GL_RGBA, 1);
    free(px);
    (void)W;
    button_px = H - 2 * (int)floorf(H * 0.14f + 0.5f) - (H > 44 ? 2 : 1);
    float r = button_px * 0.24f;
    t_button = make_panel(button_px, button_px, r, 1.2f, 0x2E5CC8FFu, 0x0B1D66FFu, 0xB9D4FFFFu, 0.22f);
    t_button_hot = make_panel(button_px, button_px, r, 1.4f, 0x4F86F0FFu, 0x153094FFu, 0xFFF1BFFFu, 0.34f);
    t_button_close = make_panel(button_px, button_px, r, 1.4f, 0xE2604CFFu, 0x7E1420FFu, 0xFFF1BFFFu, 0.30f);
    for (int i = 0; i < IC_COUNT; i++) t_icons[i] = make_icon(i, button_px);
    t_dot = make_dot(64);
    font_bake(fmaxf(12, floorf(H * 0.46f + 0.5f)));
    skins_for = H;
}

// ---------------------------------------------------------------------------------------------------------
// The bar, its menu and the messages
// ---------------------------------------------------------------------------------------------------------
enum { B_BACK, B_SPEAKER, B_CAMERA, B_FULL, B_GEAR, B_MIN, B_MAX, B_CLOSE, B_COUNT };
static UiRect buttons[B_COUNT], slider, menu;
static int mouse_x = -1, mouse_y = -1, hot = -1, pressed = -1, dragging_volume, menu_open, menu_hot = -1;
static float fps_now; static unsigned long fps_frames; static uint64_t fps_since;
static char toast[160]; static uint64_t toast_until;
enum { M_FPS, M_SMOOTH, M_VSYNC, M_BGSOUND, M_SIZE, M_FULL, M_FOLDER, M_ROWS };
static const char *menu_labels[M_ROWS] = {"Show frame rate", "Smooth picture", "Match the screen's refresh", "Sound while in the background", "Window size", "Full screen", "Open pictures folder"};

void ui_toast(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    SDL_vsnprintf(toast, sizeof toast, fmt, ap);
    va_end(ap);
    toast_until = now_ns() + 1700000000ULL;
}
void ui_frame_shown(void) {
    uint64_t now = now_ns();
    fps_frames++;
    if (!fps_since) fps_since = now;
    if (now - fps_since >= 1000000000ULL) { fps_now = (float)(fps_frames * 1e9 / (double)(now - fps_since)); fps_frames = 0; fps_since = now; }
}
static int inside(const UiRect *r, int x, int y) { return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h; }
static void bar_layout(void) {
    int H = ui_bar.h, m = (int)floorf(H * 0.14f + 0.5f), b = button_px, gap = (int)floorf(H * 0.16f + 0.5f);
    int y = ui_bar.y + (H - (H > 44 ? 3 : 2) - b) / 2, right = ui_bar.x + ui_bar.w - m;
    buttons[B_MIN] = buttons[B_MAX] = buttons[B_CLOSE] = (UiRect){0, 0, 0, 0};
    if (ui_own_frame) {                       // the window's own buttons, at the window's right edge
        int wx = ui_strip.x + ui_strip.w - m - b;
        buttons[B_CLOSE] = (UiRect){wx, y, b, b}; wx -= b + gap;
        buttons[B_MAX] = (UiRect){wx, y, b, b}; wx -= b + gap;
        buttons[B_MIN] = (UiRect){wx, y, b, b};
        if (right > wx - gap * 2) right = wx - gap * 2;
    }
    int x = right - b;
    buttons[B_BACK] = (UiRect){ui_bar.x + m, y, b, b};
    buttons[B_GEAR] = (UiRect){x, y, b, b}; x -= b + gap;
    buttons[B_FULL] = (UiRect){x, y, b, b}; x -= b + gap;
    buttons[B_CAMERA] = (UiRect){x, y, b, b}; x -= gap * 2;
    int pct = (int)ui_text_width(H * 0.40f, "100%") + gap / 2;
    int sw = (int)(ui_bar.w * 0.26f); if (sw < 70) sw = 70; if (sw > 300) sw = 300;
    x -= pct + sw;
    slider = (UiRect){x, ui_bar.y, sw, H - 2};
    buttons[B_SPEAKER] = (UiRect){x - gap - b, y, b, b};
    float fs = H * 0.40f, rh = H * 0.98f;
    int pw = (int)fmaxf(fminf(ui_bar.w - 2.0f * m, fs * 21.0f), ui_bar.w * 0.5f), ph = (int)(rh * M_ROWS + fs * 2.2f + m * 2);
    menu = (UiRect){ui_bar.x + ui_bar.w - m - pw, ui_bar.y + H + m / 2, pw, ph};
    if (t_panel && (panel_w != pw || panel_h != ph)) { hg.DeleteTextures(1, &t_panel); t_panel = 0; }
}
static void set_volume_from(int x) {
    float knob = slider.h * 0.36f, t = clamp01((x - slider.x - knob) / (slider.w - 2 * knob));
    int v = (int)(t * 100 + 0.5f);
    audio_set_master(v);
    if (v > 0 && audio_muted()) audio_set_muted(0);
}
static void draw_switch(float x, float y, float h, int on) {
    float w = h * 1.9f;
    pill(x - 1, y - 1, w + 2, h + 2, 0xB9D4FFB0u);
    pill(x, y, w, h, on ? 0xE2B84BFFu : 0x141C4AFFu);
    float k = h * 0.78f, kx = on ? x + w - k - (h - k) * 0.5f : x + (h - k) * 0.5f;
    ui_image(t_dot, kx, y + (h - k) * 0.5f, k, k, 0, 0, 1, 1, 0xFFFFFFFFu);
}
static void draw_button(int id, int icon) {
    const UiRect *r = &buttons[id];
    int lit = hot == id || pressed == id;
    ui_image(lit ? (id == B_CLOSE ? t_button_close : t_button_hot) : t_button, (float)r->x, (float)r->y + (pressed == id ? 1 : 0), (float)r->w, (float)r->h, 0, 0, 1, 1, 0xFFFFFFFFu);
    ui_image(t_icons[icon], (float)r->x, (float)r->y + 1 + (pressed == id ? 1 : 0), (float)r->w, (float)r->h, 0, 0, 1, 1, 0x00000070u);
    ui_image(t_icons[icon], (float)r->x, (float)r->y + (pressed == id ? 1 : 0), (float)r->w, (float)r->h, 0, 0, 1, 1, 0xFFFFFFFFu);
}
static void draw_bar(void) {
    int H = ui_bar.h;
    ui_image(t_bar, (float)ui_strip.x, (float)ui_strip.y, (float)ui_strip.w, (float)H, 0, 0, 1, 1, 0xFFFFFFFFu);
    if (hot >= 0) {                           // over the part of the bar that moves the window no pointer events arrive: look
        int px, py;
        if (!video_mouse(&px, &py) || !inside(&buttons[hot], px, py)) hot = -1;
    }
    if (ui_own_frame) {
        draw_button(B_MIN, IC_MIN);
        draw_button(B_MAX, video_is_maximized() || video_is_fullscreen() ? IC_RESTORE : IC_MAX);
        draw_button(B_CLOSE, IC_CLOSE);
    }
    draw_button(B_BACK, IC_BACK);
    int muted = audio_muted(), vol = audio_master();
    draw_button(B_SPEAKER, muted || vol == 0 ? IC_MUTED : IC_SPEAKER);
    draw_button(B_CAMERA, IC_CAMERA);
    draw_button(B_FULL, video_is_fullscreen() ? IC_WINDOWED : IC_FULL);
    draw_button(B_GEAR, IC_GEAR);
    // volume: a groove, the filled part in gold, a round knob
    float th = fmaxf(4, floorf(H * 0.13f)), knob = slider.h * 0.36f, cy = ui_bar.y + (H - 2) * 0.5f;
    float x0 = slider.x + knob, x1 = slider.x + slider.w - knob, kx = x0 + (x1 - x0) * vol / 100.0f;
    pill(x0 - th * 0.5f - 1, cy - th * 0.5f - 1, x1 - x0 + th + 2, th + 2, 0xB9D4FF90u);
    pill(x0 - th * 0.5f, cy - th * 0.5f, x1 - x0 + th, th, 0x050A26FFu);
    if (vol > 0) pill(x0 - th * 0.5f, cy - th * 0.5f, kx - x0 + th, th, muted ? 0x7C86A8FFu : 0xE8C46AFFu);
    ui_image(t_dot, kx - knob - 1, cy - knob - 1 + 1, knob * 2 + 2, knob * 2 + 2, 0, 0, 1, 1, 0x00000080u);
    ui_image(t_dot, kx - knob, cy - knob, knob * 2, knob * 2, 0, 0, 1, 1, dragging_volume ? 0xFFF1BFFFu : 0xFFFFFFFFu);
    char t[32]; SDL_snprintf(t, sizeof t, muted ? "Off" : "%d%%", vol);
    float fs = H * 0.40f;
    text_shadowed(slider.x + slider.w + H * 0.06f, ui_bar.y + (H - 2 - fs) * 0.5f - fs * 0.06f, fs, 0xFFFFFFFFu, t);
    if (cfg_int("fps", 0)) {
        SDL_snprintf(t, sizeof t, "%.0f fps", fps_now);
        float x = buttons[B_BACK].x + buttons[B_BACK].w + H * 0.25f;
        if (x + ui_text_width(fs, t) < buttons[B_SPEAKER].x - 4) text_shadowed(x, ui_bar.y + (H - 2 - fs) * 0.5f - fs * 0.06f, fs, 0xFFF1BFFFu, t);
    }
}
static void menu_row_rect(int row, UiRect *out) {
    float rh = ui_bar.h * 0.98f; int m = (int)floorf(ui_bar.h * 0.14f + 0.5f);
    *out = (UiRect){menu.x + m, menu.y + m + (int)(rh * row), menu.w - 2 * m, (int)rh};
}
static int menu_value(int row) {
    switch (row) {
    case M_FPS: return cfg_int("fps", 0);
    case M_SMOOTH: return cfg_int("smooth", 1);
    case M_VSYNC: return cfg_int("vsync", 0);
    case M_BGSOUND: return cfg_int("background_sound", 1);
    case M_FULL: return video_is_fullscreen();
    }
    return 0;
}
static void draw_menu(void) {
    if (!t_panel) { panel_w = menu.w; panel_h = menu.h; t_panel = make_panel(menu.w, menu.h, ui_bar.h * 0.22f, 1.5f, 0x16286EF4u, 0x060B2CF4u, 0xB9D4FFFFu, 0.05f); }
    ui_fill((float)menu.x + 3, (float)menu.y + menu.h - 2, (float)menu.w - 6, 5, 0x00000050u, 0x00000000u);
    ui_image(t_panel, (float)menu.x, (float)menu.y, (float)menu.w, (float)menu.h, 0, 0, 1, 1, 0xFFFFFFFFu);
    float fs = ui_bar.h * 0.40f, sh = ui_bar.h * 0.46f;
    for (int i = 0; i < M_ROWS; i++) {
        UiRect r; menu_row_rect(i, &r);
        if (menu_hot == i) ui_fill((float)r.x, (float)r.y + 1, (float)r.w, (float)r.h - 2, 0xFFFFFF22u, 0xFFFFFF12u);
        if (i) ui_fill((float)r.x, (float)r.y, (float)r.w, 1, 0xB9D4FF30u, 0xB9D4FF30u);
        text_shadowed(r.x + fs * 0.5f, r.y + (r.h - fs) * 0.5f - fs * 0.06f, fs, 0xFFFFFFFFu, menu_labels[i]);
        float right = (float)(r.x + r.w) - fs * 0.5f;
        if (i == M_SIZE) {
            static const char *names[] = {"S", "M", "L"};
            int now = cfg_int("size", 2);
            float bw = sh * 1.5f;
            for (int k = 2; k >= 0; k--) {
                float bx = right - bw * (3 - k) - (2 - k) * fs * 0.3f, by = r.y + (r.h - sh) * 0.5f;
                pill(bx - 1, by - 1, bw + 2, sh + 2, 0xB9D4FFB0u);
                pill(bx, by, bw, sh, k == now && !video_is_fullscreen() ? 0xE2B84BFFu : 0x141C4AFFu);
                ui_text(bx + (bw - ui_text_width(fs * 0.9f, names[k])) * 0.5f, by + (sh - fs * 0.9f) * 0.5f - fs * 0.05f, fs * 0.9f, k == now && !video_is_fullscreen() ? 0x1A1030FFu : 0xFFFFFFFFu, names[k]);
            }
        } else if (i == M_FOLDER) {
            ui_text(right - ui_text_width(fs, ">"), r.y + (r.h - fs) * 0.5f - fs * 0.06f, fs, 0xE8C46AFFu, ">");
        } else draw_switch(right - sh * 1.9f, r.y + (r.h - sh) * 0.5f, sh, menu_value(i));
    }
    UiRect last; menu_row_rect(M_ROWS, &last);
    ui_fill((float)last.x, (float)last.y, (float)last.w, 1, 0xB9D4FF30u, 0xB9D4FF30u);
    const char *keys = "Esc  Back     F9 / F10  Volume     F11  Full screen     F12  Picture";
    float ks = fs * 0.78f, kw = ui_text_width(ks, keys);
    if (kw > last.w - fs) ks *= (last.w - fs) / kw;
    ui_text(last.x + fs * 0.5f, last.y + fs * 0.55f, ks, 0xB4ABC9FFu, keys);
}
static void draw_toast(void) {
    uint64_t now = now_ns();
    if (!toast[0] || now >= toast_until) return;
    float left = (float)(toast_until - now) / 1e9f, a = left < 0.3f ? left / 0.3f : 1.0f;
    float fs = ui_bar.h * 0.44f, w = ui_text_width(fs, toast) + fs * 2.2f, h = fs * 2.0f;
    float x = ui_view.x + (ui_view.w - w) * 0.5f, y = ui_view.y + ui_view.h - h * 2.2f;
    uint32_t al = (uint32_t)(a * 255);
    pill(x - 1.5f, y - 1.5f, w + 3, h + 3, 0xB9D4FF00u | (al * 3 / 4));
    pill(x, y, w, h, 0x0A123800u | (al * 15 / 16));
    ui_text(x + fs * 1.1f, y + (h - fs) * 0.5f - fs * 0.06f, fs, 0xFFFFFF00u | al, toast);
}

void ui_init(int game_w, int game_h) { ui_game_w = game_w; ui_game_h = game_h; }

void ui_draw(int win_w, int win_h, unsigned game_texture) {
    draw_win_h = win_h;
    tex_on = -1;
    if (skins_for != ui_bar.h) skins_build();
    bar_layout();
    hg.Viewport(0, 0, win_w, win_h);
    hg.MatrixMode(0x1701); hg.LoadIdentity(); hg.Ortho(0, win_w, win_h, 0, -1, 1);
    hg.MatrixMode(0x1700); hg.LoadIdentity();
    hg.TexEnvi(0x2300, 0x2200, 0x2100);                              // texture colour x drawing colour
    hg.ClearColor(0.008f, 0.014f, 0.055f, 1);
    hg.Clear(0x4000);
    hg.Disable(GL_BLEND);
    ui_image(game_texture, (float)ui_view.x, (float)ui_view.y, (float)ui_view.w, (float)ui_view.h, 0, 1, 1, 0, 0xFFFFFFFFu);
    hg.Enable(GL_BLEND);
    if (hg.BlendEquation) hg.BlendEquation(0x8006);
    hg.BlendFunc(0x0302, 0x0303);
    web_draw();
    draw_bar();
    if (menu_open) draw_menu();
    draw_toast();
    if (ui_own_frame && !video_is_fullscreen() && !video_is_maximized()) {       // a thin edge in place of the system's frame
        float w = (float)win_w, h = (float)win_h, top = (float)ui_strip.h;
        ui_fill(0, 0, w, 1, 0x7FA4F0FFu, 0x7FA4F0FFu);
        ui_fill(0, top, 1, h - top, 0x33489CFFu, 0x1B2866FFu);
        ui_fill(w - 1, top, 1, h - top, 0x33489CFFu, 0x1B2866FFu);
        ui_fill(0, 0, 1, top, 0x7FA4F0FFu, 0x33489CFFu);
        ui_fill(w - 1, 0, 1, top, 0x7FA4F0FFu, 0x33489CFFu);
        ui_fill(0, h - 1, w, 1, 0x1B2866FFu, 0x1B2866FFu);
    }
    use_tex(0);
}

int ui_draggable(int x, int y) {
    if (!ui_own_frame || !inside(&ui_strip, x, y)) return 0;
    if (menu_open && inside(&menu, x, y)) return 0;
    for (int i = 0; i < B_COUNT; i++) {
        UiRect r = {buttons[i].x - 2, buttons[i].y - 2, buttons[i].w + 4, buttons[i].h + 4};
        if (buttons[i].w && inside(&r, x, y)) return 0;
    }
    UiRect grab = {slider.x - 6, slider.y, slider.w + 12, slider.h};
    return !inside(&grab, x, y);
}

static void click_button(int id) {
    switch (id) {
    case B_BACK: video_press_back(); break;
    case B_SPEAKER: audio_set_muted(!audio_muted()); cfg_set("muted", audio_muted()); ui_toast(audio_muted() ? "Sound off" : "Sound on"); break;
    case B_CAMERA: video_take_picture(); break;
    case B_FULL: video_toggle_fullscreen(); break;
    case B_GEAR: menu_open = !menu_open; menu_hot = -1; break;
    case B_MIN: hot = -1; video_minimize(); break;
    case B_MAX: video_toggle_maximize(); break;
    case B_CLOSE: video_close(); break;
    }
}
static void click_menu(int row, int x) {
    switch (row) {
    case M_FPS: cfg_set("fps", !cfg_int("fps", 0)); break;
    case M_SMOOTH: cfg_set("smooth", !cfg_int("smooth", 1)); video_set_smooth(cfg_int("smooth", 1)); break;
    case M_VSYNC:
        cfg_set("vsync", !cfg_int("vsync", 0)); video_set_vsync(cfg_int("vsync", 0));
        if (cfg_int("vsync", 0) && !video_paced()) ui_toast("This screen is not at 60 Hz: the game keeps its own timing");
        break;
    case M_BGSOUND: cfg_set("background_sound", !cfg_int("background_sound", 1)); break;
    case M_FULL: video_toggle_fullscreen(); break;
    case M_FOLDER: video_open_folder(video_pictures_folder()); menu_open = 0; break;
    case M_SIZE: {
        UiRect r; menu_row_rect(row, &r);
        float fs = ui_bar.h * 0.40f, sh = ui_bar.h * 0.46f, bw = sh * 1.5f, right = (float)(r.x + r.w) - fs * 0.5f;
        for (int k = 0; k < 3; k++) {
            float bx = right - bw * (3 - k) - (2 - k) * fs * 0.3f;
            if (x >= bx - fs * 0.15f && x < bx + bw + fs * 0.15f) { video_set_window_size(k); }
        }
        break;
    }
    }
}
static int focus_muted;
int ui_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_MOUSEMOTION: {
        mouse_x = e->motion.x; mouse_y = e->motion.y;
        if (dragging_volume) { set_volume_from(mouse_x); return 1; }
        hot = -1; menu_hot = -1;
        for (int i = 0; i < B_COUNT; i++) if (inside(&buttons[i], mouse_x, mouse_y)) hot = i;
        if (menu_open) for (int i = 0; i < M_ROWS; i++) { UiRect r; menu_row_rect(i, &r); if (inside(&r, mouse_x, mouse_y)) menu_hot = i; }
        if (web_event(e, mouse_x, mouse_y)) return 1;
        return inside(&ui_strip, mouse_x, mouse_y) || (menu_open && inside(&menu, mouse_x, mouse_y));
    }
    case SDL_MOUSEBUTTONDOWN: {
        if (e->button.button != SDL_BUTTON_LEFT) return 0;
        int x = e->button.x, y = e->button.y;
        mouse_x = x; mouse_y = y;
        if (menu_open) {
            if (inside(&menu, x, y)) { for (int i = 0; i < M_ROWS; i++) { UiRect r; menu_row_rect(i, &r); if (inside(&r, x, y)) click_menu(i, x); } return 1; }
            if (!inside(&buttons[B_GEAR], x, y)) { menu_open = 0; return 1; }     // a click elsewhere only closes the menu
        }
        if (inside(&ui_strip, x, y)) {
            for (int i = 0; i < B_COUNT; i++) if (inside(&buttons[i], x, y)) { pressed = i; return 1; }
            UiRect grab = {slider.x - 4, slider.y, slider.w + 8, slider.h};
            if (inside(&grab, x, y)) { dragging_volume = 1; set_volume_from(x); }
            return 1;
        }
        return web_event(e, x, y);
    }
    case SDL_MOUSEBUTTONUP: {
        if (e->button.button != SDL_BUTTON_LEFT) return 0;
        if (dragging_volume) { dragging_volume = 0; cfg_set("volume", audio_master()); return 1; }
        if (pressed >= 0) { int id = pressed; pressed = -1; if (inside(&buttons[id], e->button.x, e->button.y)) click_button(id); return 1; }
        return web_event(e, e->button.x, e->button.y);
    }
    case SDL_MOUSEWHEEL:
        return web_event(e, mouse_x, mouse_y);
    case SDL_KEYDOWN: {
        int sym = e->key.keysym.sym;
        if (sym == SDLK_F9 || sym == SDLK_F10) {
            int v = audio_master() + (sym == SDLK_F9 ? -5 : 5);
            audio_set_master(v); cfg_set("volume", audio_master());
            if (audio_muted() && sym == SDLK_F10) audio_set_muted(0);
            ui_toast("Volume %d%%", audio_master());
            return 1;
        }
        if (sym == SDLK_F11 || (sym == SDLK_RETURN && (e->key.keysym.mod & KMOD_ALT))) { if (!e->key.repeat) video_toggle_fullscreen(); return 1; }
        if (sym == SDLK_F12) { if (!e->key.repeat) video_take_picture(); return 1; }
        if (sym == SDLK_ESCAPE && menu_open) { menu_open = 0; return 1; }
        return web_event(e, mouse_x, mouse_y);
    }
    case SDL_KEYUP: {
        int sym = e->key.keysym.sym;
        return sym == SDLK_F9 || sym == SDLK_F10 || sym == SDLK_F11 || sym == SDLK_F12;
    }
    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_LEAVE) { hot = -1; menu_hot = -1; }
        if (e->window.event == SDL_WINDOWEVENT_FOCUS_LOST && !cfg_int("background_sound", 1) && !audio_muted()) { focus_muted = 1; audio_set_muted(1); }
        if (e->window.event == SDL_WINDOWEVENT_FOCUS_GAINED && focus_muted) { focus_muted = 0; audio_set_muted(0); }
        return 0;
    }
    return 0;
}
