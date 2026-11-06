/* SWOTL Enhanced - real-mode x86 interpreter (8086 + 80186/286 real-mode
 * instructions). Flags are computed eagerly. GPL-3.0-or-later. */
#include "cpu.h"
#include <stdio.h>
#include <string.h>

static uint8_t parity_tab[256];

/* ---------------------------------------------------------------- memory */
uint8_t mem_rb(CPU *c, uint32_t a) {
    if (c->vga_hooked && a >= 0xA0000 && a < 0xB0000) return c->vga_rd(c->host, a);
    return c->mem[a];
}
void mem_wb(CPU *c, uint32_t a, uint8_t v) {
    if (a >= 0xA0000) {
        if (a < 0xB0000) {
            c->vram_writes++;
            if (c->vga_hooked) { c->vga_wr(c->host, a, v); return; }
        } else if (a >= 0xC0000 && a < 0x100000 && !c->rom_writable) {
            return; /* ROM */
        }
    }
    c->mem[a] = v;
}
uint16_t mem_rw(CPU *c, uint32_t a) {
    return (uint16_t)(mem_rb(c, a) | (mem_rb(c, a + 1) << 8));
}
void mem_ww(CPU *c, uint32_t a, uint16_t v) {
    mem_wb(c, a, (uint8_t)v);
    mem_wb(c, a + 1, (uint8_t)(v >> 8));
}

/* segment:offset accessors with 16-bit offset wrap; fast path for plain RAM */
static inline uint8_t rb(CPU *c, uint16_t s, uint16_t o) {
    uint32_t a = cpu_lin(c, s, o);
    if (a < 0xA0000) return c->mem[a];
    return mem_rb(c, a);
}
static inline uint16_t rw(CPU *c, uint16_t s, uint16_t o) {
    uint32_t a = cpu_lin(c, s, o);
    if (a < 0x9FFFF && o != 0xFFFF) return (uint16_t)(c->mem[a] | (c->mem[a + 1] << 8));
    return (uint16_t)(rb(c, s, o) | (rb(c, s, (uint16_t)(o + 1)) << 8));
}
static inline void wb(CPU *c, uint16_t s, uint16_t o, uint8_t v) {
    uint32_t a = cpu_lin(c, s, o);
    uint32_t d = a - c->wt_lo;
    if (d < c->wt_len) c->wt_map[d] = 1;
    if (a < 0xA0000) { c->mem[a] = v; return; }
    mem_wb(c, a, v);
}
static inline void ww(CPU *c, uint16_t s, uint16_t o, uint16_t v) {
    uint32_t a = cpu_lin(c, s, o);
    if (a < 0x9FFFF && o != 0xFFFF) {
        uint32_t d = a - c->wt_lo;
        if (d < c->wt_len) c->wt_map[d] = 1;
        if (d + 1 < c->wt_len) c->wt_map[d + 1] = 1;
        c->mem[a] = (uint8_t)v; c->mem[a + 1] = (uint8_t)(v >> 8); return;
    }
    wb(c, s, o, (uint8_t)v);
    wb(c, s, (uint16_t)(o + 1), (uint8_t)(v >> 8));
}

void cpu_push(CPU *c, uint16_t v) { c->r[SP] -= 2; ww(c, c->s[SS], c->r[SP], v); }
uint16_t cpu_pop(CPU *c) { uint16_t v = rw(c, c->s[SS], c->r[SP]); c->r[SP] += 2; return v; }

static inline uint8_t fetch8(CPU *c) { uint8_t v = c->mem[cpu_lin(c, c->s[CS], c->ip)]; c->ip++; return v; }
static inline uint16_t fetch16(CPU *c) { uint16_t v = rw(c, c->s[CS], c->ip); c->ip += 2; return v; }

/* byte registers: AL CL DL BL AH CH DH BH */
static inline uint8_t getr8(CPU *c, int r) { return r < 4 ? (uint8_t)c->r[r] : (uint8_t)(c->r[r - 4] >> 8); }
static inline void setr8(CPU *c, int r, uint8_t v) {
    if (r < 4) c->r[r] = (uint16_t)((c->r[r] & 0xFF00) | v);
    else c->r[r - 4] = (uint16_t)((c->r[r - 4] & 0x00FF) | (v << 8));
}

void cpu_reset(CPU *c) {
    for (int i = 0; i < 256; i++) {
        int p = 0;
        for (int b = 0; b < 8; b++) p ^= (i >> b) & 1;
        parity_tab[i] = p ? 0 : F_PF;
    }
    memset(c->r, 0, sizeof c->r);
    memset(c->s, 0, sizeof c->s);
    c->fl = 0x0002;
    c->ip = 0;
    c->halted = 0;
    c->icount = 0;
}

void cpu_interrupt(CPU *c, uint8_t num) {
    cpu_push(c, c->fl);
    cpu_push(c, c->s[CS]);
    cpu_push(c, c->ip);
    c->fl &= (uint16_t)~(F_IF | F_TF);
    c->ip = rw(c, 0, (uint16_t)(num * 4));
    c->s[CS] = rw(c, 0, (uint16_t)(num * 4 + 2));
    c->halted = 0;
}

/* ---------------------------------------------------------------- flags */
static inline void set_szp(CPU *c, uint32_t res, int w) {
    c->fl &= (uint16_t)~(F_SF | F_ZF | F_PF);
    if (w) {
        res &= 0xFFFF;
        if (res & 0x8000) c->fl |= F_SF;
    } else {
        res &= 0xFF;
        if (res & 0x80) c->fl |= F_SF;
    }
    if (!res) c->fl |= F_ZF;
    c->fl |= parity_tab[res & 0xFF];
}
#define SETF(f, cond) do { if (cond) c->fl |= (f); else c->fl &= (uint16_t)~(f); } while (0)

/* op: 0 add,1 or,2 adc,3 sbb,4 and,5 sub,6 xor,7 cmp */
static uint16_t alu(CPU *c, int op, uint16_t a, uint16_t b, int w) {
    uint32_t mask = w ? 0xFFFF : 0xFF, sign = w ? 0x8000 : 0x80;
    uint32_t res, cf;
    switch (op) {
    case 0: case 2:
        cf = (op == 2) ? (c->fl & F_CF) : 0;
        res = (uint32_t)a + b + cf;
        SETF(F_CF, res > mask);
        SETF(F_OF, ((a ^ res) & (b ^ res) & sign) != 0);
        SETF(F_AF, ((a ^ b ^ res) & 0x10) != 0);
        set_szp(c, res, w);
        return (uint16_t)(res & mask);
    case 3: case 5: case 7:
        cf = (op == 3) ? (c->fl & F_CF) : 0;
        res = (uint32_t)a - b - cf;
        SETF(F_CF, ((uint32_t)a) < (uint32_t)b + cf);
        SETF(F_OF, ((a ^ b) & (a ^ res) & sign) != 0);
        SETF(F_AF, ((a ^ b ^ res) & 0x10) != 0);
        set_szp(c, res, w);
        return (uint16_t)(res & mask);
    case 1: res = a | b; break;
    case 4: res = a & b; break;
    default: res = a ^ b; break;
    }
    c->fl &= (uint16_t)~(F_CF | F_OF | F_AF);
    set_szp(c, res, w);
    return (uint16_t)(res & mask);
}

static uint16_t inc_dec(CPU *c, uint16_t a, int dec, int w) {
    uint16_t cf = c->fl & F_CF;
    uint16_t r = alu(c, dec ? 5 : 0, a, 1, w);
    c->fl = (uint16_t)((c->fl & ~F_CF) | cf);
    return r;
}

/* shifts/rotates: op 0 rol,1 ror,2 rcl,3 rcr,4 shl,5 shr,6 sal(=shl),7 sar */
static uint16_t shift(CPU *c, int op, uint16_t v, uint8_t cnt, int w) {
    int bits = w ? 16 : 8;
    uint32_t mask = w ? 0xFFFF : 0xFF, sign = w ? 0x8000 : 0x80;
    cnt &= 0x1F;
    if (!cnt) return v;
    uint32_t x = v & mask;
    int cf = 0;
    switch (op) {
    case 0:
        for (int i = 0; i < cnt; i++) { cf = (x & sign) != 0; x = ((x << 1) | cf) & mask; }
        SETF(F_CF, cf);
        SETF(F_OF, ((x & sign) != 0) ^ cf);
        break;
    case 1:
        for (int i = 0; i < cnt; i++) { cf = x & 1; x = (x >> 1) | (cf ? sign : 0); }
        SETF(F_CF, cf);
        SETF(F_OF, (((x << 1) ^ x) & sign) != 0);
        break;
    case 2:
        cf = (c->fl & F_CF) != 0;
        for (int i = 0; i < cnt; i++) { int nc = (x & sign) != 0; x = ((x << 1) | cf) & mask; cf = nc; }
        SETF(F_CF, cf);
        SETF(F_OF, ((x & sign) != 0) ^ cf);
        break;
    case 3:
        cf = (c->fl & F_CF) != 0;
        for (int i = 0; i < cnt; i++) {
            if (i == cnt - 1) SETF(F_OF, (((x & sign) != 0) ^ cf));
            int nc = x & 1; x = (x >> 1) | (cf ? sign : 0); cf = nc;
        }
        SETF(F_CF, cf);
        break;
    case 4: case 6:
        if (cnt <= bits) cf = (x >> (bits - cnt)) & 1; else cf = 0;
        x = cnt < 32 ? (x << cnt) & mask : 0;
        SETF(F_CF, cf);
        SETF(F_OF, ((x & sign) != 0) ^ cf);
        SETF(F_AF, 0);
        set_szp(c, x, w);
        break;
    case 5:
        SETF(F_OF, (x & sign) != 0);
        cf = cnt <= bits ? (x >> (cnt - 1)) & 1 : 0;
        x = cnt < bits ? x >> cnt : 0;
        SETF(F_CF, cf);
        SETF(F_AF, 0);
        set_szp(c, x, w);
        break;
    case 7: {
        int32_t sx = w ? (int16_t)x : (int8_t)x;
        int sc = cnt >= bits ? bits - 1 : cnt - 1;
        cf = (sx >> sc) & 1;
        sx >>= (cnt >= bits ? bits - 1 : cnt);
        if (cnt >= bits) cf = sx & 1;
        x = (uint32_t)sx & mask;
        SETF(F_CF, cf);
        SETF(F_OF, 0);
        SETF(F_AF, 0);
        set_szp(c, x, w);
        break;
    }
    }
    return (uint16_t)x;
}

/* ---------------------------------------------------------------- modrm */
typedef struct {
    int mod, reg, rm;
    uint16_t seg, off;
} ModRM;

static int seg_ovr; /* -1 none, else segment index */

static void decode_modrm(CPU *c, ModRM *m) {
    uint8_t b = fetch8(c);
    m->mod = b >> 6; m->reg = (b >> 3) & 7; m->rm = b & 7;
    if (m->mod == 3) return;
    uint16_t off = 0; int seg = DS;
    switch (m->rm) {
    case 0: off = c->r[BX] + c->r[SI]; break;
    case 1: off = c->r[BX] + c->r[DI]; break;
    case 2: off = c->r[BP] + c->r[SI]; seg = SS; break;
    case 3: off = c->r[BP] + c->r[DI]; seg = SS; break;
    case 4: off = c->r[SI]; break;
    case 5: off = c->r[DI]; break;
    case 6:
        if (m->mod == 0) off = fetch16(c);
        else { off = c->r[BP]; seg = SS; }
        break;
    case 7: off = c->r[BX]; break;
    }
    if (m->mod == 1) off += (uint16_t)(int8_t)fetch8(c);
    else if (m->mod == 2) off += fetch16(c);
    m->off = off;
    m->seg = c->s[seg_ovr >= 0 ? seg_ovr : seg];
}

static inline uint8_t rm_r8(CPU *c, ModRM *m) { return m->mod == 3 ? getr8(c, m->rm) : rb(c, m->seg, m->off); }
static inline uint16_t rm_r16(CPU *c, ModRM *m) { return m->mod == 3 ? c->r[m->rm] : rw(c, m->seg, m->off); }
static inline void rm_w8(CPU *c, ModRM *m, uint8_t v) { if (m->mod == 3) setr8(c, m->rm, v); else wb(c, m->seg, m->off, v); }
static inline void rm_w16(CPU *c, ModRM *m, uint16_t v) { if (m->mod == 3) c->r[m->rm] = v; else ww(c, m->seg, m->off, v); }
static inline uint16_t rm_read(CPU *c, ModRM *m, int w) { return w ? rm_r16(c, m) : rm_r8(c, m); }
static inline void rm_write(CPU *c, ModRM *m, int w, uint16_t v) { if (w) rm_w16(c, m, v); else rm_w8(c, m, (uint8_t)v); }
static inline uint16_t reg_read(CPU *c, int r, int w) { return w ? c->r[r] : getr8(c, r); }
static inline void reg_write(CPU *c, int r, int w, uint16_t v) { if (w) c->r[r] = v; else setr8(c, r, (uint8_t)v); }

static uint16_t in16(CPU *c, uint16_t p) { return (uint16_t)(c->in8(c->host, p) | (c->in8(c->host, (uint16_t)(p + 1)) << 8)); }
static void out16(CPU *c, uint16_t p, uint16_t v) { c->out8(c->host, p, (uint8_t)v); c->out8(c->host, (uint16_t)(p + 1), (uint8_t)(v >> 8)); }

static void div_fault(CPU *c) {
    /* 286+: return address is the faulting instruction */
    c->ip = c->op_ip; c->s[CS] = c->op_cs;
    cpu_interrupt(c, 0);
}

static int cond(CPU *c, int cc) {
    uint16_t f = c->fl; int r;
    switch (cc >> 1) {
    case 0: r = (f & F_OF) != 0; break;
    case 1: r = (f & F_CF) != 0; break;
    case 2: r = (f & F_ZF) != 0; break;
    case 3: r = (f & (F_CF | F_ZF)) != 0; break;
    case 4: r = (f & F_SF) != 0; break;
    case 5: r = (f & F_PF) != 0; break;
    case 6: r = ((f & F_SF) != 0) != ((f & F_OF) != 0); break;
    default: r = (f & F_ZF) || (((f & F_SF) != 0) != ((f & F_OF) != 0)); break;
    }
    return (cc & 1) ? !r : r;
}

/* string op helpers */
static void string_op(CPU *c, uint8_t op, int rep) {
    int w = op & 1;
    int d = (c->fl & F_DF) ? -(1 + w) : (1 + w);
    uint16_t sseg = c->s[seg_ovr >= 0 ? seg_ovr : DS];
    if (rep && c->s[ES] == 0xA000) { c->vram_blits++; c->vram_bytes += (uint32_t)c->r[CX] << w; }
    for (;;) {
        if (rep && c->r[CX] == 0) break;
        switch (op) {
        case 0x6C: case 0x6D: /* INS */
            if (w) ww(c, c->s[ES], c->r[DI], in16(c, c->r[DX]));
            else wb(c, c->s[ES], c->r[DI], c->in8(c->host, c->r[DX]));
            c->r[DI] += d; break;
        case 0x6E: case 0x6F: /* OUTS */
            if (w) out16(c, c->r[DX], rw(c, sseg, c->r[SI]));
            else c->out8(c->host, c->r[DX], rb(c, sseg, c->r[SI]));
            c->r[SI] += d; break;
        case 0xA4: case 0xA5:
            if (w) ww(c, c->s[ES], c->r[DI], rw(c, sseg, c->r[SI]));
            else wb(c, c->s[ES], c->r[DI], rb(c, sseg, c->r[SI]));
            c->r[SI] += d; c->r[DI] += d; break;
        case 0xA6: case 0xA7: {
            uint16_t a = w ? rw(c, sseg, c->r[SI]) : rb(c, sseg, c->r[SI]);
            uint16_t b = w ? rw(c, c->s[ES], c->r[DI]) : rb(c, c->s[ES], c->r[DI]);
            alu(c, 7, a, b, w);
            c->r[SI] += d; c->r[DI] += d; break;
        }
        case 0xAA: case 0xAB:
            if (w) ww(c, c->s[ES], c->r[DI], c->r[AX]);
            else wb(c, c->s[ES], c->r[DI], (uint8_t)c->r[AX]);
            c->r[DI] += d; break;
        case 0xAC: case 0xAD:
            if (w) c->r[AX] = rw(c, sseg, c->r[SI]);
            else setr8(c, 0, rb(c, sseg, c->r[SI]));
            c->r[SI] += d; break;
        case 0xAE: case 0xAF: {
            uint16_t b = w ? rw(c, c->s[ES], c->r[DI]) : rb(c, c->s[ES], c->r[DI]);
            alu(c, 7, w ? c->r[AX] : (uint8_t)c->r[AX], b, w);
            c->r[DI] += d; break;
        }
        }
        if (!rep) break;
        c->r[CX]--;
        c->icount++;
        if ((op & 0xF6) == 0xA6) { /* CMPS/SCAS */
            int zf = (c->fl & F_ZF) != 0;
            if (rep == 1 && !zf) break;  /* REPE */
            if (rep == 2 && zf) break;   /* REPNE */
        }
    }
}

static void unimpl(CPU *c, uint8_t op) {
    fprintf(stderr, "[cpu] unimplemented opcode %02X at %04X:%04X\n", op, c->op_cs, c->op_ip);
    c->fault = 1;
    /* raise invalid opcode (int 6) like a 186+ */
    c->ip = c->op_ip; c->s[CS] = c->op_cs;
    cpu_interrupt(c, 6);
}

static void step(CPU *c) {
    int rep = 0;
    seg_ovr = -1;
    c->op_cs = c->s[CS]; c->op_ip = c->ip;
    uint8_t op;
    for (;;) {
        op = fetch8(c);
        switch (op) {
        case 0x26: seg_ovr = ES; continue;
        case 0x2E: seg_ovr = CS; continue;
        case 0x36: seg_ovr = SS; continue;
        case 0x3E: seg_ovr = DS; continue;
        case 0xF0: continue;           /* LOCK */
        case 0xF2: rep = 2; continue;  /* REPNE */
        case 0xF3: rep = 1; continue;  /* REP/REPE */
        }
        break;
    }
    ModRM m;
    int w = op & 1;
    uint16_t a, b, t;

    if (op < 0x40 && (op & 7) < 6) {
        int aop = op >> 3;
        switch (op & 7) {
        case 0: case 1: decode_modrm(c, &m);
            a = rm_read(c, &m, w); b = reg_read(c, m.reg, w);
            t = alu(c, aop, a, b, w);
            if (aop != 7) rm_write(c, &m, w, t);
            return;
        case 2: case 3: decode_modrm(c, &m);
            a = reg_read(c, m.reg, w); b = rm_read(c, &m, w);
            t = alu(c, aop, a, b, w);
            if (aop != 7) reg_write(c, m.reg, w, t);
            return;
        case 4:
            b = fetch8(c);
            t = alu(c, aop, (uint8_t)c->r[AX], b, 0);
            if (aop != 7) setr8(c, 0, (uint8_t)t);
            return;
        case 5:
            b = fetch16(c);
            t = alu(c, aop, c->r[AX], b, 1);
            if (aop != 7) c->r[AX] = t;
            return;
        }
    }

    switch (op) {
    case 0x06: cpu_push(c, c->s[ES]); break;
    case 0x07: c->s[ES] = cpu_pop(c); break;
    case 0x0E: cpu_push(c, c->s[CS]); break;
    case 0x16: cpu_push(c, c->s[SS]); break;
    case 0x17: c->s[SS] = cpu_pop(c); c->inhibit_irq = 1; break;
    case 0x1E: cpu_push(c, c->s[DS]); break;
    case 0x1F: c->s[DS] = cpu_pop(c); break;
    case 0x27: { /* DAA */
        uint8_t al = (uint8_t)c->r[AX], old = al; int cf = c->fl & F_CF, af = c->fl & F_AF;
        if ((al & 0x0F) > 9 || af) { al += 6; c->fl |= F_AF; } else c->fl &= ~F_AF;
        if (old > (af ? 0x9F : 0x99) || cf) { al += 0x60; c->fl |= F_CF; } else c->fl &= ~F_CF;
        setr8(c, 0, al); set_szp(c, al, 0); break;
    }
    case 0x2F: { /* DAS */
        uint8_t al = (uint8_t)c->r[AX], old = al; int cf = c->fl & F_CF, af = c->fl & F_AF;
        if ((al & 0x0F) > 9 || af) { al -= 6; c->fl |= F_AF; } else c->fl &= ~F_AF;
        if (old > (af ? 0x9F : 0x99) || cf) { al -= 0x60; c->fl |= F_CF; } else c->fl &= ~F_CF;
        setr8(c, 0, al); set_szp(c, al, 0); break;
    }
    case 0x37: case 0x3F: { /* AAA / AAS */
        if ((c->r[AX] & 0x0F) > 9 || (c->fl & F_AF)) {
            if (op == 0x37) c->r[AX] += 0x106; else c->r[AX] -= 0x106;
            c->fl |= F_AF | F_CF;
        } else c->fl &= ~(F_AF | F_CF);
        c->r[AX] &= 0xFF0F;
        break;
    }
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
        c->r[op & 7] = inc_dec(c, c->r[op & 7], 0, 1); break;
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        c->r[op & 7] = inc_dec(c, c->r[op & 7], 1, 1); break;
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x55: case 0x56: case 0x57:
        cpu_push(c, c->r[op & 7]); break;
    case 0x54: /* PUSH SP: 286 pushes old value */
        t = c->r[SP]; cpu_push(c, t); break;
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        t = cpu_pop(c); c->r[op & 7] = t; break;
    case 0x60: { /* PUSHA */
        uint16_t sp = c->r[SP];
        for (int i = 0; i < 8; i++) cpu_push(c, i == SP ? sp : c->r[i]);
        break;
    }
    case 0x61: /* POPA */
        for (int i = 7; i >= 0; i--) { t = cpu_pop(c); if (i != SP) c->r[i] = t; }
        break;
    case 0x62: { /* BOUND */
        decode_modrm(c, &m);
        int16_t lo = (int16_t)rw(c, m.seg, m.off), hi = (int16_t)rw(c, m.seg, (uint16_t)(m.off + 2));
        int16_t v = (int16_t)c->r[m.reg];
        if (v < lo || v > hi) { c->ip = c->op_ip; cpu_interrupt(c, 5); }
        break;
    }
    case 0x68: cpu_push(c, fetch16(c)); break;
    case 0x6A: cpu_push(c, (uint16_t)(int8_t)fetch8(c)); break;
    case 0x69: case 0x6B: {
        decode_modrm(c, &m);
        int32_t x = (int16_t)rm_r16(c, &m);
        int32_t y = op == 0x69 ? (int16_t)fetch16(c) : (int8_t)fetch8(c);
        int32_t r = x * y;
        c->r[m.reg] = (uint16_t)r;
        SETF(F_CF | F_OF, r != (int16_t)r);
        break;
    }
    case 0x6C: case 0x6D: case 0x6E: case 0x6F:
    case 0xA4: case 0xA5: case 0xA6: case 0xA7:
    case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        string_op(c, op, rep); break;
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        int8_t d = (int8_t)fetch8(c);
        if (cond(c, op & 15)) c->ip += (uint16_t)d;
        break;
    }
    case 0x80: case 0x81: case 0x82: case 0x83:
        decode_modrm(c, &m);
        a = rm_read(c, &m, w);
        if (op == 0x81) b = fetch16(c);
        else if (op == 0x83) b = (uint16_t)(int8_t)fetch8(c);
        else b = fetch8(c);
        t = alu(c, m.reg, a, b, w);
        if (m.reg != 7) rm_write(c, &m, w, t);
        break;
    case 0x84: case 0x85:
        decode_modrm(c, &m);
        alu(c, 4, rm_read(c, &m, w), reg_read(c, m.reg, w), w);
        break;
    case 0x86: case 0x87:
        decode_modrm(c, &m);
        a = rm_read(c, &m, w); b = reg_read(c, m.reg, w);
        rm_write(c, &m, w, b); reg_write(c, m.reg, w, a);
        break;
    case 0x88: case 0x89: decode_modrm(c, &m); rm_write(c, &m, w, reg_read(c, m.reg, w)); break;
    case 0x8A: case 0x8B: decode_modrm(c, &m); reg_write(c, m.reg, w, rm_read(c, &m, w)); break;
    case 0x8C: decode_modrm(c, &m); rm_w16(c, &m, c->s[m.reg & 3]); break;
    case 0x8D: decode_modrm(c, &m); c->r[m.reg] = m.off; break;
    case 0x8E: decode_modrm(c, &m); c->s[m.reg & 3] = rm_r16(c, &m); if ((m.reg & 3) == SS) c->inhibit_irq = 1; break;
    case 0x8F: {
        uint16_t v = cpu_pop(c);
        decode_modrm(c, &m);
        rm_w16(c, &m, v);
        break;
    }
    case 0x90: break;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
        t = c->r[AX]; c->r[AX] = c->r[op & 7]; c->r[op & 7] = t; break;
    case 0x98: c->r[AX] = (uint16_t)(int8_t)c->r[AX]; break;
    case 0x99: c->r[DX] = (c->r[AX] & 0x8000) ? 0xFFFF : 0; break;
    case 0x9A: {
        uint16_t off = fetch16(c), seg = fetch16(c);
        cpu_push(c, c->s[CS]); cpu_push(c, c->ip);
        c->s[CS] = seg; c->ip = off; break;
    }
    case 0x9B: break; /* WAIT */
    case 0x9C: cpu_push(c, (uint16_t)((c->fl & 0x0FD5) | 0x0002)); break; /* 286 real mode: bits 12-15 clear */
    case 0x9D: c->fl = (uint16_t)((cpu_pop(c) & 0x0FD5) | 0x0002); break;
    case 0x9E: c->fl = (uint16_t)((c->fl & 0xFF00) | ((c->r[AX] >> 8) & 0xD5) | 0x02); break;
    case 0x9F: setr8(c, 4, (uint8_t)((c->fl & 0xD5) | 0x02)); break;
    case 0xA0: { uint16_t o = fetch16(c); setr8(c, 0, rb(c, c->s[seg_ovr >= 0 ? seg_ovr : DS], o)); break; }
    case 0xA1: { uint16_t o = fetch16(c); c->r[AX] = rw(c, c->s[seg_ovr >= 0 ? seg_ovr : DS], o); break; }
    case 0xA2: { uint16_t o = fetch16(c); wb(c, c->s[seg_ovr >= 0 ? seg_ovr : DS], o, (uint8_t)c->r[AX]); break; }
    case 0xA3: { uint16_t o = fetch16(c); ww(c, c->s[seg_ovr >= 0 ? seg_ovr : DS], o, c->r[AX]); break; }
    case 0xA8: alu(c, 4, (uint8_t)c->r[AX], fetch8(c), 0); break;
    case 0xA9: alu(c, 4, c->r[AX], fetch16(c), 1); break;
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        setr8(c, op & 7, fetch8(c)); break;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        c->r[op & 7] = fetch16(c); break;
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        decode_modrm(c, &m);
        uint8_t cnt = (op <= 0xC1) ? fetch8(c) : (op <= 0xD1 ? 1 : (uint8_t)c->r[CX]);
        rm_write(c, &m, w, shift(c, m.reg, rm_read(c, &m, w), cnt, w));
        break;
    }
    case 0xC2: t = fetch16(c); c->ip = cpu_pop(c); c->r[SP] += t; break;
    case 0xC3: c->ip = cpu_pop(c); break;
    case 0xC4: case 0xC5:
        decode_modrm(c, &m);
        c->r[m.reg] = rw(c, m.seg, m.off);
        c->s[op == 0xC4 ? ES : DS] = rw(c, m.seg, (uint16_t)(m.off + 2));
        break;
    case 0xC6: decode_modrm(c, &m); rm_w8(c, &m, fetch8(c)); break;
    case 0xC7: decode_modrm(c, &m); rm_w16(c, &m, fetch16(c)); break;
    case 0xC8: { /* ENTER */
        uint16_t sz = fetch16(c); uint8_t lvl = fetch8(c) & 31;
        cpu_push(c, c->r[BP]);
        uint16_t fp = c->r[SP];
        if (lvl) {
            for (int i = 1; i < lvl; i++) { c->r[BP] -= 2; cpu_push(c, rw(c, c->s[SS], c->r[BP])); }
            cpu_push(c, fp);
        }
        c->r[BP] = fp; c->r[SP] -= sz;
        break;
    }
    case 0xC9: c->r[SP] = c->r[BP]; c->r[BP] = cpu_pop(c); break;
    case 0xCA: t = fetch16(c); c->ip = cpu_pop(c); c->s[CS] = cpu_pop(c); c->r[SP] += t; break;
    case 0xCB: c->ip = cpu_pop(c); c->s[CS] = cpu_pop(c); break;
    case 0xCC: cpu_interrupt(c, 3); break;
    case 0xCD: { uint8_t n = fetch8(c); cpu_interrupt(c, n); break; }
    case 0xCE: if (c->fl & F_OF) cpu_interrupt(c, 4); break;
    case 0xCF:
        c->ip = cpu_pop(c); c->s[CS] = cpu_pop(c);
        c->fl = (uint16_t)((cpu_pop(c) & 0x0FD5) | 0x0002);
        break;
    case 0xD4: { /* AAM */
        uint8_t base = fetch8(c), al = (uint8_t)c->r[AX];
        if (!base) { div_fault(c); break; }
        c->r[AX] = (uint16_t)(((al / base) << 8) | (al % base));
        set_szp(c, c->r[AX] & 0xFF, 0);
        break;
    }
    case 0xD5: { /* AAD */
        uint8_t base = fetch8(c);
        uint8_t al = (uint8_t)((c->r[AX] & 0xFF) + (c->r[AX] >> 8) * base);
        c->r[AX] = al; set_szp(c, al, 0);
        break;
    }
    case 0xD6: setr8(c, 0, (c->fl & F_CF) ? 0xFF : 0); break; /* SALC */
    case 0xD7: setr8(c, 0, rb(c, c->s[seg_ovr >= 0 ? seg_ovr : DS], (uint16_t)(c->r[BX] + (c->r[AX] & 0xFF)))); break;
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        decode_modrm(c, &m); /* no coprocessor: ignore */
        break;
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: {
        int8_t d = (int8_t)fetch8(c);
        int jmp;
        if (op == 0xE3) jmp = c->r[CX] == 0;
        else {
            c->r[CX]--;
            jmp = c->r[CX] != 0;
            if (op == 0xE0) jmp = jmp && !(c->fl & F_ZF);
            if (op == 0xE1) jmp = jmp && (c->fl & F_ZF);
        }
        if (jmp) c->ip += (uint16_t)d;
        break;
    }
    case 0xE4: setr8(c, 0, c->in8(c->host, fetch8(c))); break;
    case 0xE5: c->r[AX] = in16(c, fetch8(c)); break;
    case 0xE6: c->out8(c->host, fetch8(c), (uint8_t)c->r[AX]); break;
    case 0xE7: out16(c, fetch8(c), c->r[AX]); break;
    case 0xE8: t = fetch16(c); cpu_push(c, c->ip); c->ip += t; break;
    case 0xE9: t = fetch16(c); c->ip += t; break;
    case 0xEA: { uint16_t off = fetch16(c), seg = fetch16(c); c->ip = off; c->s[CS] = seg; break; }
    case 0xEB: { int8_t d = (int8_t)fetch8(c); c->ip += (uint16_t)d; break; }
    case 0xEC: setr8(c, 0, c->in8(c->host, c->r[DX])); break;
    case 0xED: c->r[AX] = in16(c, c->r[DX]); break;
    case 0xEE: c->out8(c->host, c->r[DX], (uint8_t)c->r[AX]); break;
    case 0xEF: out16(c, c->r[DX], c->r[AX]); break;
    case 0xF1: { uint8_t n = fetch8(c); if (c->hle) c->hle(c->host, c, n); break; }
    case 0xF4: c->halted = 1; break;
    case 0xF5: c->fl ^= F_CF; break;
    case 0xF6: case 0xF7: {
        decode_modrm(c, &m);
        switch (m.reg) {
        case 0: case 1:
            alu(c, 4, rm_read(c, &m, w), w ? fetch16(c) : fetch8(c), w); break;
        case 2: rm_write(c, &m, w, (uint16_t)~rm_read(c, &m, w)); break;
        case 3: rm_write(c, &m, w, alu(c, 5, 0, rm_read(c, &m, w), w)); break;
        case 4:
            if (w) {
                uint32_t r = (uint32_t)c->r[AX] * rm_r16(c, &m);
                c->r[AX] = (uint16_t)r; c->r[DX] = (uint16_t)(r >> 16);
                SETF(F_CF | F_OF, c->r[DX] != 0);
                set_szp(c, c->r[AX], 1);
            } else {
                uint16_t r = (uint16_t)((c->r[AX] & 0xFF) * rm_r8(c, &m));
                c->r[AX] = r;
                SETF(F_CF | F_OF, (r & 0xFF00) != 0);
                set_szp(c, r & 0xFF, 0);
            }
            break;
        case 5:
            if (w) {
                int32_t r = (int32_t)(int16_t)c->r[AX] * (int16_t)rm_r16(c, &m);
                c->r[AX] = (uint16_t)r; c->r[DX] = (uint16_t)((uint32_t)r >> 16);
                SETF(F_CF | F_OF, r != (int16_t)r);
                set_szp(c, c->r[AX], 1);
            } else {
                int16_t r = (int16_t)((int8_t)c->r[AX] * (int8_t)rm_r8(c, &m));
                c->r[AX] = (uint16_t)r;
                SETF(F_CF | F_OF, r != (int8_t)r);
                set_szp(c, (uint16_t)r & 0xFF, 0);
            }
            break;
        case 6:
            if (w) {
                uint32_t num = ((uint32_t)c->r[DX] << 16) | c->r[AX];
                uint16_t dv = rm_r16(c, &m);
                if (!dv || num / dv > 0xFFFF) { div_fault(c); break; }
                c->r[AX] = (uint16_t)(num / dv); c->r[DX] = (uint16_t)(num % dv);
            } else {
                uint16_t num = c->r[AX]; uint8_t dv = rm_r8(c, &m);
                if (!dv || num / dv > 0xFF) { div_fault(c); break; }
                c->r[AX] = (uint16_t)(((num % dv) << 8) | (num / dv));
            }
            break;
        case 7:
            if (w) {
                int32_t num = (int32_t)(((uint32_t)c->r[DX] << 16) | c->r[AX]);
                int16_t dv = (int16_t)rm_r16(c, &m);
                if (!dv || (num == INT32_MIN && dv == -1)) { div_fault(c); break; }
                int32_t q = num / dv, r = num % dv;
                if (q > 32767 || q < -32768) { div_fault(c); break; }
                c->r[AX] = (uint16_t)q; c->r[DX] = (uint16_t)r;
            } else {
                int16_t num = (int16_t)c->r[AX]; int8_t dv = (int8_t)rm_r8(c, &m);
                if (!dv) { div_fault(c); break; }
                int q = num / dv, r = num % dv;
                if (q > 127 || q < -128) { div_fault(c); break; }
                c->r[AX] = (uint16_t)(((uint8_t)r << 8) | (uint8_t)q);
            }
            break;
        }
        break;
    }
    case 0xF8: c->fl &= ~F_CF; break;
    case 0xF9: c->fl |= F_CF; break;
    case 0xFA: c->fl &= ~F_IF; break;
    case 0xFB: c->fl |= F_IF; c->inhibit_irq = 1; break;
    case 0xFC: c->fl &= ~F_DF; break;
    case 0xFD: c->fl |= F_DF; break;
    case 0xFE:
        decode_modrm(c, &m);
        if (m.reg < 2) rm_w8(c, &m, (uint8_t)inc_dec(c, rm_r8(c, &m), m.reg, 0));
        else unimpl(c, op);
        break;
    case 0xFF:
        decode_modrm(c, &m);
        switch (m.reg) {
        case 0: case 1: rm_w16(c, &m, inc_dec(c, rm_r16(c, &m), m.reg, 1)); break;
        case 2: t = rm_r16(c, &m); cpu_push(c, c->ip); c->ip = t; break;
        case 3: {
            uint16_t off = rw(c, m.seg, m.off), seg = rw(c, m.seg, (uint16_t)(m.off + 2));
            cpu_push(c, c->s[CS]); cpu_push(c, c->ip);
            c->s[CS] = seg; c->ip = off; break;
        }
        case 4: c->ip = rm_r16(c, &m); break;
        case 5: c->ip = rw(c, m.seg, m.off); c->s[CS] = rw(c, m.seg, (uint16_t)(m.off + 2)); break;
        case 6: t = rm_r16(c, &m); cpu_push(c, t); break;
        default: unimpl(c, op); break;
        }
        break;
    default:
        unimpl(c, op);
        break;
    }
}

void cpu_step(CPU *c) { step(c); }

int cpu_run(CPU *c, int n) {
    int i;
    for (i = 0; i < n && !c->halted; i++) {
        c->inhibit_irq = 0;
        if (c->watch_lin && ((uint32_t)c->s[CS] << 4) + c->ip == c->watch_lin) {
            c->watch_hits++;
            if (c->watch_hook) c->watch_hook(c->host, c);
        }
        if (c->bp_cs && c->s[CS] == c->bp_cs && (c->bp_map[c->ip >> 3] >> (c->ip & 7) & 1)) c->bp_hook(c->host, c);
        step(c);
        c->icount++;
        if (c->inhibit_irq) { i--; continue; }
        /* return as soon as interrupts are enabled with an IRQ waiting, or
         * when the host wants the slice to end (time passed in I/O waits) */
        if (c->stop) { c->stop = 0; i++; break; }
        if (c->irq_pending && (c->fl & F_IF)) { i++; break; }
    }
    return i;
}
