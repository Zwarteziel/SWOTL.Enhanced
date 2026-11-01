/* SWOTL Enhanced - OpenGL presentation with CRT shaders. GPL-3.0-or-later.
 *
 * The CRT shaders come unchanged from DOSBox Staging 0.83.0 (MIT licensed,
 * see third_party/dosbox-staging-shaders). The automatic choice between them
 * follows the rules of DOSBox Staging's 'shader = crt-auto' for VGA. */
#ifndef SWOTL_CRT_GL_H
#define SWOTL_CRT_GL_H
#include <SDL3/SDL.h>
#include <stdint.h>

enum {
    SHADER_NONE = 0,     /* plain scaling, see scale mode */
    SHADER_CRT_AUTO,     /* pick the CRT variant from the window size */
    SHADER_CRT_1080P,    /* VGA for 1080p: fake double scan (320x200) */
    SHADER_CRT_1440P,    /* crt-hyllian, vga-1440p preset */
    SHADER_CRT_4K        /* crt-hyllian, vga-4k preset */
};

typedef struct CrtGL CrtGL;

/* Call before creating the window with SDL_WINDOW_OPENGL. */
void crtgl_set_attributes(void);
/* Creates an OpenGL 3.3 context on a window made with SDL_WINDOW_OPENGL and
 * builds all shaders. Returns NULL (and a reason in err) on failure. */
CrtGL *crtgl_create(SDL_Window *win, char *err, int errlen);
void crtgl_destroy(CrtGL *g);

/* Draws one frame of the game (RGBA bytes, w x h) to the window.
 * shader: SHADER_*; scale_mode: 0 nearest, 1 linear, 2 sharp (used without
 * CRT shader, and when the window is too small for one); aspect: 1 = 4:3.
 * dst receives the picture rectangle in window pixels. Returns a short name
 * of the variant in use. */
const char *crtgl_frame(CrtGL *g, const uint32_t *rgba, int w, int h, int shader,
                        int scale_mode, int aspect, int integer_only, SDL_FRect *dst);
/* Draws an RGBA overlay (with alpha) at window pixel x,y, scaled up. */
void crtgl_overlay(CrtGL *g, const uint32_t *rgba, int w, int h, int x, int y, int scale);
void crtgl_output_size(CrtGL *g, int *w, int *h);
/* Reads the window back (RGBA, top row first) before crtgl_swap; for tests. */
void crtgl_read(CrtGL *g, uint8_t *rgba, int w, int h);
void crtgl_swap(CrtGL *g);
const char *crtgl_info(CrtGL *g);

#endif
