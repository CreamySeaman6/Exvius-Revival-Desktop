// In-game web pages: the News notices, the Drop Rates pages and the reward lists.
// On a phone the game opens these in a web view laid over part of its screen. Here the local server draws the
// page as one tall picture (app/webpage.py, asked for at /__desktop/page) and this file shows it in the same
// rectangle: it scrolls with the wheel or by dragging, and its links can be clicked.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "runner.h"
#include "ui.h"

#define STRIP 1024                     // the picture is kept as textures of at most this many rows
#define MAXSTRIPS 256
#define MAXLINKS 2048
typedef struct { int x, y, w, h; char *url; } Link;
typedef struct {                       // one finished download, handed from the worker thread to the drawing thread
    int serial, ok, w, h, n_links;
    uint8_t *pixels, *owner;           // pixels points into owner (the server's whole answer), which is what gets freed
    Link *links; char message[200];
} Loaded;

static struct {
    int state;                         // 0 nothing on screen, 1 loading, 2 showing, 3 could not be loaded
    int visible, serial;
    int gx, gy, gw, gh;                // where the game put the page, in the game's own pixels (from the top left)
    char url[2048], post[2048];
    int page_w, page_h, n_strips, uploaded_rows, asked_w;
    unsigned strips[MAXSTRIPS];
    uint8_t *pixels, *owner;           // rows still to be handed to OpenGL, and the block they live in
    Link *links; int n_links;
    float scroll, velocity, keep_fraction;
    uint64_t seen_ns;                  // when the page was last on the screen
    int dragging, moved, press_x, press_y, last_y; uint64_t last_ns;
    char param[2048]; int has_param;
    char message[200];
} web;
static SDL_mutex *lock;
static Loaded *arrived;                // set by the worker, taken by the drawing thread

static void rect_on_screen(UiRect *r) {
    double sx = (double)ui_view.w / ui_game_w, sy = (double)ui_view.h / ui_game_h;
    r->x = ui_view.x + (int)floor(web.gx * sx + 0.5); r->y = ui_view.y + (int)floor(web.gy * sy + 0.5);
    r->w = (int)floor(web.gw * sx + 0.5); r->h = (int)floor(web.gh * sy + 0.5);
    if (r->x + r->w > ui_view.x + ui_view.w) r->w = ui_view.x + ui_view.w - r->x;
    if (r->y + r->h > ui_view.y + ui_view.h) r->h = ui_view.y + ui_view.h - r->y;
}
static void escape(char *out, size_t cap, const char *in) {
    size_t o = 0;
    for (const unsigned char *s = (const unsigned char *)in; *s && o + 4 < cap; s++) {
        if ((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || strchr("-_.~", *s)) out[o++] = (char)*s;
        else o += (size_t)SDL_snprintf(out + o, 4, "%%%02X", *s);
    }
    out[o] = 0;
}
typedef struct { int serial, w, h; char url[2048], post[2048]; } Ask;
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static int fetch_thread(void *ud) {
    Ask *ask = ud;
    Loaded *out = calloc(1, sizeof *out);
    out->serial = ask->serial;
    char u[2048 * 3 + 64], p[2048 * 3 + 64], *request = malloc(16384);
    escape(u, sizeof u, ask->url); escape(p, sizeof p, ask->post);
    // 390 is the width in CSS pixels of the phone screen these pages were laid out for.
    SDL_snprintf(request, 16384, "http://local/__desktop/page?width=%d&height=%d&scale=%.4f&url=%s&post=%s", ask->w, ask->h,
                 (double)ui_view.w / 390.0, u, p);
    uint8_t *body = NULL; size_t len = 0;
    int status = http_fetch(request, "GET", NULL, NULL, 0, &body, &len);
    free(request);
    if (status == 200 && len >= 20 && !memcmp(body, "FFPG", 4)) {
        out->w = (int)rd32(body + 8); out->h = (int)rd32(body + 12); out->n_links = (int)rd32(body + 16);
        size_t at = 20;
        if (out->n_links > MAXLINKS) out->n_links = MAXLINKS;
        out->links = calloc((size_t)out->n_links + 1, sizeof(Link));
        int good = out->w > 0 && out->h > 0;
        for (int i = 0; i < out->n_links && good; i++) {
            if (at + 18 > len) { good = 0; break; }
            Link *l = &out->links[i];
            l->x = (int)rd32(body + at); l->y = (int)rd32(body + at + 4); l->w = (int)rd32(body + at + 8); l->h = (int)rd32(body + at + 12);
            size_t n = (size_t)body[at + 16] | (size_t)body[at + 17] << 8;
            at += 18;
            if (at + n > len) { good = 0; break; }
            l->url = malloc(n + 1); memcpy(l->url, body + at, n); l->url[n] = 0;
            at += n;
        }
        size_t need = (size_t)out->w * (size_t)out->h * 3;
        if (good && at + need <= len) { out->pixels = body + at; out->owner = body; body = NULL; out->ok = 1; }
    }
    if (!out->ok) {
        if (status == 0) SDL_snprintf(out->message, sizeof out->message, "The game server did not answer.");
        else if (status != 200) SDL_snprintf(out->message, sizeof out->message, "This page is not available (%d).", status);
        else SDL_snprintf(out->message, sizeof out->message, "This page could not be drawn.");
        rlog("[web] %s -> %s\n", ask->url, out->message);
    }
    free(body);
    free(ask);
    SDL_LockMutex(lock);
    if (arrived) { free(arrived->owner); for (int i = 0; i < arrived->n_links; i++) free(arrived->links[i].url); free(arrived->links); free(arrived); }
    arrived = out;
    SDL_UnlockMutex(lock);
    return 0;
}
static void start_fetch(void) {
    if (!lock) lock = SDL_CreateMutex();
    UiRect r; rect_on_screen(&r);
    if (r.w < 8 || r.h < 8) { web.state = 3; SDL_snprintf(web.message, sizeof web.message, "No room for this page."); return; }
    Ask *ask = calloc(1, sizeof *ask);
    ask->serial = ++web.serial; ask->w = r.w; ask->h = r.h;
    SDL_snprintf(ask->url, sizeof ask->url, "%s", web.url);
    SDL_snprintf(ask->post, sizeof ask->post, "%s", web.post);
    web.asked_w = r.w;
    SDL_Thread *t = SDL_CreateThread(fetch_thread, "page", ask);
    if (t) SDL_DetachThread(t); else { free(ask); web.state = 3; }
}
static void drop_page(void) {
    for (int i = 0; i < web.n_strips; i++) if (web.strips[i]) hg.DeleteTextures(1, &web.strips[i]);
    memset(web.strips, 0, sizeof web.strips);
    web.n_strips = 0; web.uploaded_rows = 0;
    free(web.owner); web.owner = NULL; web.pixels = NULL;
    for (int i = 0; i < web.n_links; i++) free(web.links[i].url);
    free(web.links); web.links = NULL; web.n_links = 0;
    web.page_w = web.page_h = 0;
}

// ---- called by the game (through the host calls) ---------------------------------------------------------
// A web view on a phone keeps its place when the game takes it off the screen for a moment and puts it back (a
// message box over it, the app going to the background and coming back). Here the page's picture is thrown away
// when it is hidden, so the place is remembered as a share of the page's height and gone back to when the same
// page is shown again shortly afterwards. Only a page asked for afresh starts at its top.
static float place_now(void) { return web.page_h > 0 ? web.scroll / (float)web.page_h : web.keep_fraction; }
void web_show(const char *url, const char *post, int x, int y, int w, int h) {
    int same = web.url[0] && !strcmp(web.url, url ? url : "") && !strcmp(web.post, post ? post : "") && now_ns() - web.seen_ns < 2000000000ULL;
    float keep = same ? place_now() : 0;
    SDL_snprintf(web.url, sizeof web.url, "%s", url ? url : "");
    SDL_snprintf(web.post, sizeof web.post, "%s", post ? post : "");
    web.gx = x; web.gy = y; web.gw = w; web.gh = h;
    web.visible = 1; web.state = 1; web.scroll = 0; web.velocity = 0; web.keep_fraction = keep; web.dragging = 0; web.has_param = 0;
    rlog("[web] showing %s%s%s at %d,%d size %dx%d%s\n", web.url, web.post[0] ? " with " : "", web.post, x, y, w, h, same ? " (the same page again: its place is kept)" : "");
    start_fetch();
}
void web_set_visible(int on) {
    if (on && !web.visible && web.url[0]) {
        rlog("[web] back on the screen\n");
        web.visible = 1; if (!web.page_h) { web.state = 1; start_fetch(); } else web.state = 2; return;
    }
    if (!on && web.visible) {                               // the page is thrown away at the next frame
        web.keep_fraction = place_now(); web.seen_ns = now_ns();
        rlog("[web] taken off the screen at %.0f of %d rows\n", web.scroll, web.page_h);
        web.visible = 0; web.state = 0; web.serial++; web.dragging = 0;
    }
}
int web_state(void) { return web.visible ? web.state : 0; }
int web_take_param(char *out, size_t cap) {
    if (!web.has_param) return 0;
    SDL_snprintf(out, cap, "%s", web.param);
    web.has_param = 0;
    return 1;
}
void web_relayout(void) {                                     // the window changed size: draw the page again at the new width
    if (!web.visible || web.state == 0) return;
    UiRect r; rect_on_screen(&r);
    if (r.w == web.asked_w || web.state == 1) return;
    web.keep_fraction = web.page_h > 0 ? web.scroll / (float)web.page_h : 0;
    start_fetch();
}

// ---- drawing ---------------------------------------------------------------------------------------------
static float max_scroll(const UiRect *r) {
    float shown_h = web.page_w ? (float)web.page_h * r->w / web.page_w : 0;
    return shown_h > r->h ? shown_h - r->h : 0;
}
void web_draw(void) {
    if (lock) {
        SDL_LockMutex(lock);
        Loaded *got = arrived; arrived = NULL;
        SDL_UnlockMutex(lock);
        if (got) {
            if (got->serial == web.serial && web.visible) {
                drop_page();
                if (got->ok) {
                    web.page_w = got->w; web.page_h = got->h; web.pixels = got->pixels; web.owner = got->owner; web.links = got->links; web.n_links = got->n_links;
                    web.n_strips = (got->h + STRIP - 1) / STRIP; if (web.n_strips > MAXSTRIPS) web.n_strips = MAXSTRIPS;
                    web.state = 2;
                    web.scroll = web.keep_fraction * web.page_h; web.keep_fraction = 0;
                    got->pixels = NULL; got->owner = NULL; got->links = NULL; got->n_links = 0;
                } else { web.state = 3; SDL_snprintf(web.message, sizeof web.message, "%s", got->message); }
            }
            free(got->owner); for (int i = 0; i < got->n_links; i++) free(got->links[i].url); free(got->links); free(got);
        }
    }
    if (!web.visible) { if (web.page_h || web.owner) drop_page(); return; }
    web.seen_ns = now_ns();
    UiRect r; rect_on_screen(&r);
    if (r.w <= 0 || r.h <= 0) return;
    ui_fill((float)r.x, (float)r.y, (float)r.w, (float)r.h, 0x0D1230FFu, 0x0D1230FFu);
    float fs = ui_bar.h * 0.42f;
    if (web.state == 1 && !web.page_h) {
        static const char *dots[] = {"Loading", "Loading.", "Loading..", "Loading..."};
        const char *t = dots[(now_ns() / 300000000ULL) % 4];
        ui_text(r.x + (r.w - ui_text_width(fs, "Loading...")) * 0.5f, r.y + r.h * 0.42f, fs, 0xE8C46AFFu, t);
        return;
    }
    if (web.state == 3) {
        ui_text(r.x + (r.w - ui_text_width(fs, web.message)) * 0.5f, r.y + r.h * 0.42f, fs, 0xF3ECD9FFu, web.message);
        return;
    }
    // Hand the picture to OpenGL a few strips per frame, so a very long page does not hold up the game.
    for (int budget = 4; budget > 0 && web.pixels && web.uploaded_rows < web.page_h; budget--) {
        int i = web.uploaded_rows / STRIP, rows = web.page_h - web.uploaded_rows; if (rows > STRIP) rows = STRIP;
        if (i >= MAXSTRIPS) { web.uploaded_rows = web.page_h; break; }
        web.strips[i] = ui_texture(web.pixels + (size_t)web.uploaded_rows * web.page_w * 3, web.page_w, rows, 0x1907, 1);
        web.uploaded_rows += rows;
    }
    if (web.pixels && web.uploaded_rows >= web.page_h) { free(web.owner); web.owner = NULL; web.pixels = NULL; }
    // Glide after a drag, and never past either end.
    float k = (float)r.w / (float)web.page_w, limit = max_scroll(&r) / k;
    float top_speed = (float)r.h * 0.10f / k;               // a glide never covers more than about a screen and a half
    if (web.velocity > top_speed) web.velocity = top_speed; if (web.velocity < -top_speed) web.velocity = -top_speed;
    if (!web.dragging && fabsf(web.velocity) > 0.05f) { web.scroll += web.velocity; web.velocity *= 0.94f; }
    if (web.scroll > limit) { web.scroll = limit; web.velocity = 0; }
    if (web.scroll < 0) { web.scroll = 0; web.velocity = 0; }
    ui_clip((float)r.x, (float)r.y, (float)r.w, (float)r.h);
    for (int i = 0; i < web.n_strips; i++) {
        if (!web.strips[i]) continue;
        int rows = web.page_h - i * STRIP; if (rows > STRIP) rows = STRIP;
        float y0 = r.y + ((float)(i * STRIP) - web.scroll) * k, hh = rows * k;
        if (y0 + hh < r.y || y0 > r.y + r.h) continue;
        ui_image(web.strips[i], (float)r.x, floorf(y0 + 0.5f), (float)r.w, k == 1.0f ? (float)rows : hh + 0.5f, 0, 0, 1, 1, 0xFFFFFFFFu);
    }
    if (limit > 0) {                                           // where we are in the page
        float track = (float)r.h - 8, bar = fmaxf(24, track * r.h / (web.page_h * k)), at = (track - bar) * web.scroll / limit;
        ui_fill((float)(r.x + r.w - 6), r.y + 4 + at, 3, bar, 0xE8C46AB0u, 0xE8C46AB0u);
    }
    ui_unclip();
}

// ---- input -----------------------------------------------------------------------------------------------
static void follow(const char *url) {
    if (!strncmp(url, "ffbecall://", 11)) { SDL_snprintf(web.param, sizeof web.param, "%s", url + 11); web.has_param = 1; return; }
    if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) return;
    SDL_snprintf(web.url, sizeof web.url, "%s", url);
    web.post[0] = 0;
    web.state = 1; web.scroll = 0; web.velocity = 0; web.keep_fraction = 0;
    drop_page();
    start_fetch();
}
int web_event(const SDL_Event *e, int mx, int my) {
    if (!web.visible) return 0;
    UiRect r; rect_on_screen(&r);
    int over = mx >= r.x && mx < r.x + r.w && my >= r.y && my < r.y + r.h;
    float k = web.page_w ? (float)r.w / (float)web.page_w : 1;
    switch (e->type) {
    case SDL_MOUSEWHEEL:
        if (!over) return 0;
        web.scroll -= (float)e->wheel.y * ui_view.h * 0.085f / k; web.velocity = 0;
        return 1;
    case SDL_MOUSEBUTTONDOWN:
        if (!over) return 0;
        web.dragging = 1; web.moved = 0; web.press_x = mx; web.press_y = my; web.last_y = my; web.velocity = 0; web.last_ns = now_ns();
        return 1;
    case SDL_MOUSEMOTION:
        if (!web.dragging) return 0;
        // The button is up, so the release was missed (let go outside the window, or while another program was in
        // front): the page must not go on following the pointer. (9999: the test channel's own events.)
        if (!(e->motion.state & SDL_BUTTON_LMASK) && e->motion.which != 9999) { web.dragging = 0; web.velocity = 0; return 0; }
        if (abs(my - web.press_y) > 5 || abs(mx - web.press_x) > 5) web.moved = 1;
        if (web.moved) { float d = (float)(web.last_y - my) / k; web.scroll += d; web.velocity = web.velocity * 0.5f + d * 0.5f; }
        web.last_y = my; web.last_ns = now_ns();
        return 1;
    case SDL_MOUSEBUTTONUP: {
        if (!web.dragging) return 0;
        web.dragging = 0;
        if (now_ns() - web.last_ns > 80000000ULL) web.velocity = 0;      // held still before letting go: no glide
        if (!web.moved && web.state == 2) {
            int px = (int)((mx - r.x) / k), py = (int)((my - r.y) / k + web.scroll);
            for (int i = web.n_links - 1; i >= 0; i--) {
                Link *l = &web.links[i];
                if (px >= l->x && px < l->x + l->w && py >= l->y && py < l->y + l->h) { rlog("[web] link %s\n", l->url); follow(l->url); break; }
            }
        }
        return 1;
    }
    case SDL_KEYDOWN: {
        float page = (float)r.h / k;
        switch (e->key.keysym.sym) {
        case SDLK_PAGEDOWN: case SDLK_SPACE: web.scroll += page * 0.9f; return 1;
        case SDLK_PAGEUP: web.scroll -= page * 0.9f; return 1;
        case SDLK_DOWN: web.scroll += page * 0.12f; return 1;
        case SDLK_UP: web.scroll -= page * 0.12f; return 1;
        case SDLK_HOME: web.scroll = 0; return 1;
        case SDLK_END: web.scroll = 1e9f; return 1;
        }
        return 0;
    }
    }
    return 0;
}
