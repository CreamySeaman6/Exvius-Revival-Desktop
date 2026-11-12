#pragma once
typedef struct {
    int size; float spacing;
    int r, g, b, a, align, width, height;
    int stroke, sr, sg, sb, sa; float stroke_size;
    int wrap, overflow;
} TextParams;
int text_render(const char *utf8, int len, const TextParams *p, int *out_w, int *out_h, unsigned char **out_rgba);
void text_measure(const char *utf8, int len, int size, float *w, float *h);
