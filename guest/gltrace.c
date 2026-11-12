// OpenGL ES stand-in that answers the engine and records every call (with its data) to a trace file.
// The trace is replayed by replay.py against a real GLES2 context on the host, which gives real pixels
// without needing an ARM64 graphics driver in this sandbox.
#define _GNU_SOURCE
#include <link.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shared.h"

static FILE *tr;
static void emit(const char *name, int ni, const long *iv, int nf, const float *fv, const void *blob, size_t blen);
static unsigned ids = 100;
static unsigned long n_draws, n_tex, tex_bytes, n_shaders, n_programs, n_calls;
static int unpack_align = 4;
static unsigned array_buf, elem_buf;
static struct { int size, type, norm, stride; const void *ptr; unsigned vbo; int enabled; } attr[16];
static unsigned char caps[0x10000];

int g_width = 720, g_height = 1280;
void gltrace_open(const char *path) { tr = fopen(path, "wb"); }
void gltrace_size(int w, int h) { g_width = w; g_height = h; long v[] = {w, h}; emit("SIZE", 2, v, 0, 0, 0, 0); }

static void emit(const char *name, int ni, const long *iv, int nf, const float *fv, const void *blob, size_t blen) {
    n_calls++;
    if (!tr) return;
    unsigned char nl = (unsigned char)strlen(name), h[2] = {(unsigned char)ni, (unsigned char)nf};
    uint32_t bl = (uint32_t)blen;
    fwrite(&nl, 1, 1, tr); fwrite(name, 1, nl, tr); fwrite(h, 1, 2, tr); fwrite(&bl, 4, 1, tr);
    if (ni) fwrite(iv, 8, ni, tr);
    if (nf) fwrite(fv, 4, nf, tr);
    if (blen) fwrite(blob, 1, blen, tr);
}
void gltrace_frame(int n) { long v[] = {n}; emit("FRAME", 1, v, 0, 0, 0, 0); if (tr) fflush(tr); }
void gltrace_text(const char *text, int len, int w, int h, int size) { long v[] = {w, h, size}; emit("TEXT", 3, v, 0, 0, text, len); }

#define I1(n) static void n(int a) { long v[] = {a}; emit(#n, 1, v, 0, 0, 0, 0); }
#define I2(n) static void n(int a, int b) { long v[] = {a, b}; emit(#n, 2, v, 0, 0, 0, 0); }
#define I3(n) static void n(int a, int b, int c) { long v[] = {a, b, c}; emit(#n, 3, v, 0, 0, 0, 0); }
#define I4(n) static void n(int a, int b, int c, int d) { long v[] = {a, b, c, d}; emit(#n, 4, v, 0, 0, 0, 0); }
#define I5(n) static void n(int a, int b, int c, int d, int e) { long v[] = {a, b, c, d, e}; emit(#n, 5, v, 0, 0, 0, 0); }
I1(glClear) I1(glUseProgram) I1(glCompileShader) I1(glLinkProgram) I1(glDeleteShader) I1(glDeleteProgram) I1(glActiveTexture)
I1(glDepthFunc) I1(glDepthMask) I1(glStencilMask) I1(glClearStencil) I1(glBlendEquation) I1(glCullFace) I1(glFrontFace) I1(glGenerateMipmap)
I2(glBlendFunc) I2(glBindTexture) I2(glBindFramebuffer) I2(glBindRenderbuffer) I2(glAttachShader) I2(glDetachShader) I2(glUniform1i) I2(glHint)
I2(glBlendEquationSeparate)
I3(glTexParameteri) I3(glStencilFunc) I3(glStencilOp) I3(glUniform2i)
I4(glViewport) I4(glScissor) I4(glColorMask) I4(glBlendFuncSeparate) I4(glRenderbufferStorage) I4(glFramebufferRenderbuffer)
I5(glFramebufferTexture2D)
static void glEnable(unsigned c) { if (c < 0x10000) caps[c] = 1; long v[] = {c}; emit("glEnable", 1, v, 0, 0, 0, 0); }
static void glDisable(unsigned c) { if (c < 0x10000) caps[c] = 0; long v[] = {c}; emit("glDisable", 1, v, 0, 0, 0, 0); }
static unsigned char glIsEnabled(unsigned c) { return c < 0x10000 ? caps[c] : 0; }
static void glGetBooleanv(unsigned p, unsigned char *out) { *out = p < 0x10000 ? caps[p] : 0; if (p == 0x0B72 || p == 0x0C23) memset(out, 1, p == 0x0C23 ? 4 : 1); }
static void glClearColor(float r, float g, float b, float a) { float f[] = {r, g, b, a}; emit("glClearColor", 0, 0, 4, f, 0, 0); }
static void glClearDepthf(float d) { emit("glClearDepthf", 0, 0, 1, &d, 0, 0); }
static void glLineWidth(float d) { emit("glLineWidth", 0, 0, 1, &d, 0, 0); }
static void glPolygonOffset(float a, float b) { float f[] = {a, b}; emit("glPolygonOffset", 0, 0, 2, f, 0, 0); }
static void glTexParameterf(int t, int p, float f) { long v[] = {t, p}; emit("glTexParameterf", 2, v, 1, &f, 0, 0); }
static void glUniform1f(int l, float a) { long v[] = {l}; emit("glUniform1f", 1, v, 1, &a, 0, 0); }
static void glUniform2f(int l, float a, float b) { long v[] = {l}; float f[] = {a, b}; emit("glUniform2f", 1, v, 2, f, 0, 0); }
static void glUniform3f(int l, float a, float b, float c) { long v[] = {l}; float f[] = {a, b, c}; emit("glUniform3f", 1, v, 3, f, 0, 0); }
static void glUniform4f(int l, float a, float b, float c, float d) { long v[] = {l}; float f[] = {a, b, c, d}; emit("glUniform4f", 1, v, 4, f, 0, 0); }
#define UFV(n, k) static void n(int l, int c, const float *p) { long v[] = {l, c}; emit(#n, 2, v, 0, 0, p, (size_t)c * k * 4); }
UFV(glUniform1fv, 1) UFV(glUniform2fv, 2) UFV(glUniform3fv, 3) UFV(glUniform4fv, 4)
static void glUniform1iv(int l, int c, const int *p) { long v[] = {l, c}; emit("glUniform1iv", 2, v, 0, 0, p, (size_t)c * 4); }
#define UMV(n, k) static void n(int l, int c, int t, const float *p) { long v[] = {l, c, t}; emit(#n, 3, v, 0, 0, p, (size_t)c * k * 4); }
UMV(glUniformMatrix2fv, 4) UMV(glUniformMatrix3fv, 9) UMV(glUniformMatrix4fv, 16)

static const char *glGetString(unsigned name) {
    switch (name) {
    case 0x1F00: return "Loader";
    case 0x1F01: return "Trace GLES";
    case 0x1F02: return "OpenGL ES 2.0 (trace)";
    case 0x1F03: return "GL_OES_rgb8_rgba8 GL_OES_depth24 GL_OES_packed_depth_stencil";
    case 0x8B8C: return "OpenGL ES GLSL ES 1.00";
    }
    return "";
}
static void glGetIntegerv(unsigned p, int *out) {
    switch (p) {
    case 0x0D33: *out = 4096; break;
    case 0x8872: case 0x8B4D: *out = 8; break;
    case 0x8869: *out = 16; break;
    case 0x0BA2: out[0] = 0; out[1] = 0; out[2] = g_width; out[3] = g_height; break;
    default: *out = 0;
    }
}
static unsigned glGetError(void) { return 0; }
static unsigned glCheckFramebufferStatus(unsigned t) { (void)t; return 0x8CD5; }
static unsigned glCreateShader(unsigned t) { n_shaders++; unsigned id = ++ids; long v[] = {t, id}; emit("glCreateShader", 2, v, 0, 0, 0, 0); return id; }
static unsigned glCreateProgram(void) { n_programs++; unsigned id = ++ids; long v[] = {id}; emit("glCreateProgram", 1, v, 0, 0, 0, 0); return id; }
static void glGetShaderiv(unsigned s, unsigned p, int *out) { (void)s; *out = p == 0x8B81 ? 1 : 0; }
static void glGetProgramiv(unsigned s, unsigned p, int *out) { (void)s; *out = p == 0x8B82 ? 1 : 0; }
static void glGetProgramInfoLog(unsigned p, int n, int *len, char *log) { (void)p; if (len) *len = 0; if (n > 0 && log) log[0] = 0; }
static void glGetShaderInfoLog(unsigned p, int n, int *len, char *log) { (void)p; if (len) *len = 0; if (n > 0 && log) log[0] = 0; }
#define GEN(n) static void n(int c, unsigned *out) { for (int i = 0; i < c; i++) { out[i] = ++ids; long v[] = {out[i]}; emit(#n, 1, v, 0, 0, 0, 0); } }
GEN(glGenTextures) GEN(glGenBuffers) GEN(glGenFramebuffers) GEN(glGenRenderbuffers)
#define DEL(n) static void n(int c, const unsigned *in) { for (int i = 0; i < c; i++) { long v[] = {in[i]}; emit(#n, 1, v, 0, 0, 0, 0); } }
DEL(glDeleteTextures) DEL(glDeleteBuffers) DEL(glDeleteFramebuffers) DEL(glDeleteRenderbuffers)
static void glShaderSource(unsigned s, int count, const char **str, const int *len) {
    size_t total = 0;
    for (int i = 0; i < count; i++) total += len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]);
    char *buf = malloc(total + 1), *p = buf;
    for (int i = 0; i < count; i++) { size_t l = len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]); memcpy(p, str[i], l); p += l; }
    long v[] = {s};
    emit("glShaderSource", 1, v, 0, 0, buf, total);
    free(buf);
}
static void glBindAttribLocation(unsigned p, unsigned i, const char *n) { long v[] = {p, i}; emit("glBindAttribLocation", 2, v, 0, 0, n, strlen(n)); }
static int glGetUniformLocation(unsigned p, const char *n) { int id = (int)++ids; long v[] = {p, id}; emit("glGetUniformLocation", 2, v, 0, 0, n, strlen(n)); return id; }
static int glGetAttribLocation(unsigned p, const char *n) {
    static int next = 8;
    int idx = next; next = next == 15 ? 8 : next + 1;
    long v[] = {p, idx}; emit("glGetAttribLocation", 2, v, 0, 0, n, strlen(n));
    return idx;
}
static void glBindBuffer(unsigned t, unsigned b) { if (t == 0x8892) array_buf = b; else if (t == 0x8893) elem_buf = b; long v[] = {t, b}; emit("glBindBuffer", 2, v, 0, 0, 0, 0); }
static void glBufferData(unsigned t, long size, const void *d, unsigned usage) { long v[] = {t, size, usage, d != NULL}; emit("glBufferData", 4, v, 0, 0, d, d ? (size_t)size : 0); }
static void glBufferSubData(unsigned t, long off, long size, const void *d) { long v[] = {t, off, size}; emit("glBufferSubData", 3, v, 0, 0, d, (size_t)size); }
static void glPixelStorei(unsigned p, int val) { if (p == 0x0CF5) unpack_align = val; long v[] = {p, val}; emit("glPixelStorei", 2, v, 0, 0, 0, 0); }
static size_t pixel_bytes(int w, int h, unsigned fmt, unsigned type) {
    int bpp = type == 0x8363 || type == 0x8033 || type == 0x8034 ? 2 : fmt == 0x1908 ? 4 : fmt == 0x1907 ? 3 : fmt == 0x190A ? 2 : 1;
    size_t row = ((size_t)w * bpp + unpack_align - 1) / unpack_align * unpack_align;
    return row * h;
}
static void glTexImage2D(unsigned t, int l, int f, int w, int h, int b, unsigned fmt, unsigned ty, const void *px) {
    n_tex++; tex_bytes += (unsigned long)w * h * 4;
    if (g_verbose > 1) logf_("[gl] texture %dx%d\n", w, h);
    long v[] = {t, l, f, w, h, b, fmt, ty, px != NULL};
    emit("glTexImage2D", 9, v, 0, 0, px, px ? pixel_bytes(w, h, fmt, ty) : 0);
}
static void glTexSubImage2D(unsigned t, int l, int x, int y, int w, int h, unsigned fmt, unsigned ty, const void *px) {
    long v[] = {t, l, x, y, w, h, fmt, ty};
    emit("glTexSubImage2D", 8, v, 0, 0, px, pixel_bytes(w, h, fmt, ty));
}
static void glCompressedTexImage2D(unsigned t, int l, unsigned f, int w, int h, int b, int size, const void *px) {
    n_tex++; tex_bytes += size;
    long v[] = {t, l, f, w, h, b, size};
    emit("glCompressedTexImage2D", 7, v, 0, 0, px, size);
}
static void glEnableVertexAttribArray(unsigned i) { if (i < 16) attr[i].enabled = 1; long v[] = {i}; emit("glEnableVertexAttribArray", 1, v, 0, 0, 0, 0); }
static void glDisableVertexAttribArray(unsigned i) { if (i < 16) attr[i].enabled = 0; long v[] = {i}; emit("glDisableVertexAttribArray", 1, v, 0, 0, 0, 0); }
static void glVertexAttribPointer(unsigned i, int size, unsigned type, unsigned char norm, int stride, const void *ptr) {
    if (i < 16) { attr[i].size = size; attr[i].type = type; attr[i].norm = norm; attr[i].stride = stride; attr[i].ptr = ptr; attr[i].vbo = array_buf; }
    if (array_buf) { long v[] = {i, size, type, norm, stride, (long)ptr}; emit("glVertexAttribPointer", 6, v, 0, 0, 0, 0); }
}
static int type_size(unsigned t) { return t == 0x1406 || t == 0x1404 || t == 0x1405 ? 4 : t == 0x1402 || t == 0x1403 ? 2 : 1; }
static void client_arrays(int nverts) {
    for (int i = 0; i < 16; i++) {
        if (!attr[i].enabled || attr[i].vbo || !attr[i].ptr) continue;
        int es = attr[i].size * type_size(attr[i].type), stride = attr[i].stride ? attr[i].stride : es;
        long v[] = {i, attr[i].size, attr[i].type, attr[i].norm, attr[i].stride};
        emit("CLIENT_ATTRIB", 5, v, 0, 0, attr[i].ptr, nverts > 0 ? (size_t)stride * (nverts - 1) + es : 0);
    }
}
static void glDrawArrays(unsigned mode, int first, int count) {
    n_draws++;
    client_arrays(first + count);
    long v[] = {mode, first, count};
    emit("glDrawArrays", 3, v, 0, 0, 0, 0);
}
static void glDrawElements(unsigned mode, int count, unsigned type, const void *indices) {
    n_draws++;
    if (elem_buf) {
        client_arrays(65536 > count * 4 ? count * 4 : 65536);
        long v[] = {mode, count, type, (long)indices, 1};
        emit("glDrawElements", 5, v, 0, 0, 0, 0);
    } else {
        int max = 0;
        for (int i = 0; i < count; i++) { int ix = type == 0x1403 ? ((const uint16_t *)indices)[i] : type == 0x1405 ? (int)((const uint32_t *)indices)[i] : ((const uint8_t *)indices)[i]; if (ix > max) max = ix; }
        client_arrays(max + 1);
        long v[] = {mode, count, type, 0, 0};
        emit("glDrawElements", 5, v, 0, 0, indices, (size_t)count * type_size(type));
    }
}

void gl_report(void) {
    logf_("\n== GL totals ==\n  calls %lu, shaders %lu, programs %lu, texture uploads %lu (%.1f MB as RGBA), draw calls %lu\n",
          n_calls, n_shaders, n_programs, n_tex, tex_bytes / 1048576.0, n_draws);
    if (tr) fflush(tr);
}

typedef struct { const char *name; void *ptr; } Entry;
#define E(n) { #n, (void *)n }
static Entry table[] = {
    E(glClear), E(glUseProgram), E(glCompileShader), E(glLinkProgram), E(glDeleteShader), E(glDeleteProgram), E(glActiveTexture),
    E(glDepthFunc), E(glDepthMask), E(glStencilMask), E(glClearStencil), E(glBlendEquation), E(glCullFace), E(glFrontFace), E(glGenerateMipmap),
    E(glBlendFunc), E(glBindTexture), E(glBindFramebuffer), E(glBindRenderbuffer), E(glAttachShader), E(glDetachShader), E(glUniform1i), E(glHint),
    E(glBlendEquationSeparate), E(glTexParameteri), E(glStencilFunc), E(glStencilOp), E(glUniform2i),
    E(glViewport), E(glScissor), E(glColorMask), E(glBlendFuncSeparate), E(glRenderbufferStorage), E(glFramebufferRenderbuffer), E(glFramebufferTexture2D),
    E(glEnable), E(glDisable), E(glIsEnabled), E(glGetBooleanv), E(glClearColor), E(glClearDepthf), E(glLineWidth), E(glPolygonOffset), E(glTexParameterf),
    E(glUniform1f), E(glUniform2f), E(glUniform3f), E(glUniform4f), E(glUniform1fv), E(glUniform2fv), E(glUniform3fv), E(glUniform4fv), E(glUniform1iv),
    E(glUniformMatrix2fv), E(glUniformMatrix3fv), E(glUniformMatrix4fv),
    E(glGetString), E(glGetIntegerv), E(glGetError), E(glCheckFramebufferStatus), E(glCreateShader), E(glCreateProgram),
    E(glGetShaderiv), E(glGetProgramiv), E(glGetProgramInfoLog), E(glGetShaderInfoLog),
    E(glGenTextures), E(glGenBuffers), E(glGenFramebuffers), E(glGenRenderbuffers),
    E(glDeleteTextures), E(glDeleteBuffers), E(glDeleteFramebuffers), E(glDeleteRenderbuffers),
    E(glShaderSource), E(glBindAttribLocation), E(glGetUniformLocation), E(glGetAttribLocation),
    E(glBindBuffer), E(glBufferData), E(glBufferSubData), E(glPixelStorei), E(glTexImage2D), E(glTexSubImage2D), E(glCompressedTexImage2D),
    E(glEnableVertexAttribArray), E(glDisableVertexAttribArray), E(glVertexAttribPointer), E(glDrawArrays), E(glDrawElements),
};
void *gltrace_lookup(const char *name) {
    for (size_t i = 0; i < sizeof table / sizeof *table; i++) if (strcmp(table[i].name, name) == 0) return table[i].ptr;
    return NULL;
}
