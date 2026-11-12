// Text bitmaps. On Android the game asks Java (Cocos2dxBitmap) to lay out and draw a string and hand back
// the pixels. This does the same job with stb_truetype, following that Java routine's sizing rules.
#define _GNU_SOURCE
#include <link.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <stb/stb_truetype.h>

#include "shared.h"
#include "text.h"

static stbtt_fontinfo font;
static int font_ok = -1;

static void load_font(void) {
    if (font_ok >= 0) return;
    font_ok = 0;
    const char *path = getenv("FFBE_FONT") ? getenv("FFBE_FONT") : "/ffbe/font.ttf";
    FILE *f = fopen(path, "rb");
    if (!f) { logf_("[text] no font at %s; labels drawn by the system font will be blank\n", path); return; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *data = malloc((size_t)n);
    if (fread(data, 1, (size_t)n, f) == (size_t)n && stbtt_InitFont(&font, data, stbtt_GetFontOffsetForIndex(data, 0))) font_ok = 1;
    fclose(f);
}
static int utf8(const char **p, const char *end) {
    const unsigned char *s = (const unsigned char *)*p;
    int c = *s, n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    if ((const char *)s + n > end) n = 1;
    if (n == 2) c = (c & 0x1F) << 6 | (s[1] & 0x3F);
    else if (n == 3) c = (c & 0x0F) << 12 | (s[1] & 0x3F) << 6 | (s[2] & 0x3F);
    else if (n == 4) c = (c & 0x07) << 18 | (s[1] & 0x3F) << 12 | (s[2] & 0x3F) << 6 | (s[3] & 0x3F);
    *p += n;
    return c;
}
typedef struct { int start, end; float width; } Line;     // indices into the codepoint array

static float advance(int cp, int next, float scale) {
    int adv, lsb;
    stbtt_GetCodepointHMetrics(&font, cp, &adv, &lsb);
    float a = adv * scale;
    if (next) a += stbtt_GetCodepointKernAdvance(&font, cp, next) * scale;
    return a;
}
static float span_width(const int *cps, int a, int b, float scale) {
    float w = 0;
    for (int i = a; i < b; i++) w += advance(cps[i], i + 1 < b ? cps[i + 1] : 0, scale);
    return w;
}
// Greedy line breaking at spaces, at explicit newlines, and inside a word only when the word alone is too wide.
static int break_lines(const int *cps, int n, float scale, float max_width, Line *lines, int cap) {
    int count = 0, i = 0;
    while (i <= n && count < cap) {
        int start = i, last_space = -1;
        float w = 0;
        while (i < n && cps[i] != '\n') {
            float a = advance(cps[i], i + 1 < n && cps[i + 1] != '\n' ? cps[i + 1] : 0, scale);
            if (max_width > 0 && w + advance(cps[i], 0, scale) > max_width + 0.01f && i > start) {
                if (cps[i] == ' ') break;
                if (last_space >= 0) { i = last_space; break; }
                break;
            }
            if (cps[i] == ' ') last_space = i;
            w += a;
            i++;
        }
        int end = i;
        while (end > start && cps[end - 1] == ' ') end--;
        lines[count++] = (Line){start, end, span_width(cps, start, end, scale)};
        if (i >= n) break;
        if (cps[i] == '\n' || cps[i] == ' ') i++;
    }
    return count;
}
static void draw_run(unsigned char *cov, int w, int h, const int *cps, int a, int b, float scale, float x, float baseline) {
    for (int i = a; i < b; i++) {
        int x0, y0, x1, y1;
        float fx = x - floorf(x);
        stbtt_GetCodepointBitmapBoxSubpixel(&font, cps[i], scale, scale, fx, 0, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0) {
            unsigned char *g = calloc((size_t)gw, (size_t)gh);
            stbtt_MakeCodepointBitmapSubpixel(&font, g, gw, gh, gw, scale, scale, fx, 0, cps[i]);
            int ox = (int)floorf(x) + x0, oy = (int)lroundf(baseline) + y0;
            for (int yy = 0; yy < gh; yy++) {
                int py = oy + yy;
                if (py < 0 || py >= h) continue;
                for (int xx = 0; xx < gw; xx++) {
                    int px = ox + xx;
                    if (px < 0 || px >= w) continue;
                    int v = cov[py * w + px] + g[yy * gw + xx];
                    cov[py * w + px] = (unsigned char)(v > 255 ? 255 : v);
                }
            }
            free(g);
        }
        x += advance(cps[i], i + 1 < b ? cps[i + 1] : 0, scale);
    }
}

void text_measure(const char *s, int len, int size, float *w, float *h) {
    load_font();
    *w = 0; *h = 0;
    if (!font_ok || size <= 0) { *w = len * size * 0.55f; *h = size * 0.75f; return; }
    float scale = stbtt_ScaleForMappingEmToPixels(&font, (float)size);
    int *cps = malloc(sizeof(int) * (size_t)(len + 1)), n = 0;
    for (const char *p = s, *end = s + len; p < end;) cps[n++] = utf8(&p, end);
    *w = span_width(cps, 0, n, scale);
    int top = 0, bottom = 0;
    for (int i = 0; i < n; i++) {
        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBox(&font, cps[i], scale, scale, &x0, &y0, &x1, &y1);
        if (y0 < top) top = y0;
        if (y1 > bottom) bottom = y1;
    }
    *h = (float)(bottom - top);
    free(cps);
}

int text_render(const char *s, int len, const TextParams *tp, int *out_w, int *out_h, unsigned char **out) {
    load_font();
    int size = tp->size > 0 ? tp->size : 12;
    if (!font_ok) {                                  // keep the game going with an empty bitmap of a plausible size
        *out_w = len * size / 2 + 4; *out_h = size + size / 3; *out = calloc((size_t)*out_w * *out_h, 4);
        return 1;
    }
    int *cps = malloc(sizeof(int) * (size_t)(len + 1)), n = 0;
    for (const char *p = s, *end = s + len; p < end;) { int c = utf8(&p, end); if (c != '\r') cps[n++] = c; }
    Line *lines = malloc(sizeof(Line) * 256);
    int nlines, layout_w, layout_h, asc, desc, top, bottom, line_h;
    float scale;
    for (;; size--) {
        scale = stbtt_ScaleForMappingEmToPixels(&font, (float)size);
        int a, d, g, bx0, by0, bx1, by1;
        stbtt_GetFontVMetrics(&font, &a, &d, &g);
        stbtt_GetFontBoundingBox(&font, &bx0, &by0, &bx1, &by1);
        asc = (int)lroundf(a * scale); desc = (int)lroundf(-d * scale);
        top = (int)lroundf(by1 * scale); bottom = (int)lroundf(-by0 * scale);
        if (top < asc) top = asc;
        if (bottom < desc) bottom = desc;
        line_h = (int)lroundf((asc + desc) * (tp->spacing > 0 ? tp->spacing : 1.0f));
        float desired = 0;
        for (int i = 0, st = 0; i <= n; i++) if (i == n || cps[i] == '\n') { float w = span_width(cps, st, i, scale); if (w > desired) desired = w; st = i + 1; }
        layout_w = tp->width > 0 ? tp->width : (int)ceilf(desired);
        if (tp->overflow == 1 && !tp->wrap) layout_w = (int)ceilf(desired);
        int fitted = tp->width <= 0 || (tp->overflow == 1 && !tp->wrap);           // the layout is as wide as its longest line
        nlines = break_lines(cps, n, scale, fitted ? 0 : (float)layout_w, lines, 256);
        layout_h = nlines * line_h + (top - asc) + (bottom - desc);
        if (tp->overflow != 2 || size <= 6) break;                       // 2 = shrink to fit
        if ((tp->height <= 0 || layout_h <= tp->height) && (tp->width <= 0 || desired <= tp->width || tp->wrap)) break;
    }
    int bw = layout_w > tp->width ? layout_w : tp->width, bh = tp->height > 0 ? tp->height : layout_h;
    if (tp->overflow == 1 && !tp->wrap && tp->width > 0) bw = tp->width;
    if (bw <= 0 || bh <= 0) { free(cps); free(lines); return 0; }
    if (bw > 4096) bw = 4096;
    if (bh > 4096) bh = 4096;
    int halign = tp->align & 15, valign = (tp->align >> 4) & 15;
    int off_x = halign == 3 ? (bw - layout_w) / 2 : halign == 2 ? bw - layout_w : 0;
    int off_y = valign == 2 ? bh - layout_h : valign == 3 ? (bh - layout_h) / 2 : 0;
    unsigned char *cov = calloc((size_t)bw, (size_t)bh);
    for (int l = 0; l < nlines; l++) {
        float lx = halign == 3 ? (layout_w - lines[l].width) / 2 : halign == 2 ? layout_w - lines[l].width : 0;
        float baseline = (float)(off_y + (top - asc) + l * line_h + asc);
        draw_run(cov, bw, bh, cps, lines[l].start, lines[l].end, scale, off_x + lx, baseline);
    }
    unsigned char *px = calloc((size_t)bw * bh, 4), *edge = NULL;
    if (tp->stroke && tp->stroke_size > 0) {                             // outline: the glyphs grown by the stroke width
        int r = (int)ceilf(tp->stroke_size / 2);
        if (r < 1) r = 1;
        if (r > 6) r = 6;
        edge = calloc((size_t)bw, (size_t)bh);
        for (int y = 0; y < bh; y++) for (int x = 0; x < bw; x++) {
            int m = 0;
            for (int dy = -r; dy <= r && m < 255; dy++) { int yy = y + dy; if (yy < 0 || yy >= bh) continue;
                for (int dx = -r; dx <= r; dx++) { int xx = x + dx; if (xx < 0 || xx >= bw || dx * dx + dy * dy > r * r + 1) continue; int v = cov[yy * bw + xx]; if (v > m) m = v; } }
            edge[y * bw + x] = (unsigned char)m;
        }
    }
    for (int i = 0; i < bw * bh; i++) {
        int fa = cov[i] * tp->a / 255, r = tp->r * fa / 255, g = tp->g * fa / 255, b = tp->b * fa / 255, a = fa;
        if (edge) {
            int ea = edge[i] * tp->sa / 255, inv = 255 - fa;
            r += tp->sr * ea / 255 * inv / 255; g += tp->sg * ea / 255 * inv / 255; b += tp->sb * ea / 255 * inv / 255; a += ea * inv / 255;
        }
        px[i * 4] = (unsigned char)r; px[i * 4 + 1] = (unsigned char)g; px[i * 4 + 2] = (unsigned char)b; px[i * 4 + 3] = (unsigned char)a;
    }
    free(cov); free(edge); free(cps); free(lines);
    *out_w = bw; *out_h = bh; *out = px;
    return 1;
}
