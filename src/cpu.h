/* SWOTL Enhanced - real-mode x86 (8086/80186/80286 real mode) interpreter.
 * GPL-3.0-or-later. */
#ifndef SWOTL_CPU_H
#define SWOTL_CPU_H
#include <stdint.h>

enum { AX, CX, DX, BX, SP, BP, SI, DI };
enum { ES, CS, SS, DS };

#define F_CF 0x0001
#define F_PF 0x0004
#define F_AF 0x0010
#define F_ZF 0x0040
#define F_SF 0x0080
#define F_TF 0x0100
#define F_IF 0x0200
#define F_DF 0x0400
#define F_OF 0x0800

#define MEM_SIZE (0x110000 + 16)

typedef struct CPU CPU;

struct CPU {
    uint16_t r[8];
    uint16_t s[4];
    uint16_t ip, fl;
    uint8_t *mem;               /* MEM_SIZE bytes */
    void *host;

    uint8_t (*in8)(void *host, uint16_t port);
    void (*out8)(void *host, uint16_t port, uint8_t v);
    /* HLE trap: opcode F1 nn. */
    void (*hle)(void *host, CPU *c, uint8_t num);
    /* Optional memory hooks for the VGA window (A0000-BFFFF) when planar. */
    int vga_hooked;
    uint8_t (*vga_rd)(void *host, uint32_t addr);
    void (*vga_wr)(void *host, uint32_t addr, uint8_t v);

    uint64_t icount;            /* instructions executed */
    int halted;
    int inhibit_irq;            /* one-instruction interrupt shadow */
    uint16_t op_cs, op_ip;      /* address of current instruction */
    int a20;                    /* 0 = wrap at 1MB */
    int rom_writable;           /* tests only */
    int fault;                  /* set on unimplemented opcode */
    int stop;                   /* host asks cpu_run to return early */
    int irq_pending;            /* host: a deliverable IRQ is waiting */
    uint32_t vram_blits, vram_bytes; /* debug: rep string ops into A000 */
    uint64_t vram_writes;       /* bytes written to the VGA window */
    uint32_t watch_lin;         /* linear address to count executions of */
    uint64_t watch_hits;
    void (*watch_hook)(void *host, CPU *c); /* called before that instruction */
    /* one-shot: ret_hook is called when execution reaches linear address
     * ret_lin with SP == ret_sp (the return from a call); 0 = off */
    uint32_t ret_lin; uint16_t ret_sp;
    void (*ret_hook)(void *host, CPU *c);
    /* breakpoints: bp_hook is called before any instruction in segment bp_cs
     * whose offset has its bit set in bp_map (8 KB). bp_cs = 0: none. */
    uint16_t bp_cs;
    const uint8_t *bp_map;
    void (*bp_hook)(void *host, CPU *c);
    /* write tracking: a write to linear address wt_lo + i (i < wt_len) sets
     * wt_map[i] = 1. wt_len = 0: off. */
    uint32_t wt_lo, wt_len;
    uint8_t *wt_map;
    /* with wt_tag set, a tracked write also stores the 16-bit value at
     * linear address wt_tag_src in wt_tag[i] (the renderer's scanline) */
    uint16_t *wt_tag;
    uint32_t wt_tag_src;
};

void cpu_reset(CPU *c);
/* Execute up to n instructions; returns number executed. */
int cpu_run(CPU *c, int n);
/* Execute exactly one instruction, without interrupts or hooks (used to run a
 * game routine on the side; the caller saves and restores the state). */
void cpu_step(CPU *c);
/* Raise a software/hardware interrupt now (pushes flags, cs, ip). */
void cpu_interrupt(CPU *c, uint8_t num);

static inline uint32_t cpu_lin(CPU *c, uint16_t seg, uint16_t off) {
    uint32_t a = ((uint32_t)seg << 4) + off;
    if (!c->a20) a &= 0xFFFFF;
    return a;
}
uint8_t  mem_rb(CPU *c, uint32_t a);
uint16_t mem_rw(CPU *c, uint32_t a);
void     mem_wb(CPU *c, uint32_t a, uint8_t v);
void     mem_ww(CPU *c, uint32_t a, uint16_t v);
void cpu_push(CPU *c, uint16_t v);
uint16_t cpu_pop(CPU *c);

#endif
