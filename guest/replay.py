#!/usr/bin/env python3
"""Replay a GL trace recorded by the loader against a real OpenGL ES 2 context (Mesa software renderer)
and save chosen frames as PNG files.

    replay.py <trace> <out-dir> <frame,frame,...>

Draws to the screen are only executed for the chosen frames; everything that builds state (textures,
shaders, buffers, uniforms, draws into off-screen targets) is always executed.
Text bitmaps, which Android draws in Java, are drawn here with a stand-in font.
"""
import ctypes
import os
import struct
import sys
import textwrap

os.environ['PYOPENGL_PLATFORM'] = 'egl'
os.environ['EGL_PLATFORM'] = 'surfaceless'
from OpenGL import EGL  # noqa: E402
from PIL import Image, ImageDraw, ImageFont  # noqa: E402

trace_path, out_dir, frames = sys.argv[1], sys.argv[2], {int(x) for x in sys.argv[3].split(',')}
os.makedirs(out_dir, exist_ok=True)
FONT = '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf'

gl = ctypes.CDLL('libGLESv2.so.2')
F, I, U, P = ctypes.c_float, ctypes.c_int, ctypes.c_uint, ctypes.c_void_p
gl.glClearColor.argtypes = [F] * 4
gl.glClearDepthf.argtypes = [F]
gl.glLineWidth.argtypes = [F]
gl.glPolygonOffset.argtypes = [F, F]
gl.glTexParameterf.argtypes = [U, U, F]
gl.glUniform1f.argtypes = [I, F]
gl.glUniform2f.argtypes = [I, F, F]
gl.glUniform3f.argtypes = [I, F, F, F]
gl.glUniform4f.argtypes = [I, F, F, F, F]
gl.glVertexAttribPointer.argtypes = [U, I, U, ctypes.c_ubyte, I, P]
gl.glDrawElements.argtypes = [U, I, U, P]
gl.glTexImage2D.argtypes = [U, I, I, I, I, I, U, U, P]
gl.glTexSubImage2D.argtypes = [U, I, I, I, I, I, U, U, P]
gl.glCompressedTexImage2D.argtypes = [U, I, U, I, I, I, I, P]
gl.glBufferData.argtypes = [U, ctypes.c_ssize_t, P, U]
gl.glBufferSubData.argtypes = [U, ctypes.c_ssize_t, ctypes.c_ssize_t, P]
gl.glReadPixels.argtypes = [I, I, I, I, U, U, P]
gl.glGetUniformLocation.argtypes = [U, ctypes.c_char_p]
gl.glBindAttribLocation.argtypes = [U, U, ctypes.c_char_p]
for name in ('glUniform1fv', 'glUniform2fv', 'glUniform3fv', 'glUniform4fv', 'glUniform1iv'):
    getattr(gl, name).argtypes = [I, I, P]
for name in ('glUniformMatrix2fv', 'glUniformMatrix3fv', 'glUniformMatrix4fv'):
    getattr(gl, name).argtypes = [I, I, ctypes.c_ubyte, P]


def make_context(w, h):
    d = EGL.eglGetDisplay(EGL.EGL_DEFAULT_DISPLAY)
    EGL.eglInitialize(d, ctypes.c_long(), ctypes.c_long())
    attrs = (EGL.EGLint * 13)(EGL.EGL_SURFACE_TYPE, EGL.EGL_PBUFFER_BIT, EGL.EGL_RENDERABLE_TYPE, EGL.EGL_OPENGL_ES2_BIT,
                              EGL.EGL_RED_SIZE, 8, EGL.EGL_ALPHA_SIZE, 8, EGL.EGL_DEPTH_SIZE, 24, EGL.EGL_STENCIL_SIZE, 8, EGL.EGL_NONE)
    cfg, n = (EGL.EGLConfig * 1)(), ctypes.c_long()
    EGL.eglChooseConfig(d, attrs, cfg, 1, n)
    EGL.eglBindAPI(EGL.EGL_OPENGL_ES_API)
    surf = EGL.eglCreatePbufferSurface(d, cfg[0], (EGL.EGLint * 5)(EGL.EGL_WIDTH, w, EGL.EGL_HEIGHT, h, EGL.EGL_NONE))
    ctx = EGL.eglCreateContext(d, cfg[0], EGL.EGL_NO_CONTEXT, (EGL.EGLint * 3)(EGL.EGL_CONTEXT_CLIENT_VERSION, 2, EGL.EGL_NONE))
    assert EGL.eglMakeCurrent(d, surf, surf, ctx)


def render_text(text, w, h, size):
    img = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    font = ImageFont.truetype(FONT, max(8, int(size * 0.92)))
    draw = ImageDraw.Draw(img)
    per = max(4, int(w / (size * 0.56)))
    lines = []
    for raw in text.split('\n'):
        lines += textwrap.wrap(raw, per) or ['']
    y = 2
    for line in lines:
        draw.text((2, y), line, font=font, fill=(255, 255, 255, 255))
        y += int(size * 1.35)
    return img.tobytes()


def records(path):
    with open(path, 'rb') as f:
        data = f.read()
    pos, end = 0, len(data)
    while pos < end:
        nl = data[pos]; pos += 1
        name = data[pos:pos + nl].decode(); pos += nl
        ni, nf = data[pos], data[pos + 1]; pos += 2
        (bl,) = struct.unpack_from('<I', data, pos); pos += 4
        ints = struct.unpack_from('<%dq' % ni, data, pos); pos += 8 * ni
        floats = struct.unpack_from('<%df' % nf, data, pos); pos += 4 * nf
        blob = data[pos:pos + bl]; pos += bl
        yield name, ints, floats, blob


M = 0xFFFFFFFF
tex, buf, fbo, rbo, shader, program = {0: 0}, {0: 0}, {0: 0}, {0: 0}, {}, {}
uniforms, uloc_cache = {}, {}
cur_program = [0]
bound_array, bound_fbo = [0], [0]
keep = {}
pending_text = []
frame = 0
W = H = 0
stats = {'draws_executed': 0, 'compile_errors': 0, 'link_errors': 0, 'gl_errors': 0}
shader_src = {}


def gen(fn):
    out = U(0)
    fn(1, ctypes.byref(out))
    return out.value


def uloc(fake):
    if fake not in uniforms:
        return -1
    key = (cur_program[0], fake)
    if key not in uloc_cache:
        prog, name = uniforms[fake]
        uloc_cache[key] = gl.glGetUniformLocation(program.get(prog, 0), name)
    return uloc_cache[key]


def on_screen():
    return bound_fbo[0] != 0 or frame in frames


def info_log(get_len, get_log, obj):
    n = I(0)
    get_len(obj, 0x8B84, ctypes.byref(n))
    b = ctypes.create_string_buffer(max(n.value, 1))
    get_log(obj, n.value, None, b)
    return b.value.decode(errors='replace')


simple = {'glActiveTexture', 'glDepthFunc', 'glStencilMask', 'glClearStencil', 'glBlendEquation', 'glCullFace', 'glFrontFace',
          'glGenerateMipmap', 'glBlendFunc', 'glHint', 'glBlendEquationSeparate', 'glTexParameteri', 'glStencilFunc', 'glStencilOp',
          'glViewport', 'glScissor', 'glBlendFuncSeparate', 'glPixelStorei', 'glEnable', 'glDisable',
          'glEnableVertexAttribArray', 'glDisableVertexAttribArray'}

for name, v, fl, blob in records(trace_path):
    if name == 'SIZE':
        W, H = v
        make_context(W, H)
        print('renderer:', ctypes.cast(gl.glGetString(0x1F01), ctypes.c_char_p).value if False else 'GLES2 context %dx%d' % (W, H))
    elif name == 'FRAME':
        if v[0] in frames:
            pix = ctypes.create_string_buffer(W * H * 4)
            gl.glBindFramebuffer(0x8D40, 0)
            gl.glReadPixels(0, 0, W, H, 0x1908, 0x1401, pix)
            gl.glBindFramebuffer(0x8D40, fbo.get(bound_fbo[0], 0))
            img = Image.frombytes('RGBA', (W, H), pix.raw).transpose(Image.FLIP_TOP_BOTTOM).convert('RGB')
            path = os.path.join(out_dir, 'frame_%05d.png' % v[0])
            img.save(path)
            print('saved', path, 'distinct colours:', len(img.resize((90, 160)).getcolors(20000) or []))
        frame = v[0] + 1
    elif name == 'TEXT':
        pending_text.append((v[0], v[1], v[2], blob.decode(errors='replace')))
    elif name in simple:
        getattr(gl, name)(*[x & M for x in v])
    elif name == 'glDepthMask':
        gl.glDepthMask(1 if v[0] & 0xFF else 0)
    elif name == 'glColorMask':
        gl.glColorMask(*[1 if x & 0xFF else 0 for x in v])
    elif name == 'glClear':
        if on_screen():
            gl.glClear(v[0] & M)
    elif name in ('glClearColor', 'glClearDepthf', 'glLineWidth', 'glPolygonOffset'):
        getattr(gl, name)(*fl)
    elif name == 'glTexParameterf':
        gl.glTexParameterf(v[0] & M, v[1] & M, fl[0])
    elif name == 'glGenTextures':
        tex[v[0]] = gen(gl.glGenTextures)
    elif name == 'glGenBuffers':
        buf[v[0]] = gen(gl.glGenBuffers)
    elif name == 'glGenFramebuffers':
        fbo[v[0]] = gen(gl.glGenFramebuffers)
    elif name == 'glGenRenderbuffers':
        rbo[v[0]] = gen(gl.glGenRenderbuffers)
    elif name == 'glDeleteTextures':
        r = U(tex.pop(v[0], 0)); gl.glDeleteTextures(1, ctypes.byref(r))
    elif name == 'glDeleteBuffers':
        r = U(buf.pop(v[0], 0)); gl.glDeleteBuffers(1, ctypes.byref(r))
    elif name == 'glDeleteFramebuffers':
        r = U(fbo.pop(v[0], 0)); gl.glDeleteFramebuffers(1, ctypes.byref(r))
    elif name == 'glDeleteRenderbuffers':
        r = U(rbo.pop(v[0], 0)); gl.glDeleteRenderbuffers(1, ctypes.byref(r))
    elif name == 'glBindTexture':
        gl.glBindTexture(v[0] & M, tex.get(v[1], 0))
    elif name == 'glBindBuffer':
        if v[0] == 0x8892:
            bound_array[0] = v[1]
        gl.glBindBuffer(v[0] & M, buf.get(v[1], 0))
    elif name == 'glBindFramebuffer':
        bound_fbo[0] = v[1]
        gl.glBindFramebuffer(v[0] & M, fbo.get(v[1], 0))
    elif name == 'glBindRenderbuffer':
        gl.glBindRenderbuffer(v[0] & M, rbo.get(v[1], 0))
    elif name == 'glFramebufferTexture2D':
        gl.glFramebufferTexture2D(v[0] & M, v[1] & M, v[2] & M, tex.get(v[3], 0), v[4])
    elif name == 'glFramebufferRenderbuffer':
        gl.glFramebufferRenderbuffer(v[0] & M, v[1] & M, v[2] & M, rbo.get(v[3], 0))
    elif name == 'glRenderbufferStorage':
        gl.glRenderbufferStorage(*[x & M for x in v])
    elif name == 'glBufferData':
        gl.glBufferData(v[0] & M, v[1], blob if v[3] else None, v[2] & M)
    elif name == 'glBufferSubData':
        gl.glBufferSubData(v[0] & M, v[1], v[2], blob)
    elif name == 'glTexImage2D':
        t, l, f, w, h, b, fmt, ty, has = v
        data = blob if has else None
        if pending_text and has and (pending_text[0][0], pending_text[0][1]) == (w, h) and fmt == 0x1908:
            pw, ph, size, text = pending_text.pop(0)
            data = render_text(text, pw, ph, size)
        gl.glTexImage2D(t & M, l, f, w, h, b, fmt & M, ty & M, data)
    elif name == 'glTexSubImage2D':
        gl.glTexSubImage2D(v[0] & M, v[1], v[2], v[3], v[4], v[5], v[6] & M, v[7] & M, blob)
    elif name == 'glCompressedTexImage2D':
        gl.glCompressedTexImage2D(v[0] & M, v[1], v[2] & M, v[3], v[4], v[5], v[6], blob)
    elif name == 'glCreateShader':
        shader[v[1]] = gl.glCreateShader(v[0] & M)
    elif name == 'glShaderSource':
        shader_src[v[0]] = blob
        src = ctypes.c_char_p(blob)
        n = I(len(blob))
        gl.glShaderSource(shader[v[0]], 1, ctypes.byref(src), ctypes.byref(n))
    elif name == 'glCompileShader':
        s = shader[v[0]]
        gl.glCompileShader(s)
        ok = I(0)
        gl.glGetShaderiv(s, 0x8B81, ctypes.byref(ok))
        if not ok.value:
            stats['compile_errors'] += 1
            print('shader compile failed:', info_log(gl.glGetShaderiv, gl.glGetShaderInfoLog, s)[:300])
    elif name == 'glCreateProgram':
        program[v[0]] = gl.glCreateProgram()
    elif name == 'glAttachShader':
        gl.glAttachShader(program[v[0]], shader[v[1]])
    elif name == 'glDetachShader':
        gl.glDetachShader(program.get(v[0], 0), shader.get(v[1], 0))
    elif name == 'glBindAttribLocation':
        gl.glBindAttribLocation(program[v[0]], v[1], blob)
    elif name == 'glLinkProgram':
        p = program[v[0]]
        gl.glLinkProgram(p)
        ok = I(0)
        gl.glGetProgramiv(p, 0x8B82, ctypes.byref(ok))
        if not ok.value:
            stats['link_errors'] += 1
            print('program link failed:', info_log(gl.glGetProgramiv, gl.glGetProgramInfoLog, p)[:300])
        uloc_cache.clear()
    elif name == 'glGetAttribLocation':
        p = program[v[0]]
        gl.glBindAttribLocation(p, v[1], blob)
        gl.glLinkProgram(p)
        uloc_cache.clear()
    elif name == 'glDeleteShader':
        gl.glDeleteShader(shader.get(v[0], 0))
    elif name == 'glDeleteProgram':
        gl.glDeleteProgram(program.pop(v[0], 0))
    elif name == 'glUseProgram':
        cur_program[0] = v[0]
        gl.glUseProgram(program.get(v[0], 0))
    elif name == 'glGetUniformLocation':
        uniforms[v[1]] = (v[0], blob)
    elif name == 'glUniform1i':
        gl.glUniform1i(uloc(v[0]), v[1])
    elif name == 'glUniform2i':
        gl.glUniform2i(uloc(v[0]), v[1], v[2])
    elif name in ('glUniform1f', 'glUniform2f', 'glUniform3f', 'glUniform4f'):
        getattr(gl, name)(uloc(v[0]), *fl)
    elif name in ('glUniform1fv', 'glUniform2fv', 'glUniform3fv', 'glUniform4fv', 'glUniform1iv'):
        getattr(gl, name)(uloc(v[0]), v[1], blob)
    elif name in ('glUniformMatrix2fv', 'glUniformMatrix3fv', 'glUniformMatrix4fv'):
        getattr(gl, name)(uloc(v[0]), v[1], 1 if v[2] & 0xFF else 0, blob)
    elif name == 'glVertexAttribPointer':
        gl.glVertexAttribPointer(v[0], v[1], v[2] & M, v[3] & 0xFF, v[4], v[5])
    elif name == 'CLIENT_ATTRIB':
        keep[v[0]] = ctypes.create_string_buffer(blob, len(blob) or 1)
        gl.glBindBuffer(0x8892, 0)
        gl.glVertexAttribPointer(v[0], v[1], v[2] & M, v[3] & 0xFF, v[4], ctypes.cast(keep[v[0]], P))
        gl.glBindBuffer(0x8892, buf.get(bound_array[0], 0))
    elif name == 'glDrawArrays':
        if on_screen():
            gl.glDrawArrays(v[0] & M, v[1], v[2]); stats['draws_executed'] += 1
    elif name == 'glDrawElements':
        if on_screen():
            if v[4]:
                gl.glDrawElements(v[0] & M, v[1], v[2] & M, v[3])
            else:
                ib = ctypes.create_string_buffer(blob, len(blob) or 1)
                gl.glDrawElements(v[0] & M, v[1], v[2] & M, ctypes.cast(ib, P))
            stats['draws_executed'] += 1
    else:
        stats.setdefault('unhandled:' + name, 0)
        stats['unhandled:' + name] += 1
    if name.startswith('gl') and frame in frames:
        e = gl.glGetError()
        if e:
            stats['gl_errors'] += 1
            if stats['gl_errors'] <= 8:
                print('GL error 0x%x after %s %s' % (e, name, v))
print(stats)
