/* SWOTL Enhanced - OpenGL presentation with CRT shaders. GPL-3.0-or-later.
 *
 * Pipeline for the CRT look, the same as DOSBox Staging 0.83 uses for VGA:
 *   game frame (320x200) --image adjustments--> render size texture
 *   (640x400 double scanned, or 640x200 for the 1080p fake double scan
 *   shader) --CRT shader--> picture rectangle in the window.
 * The image adjustment pass emulates the colours of a VGA monitor (P22
 * phosphors, 7800 K white point) with DOSBox Staging's default brightness and
 * contrast. Without a CRT shader the frame is scaled directly (nearest,
 * linear or sharp). */
#include "crt_gl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <math.h>

#include "shaders_embedded.inc"

/* ---- minimal OpenGL declarations (loaded through SDL at run time) ---- */
typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef float GLfloat;
typedef char GLchar;
typedef unsigned char GLubyte, GLboolean;
typedef ptrdiff_t GLsizeiptr;
#if defined(_WIN32) && !defined(_WIN64)
#define GLCC __stdcall
#else
#define GLCC
#endif

#define GL_TRIANGLES 0x0004
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_BLEND 0x0BE2
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_TEXTURE_2D 0x0DE1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_RGBA 0x1908
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_RGBA8 0x8058
#define GL_CLAMP_TO_BORDER 0x812D
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_TEXTURE0 0x84C0
#define GL_RGBA16F 0x881A
#define GL_ARRAY_BUFFER 0x8892
#define GL_STATIC_DRAW 0x88E4
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER 0x8D40

#define GL_FUNCS(X) \
    X(const GLubyte *, GetString, (GLenum)) \
    X(void, Viewport, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, Clear, (GLbitfield)) \
    X(void, GenTextures, (GLsizei, GLuint *)) \
    X(void, DeleteTextures, (GLsizei, const GLuint *)) \
    X(void, BindTexture, (GLenum, GLuint)) \
    X(void, TexParameteri, (GLenum, GLenum, GLint)) \
    X(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(void, TexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
    X(void, PixelStorei, (GLenum, GLint)) \
    X(void, ActiveTexture, (GLenum)) \
    X(void, GenFramebuffers, (GLsizei, GLuint *)) \
    X(void, DeleteFramebuffers, (GLsizei, const GLuint *)) \
    X(void, BindFramebuffer, (GLenum, GLuint)) \
    X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(GLenum, CheckFramebufferStatus, (GLenum)) \
    X(GLuint, CreateShader, (GLenum)) \
    X(void, ShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    X(void, CompileShader, (GLuint)) \
    X(void, GetShaderiv, (GLuint, GLenum, GLint *)) \
    X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void, DeleteShader, (GLuint)) \
    X(GLuint, CreateProgram, (void)) \
    X(void, AttachShader, (GLuint, GLuint)) \
    X(void, BindAttribLocation, (GLuint, GLuint, const GLchar *)) \
    X(void, LinkProgram, (GLuint)) \
    X(void, GetProgramiv, (GLuint, GLenum, GLint *)) \
    X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void, DeleteProgram, (GLuint)) \
    X(void, UseProgram, (GLuint)) \
    X(GLint, GetUniformLocation, (GLuint, const GLchar *)) \
    X(void, Uniform1i, (GLint, GLint)) \
    X(void, Uniform1f, (GLint, GLfloat)) \
    X(void, Uniform2f, (GLint, GLfloat, GLfloat)) \
    X(void, Uniform3f, (GLint, GLfloat, GLfloat, GLfloat)) \
    X(void, GenVertexArrays, (GLsizei, GLuint *)) \
    X(void, BindVertexArray, (GLuint)) \
    X(void, DeleteVertexArrays, (GLsizei, const GLuint *)) \
    X(void, GenBuffers, (GLsizei, GLuint *)) \
    X(void, BindBuffer, (GLenum, GLuint)) \
    X(void, BufferData, (GLenum, GLsizeiptr, const void *, GLenum)) \
    X(void, DeleteBuffers, (GLsizei, const GLuint *)) \
    X(void, EnableVertexAttribArray, (GLuint)) \
    X(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(void, DrawArrays, (GLenum, GLint, GLsizei)) \
    X(void, Enable, (GLenum)) \
    X(void, Disable, (GLenum)) \
    X(void, BlendFunc, (GLenum, GLenum)) \
    X(void, ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))

#define X(ret, name, args) typedef ret (GLCC *PFN_gl##name) args; static PFN_gl##name p_gl##name;
GL_FUNCS(X)
#undef X

/* ---- our own small shader: plain / sharp scaling and overlays ---- */
static const char SH_SIMPLE[] =
    "#version 330 core\n"
    "#if defined(VERTEX)\n"
    "layout (location = 0) in vec2 a_position;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    gl_Position = vec4(a_position, 0.0, 1.0);\n"
    "    v_uv = vec2(a_position.x + 1.0, 1.0 - a_position.y) / 2.0;\n"
    "}\n"
    "#elif defined(FRAGMENT)\n"
    "in vec2 v_uv;\n"
    "out vec4 FragColor;\n"
    "uniform sampler2D tex;\n"
    "uniform vec2 tex_size;\n"
    "/* 0 = sample as is; k > 0 = sharp: the result of a nearest k-times\n"
    " * upscale that is then scaled bilinearly to the output size */\n"
    "uniform float prescale;\n"
    "void main() {\n"
    "    vec2 uv = v_uv;\n"
    "    if (prescale > 0.0) {\n"
    "        vec2 p  = uv * tex_size * prescale - 0.5;\n"
    "        vec2 i0 = floor(p);\n"
    "        vec2 f  = p - i0;\n"
    "        vec2 a  = floor(i0 / prescale);\n"
    "        vec2 b  = floor((i0 + 1.0) / prescale);\n"
    "        uv = (a + 0.5 + (b - a) * f) / tex_size;\n"
    "    }\n"
    "    FragColor = texture(tex, uv);\n"
    "}\n"
    "#endif\n";

#define MAX_PARAMS 24
typedef struct {
    GLuint prog;
    int nparam;
    char pname[MAX_PARAMS][32];
    float pdef[MAX_PARAMS];
} Prog;

enum { P_SIMPLE, P_IMAGE_ADJ, P_HYLLIAN, P_VGA1080, P_VGA1080_FDS, P_COUNT };

struct CrtGL {
    SDL_Window *win;
    SDL_GLContext ctx;
    Prog p[P_COUNT];
    GLuint vao, vbo;
    GLuint src_tex; int src_w, src_h;
    GLuint ia_tex, ia_fbo; int ia_w, ia_h;
    GLuint ovl_tex; int ovl_w, ovl_h;
    char info[256];
};

/* Parameter values of DOSBox Staging's crt-hyllian presets for VGA
 * (resources/shader-presets/crt/crt-hyllian/vga-1440p and vga-4k). */
static const char *hyl_names[] = {
    "BEAM_PROFILE", "HFILTER_PROFILE", "BEAM_MIN_WIDTH", "BEAM_MAX_WIDTH",
    "SCANLINES_STRENGTH", "COLOR_BOOST", "SHARPNESS_HACK", "PHOSPHOR_LAYOUT",
    "MASK_INTENSITY", "CRT_ANTI_RINGING", "INPUT_GAMMA", "OUTPUT_GAMMA"};
static const float hyl_vga1440p[] = {0, 0, 0.95f, 1.30f, 0.85f, 2.50f, 1, 2, 0.55f, 1, 2.40f, 2.48f};
static const float hyl_vga4k[] = {0, 0, 1.00f, 1.40f, 0.90f, 3.65f, 1, 11, 0.65f, 1, 2.40f, 2.48f};

static void parse_params(Prog *p, const char *src) {
    const char *s = src;
    p->nparam = 0;
    while ((s = strstr(s, "#pragma parameter")) && p->nparam < MAX_PARAMS) {
        s += strlen("#pragma parameter");
        char name[32]; float def;
        const char *q1 = strchr(s, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
        if (!q2 || sscanf(s, " %31s", name) != 1 || sscanf(q2 + 1, " %f", &def) != 1) continue;
        snprintf(p->pname[p->nparam], sizeof p->pname[0], "%s", name);
        p->pdef[p->nparam++] = def;
    }
}

static GLuint compile(GLenum type, const char *src, char *err, int errlen, const char *what) {
    char top[160];
    const char *body = src;
    const char *ver = strstr(src, "#version ");
    if (ver) {
        const char *e = strchr(ver, '\n');
        size_t n = (size_t)(e - src + 1);
        if (n >= sizeof top - 32) n = sizeof top - 32;
        memcpy(top, src, n); top[n] = 0;
        body = e + 1;
    } else {
        snprintf(top, sizeof top, "#version 330 core\n");
    }
    strcat(top, type == GL_VERTEX_SHADER ? "#define VERTEX 1\n" : "#define FRAGMENT 1\n");
    const char *srcs[2] = {top, body};
    GLuint sh = p_glCreateShader(type);
    p_glShaderSource(sh, 2, srcs, NULL);
    p_glCompileShader(sh);
    GLint ok = 0;
    p_glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = "";
        p_glGetShaderInfoLog(sh, sizeof log, NULL, log);
        snprintf(err, (size_t)errlen, "%s %s shader: %s", what, type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        p_glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static int build(Prog *p, const char *src, char *err, int errlen, const char *what) {
    GLuint vs = compile(GL_VERTEX_SHADER, src, err, errlen, what);
    if (!vs) return 0;
    GLuint fs = compile(GL_FRAGMENT_SHADER, src, err, errlen, what);
    if (!fs) { p_glDeleteShader(vs); return 0; }
    p->prog = p_glCreateProgram();
    p_glAttachShader(p->prog, vs);
    p_glAttachShader(p->prog, fs);
    p_glBindAttribLocation(p->prog, 0, "a_position");
    p_glLinkProgram(p->prog);
    p_glDeleteShader(vs);
    p_glDeleteShader(fs);
    GLint ok = 0;
    p_glGetProgramiv(p->prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = "";
        p_glGetProgramInfoLog(p->prog, sizeof log, NULL, log);
        snprintf(err, (size_t)errlen, "%s link: %s", what, log);
        return 0;
    }
    parse_params(p, src);
    return 1;
}

static void set1f(GLuint prog, const char *name, float v) {
    GLint loc = p_glGetUniformLocation(prog, name);
    if (loc >= 0) p_glUniform1f(loc, v);
}
static void set1i(GLuint prog, const char *name, int v) {
    GLint loc = p_glGetUniformLocation(prog, name);
    if (loc >= 0) p_glUniform1i(loc, v);
}
static void set2f(GLuint prog, const char *name, float x, float y) {
    GLint loc = p_glGetUniformLocation(prog, name);
    if (loc >= 0) p_glUniform2f(loc, x, y);
}
static void set3f(GLuint prog, const char *name, float x, float y, float z) {
    GLint loc = p_glGetUniformLocation(prog, name);
    if (loc >= 0) p_glUniform3f(loc, x, y, z);
}

static void tex_params(GLuint tex, int linear, int border) {
    p_glBindTexture(GL_TEXTURE_2D, tex);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, border ? GL_CLAMP_TO_BORDER : GL_CLAMP_TO_EDGE);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, border ? GL_CLAMP_TO_BORDER : GL_CLAMP_TO_EDGE);
}

void crtgl_destroy(CrtGL *g) {
    if (!g) return;
    if (g->ctx) {
        SDL_GL_MakeCurrent(g->win, g->ctx);
        if (p_glDeleteProgram) for (int i = 0; i < P_COUNT; i++) if (g->p[i].prog) p_glDeleteProgram(g->p[i].prog);
        if (p_glDeleteTextures) {
            if (g->src_tex) p_glDeleteTextures(1, &g->src_tex);
            if (g->ia_tex) p_glDeleteTextures(1, &g->ia_tex);
            if (g->ovl_tex) p_glDeleteTextures(1, &g->ovl_tex);
        }
        if (g->ia_fbo && p_glDeleteFramebuffers) p_glDeleteFramebuffers(1, &g->ia_fbo);
        if (g->vbo && p_glDeleteBuffers) p_glDeleteBuffers(1, &g->vbo);
        if (g->vao && p_glDeleteVertexArrays) p_glDeleteVertexArrays(1, &g->vao);
        SDL_GL_MakeCurrent(g->win, NULL);
        SDL_GL_DestroyContext(g->ctx);
    }
    free(g);
}

void crtgl_set_attributes(void) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
}

CrtGL *crtgl_create(SDL_Window *win, char *err, int errlen) {
    CrtGL *g = calloc(1, sizeof *g);
    if (!g) return NULL;
    g->win = win;
    snprintf(err, (size_t)errlen, "unknown error");
    g->ctx = SDL_GL_CreateContext(win);
    if (!g->ctx) { snprintf(err, (size_t)errlen, "no OpenGL 3.3 context: %s", SDL_GetError()); goto fail; }
    if (!SDL_GL_MakeCurrent(win, g->ctx)) { snprintf(err, (size_t)errlen, "SDL_GL_MakeCurrent: %s", SDL_GetError()); goto fail; }
#define X(ret, name, args) \
    p_gl##name = (PFN_gl##name)SDL_GL_GetProcAddress("gl" #name); \
    if (!p_gl##name) { snprintf(err, (size_t)errlen, "OpenGL function gl" #name " missing"); goto fail; }
    GL_FUNCS(X)
#undef X
    snprintf(g->info, sizeof g->info, "%s | %s", (const char *)p_glGetString(GL_RENDERER), (const char *)p_glGetString(GL_VERSION));
    if (!build(&g->p[P_SIMPLE], SH_SIMPLE, err, errlen, "scaling")) goto fail;
    if (!build(&g->p[P_IMAGE_ADJ], SH_IMAGE_ADJ, err, errlen, "image-adjustments")) goto fail;
    if (!build(&g->p[P_HYLLIAN], SH_CRT_HYLLIAN, err, errlen, "crt-hyllian")) goto fail;
    if (!build(&g->p[P_VGA1080], SH_VGA_1080P, err, errlen, "vga-1080p")) goto fail;
    if (!build(&g->p[P_VGA1080_FDS], SH_VGA_1080P_FDS, err, errlen, "vga-1080p-fake-double-scan")) goto fail;

    static const float tri[6] = {-1, -1, 3, -1, -1, 3};
    p_glGenVertexArrays(1, &g->vao);
    p_glBindVertexArray(g->vao);
    p_glGenBuffers(1, &g->vbo);
    p_glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
    p_glBufferData(GL_ARRAY_BUFFER, sizeof tri, tri, GL_STATIC_DRAW);
    p_glEnableVertexAttribArray(0);
    p_glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, NULL);
    p_glGenTextures(1, &g->src_tex);
    p_glGenTextures(1, &g->ia_tex);
    p_glGenTextures(1, &g->ovl_tex);
    p_glGenFramebuffers(1, &g->ia_fbo);
    SDL_GL_SetSwapInterval(1);
    err[0] = 0;
    return g;
fail:
    crtgl_destroy(g);
    return NULL;
}

const char *crtgl_info(CrtGL *g) { return g->info; }

void crtgl_output_size(CrtGL *g, int *w, int *h) { SDL_GetWindowSizeInPixels(g->win, w, h); }

static void draw_tri(CrtGL *g) {
    p_glBindVertexArray(g->vao);
    p_glDrawArrays(GL_TRIANGLES, 0, 3);
}

/* plain or sharp scaling of 'tex' (tw x th) into the window rectangle */
static void draw_simple(CrtGL *g, GLuint tex, int tw, int th, int linear, float prescale,
                        int x, int y, int w, int h, int oh) {
    GLuint pr = g->p[P_SIMPLE].prog;
    p_glUseProgram(pr);
    p_glActiveTexture(GL_TEXTURE0);
    tex_params(tex, linear, 0);
    set1i(pr, "tex", 0);
    set2f(pr, "tex_size", (float)tw, (float)th);
    set1f(pr, "prescale", prescale);
    p_glViewport(x, oh - y - h, w, h);
    draw_tri(g);
}

const char *crtgl_frame(CrtGL *g, const uint32_t *rgba, int w, int h, int shader,
                        int scale_mode, int aspect, int integer_only, SDL_FRect *dst) {
    int ow, oh;
    SDL_GetWindowSizeInPixels(g->win, &ow, &oh);
    if (ow < 1 || oh < 1) return "";

    /* upload the game frame */
    p_glActiveTexture(GL_TEXTURE0);
    p_glBindTexture(GL_TEXTURE_2D, g->src_tex);
    p_glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    p_glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    if (w != g->src_w || h != g->src_h) {
        p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        g->src_w = w; g->src_h = h;
    } else {
        p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }

    /* choose the variant, as DOSBox Staging's crt-auto does for VGA: the CRT
     * shaders need at least 2 window pixels per double-scanned line (3 for
     * real scanline emulation); otherwise it falls back to plain scaling */
    double dar = aspect ? 4.0 / 3.0 : 1.6;
    double fit_h = SDL_min((double)oh, ow / dar);
    int dbl = h <= 240;                      /* double-scanned on a VGA monitor */
    int rw = w < 640 ? w * 2 : w;            /* render size with pixel doubling */
    int rh = dbl ? h * 2 : h;
    int fds_ok = dbl && h <= 1080 / 5;
    enum { V_PLAIN, V_FDS, V_1080, V_1440, V_4K } v = V_PLAIN;
    switch (shader) {
    case SHADER_CRT_AUTO: {
        int pps = (int)floor(fit_h / rh + 1e-6);
        v = pps >= 4 ? V_4K : pps >= 3 ? V_1440 : pps >= 2 ? (fds_ok ? V_FDS : V_1080) : V_PLAIN;
        break;
    }
    case SHADER_CRT_1080P: v = fds_ok ? V_FDS : V_1080; break;
    case SHADER_CRT_1440P: v = V_1440; break;
    case SHADER_CRT_4K: v = V_4K; break;
    default: v = V_PLAIN; break;
    }

    /* picture rectangle */
    double dw, dh;
    if (v != V_PLAIN) {
        /* DOSBox 'integer_scaling = auto' for CRT shaders: whole steps below
         * 3x, half steps from 3x to 5x, free scaling above 5x */
        int mrh = v == V_FDS ? h : rh;
        double f = fit_h / mrh;
        if (f >= 5.0) { /* free */ }
        else if (f >= 3.0) f = floor(f * 2) / 2;
        else if (f >= 1.0) f = floor(f);
        dh = mrh * f; dw = dh * dar;
    } else {
        double src_w = w >= 640 ? 640 : 320;
        double src_h = aspect ? src_w * 3 / 4 : src_w / 1.6;
        double s = SDL_min(ow / src_w, oh / src_h);
        if (integer_only && s >= 1) s = floor(s);
        dw = src_w * s; dh = src_h * s;
    }
    int dwi = (int)(dw + 0.5), dhi = (int)(dh + 0.5);
    int dx = (ow - dwi) / 2, dy = (oh - dhi) / 2;
    if (dst) { dst->x = (float)dx; dst->y = (float)dy; dst->w = (float)dwi; dst->h = (float)dhi; }

    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    p_glViewport(0, 0, ow, oh);
    p_glClearColor(0, 0, 0, 1);
    p_glClear(GL_COLOR_BUFFER_BIT);

    if (v == V_PLAIN) {
        float k = 0;
        if (scale_mode == 2) {
            k = (float)ceil(SDL_max((double)dwi / w, (double)dhi / h));
            if (k < 1) k = 1;
            if (k > 8) k = 8;
        }
        draw_simple(g, g->src_tex, w, h, scale_mode != 0, k, dx, dy, dwi, dhi, oh);
        return scale_mode == 0 ? "nearest" : scale_mode == 1 ? "linear" : "sharp";
    }

    /* pass 1: image adjustments, into a texture at render size */
    int iw = rw, ih = v == V_FDS ? h : rh;
    if (iw != g->ia_w || ih != g->ia_h) {
        p_glBindTexture(GL_TEXTURE_2D, g->ia_tex);
        p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, iw, ih, 0, GL_RGBA, GL_FLOAT, NULL);
        p_glBindFramebuffer(GL_FRAMEBUFFER, g->ia_fbo);
        p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g->ia_tex, 0);
        g->ia_w = iw; g->ia_h = ih;
    }
    p_glBindFramebuffer(GL_FRAMEBUFFER, g->ia_fbo);
    p_glViewport(0, 0, iw, ih);
    GLuint pr = g->p[P_IMAGE_ADJ].prog;
    p_glUseProgram(pr);
    p_glActiveTexture(GL_TEXTURE0);
    tex_params(g->src_tex, 0, 1);
    set1i(pr, "INPUT_TEXTURE_0", 0);
    set2f(pr, "INPUT_SIZE_0", (float)w, (float)h);
    set2f(pr, "OUTPUT_SIZE", (float)iw, (float)ih);
    set1i(pr, "COLOR_SPACE", 0);            /* sRGB */
    set1i(pr, "ENABLE_ADJUSTMENTS", 1);
    set1i(pr, "COLOR_PROFILE", 2);          /* P22 phosphors */
    set1f(pr, "BRIGHTNESS", 45.0f);         /* DOSBox Staging defaults */
    set1f(pr, "CONTRAST", 65.0f);
    set1f(pr, "GAMMA", 0.0f);
    set1f(pr, "DIGITAL_CONTRAST", 0.0f);
    set3f(pr, "BLACK_LEVEL_COLOR", 0, 0, 0);
    set1f(pr, "BLACK_LEVEL", 0.0f);         /* VGA: no raised black level */
    set1f(pr, "SATURATION", 0.0f);
    set1f(pr, "COLOR_TEMPERATURE_KELVIN", 7800.0f); /* typical VGA monitor */
    set1f(pr, "COLOR_TEMPERATURE_LUMA_PRESERVE", 0.0f);
    set1f(pr, "RED_GAIN", 1.0f);
    set1f(pr, "GREEN_GAIN", 1.0f);
    set1f(pr, "BLUE_GAIN", 1.0f);
    draw_tri(g);

    /* pass 2: the CRT shader, into the picture rectangle */
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    p_glViewport(dx, oh - dy - dhi, dwi, dhi);
    Prog *P = &g->p[v == V_FDS ? P_VGA1080_FDS : v == V_1080 ? P_VGA1080 : P_HYLLIAN];
    pr = P->prog;
    p_glUseProgram(pr);
    p_glActiveTexture(GL_TEXTURE0);
    tex_params(g->ia_tex, 0, 1);
    set1i(pr, "INPUT_TEXTURE_0", 0);
    set2f(pr, "INPUT_SIZE_0", (float)iw, (float)ih);
    set2f(pr, "OUTPUT_SIZE", (float)dwi, (float)dhi);
    for (int i = 0; i < P->nparam; i++) set1f(pr, P->pname[i], P->pdef[i]);
    if (v == V_1440 || v == V_4K) {
        const float *pv = v == V_4K ? hyl_vga4k : hyl_vga1440p;
        for (int i = 0; i < (int)(sizeof hyl_names / sizeof hyl_names[0]); i++) set1f(pr, hyl_names[i], pv[i]);
    }
    draw_tri(g);
    return v == V_FDS ? "CRT VGA 1080p" : v == V_1080 ? "CRT VGA 1080p (2x)" : v == V_1440 ? "CRT VGA 1440p" : "CRT VGA 4K";
}

void crtgl_overlay(CrtGL *g, const uint32_t *rgba, int w, int h, int x, int y, int scale) {
    int ow, oh;
    SDL_GetWindowSizeInPixels(g->win, &ow, &oh);
    p_glActiveTexture(GL_TEXTURE0);
    p_glBindTexture(GL_TEXTURE_2D, g->ovl_tex);
    p_glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    g->ovl_w = w; g->ovl_h = h;
    p_glEnable(GL_BLEND);
    p_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    draw_simple(g, g->ovl_tex, w, h, 0, 0, x, y, w * scale, h * scale, oh);
    p_glDisable(GL_BLEND);
}

void crtgl_read(CrtGL *g, uint8_t *rgba, int w, int h) {
    (void)g;
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    p_glPixelStorei(GL_PACK_ALIGNMENT, 1);
    p_glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    /* flip to top row first */
    uint8_t *tmp = malloc((size_t)w * 4);
    for (int y = 0; y < h / 2; y++) {
        uint8_t *a = rgba + (size_t)y * w * 4, *b = rgba + (size_t)(h - 1 - y) * w * 4;
        memcpy(tmp, a, (size_t)w * 4); memcpy(a, b, (size_t)w * 4); memcpy(b, tmp, (size_t)w * 4);
    }
    free(tmp);
}

void crtgl_swap(CrtGL *g) { SDL_GL_SwapWindow(g->win); }
