/* SWOTL Enhanced - experimental high-resolution 3D view. GPL-3.0-or-later.
 *
 * Addresses below are offsets in the resident code segment of SWOTL.EXE of
 * the later release (LucasArts Air Combat Classics). They are only used after
 * the bytes at each of them have been checked. Notes on how they were found
 * are in the project documentation.
 *
 *   6667  projection: [22BA],[22BE],[22C2] -> screen x [22DE] (32 bit) and
 *         y [22E2] (32 bit, pointing up). Focal length 180 is loaded as an
 *         immediate at 6667 and 669B. Called from 663C, 8A88 and 8C68.
 *   7FB2  adds one polygon edge to the edge table of the scanline renderer;
 *         end points in [2366]/[2364], [23FF]/[23FD], [236A]/[2368],
 *         [236E]/[236C]; colour [23D9]; polygon number [252F].
 *   7621  scanline renderer: draws all edges added since the last call.
 *   7B42  draws one polygon directly (houses, bridges); vertex list (x32,
 *         y32) at DS:arg0, count = arg2 >> 4; at 7BBF it is known to be drawn.
 *   8CEC  draws a line (roads) from vertex arg0 to arg0+1 of the list at
 *         DS:2445; colour in [252B] at 8D26.
 *   [2591] height of the 3D view in rows, [2593] row of y = 0 minus one. */
#include "hires.h"
#include "machine.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MAX_EDGES 4096
#define MAX_VERTS 8192
#define MAX_ITEMS 2048
#define HASH_SIZE 4096
#define VIEW_MAX 200

enum { IT_GROUND, IT_LINE, IT_POLY };

typedef struct { float x1, y1, x2, y2; int32_t ix1, iy1, ix2, iy2; uint16_t ord; uint8_t col; } HEdge;
typedef struct { uint8_t type, col; uint16_t n; uint32_t first; float t0, t1; float sa, sb; } HItem;
typedef struct { float x, y; } HVert;

typedef struct {
    HEdge edges[MAX_EDGES]; int ne;
    HVert verts[MAX_VERTS]; int nv;
    HItem items[MAX_ITEMS]; int ni;
    uint8_t buf[320 * VIEW_MAX];
    int vh;              /* rows of the 3D view */
    float bot;           /* buffer row = bot - y */
    double time_us;
    uint64_t serial;
    /* pixels written by the scanline renderer (the visible ground), at the
     * game's resolution, for buffer segment gseg */
    uint8_t gmask[320 * VIEW_MAX], gval[320 * VIEW_MAX];
    uint16_t gseg;
    int gok;             /* gmask/gval belong to this frame's buffer */
    /* pixels written by the line and polygon routines, and the colour */
    uint8_t lmask[320 * VIEW_MAX], lval[320 * VIEW_MAX];
    /* pixels of the 3D view the screen shows (not covered by the cockpit),
     * taken once the game has copied the whole frame to the screen */
    uint8_t vmask[320 * VIEW_MAX];
} HFrame;

typedef struct { int32_t x, y; float fx, fy; uint32_t gen; } HSlot;

struct Hires {
    int found;           /* 1 = routines found, -1 = not supported, 0 = not yet */
    uint16_t cs;
    uint16_t ds;
    uint8_t bpmap[8192];
    HFrame fr[3];
    int cur;             /* frame being captured */
    int pend;            /* captured, being copied to the screen, -1 = none */
    int ready;           /* last frame fully on the screen, -1 = none */
    int pass_start;      /* first edge of the current ground pass */
    int pending_poly;    /* item index of a polygon waiting for 7BBF */
    int pending_line;    /* item index of a line waiting for its colour */
    HSlot hash[HASH_SIZE];
    uint32_t gen;
    uint8_t save_vars[0x600], save_stack[512], save_code[12];
    uint64_t serial;
    /* compose scratch; hidx holds the redrawn view of frame hidx_serial */
    uint8_t *hidx; size_t hidx_size;
    uint64_t hidx_serial; int hidx_n;
    uint8_t skip[MAX_ITEMS];
    uint16_t bufseg[2];  /* the two 3D buffers the game alternates between */
    uint8_t wmap[320 * VIEW_MAX]; /* CPU write tracking map */
    uint8_t vprev[320 * VIEW_MAX];   /* screen mask of the previous ready frame */
    /* pixels of the screen the game's present routine writes: the 3D view
     * (the cockpit is never written by it), collected over the frames */
    uint8_t pwmap[320 * VIEW_MAX], vacc[320 * VIEW_MAX];
    /* pixels where the screen has differed from the 3D frame (cockpit) */
    uint8_t nonview[320 * VIEW_MAX];
    int vacc_vh, pw_rows;
    uint16_t wseg; int wrows;
    /* the line or polygon being drawn: its region of the buffer before */
    int line_item, lx0, lx1, ly0, ly1; uint16_t lseg; int lrows;
    uint8_t kind1[320 * VIEW_MAX], base1[320 * VIEW_MAX], exp1[320 * VIEW_MAX], bg1[320 * VIEW_MAX], other1[320 * VIEW_MAX];
};

static int dbg_on(void);

static const struct { uint16_t off; uint8_t len; uint8_t b[12]; } sigs[] = {
    {0x6667, 10, {0xC7, 0x06, 0xD8, 0x22, 0xB4, 0x00, 0xA1, 0xBA, 0x22, 0xE8}},
    {0x669B, 9, {0xC7, 0x06, 0xD8, 0x22, 0xB4, 0x00, 0xA1, 0xBE, 0x22}},
    {0x663C, 3, {0xE8, 0x28, 0x00}},
    {0x8A88, 3, {0xE8, 0xDC, 0xDB}},
    {0x8C68, 3, {0xE8, 0xFC, 0xD9}},
    {0x7FB2, 8, {0xF7, 0x06, 0x6C, 0x23, 0xFF, 0xFF, 0x78, 0x0C}},
    {0x7621, 11, {0x55, 0x8B, 0xEC, 0x57, 0x56, 0x06, 0x33, 0xC0, 0xA3, 0x31, 0x25}},
    {0x7B42, 12, {0x55, 0x8B, 0xEC, 0x57, 0x56, 0x8B, 0x46, 0x08, 0xF7, 0x06, 0x81, 0x25}},
    {0x7BB4, 11, {0xF7, 0x06, 0x0D, 0x24, 0xFF, 0xFF, 0x79, 0x03, 0xE9, 0xF3, 0x02}},
    {0x8CEC, 12, {0x55, 0x8B, 0xEC, 0x57, 0x56, 0x8B, 0x5E, 0x08, 0xF7, 0x06, 0x81, 0x25}},
    {0x8D26, 3, {0xBE, 0x45, 0x24}},
    {0x7693, 8, {0x07, 0x5E, 0x5F, 0xA1, 0xFF, 0x23, 0x5D, 0xCB}},
    {0x911F, 4, {0x5E, 0x5F, 0x5D, 0xCB}},
    {0x7EB2, 4, {0x5E, 0x5F, 0x5D, 0xCB}},
    {0x7B62, 4, {0x8E, 0x06, 0x79, 0x25}},
    {0x8D38, 4, {0x8E, 0x06, 0x79, 0x25}},
};
static const uint16_t bps[] = {0x663F, 0x8A8B, 0x8C6B, 0x7FB2, 0x7621, 0x7B42, 0x7BBF, 0x8CEC, 0x8D26, 0x769A, 0x9122, 0x7EB5};

Hires *hires_create(void) {
    Hires *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->ready = h->pend = -1;
    h->line_item = -1;
    h->gen = 1;
    h->pending_poly = h->pending_line = -1;
    for (size_t i = 0; i < sizeof bps / sizeof bps[0]; i++) h->bpmap[bps[i] >> 3] |= (uint8_t)(1 << (bps[i] & 7));
    return h;
}

void hires_destroy(Hires *h) {
    if (!h) return;
    free(h->hidx);
    free(h);
}

const char *hires_state(Hires *h) {
    if (!h) return "";
    if (h->found < 0) return "game version not supported";
    if (h->found == 0) return "waiting for flight";
    return "";
}

static void attach(Hires *h, Machine *m) {
    m->cpu.bp_map = h->bpmap;
    m->cpu.bp_cs = h->cs;
}

int hires_scan(Hires *h, Machine *m, int in_3d) {
    if (h->found > 0) {
        /* still there? (the overlay manager may move code) */
        uint32_t base = (uint32_t)h->cs << 4;
        if (!memcmp(m->mem + base + sigs[0].off, sigs[0].b, sigs[0].len)) return 1;
        h->found = 0; m->cpu.bp_cs = 0;
    }
    if (h->found < 0) return 0;
    for (uint32_t a = 0x6667; a < 0x9F000; a += 16) {
        if (m->mem[a] != 0xC7 || memcmp(m->mem + a, sigs[0].b, sigs[0].len)) continue;
        uint32_t base = a - 0x6667;
        int ok = 1;
        for (size_t i = 1; i < sizeof sigs / sizeof sigs[0] && ok; i++)
            if (memcmp(m->mem + base + sigs[i].off, sigs[i].b, sigs[i].len)) ok = 0;
        if (!ok) continue;
        h->cs = (uint16_t)(base >> 4);
        h->found = 1;
        attach(h, m);
        return 1;
    }
    /* flying, but the routines are not there: another release of the game */
    if (in_3d) h->found = -1;
    return 0;
}

void hires_detach(Hires *h, Machine *m) {
    (void)h;
    m->cpu.bp_cs = 0;
    m->cpu.wt_len = 0;
}

/* ------------------------------------------------------------- capture */
static inline uint16_t rw(const uint8_t *mem, uint32_t a) { return (uint16_t)(mem[a] | mem[a + 1] << 8); }
static inline int32_t rd32(const uint8_t *mem, uint32_t a) { return (int32_t)(rw(mem, a) | (uint32_t)rw(mem, a + 2) << 16); }
static inline void ww(uint8_t *mem, uint32_t a, uint16_t v) { mem[a] = (uint8_t)v; mem[a + 1] = (uint8_t)(v >> 8); }

static uint32_t hkey(int32_t x, int32_t y) { return ((uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u) & (HASH_SIZE - 1); }

static void hash_put(Hires *h, int32_t x, int32_t y, float fx, float fy) {
    uint32_t k = hkey(x, y);
    for (int i = 0; i < 64; i++) {
        HSlot *s = &h->hash[(k + i) & (HASH_SIZE - 1)];
        if (s->gen != h->gen || (s->x == x && s->y == y)) { s->gen = h->gen; s->x = x; s->y = y; s->fx = fx; s->fy = fy; return; }
    }
}

/* the precise position of a projected point, or the game's own value */
static HVert lookup(Hires *h, int32_t x, int32_t y) {
    uint32_t k = hkey(x, y);
    for (int i = 0; i < 64; i++) {
        HSlot *s = &h->hash[(k + i) & (HASH_SIZE - 1)];
        if (s->gen != h->gen) break;
        if (s->x == x && s->y == y) return (HVert){s->fx, s->fy};
    }
    /* the game truncates y down by about half a pixel */
    return (HVert){(float)x, (float)y + 0.5f};
}

#define SB_N 4

/* Runs the projection routine 6667 again with focal length 180*SB_N on the
 * current inputs, and stores the result, scaled back, for the point the
 * game has just computed. The CPU state and the memory it touches are
 * restored afterwards, so the game does not notice. */
static void rerun_projection(Hires *h, Machine *m, CPU *c) {
    uint8_t *mem = m->mem;
    uint32_t ds = (uint32_t)c->s[DS] << 4, cs = (uint32_t)c->s[CS] << 4, ss = (uint32_t)c->s[SS] << 4;
    uint16_t ip = c->ip;
    int32_t x1 = rd32(mem, ds + 0x22DE), y1 = rd32(mem, ds + 0x22E2);
    const int N = SB_N;

    CPU save = *c;
    uint32_t sp0 = ss + (uint16_t)(c->r[SP] - 256);
    if (ds + 0x2800 > MEM_SIZE || sp0 + 512 > MEM_SIZE) return;
    memcpy(h->save_vars, mem + ds + 0x2200, sizeof h->save_vars);
    memcpy(h->save_stack, mem + sp0, sizeof h->save_stack);
    memcpy(h->save_code, mem + cs + 0x6667, 6);
    memcpy(h->save_code + 6, mem + cs + 0x669B, 6);

    double cx = (int16_t)rw(mem, ds + 0x2587) + (int16_t)rw(mem, ds + 0x2310);
    double cy = -(double)(int16_t)rw(mem, ds + 0x2314) + (int16_t)rw(mem, ds + 0x2595) + (int16_t)rw(mem, ds + 0x2312);
    /* focal length and range limits */
    ww(mem, cs + 0x6667 + 4, 180 * N);
    ww(mem, cs + 0x669B + 4, 180 * N);
    uint32_t l8f = (uint32_t)rw(mem, ds + 0x258F) * N, l99 = (uint32_t)rw(mem, ds + 0x2599) * N;
    ww(mem, ds + 0x258F, (uint16_t)(l8f > 0xFFFF ? 0xFFFF : l8f));
    ww(mem, ds + 0x2599, (uint16_t)(l99 > 0x7FFF ? 0x7FFF : l99));
    /* reference values of the long route, in the same scale */
    int32_t ec = rd32(mem, ds + 0x22EC) * N;
    ww(mem, ds + 0x22EC, (uint16_t)ec); ww(mem, ds + 0x22EE, (uint16_t)((uint32_t)ec >> 16));
    int32_t f0 = rd32(mem, ds + 0x22F0);
    int32_t f1 = (int32_t)lround(N * (f0 - cy) + cy);
    ww(mem, ds + 0x22F0, (uint16_t)f1); ww(mem, ds + 0x22F2, (uint16_t)((uint32_t)f1 >> 16));

    /* near call to 6667 that returns to where we are now */
    c->r[SP] -= 2;
    ww(mem, ss + c->r[SP], ip);
    c->ip = 0x6667;
    int n = 0;
    while (n < 20000 && !(c->ip == ip && c->r[SP] == save.r[SP] && c->s[CS] == save.s[CS])) { cpu_step(c); n++; }
    int32_t xn = rd32(mem, ds + 0x22DE), yn = rd32(mem, ds + 0x22E2);

    memcpy(mem + ds + 0x2200, h->save_vars, sizeof h->save_vars);
    memcpy(mem + sp0, h->save_stack, sizeof h->save_stack);
    memcpy(mem + cs + 0x6667, h->save_code, 6);
    memcpy(mem + cs + 0x669B, h->save_code + 6, 6);
    *c = save;
    if (n >= 20000) return;

    double fx = (xn - cx) / N + cx, fy = (yn - cy) / N + cy;
    /* safety net: a rare edge case of the long route differs by a pixel */
    if (fabs(fx - x1) > 1.2) fx = x1;
    if (fabs(fy - y1) > 1.6) fy = y1 + 0.5;
    hash_put(h, x1, y1, (float)fx, (float)fy);
}

static HItem *new_item(HFrame *f, int type) {
    if (f->ni >= MAX_ITEMS) return NULL;
    HItem *it = &f->items[f->ni++];
    memset(it, 0, sizeof *it);
    it->type = (uint8_t)type;
    return it;
}

/* Lines and object polygons: the game clips them, leaves out lines behind
 * the camera and polygons facing away. To know what it really drew, the CPU
 * marks every write to the rows the item can touch until the routine ends. */
static void region_start(Hires *h, Machine *m, uint32_t ds, HFrame *f, int item) {
    const uint8_t *mem = m->mem;
    h->line_item = -1;
    uint16_t bseg = rw(mem, ds + 0x2579);
    int rows = (int16_t)rw(mem, ds + 0x2591);
    float bot = (int16_t)rw(mem, ds + 0x2593) + 1.5f;
    if (rows <= 0 || rows > VIEW_MAX || !bseg || ((uint32_t)bseg << 4) + 320u * rows > 0xA0000) return;
    const HItem *it = &f->items[item];
    float fx0 = 1e9f, fx1 = -1e9f, fy0 = 1e9f, fy1 = -1e9f;
    for (int i = 0; i < it->n; i++) {
        const HVert *v = &f->verts[it->first + i];
        fx0 = fminf(fx0, v->x); fx1 = fmaxf(fx1, v->x);
        fy0 = fminf(fy0, bot - v->y); fy1 = fmaxf(fy1, bot - v->y);
    }
    int x0 = fx0 < 1 ? 0 : (int)fx0 - 1, x1 = fx1 > 318 ? 319 : (int)fx1 + 1;
    int y0 = fy0 < 1 ? 0 : (int)fy0 - 1, y1 = fy1 > rows - 2 ? rows - 1 : (int)fy1 + 1;
    h->line_item = item; h->lseg = bseg; h->lrows = rows;
    h->lx0 = x0; h->lx1 = x1; h->ly0 = y0; h->ly1 = y1;
    if (x0 > x1 || y0 > y1) return;
    /* let the CPU mark every byte written in these rows */
    uint32_t len = 320u * (uint32_t)(y1 - y0 + 1);
    memset(h->wmap, 0, len);
    m->cpu.wt_map = h->wmap;
    m->cpu.wt_lo = ((uint32_t)bseg << 4) + 320u * (uint32_t)y0;
    m->cpu.wt_len = len;
}

static void region_end(Hires *h, Machine *m, uint32_t ds, HFrame *f) {
    int li = h->line_item;
    h->line_item = -1;
    int tracked = m->cpu.wt_len != 0;
    m->cpu.wt_len = 0;
    if (li < 0 || li >= f->ni) return;
    HItem *it = &f->items[li];
    int cnt = 0;
    float t0 = 1e9f, t1 = -1e9f;
    const HVert *a = &f->verts[it->first], *b = a + 1;
    float bot = (int16_t)rw(m->mem, ds + 0x2593) + 1.5f;
    float dx = b->x - a->x, dy = b->y - a->y, l2 = dx * dx + dy * dy;
    if (l2 < 1e-6f) l2 = 1e-6f;
    if (tracked && h->lx0 <= h->lx1 && h->ly0 <= h->ly1) {
        const uint8_t *buf = m->mem + ((uint32_t)h->lseg << 4);
        for (int y = h->ly0; y <= h->ly1; y++)
            for (int x = h->lx0; x <= h->lx1; x++) {
                int i = y * 320 + x;
                if (!h->wmap[(y - h->ly0) * 320 + x]) continue;
                cnt++;
                f->lmask[i] = 1;
                f->lval[i] = buf[i];
                if (it->type == IT_LINE) {
                    float px = x + 0.5f, py = bot - (y + 0.5f);
                    float t = ((px - a->x) * dx + (py - a->y) * dy) / l2;
                    if (t < t0) t0 = t;
                    if (t > t1) t1 = t;
                }
            }
    }
    if (!cnt) it->col = 0xFF; /* not drawn at all */
    else if (it->type == IT_LINE) {
        /* the drawn part, half a pixel longer at both ends */
        float ext = 0.5f / sqrtf(l2);
        it->t0 = fmaxf(0.f, t0 - ext); it->t1 = fminf(1.f, t1 + ext);
    }
}

void hires_breakpoint(Hires *h, Machine *m, CPU *c) {
    HFrame *f = &h->fr[h->cur];
    uint8_t *mem = m->mem;
    uint32_t ds = (uint32_t)c->s[DS] << 4, ss = (uint32_t)c->s[SS] << 4;
    uint16_t sp = c->r[SP];
    h->ds = c->s[DS];
    switch (c->ip) {
    case 0x663F: case 0x8A8B: case 0x8C6B:
        rerun_projection(h, m, c);
        break;
    case 0x7FB2: {
        if (f->ne >= MAX_EDGES) break;
        int32_t x1 = (int32_t)(rw(mem, ds + 0x2366) | (uint32_t)rw(mem, ds + 0x2364) << 16);
        int32_t x2 = (int32_t)(rw(mem, ds + 0x236A) | (uint32_t)rw(mem, ds + 0x2368) << 16);
        int32_t y1 = (int32_t)(rw(mem, ds + 0x23FF) | (uint32_t)rw(mem, ds + 0x23FD) << 16);
        int32_t y2 = (int32_t)(rw(mem, ds + 0x236E) | (uint32_t)rw(mem, ds + 0x236C) << 16);
        HVert a = lookup(h, x1, y1), b = lookup(h, x2, y2);
        HEdge *e = &f->edges[f->ne++];
        e->x1 = a.x; e->y1 = a.y; e->x2 = b.x; e->y2 = b.y;
        e->ix1 = x1; e->iy1 = y1; e->ix2 = x2; e->iy2 = y2;
        e->col = (uint8_t)rw(mem, ds + 0x23D9);
        e->ord = rw(mem, ds + 0x252F);
        break;
    }
    case 0x7621: {
        if (f->ne > h->pass_start) {
            HItem *it = new_item(f, IT_GROUND);
            /* When the aircraft banks, the scanline renderer works in a
             * sheared frame in which the horizon is level: the edge y is
             * the screen y plus the height of the horizon above its highest
             * point at that x. The horizon runs from (9C9, 9CB) to
             * (9CD, 9CF), y pointing up. Screen y = y + sa*x + sb.
             * col = 1: in a steep bank (beyond 45 degrees) the renderer works
             * on columns instead ([233C] = view height); that mapping is not
             * known yet, so the ground then stays as the game drew it. */
            if (it) {
                it->first = (uint32_t)h->pass_start; it->n = (uint16_t)(f->ne - h->pass_start);
                it->col = (int16_t)rw(mem, ds + 0x233C) != 320;
                float hx0 = (int16_t)rw(mem, ds + 0x9C9), hy0 = (int16_t)rw(mem, ds + 0x9CB);
                float hx1 = (int16_t)rw(mem, ds + 0x9CD), hy1 = (int16_t)rw(mem, ds + 0x9CF);
                float k = hx1 != hx0 ? (hy1 - hy0) / (hx1 - hx0) : 0.f;
                it->sa = k;
                it->sb = hy0 - k * hx0 - fmaxf(hy0, hy1) - 0.5f; /* -0.5: measured best fit */
            }
            if (dbg_on()) fprintf(stderr, "[hires] 7621: [2338]=%d [2593]=%d [2591]=%d [2312]=%d [2314]=%d [2595]=%d [233A]=%d [233C]=%d [233E]=%d\n", (int16_t)rw(mem, ds + 0x2338), (int16_t)rw(mem, ds + 0x2593), (int16_t)rw(mem, ds + 0x2591), (int16_t)rw(mem, ds + 0x2312), (int16_t)rw(mem, ds + 0x2314), (int16_t)rw(mem, ds + 0x2595), (int16_t)rw(mem, ds + 0x233A), (int16_t)rw(mem, ds + 0x233C), (int16_t)rw(mem, ds + 0x233E));
        }
        h->pass_start = f->ne;
        /* let the CPU mark the pixels the renderer writes */
        int rows = (int16_t)rw(mem, ds + 0x2591);
        uint16_t bseg = rw(mem, ds + 0x2579);
        h->wrows = 0;
        if (rows > 0 && rows <= VIEW_MAX && bseg && ((uint32_t)bseg << 4) + 320u * rows <= 0xA0000) {
            memset(h->wmap, 0, 320u * rows);
            c->wt_map = h->wmap; c->wt_lo = (uint32_t)bseg << 4; c->wt_len = 320u * rows;
            h->wseg = bseg; h->wrows = rows;
        }
        break;
    }
    case 0x769A: {
        int rows = c->wt_len ? h->wrows : 0;
        c->wt_len = 0;
        if (!rows) break;
        const uint8_t *b = mem + ((uint32_t)h->wseg << 4);
        if (f->gseg != h->wseg) { memset(f->gmask, 0, sizeof f->gmask); f->gseg = h->wseg; }
        for (int i = 0; i < 320 * rows; i++) if (h->wmap[i]) { f->gmask[i] = 1; f->gval[i] = b[i]; }
        break;
    }
    case 0x7B42: {
        h->pending_poly = -1;
        h->line_item = -1;
        uint16_t list = rw(mem, ss + (uint16_t)(sp + 4)), a2 = rw(mem, ss + (uint16_t)(sp + 8));
        int n = a2 >> 4;
        if ((a2 & 15) == 15 || n < 3 || n > 32 || f->nv + n > MAX_VERTS) break;
        HItem *it = new_item(f, IT_POLY);
        if (!it) break;
        it->first = (uint32_t)f->nv; it->n = (uint16_t)n;
        for (int i = 0; i < n; i++) {
            uint32_t v = ds + (uint16_t)(list + 8 * i);
            f->verts[f->nv++] = lookup(h, rd32(mem, v), rd32(mem, v + 4));
        }
        it->col = 0xFF; /* not drawn unless confirmed */
        h->pending_poly = f->ni - 1;
        region_start(h, m, ds, f, f->ni - 1);
        break;
    }
    case 0x7BBF:
        if (h->pending_poly >= 0 && h->pending_poly < f->ni) f->items[h->pending_poly].col = (uint8_t)rw(mem, ds + 0x23D9);
        h->pending_poly = -1;
        break;
    case 0x8CEC: {
        h->pending_line = -1;
        uint16_t idx = rw(mem, ss + (uint16_t)(sp + 4));
        if (f->nv + 2 > MAX_VERTS) break;
        HItem *it = new_item(f, IT_LINE);
        if (!it) break;
        uint32_t v = ds + (uint16_t)(0x2445 + 8 * idx);
        it->first = (uint32_t)f->nv; it->n = 2;
        f->verts[f->nv++] = lookup(h, rd32(mem, v), rd32(mem, v + 4));
        f->verts[f->nv++] = lookup(h, rd32(mem, v + 8), rd32(mem, v + 12));
        it->col = 0xFF;
        it->t0 = 0; it->t1 = 1;
        h->pending_line = f->ni - 1;
        region_start(h, m, ds, f, f->ni - 1);
        break;
    }
    case 0x9122: case 0x7EB5:
        /* end of the line or polygon routine: which pixels did it draw? */
        region_end(h, m, ds, f);
        break;
    case 0x8D26:
        if (h->pending_line >= 0 && h->pending_line < f->ni) f->items[h->pending_line].col = (uint8_t)rw(mem, ds + 0x252B);
        h->pending_line = -1;
        break;
    }
}

void hires_present(Hires *h, Machine *m, CPU *c) {
    HFrame *f = &h->fr[h->cur];
    uint32_t ss = (uint32_t)c->s[SS] << 4;
    uint16_t seg = rw(m->mem, ss + (uint16_t)(c->r[SP] + 6));
    uint32_t ds = (uint32_t)h->ds << 4;
    if (seg != h->bufseg[0]) { h->bufseg[1] = h->bufseg[0]; h->bufseg[0] = seg; }
    int vh = h->ds ? (int16_t)rw(m->mem, ds + 0x2591) : 0;
    int bot = h->ds ? (int16_t)rw(m->mem, ds + 0x2593) : 0;
    int vw = h->ds ? (int16_t)rw(m->mem, ds + 0x2583) : 0;
    /* watch which screen pixels this present call writes */
    if (vw != 320 || vh != h->vacc_vh) { memset(h->vacc, 0, sizeof h->vacc); memset(h->nonview, 0, sizeof h->nonview); h->vacc_vh = vh; }
    h->pw_rows = 0;
    if (h->found > 0 && vw == 320 && vh > 8 && vh <= VIEW_MAX && !c->wt_len) {
        uint32_t sp = ss + c->r[SP];
        memset(h->pwmap, 0, 320u * vh);
        c->wt_map = h->pwmap; c->wt_lo = 0xA0000; c->wt_len = 320u * vh;
        c->ret_lin = ((uint32_t)rw(m->mem, sp + 2) << 4) + rw(m->mem, sp);
        c->ret_sp = 0xFFFF;
        h->pw_rows = vh;
    }
    /* The game copies a frame to the screen during its present call, so at
     * the next present the previous frame is completely on the screen: only
     * then it is used, with the mask of where the screen shows it. Using a
     * frame while it is still being copied mixes old and new pixels. */
    if (h->pend >= 0) {
        HFrame *p = &h->fr[h->pend];
        const uint8_t *vram = m->mem + 0xA0000;
        if (h->ready >= 0) memcpy(h->vprev, h->fr[h->ready].vmask, sizeof h->vprev);
        else memset(h->vprev, 0, sizeof h->vprev);
        for (int i = 0; i < 320 * p->vh; i++) {
            p->vmask[i] = vram[i] == p->buf[i];
            if (!p->vmask[i]) h->nonview[i] = 1;
        }
        if (dbg_on()) {
            int a1 = 0, a2 = 0, a3 = 0;
            const uint8_t *cur = m->mem + ((uint32_t)seg << 4);
            for (int i = 0; i < 320 * p->vh; i++) { a1 += p->vmask[i]; a2 += vram[i] == cur[i]; a3 += p->buf[i] == cur[i]; }
            fprintf(stderr, "[hires] present %04X: screen=pending %d screen=new %d pending=new %d\n", seg, a1, a2, a3);
        }
        h->ready = h->pend;
        h->pend = -1;
    }
    /* only the full-width view (the forward view from the cockpit); the
     * side and rear views use a narrower buffer placed elsewhere on screen */
    if (dbg_on()) fprintf(stderr, "[hires] present %04X ni=%d vw=%d vh=%d t=%.0f\n", seg, f->ni, vw, vh, m->time_us / 1000);
    if (h->found > 0 && f->ni > 0 && vw == 320 && vh > 8 && vh <= VIEW_MAX && ((uint32_t)seg << 4) + 320u * vh < 0xA0000) {
        memcpy(f->buf, m->mem + ((uint32_t)seg << 4), 320u * vh);
        /* no record of the renderer's pixels: allow the ground everywhere */
        f->gok = f->gseg == seg;
        if (!f->gok) memset(f->gmask, 1, sizeof f->gmask);
        f->vh = vh;
        f->bot = bot + 1.5f;
        f->time_us = m->time_us;
        f->serial = ++h->serial;
        h->pend = h->cur;
        for (int k = 0; k < 3; k++) if (k != h->pend && k != h->ready) { h->cur = k; break; }
    }
    f = &h->fr[h->cur];
    f->ne = f->nv = f->ni = 0;
    f->gseg = 0;
    memset(f->lmask, 0, sizeof f->lmask);
    h->pass_start = 0;
    h->pending_poly = h->pending_line = -1;
    h->gen++;
}

void hires_present_done(Hires *h, Machine *m, CPU *c) {
    (void)m;
    if (!h->pw_rows || c->wt_map != h->pwmap) return;
    c->wt_len = 0;
    int n = 0;
    for (int i = 0; i < 320 * h->pw_rows; i++) if (h->pwmap[i]) { h->vacc[i] = 1; n++; }
    if (dbg_on()) fprintf(stderr, "[hires] present wrote %d pixels\n", n);
    h->pw_rows = 0;
}

void hires_debug(Hires *h, Machine *m, FILE *out) {
    fprintf(out, "hires: found=%d cs=%04X ds=%04X ready=%d", h->found, h->cs, h->ds, h->ready);
    if (h->ready >= 0) {
        const HFrame *f = &h->fr[h->ready];
        int same = 0;
        for (int i = 0; i < 320 * f->vh; i++) same += m->mem[0xA0000 + i] == f->buf[i];
        fprintf(out, " vh=%d bot=%.1f edges=%d items=%d age=%.0fms same=%.0f%%", f->vh, f->bot, f->ne, f->ni,
                (m->time_us - f->time_us) / 1000, f->vh ? 100.0 * same / (320 * f->vh) : 0);
    }
    uint32_t ds = (uint32_t)h->ds << 4;
    if (h->ds) fprintf(out, " [2591]=%d [2593]=%d [2583]=%d [2587]=%d [2595]=%d", (int16_t)rw(m->mem, ds + 0x2591), (int16_t)rw(m->mem, ds + 0x2593),
                       (int16_t)rw(m->mem, ds + 0x2583), (int16_t)rw(m->mem, ds + 0x2587), (int16_t)rw(m->mem, ds + 0x2595));
    fprintf(out, "\n");
    if (getenv("HIRES_ITEMS") && h->ready >= 0) {
        const HFrame *f = &h->fr[h->ready];
        for (int i = 0; i < f->ni; i++) {
            const HItem *it = &f->items[i];
            fprintf(out, "  item %d type %d col %d n %d:", i, it->type, it->col, it->n);
            if (it->type == IT_GROUND) { for (int k = 0; k < it->n; k++) { const HEdge *e = &f->edges[it->first + k]; fprintf(out, " [%.0f,%.0f-%.0f,%.0f o%d c%d]", e->x1, e->y1, e->x2, e->y2, e->ord, e->col); } }
            else for (int k = 0; k < it->n; k++) fprintf(out, " (%.1f,%.1f)", f->verts[it->first + k].x, f->verts[it->first + k].y);
            fprintf(out, "\n");
        }
    }
}

/* ------------------------------------------------------------- drawing */
typedef struct {
    uint8_t *dst; int w, h;       /* target (palette indices) */
    int s;                        /* pixels per game pixel */
    float bot;
    const uint8_t *gmask;         /* where ground may be drawn (320 wide), or NULL */
} Target;

typedef void (*SpanFn)(const Target *t, int row, int x0, int x1, void *ctx);

/* Even-odd fill of a set of edges (y pointing up, in game pixels). */
static void fill_edges(const Target *t, const HEdge *e, int n, SpanFn fn, void *ctx) {
    float ymin = 1e9f, ymax = -1e9f;
    for (int i = 0; i < n; i++) {
        ymin = fminf(ymin, fminf(e[i].y1, e[i].y2));
        ymax = fmaxf(ymax, fmaxf(e[i].y1, e[i].y2));
    }
    int r0 = (int)floorf((t->bot - ymax) * t->s - 0.5f), r1 = (int)ceilf((t->bot - ymin) * t->s);
    if (r0 < 0) r0 = 0;
    if (r1 > t->h - 1) r1 = t->h - 1;
    float xs[64];
    for (int r = r0; r <= r1; r++) {
        float y = t->bot - (r + 0.5f) / t->s;
        int k = 0;
        for (int i = 0; i < n && k < 64; i++) {
            float y1 = e[i].y1, y2 = e[i].y2;
            if ((y1 <= y && y < y2) || (y2 <= y && y < y1)) {
                float x = e[i].x1 + (y - y1) * (e[i].x2 - e[i].x1) / (y2 - y1);
                int j = k++;
                while (j > 0 && xs[j - 1] > x) { xs[j] = xs[j - 1]; j--; }
                xs[j] = x;
            }
        }
        for (int i = 0; i + 1 < k; i += 2) {
            float a = xs[i] * t->s - 0.5f, b = xs[i + 1] * t->s - 0.5f;
            if (b < 0 || a >= t->w) continue;
            int x0 = a < 0 ? 0 : (int)ceilf(a), x1 = b >= t->w ? t->w : (int)ceilf(b);
            if (x1 > x0) fn(t, r, x0, x1, ctx);
        }
    }
}

static void fill_poly(const Target *t, const HVert *v, int n, SpanFn fn, void *ctx) {
    HEdge e[34];
    if (n > 32) n = 32;
    for (int i = 0; i < n; i++) {
        const HVert *a = &v[i], *b = &v[(i + 1) % n];
        e[i] = (HEdge){a->x, a->y, b->x, b->y, 0, 0, 0, 0, 0, 0};
    }
    fill_edges(t, e, n, fn, ctx);
}

static void fill_line(const Target *t, HVert a, HVert b, float half, SpanFn fn, void *ctx) {
    float dx = b.x - a.x, dy = b.y - a.y, len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-4f) { dx = 1; dy = 0; len = 1; }
    /* one game pixel thick in the main direction, like the game's lines */
    float px = -dy / len * half, py = dx / len * half;
    float k = fmaxf(fabsf(dx), fabsf(dy)) / len;
    px /= k; py /= k;
    HVert q[4] = {{a.x + px, a.y + py}, {b.x + px, b.y + py}, {b.x - px, b.y - py}, {a.x - px, a.y - py}};
    fill_poly(t, q, 4, fn, ctx);
}

/* The game shades the ground by distance: the colour of a ground polygon
 * gets 0..7 added per row of its scanline renderer. When banking, those rows
 * run parallel to the horizon (see 7621 in hires_breakpoint). */
#define SHADE_OFF 256
#define SHADE_N 768
static inline int ground_row(float sx, float srow, float sa, float sb) {
    int g = (int)floorf(srow + sa * sx + sb) + SHADE_OFF;
    return g < 0 ? 0 : (g >= SHADE_N ? SHADE_N - 1 : g);
}

typedef struct { uint8_t col, kind; const int8_t *shade; float sa, sb; } PaintCtx;

static void span_paint(const Target *t, int row, int x0, int x1, void *ctx) {
    const PaintCtx *p = ctx;
    uint8_t *d = t->dst + (size_t)row * t->w;
    const uint8_t *g = t->gmask && p->kind == 1 ? t->gmask + (row / t->s) * 320 : NULL;
    if (!p->shade && !g) { memset(d + x0, p->col, (size_t)(x1 - x0)); return; }
    float srow = (row + 0.5f) / t->s;
    for (int x = x0; x < x1; x++) {
        if (g && !g[x / t->s]) continue;
        uint8_t c = p->col;
        if (p->shade) c = (uint8_t)(c + p->shade[ground_row((x + 0.5f) / t->s, srow, p->sa, p->sb)]);
        d[x] = c;
    }
}

typedef struct { Hires *h; uint8_t kind, col; } MarkCtx;

static void span_mark(const Target *t, int row, int x0, int x1, void *ctx) {
    const MarkCtx *p = ctx;
    for (int x = x0; x < x1; x++) {
        if (p->kind == 1 && t->gmask && !t->gmask[row * 320 + x]) continue;
        p->h->kind1[row * 320 + x] = p->kind; p->h->base1[row * 320 + x] = p->col;
    }
}

/* draws all items of a frame into a target */
static void draw_items(Hires *h, const HFrame *f, const Target *t, int mark, const int8_t *shade) {
    for (int i = 0; i < f->ni; i++) {
        const HItem *it = &f->items[i];
        if (it->type != IT_GROUND && (it->col == 0xFF || h->skip[i])) continue;
        if (it->type == IT_GROUND) {
            if (it->col) continue;
            /* consecutive edges with the same polygon number form a polygon */
            const HEdge *e = f->edges + it->first;
            int n = it->n, s = 0;
            while (s < n) {
                int k = s + 1;
                while (k < n && e[k].ord == e[s].ord) k++;
                /* an edge that is there twice (a shared side of two parts of
                 * one shape) cancels out in the game's even-odd fill; the
                 * precise positions may differ a little, so drop both */
                HEdge pe[64]; int np = 0;
                uint8_t gone[64] = {0};
                int cnt = k - s > 64 ? 64 : k - s;
                for (int i = 0; i < cnt; i++) {
                    if (gone[i]) continue;
                    const HEdge *a = &e[s + i];
                    HEdge sh = *a;
                    sh.y1 += it->sa * sh.x1 + it->sb;
                    sh.y2 += it->sa * sh.x2 + it->sb;
                    for (int j = i + 1; j < cnt; j++) {
                        const HEdge *b = &e[s + j];
                        if (gone[j]) continue;
                        if ((a->ix1 == b->ix1 && a->iy1 == b->iy1 && a->ix2 == b->ix2 && a->iy2 == b->iy2) ||
                            (a->ix1 == b->ix2 && a->iy1 == b->iy2 && a->ix2 == b->ix1 && a->iy2 == b->iy1)) { gone[i] = gone[j] = 1; break; }
                    }
                    if (!gone[i]) pe[np++] = sh;
                }
                if (np >= 2) {
                    if (mark) { MarkCtx mc = {h, 1, e[s].col}; fill_edges(t, pe, np, span_mark, &mc); }
                    else { PaintCtx pc = {e[s].col, 1, shade, it->sa, it->sb}; fill_edges(t, pe, np, span_paint, &pc); }
                }
                s = k;
            }
        } else if (it->type == IT_LINE) {
            /* only the part the game drew (see 9122) */
            const HVert *v = f->verts + it->first;
            HVert a = {v[0].x + it->t0 * (v[1].x - v[0].x), v[0].y + it->t0 * (v[1].y - v[0].y)};
            HVert b = {v[0].x + it->t1 * (v[1].x - v[0].x), v[0].y + it->t1 * (v[1].y - v[0].y)};
            if (mark) { MarkCtx mc = {h, 2, it->col}; fill_line(t, a, b, 0.5f, span_mark, &mc); }
            else { PaintCtx pc = {it->col, 2, NULL, 0, 0}; fill_line(t, a, b, 0.45f, span_paint, &pc); }
        } else {
            const HVert *v = f->verts + it->first;
            if (mark) { MarkCtx mc = {h, 3, it->col}; fill_poly(t, v, it->n, span_mark, &mc); }
            else { PaintCtx pc = {it->col, 3, NULL, 0, 0}; fill_poly(t, v, it->n, span_paint, &pc); }
        }
    }
}

static int dbg_on(void) {
    static int on = -1;
    if (on < 0) on = getenv("SWOTL_DEBUG_HIRES") != NULL;
    return on;
}

static int dbg_why(const char *why, double a, double b) {
    if (dbg_on()) fprintf(stderr, "[hires] no: %s %.0f %.0f\n", why, a, b);
    return 0;
}

int hires_compose(Hires *h, Machine *m, int n, uint32_t *out) {
    if (!h || h->found <= 0 || h->ready < 0 || n < 2 || n > 8) return 0;
    if (m->vmode != 0x13 || m->planar) return dbg_why("mode", m->vmode, m->planar);
    const HFrame *f = &h->fr[h->ready];
    if (m->time_us - f->time_us > 500000.0) return dbg_why("age", m->time_us - f->time_us, 0);
    const int vh = f->vh, W = 320;
    const uint8_t *src = f->buf, *vram = m->mem + 0xA0000;

    /* the 3D view must take a fair part of the screen, and the screen must
     * still show this frame or the one being copied (not the map or a menu) */
    const uint8_t *nxt = h->pend >= 0 ? h->fr[h->pend].buf : src;
    int same = 0, live = 0;
    for (int i = 0; i < W * vh; i++)
        if (f->vmask[i]) { same++; live += vram[i] == src[i] || vram[i] == nxt[i]; }
    if (same < W * vh / 4) return dbg_why("same", same, W * vh);
    if (live < same / 2) return dbg_why("live", live, same);
    if (dbg_on()) fprintf(stderr, "[hires] ok serial %llu same %d\n", (unsigned long long)f->serial, same);

    int HW = W * n, HH = vh * n;
    if (h->hidx_serial == f->serial && h->hidx_n == n) goto screen;

    memset(h->skip, 0, sizeof h->skip);

    /* 1. coverage at the game's resolution */
    Target t1 = {NULL, W, vh, 1, f->bot, f->gmask};
    memset(h->kind1, 0, (size_t)W * vh);
    draw_items(h, f, &t1, 1, NULL);

    /* 2. depth shading of the ground per row: the game adds 0..7 to the base
     * colour of a ground polygon depending on the distance */
    /* the shear of the (last) ground pass */
    float gsa = 0, gsb = 0;
    for (int i = 0; i < f->ni; i++) if (f->items[i].type == IT_GROUND && !f->items[i].col) { gsa = f->items[i].sa; gsb = f->items[i].sb; }
    static int8_t shade[SHADE_N];
    static int shc[SHADE_N][16];
    int have[SHADE_N];
    memset(shc, 0, sizeof shc);
    for (int r = 0; r < vh; r++)
        for (int x = 0; x < W; x++) {
            int i = r * W + x;
            if (h->kind1[i] != 1) continue;
            int d = src[i] - h->base1[i];
            if (d >= 0 && d < 16) shc[ground_row(x + 0.5f, r + 0.5f, gsa, gsb)][d]++;
        }
    for (int g = 0; g < SHADE_N; g++) {
        int best = -1;
        for (int d = 0; d < 16; d++) if (shc[g][d] && (best < 0 || shc[g][d] > shc[g][best])) best = d;
        have[g] = best >= 0;
        shade[g] = (int8_t)(best >= 0 ? best : 0);
    }
    for (int g = 0; g < SHADE_N; g++) if (!have[g]) {
        for (int d = 1; d < SHADE_N; d++) {
            if (g - d >= 0 && have[g - d]) { shade[g] = shade[g - d]; break; }
            if (g + d < SHADE_N && have[g + d]) { shade[g] = shade[g + d]; break; }
        }
    }

    /* 3. background: the game's picture with the redrawn shapes (and one
     * pixel around them) filled from the nearest pixel of the same kind
     * (ground or sky) in the same row; if the row has none, from the row
     * below (ground) or above (sky) */
    uint8_t *unres = h->exp1; /* scratch */
    for (int r = 0; r < vh; r++) {
        for (int x = 0; x < W; x++) {
            int i = r * W + x, cov = 0;
            for (int dy = -1; dy <= 1 && !cov; dy++)
                for (int dx = -1; dx <= 1 && !cov; dx++) {
                    int yy = r + dy, xx = x + dx;
                    if (yy >= 0 && yy < vh && xx >= 0 && xx < W && h->kind1[yy * W + xx]) cov = 1;
                }
            /* pixels a line or object routine drew are not background */
            if (f->lmask[i] && src[i] == f->lval[i]) cov = 1;
            h->other1[i] = (uint8_t)cov; /* temporarily: covered mask */
        }
        for (int x = 0; x < W; x++) {
            int i = r * W + x;
            unres[i] = 0;
            if (!h->other1[i]) { h->bg1[i] = src[i]; continue; }
            uint8_t cls = f->gmask[i];
            int best = -1;
            for (int d = 1; d < W && best < 0; d++) {
                if (x - d >= 0 && !h->other1[i - d] && f->gmask[i - d] == cls) best = i - d;
                else if (x + d < W && !h->other1[i + d] && f->gmask[i + d] == cls) best = i + d;
            }
            if (best >= 0) h->bg1[i] = src[best];
            else unres[i] = 1;
        }
    }
    for (int r = 0; r < vh; r++)
        for (int x = 0; x < W; x++) {
            int i = r * W + x;
            if (unres[i] && !f->gmask[i]) { h->bg1[i] = r > 0 ? h->bg1[i - W] : src[i]; unres[i] = 0; }
        }
    for (int r = vh - 1; r >= 0; r--)
        for (int x = 0; x < W; x++) {
            int i = r * W + x;
            if (unres[i]) h->bg1[i] = r < vh - 1 ? h->bg1[i + W] : src[i];
        }

    /* 4. what the redraw gives at the game's resolution; pixels that differ
     * (and are not on the border of a shape) were drawn by something else:
     * aircraft, smoke, explosions. Those are kept from the game's picture. */
    for (int i = 0; i < W * vh; i++) {
        int k = h->kind1[i];
        h->exp1[i] = k == 0 ? h->bg1[i] : (uint8_t)(h->base1[i] + (k == 1 ? shade[ground_row(i % W + 0.5f, i / W + 0.5f, gsa, gsb)] : 0));
    }
    for (int r = 0; r < vh; r++)
        for (int x = 0; x < W; x++) {
            int i = r * W + x, e = h->exp1[i], edge = 0;
            if (x > 0 && h->exp1[i - 1] != e) edge = 1;
            if (x < W - 1 && h->exp1[i + 1] != e) edge = 1;
            if (r > 0 && h->exp1[i - W] != e) edge = 1;
            if (r < vh - 1 && h->exp1[i + W] != e) edge = 1;
            int oth = src[i] != e && !edge;
            /* a colour that the redraw has close by is a small difference in
             * where an edge falls, not something else */
            for (int dy = -2; dy <= 2 && oth; dy++)
                for (int dx = -2; dx <= 2 && oth; dx++) {
                    int yy = r + dy, xx = x + dx;
                    if (yy >= 0 && yy < vh && xx >= 0 && xx < W && h->exp1[yy * W + xx] == src[i]) oth = 0;
                }
            /* still showing what the ground, line or polygon routine drew */
            if (f->lmask[i] && src[i] == f->lval[i]) oth = 0;
            if (f->gok && f->gmask[i] && src[i] == f->gval[i]) oth = 0;
            h->other1[i] = (uint8_t)oth;
        }

    /* 5. redraw at n times the resolution */
    size_t need = (size_t)HW * HH;
    if (h->hidx_size < need) {
        free(h->hidx);
        h->hidx = malloc(need);
        if (!h->hidx) { h->hidx_size = 0; h->hidx_serial = 0; return 0; }
        h->hidx_size = need;
    }
    for (int r = 0; r < HH; r++) {
        const uint8_t *b = h->bg1 + (r / n) * W;
        uint8_t *d = h->hidx + (size_t)r * HW;
        for (int x = 0; x < W; x++) memset(d + x * n, b[x], (size_t)n);
    }
    Target tn = {h->hidx, HW, HH, n, f->bot, f->gmask};
    draw_items(h, f, &tn, 0, shade);
    for (int r = 0; r < vh; r++)
        for (int x = 0; x < W; x++)
            if (h->other1[r * W + x])
                for (int yy = 0; yy < n; yy++) memset(h->hidx + (size_t)(r * n + yy) * HW + x * n, src[r * W + x], (size_t)n);
    h->hidx_serial = f->serial; h->hidx_n = n;
    if (dbg_on()) {
        int no = 0, ns = 0, nl = 0, np = 0, nu = 0;
        for (int i = 0; i < W * vh; i++) no += h->other1[i];
        for (int i = 0; i < f->ni; i++) {
            if (f->items[i].type == IT_LINE) { nl++; ns += h->skip[i]; }
            if (f->items[i].type == IT_POLY) { np++; nu += f->items[i].col == 0xFF; }
        }
        { int gk = 0, gm = 0; for (int i = 0; i < W * vh; i++) if (h->kind1[i] == 1) { gk++; gm += h->exp1[i] == src[i]; }
          fprintf(stderr, "[hires] ground pixels %d match %.1f%%\n", gk, gk ? 100.0 * gm / gk : 0); }
        fprintf(stderr, "[hires] frame %llu items %d edges %d lines %d skipped %d polys %d unconfirmed %d other %d gseg %04X\n",
                (unsigned long long)f->serial, f->ni, f->ne, nl, ns, np, nu, no, f->gseg);
    }

screen:;
    /* 6. the screen: the redrawn view where the screen shows the 3D view,
     * the screen itself (cockpit) everywhere else. A pixel shows the 3D view
     * when it is part of the view (in this or the previous frame) and the
     * screen holds this frame or the next one there; anything the game draws
     * straight onto the screen (flak, the gunsight) is shown as it is, and
     * where it has gone again the redrawn view shows. */
    const uint32_t *pal = m->palette_rgba;
    uint8_t mask = m->dac_mask;
    for (int y = 0; y < 200; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t v = vram[y * W + x];
            uint32_t *o = out + (size_t)y * n * HW + (size_t)x * n;
            int i3 = y * W + x;
            /* part of the 3D view: written by the game's present routine at
             * some time, or never seen different from the 3D frame */
            int view = h->vacc[i3] || (!h->nonview[i3] && f->vmask[i3]);
            if (y < vh && view && (v == src[i3] || v == nxt[i3])) {
                for (int yy = 0; yy < n; yy++) {
                    const uint8_t *hi = h->hidx + (size_t)(y * n + yy) * HW + x * n;
                    uint32_t *oo = o + (size_t)yy * HW;
                    for (int xx = 0; xx < n; xx++) oo[xx] = pal[hi[xx] & mask];
                }
            } else {
                uint32_t c = pal[v & mask];
                for (int yy = 0; yy < n; yy++) {
                    uint32_t *oo = o + (size_t)yy * HW;
                    for (int xx = 0; xx < n; xx++) oo[xx] = c;
                }
            }
        }
    }
    return 1;
}
