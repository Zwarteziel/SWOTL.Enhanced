/* SWOTL Enhanced - emulated PC hardware and timing. GPL-3.0-or-later. */
#include "machine.h"
#include "hle.h"
#include "hires.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#define PIT_HZ 1193182.0
#define VGA_FRAME_US 14268.0   /* 70.086 Hz */
#define VGA_RETRACE_US 1100.0

void machine_logf(Machine *m, int level, const char *fmt, ...) {
    if (level > m->log_level || !m->log) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(m->log, "[%9.3f] ", m->time_us / 1000.0);
    vfprintf(m->log, fmt, ap);
    va_end(ap);
    fflush(m->log);
}

/* current emulated time, accurate to the instruction inside a CPU slice */
static inline double now_us(Machine *m) {
    if (m->in_slice) return m->slice_t0 + (double)(m->cpu.icount - m->slice_ic0) / m->ips;
    return m->time_us;
}
static inline void io_delay(Machine *m, double us) {
    if (m->in_slice) {
        m->slice_t0 += us;
        if (now_us(m) >= m->slice_deadline) m->cpu.stop = 1;
    } else m->time_us += us;
}

/* ------------------------------------------------------------------ PIC */
/* tell the CPU whether an IRQ could be taken as soon as IF is set */
static void pic_update(Machine *m) {
    uint8_t pend = m->pic_irr & (uint8_t)~m->pic_imr;
    int ok = 0;
    for (int irq = 0; irq < 8; irq++) {
        if (m->pic_isr & (1 << irq)) break;
        if (pend & (1 << irq)) { ok = 1; break; }
    }
    m->cpu.irq_pending = ok;
}

static void pic_raise(Machine *m, int irq) { m->pic_irr |= (uint8_t)(1 << irq); pic_update(m); }

static void pic_eoi(Machine *m) {
    for (int i = 0; i < 8; i++)
        if (m->pic_isr & (1 << i)) { m->pic_isr &= (uint8_t)~(1 << i); break; }
    pic_update(m);
}

static void deliver_irq(Machine *m) {
    CPU *c = &m->cpu;
    uint8_t pend = m->pic_irr & (uint8_t)~m->pic_imr;
    if (!pend) return;
    if (!(c->fl & F_IF)) return;
    for (int irq = 0; irq < 8; irq++) {
        if (m->pic_isr & (1 << irq)) return; /* higher or equal priority in service */
        if (pend & (1 << irq)) {
            m->pic_irr &= (uint8_t)~(1 << irq);
            m->pic_isr |= (uint8_t)(1 << irq);
            pic_update(m);
            cpu_interrupt(c, (uint8_t)(8 + irq));
            return;
        }
    }
}

/* ------------------------------------------------------------------ PIT */
static double pit_period_us(Machine *m, int ch) {
    uint32_t r = m->pit_reload[ch] ? m->pit_reload[ch] : 65536;
    return r * 1e6 / PIT_HZ;
}

static uint16_t pit_count(Machine *m, int ch) {
    uint32_t r = m->pit_reload[ch] ? m->pit_reload[ch] : 65536;
    double ticks = (now_us(m) - m->pit_start_us[ch]) * PIT_HZ / 1e6;
    uint32_t t = (uint32_t)fmod(ticks, (double)r);
    uint32_t v = r - t;
    if (m->pit_mode[ch] == 3) { v = (v * 2) % r; if (!v) v = r; }
    return (uint16_t)v;
}

static void pit_write(Machine *m, int port, uint8_t v) {
    if (port == 0x43) {
        int ch = v >> 6;
        if (ch == 3) return; /* read-back: ignore */
        int rw = (v >> 4) & 3;
        if (rw == 0) { m->pit_latch[ch] = pit_count(m, ch); m->pit_latched[ch] = 2; m->pit_rstate[ch] = 0; return; }
        m->pit_rw[ch] = (uint8_t)rw;
        m->pit_mode[ch] = (uint8_t)((v >> 1) & 7);
        if (m->pit_mode[ch] > 5) m->pit_mode[ch] -= 4;
        m->pit_wstate[ch] = 0; m->pit_rstate[ch] = 0;
        return;
    }
    int ch = port - 0x40;
    switch (m->pit_rw[ch]) {
    case 1: m->pit_reload[ch] = v; break;
    case 2: m->pit_reload[ch] = (uint16_t)(v << 8); break;
    default:
        if (m->pit_wstate[ch] == 0) { m->pit_reload[ch] = (uint16_t)((m->pit_reload[ch] & 0xFF00) | v); m->pit_wstate[ch] = 1; return; }
        m->pit_reload[ch] = (uint16_t)((m->pit_reload[ch] & 0x00FF) | (v << 8)); m->pit_wstate[ch] = 0;
        break;
    }
    m->pit_start_us[ch] = now_us(m);
    if (ch == 0) {
        m->next_irq0_us = now_us(m) + pit_period_us(m, 0);
        machine_logf(m, 1, "PIT ch0 reload=%u (%.2f Hz)\n", m->pit_reload[0], 1e6 / pit_period_us(m, 0));
    }
}

static uint8_t pit_read(Machine *m, int ch) {
    uint16_t v = m->pit_latched[ch] ? m->pit_latch[ch] : pit_count(m, ch);
    uint8_t r;
    int rw = m->pit_rw[ch] ? m->pit_rw[ch] : 3;
    if (rw == 1) r = (uint8_t)v;
    else if (rw == 2) r = (uint8_t)(v >> 8);
    else {
        r = m->pit_rstate[ch] ? (uint8_t)(v >> 8) : (uint8_t)v;
        m->pit_rstate[ch] ^= 1;
    }
    if (m->pit_latched[ch]) m->pit_latched[ch]--;
    return r;
}

/* ------------------------------------------------------------------ OPL */
static void opl_sync(Machine *m) {
    if (m->audio_rate <= 0) return;
    double target = now_us(m) * m->audio_rate / 1e6;
    while (m->audio_gen_pos < target) {
        int16_t s[2];
        OPL3_GenerateResampled(&m->opl, s);
        uint32_t used = m->audio_wpos - m->audio_rpos;
        if (used >= AUDIO_RING) m->audio_rpos++; /* overrun: drop oldest */
        uint32_t i = (m->audio_wpos % AUDIO_RING) * 2;
        int spk = 0;
        if (m->spk_on) {
            double f = PIT_HZ / (m->pit_reload[2] ? m->pit_reload[2] : 65536);
            spk = fmod(m->audio_gen_pos * f / m->audio_rate, 1.0) < 0.5 ? 3000 : -3000;
        }
        int l = s[0] + spk, r = s[1] + spk;
        if (l > 32767) l = 32767; if (l < -32768) l = -32768;
        if (r > 32767) r = 32767; if (r < -32768) r = -32768;
        m->audio_ring[i] = (int16_t)l; m->audio_ring[i + 1] = (int16_t)r;
        m->audio_wpos++;
        m->audio_gen_pos += 1.0;
    }
}

int machine_audio_pull(Machine *m, int16_t *out, int frames) {
    int n = 0;
    while (n < frames && m->audio_rpos != m->audio_wpos) {
        uint32_t i = (m->audio_rpos % AUDIO_RING) * 2;
        out[n * 2] = m->audio_ring[i]; out[n * 2 + 1] = m->audio_ring[i + 1];
        m->audio_rpos++; n++;
    }
    return n;
}

static uint8_t opl_status(Machine *m) {
    uint8_t st = 0;
    if ((m->opl_ctrl & 1) && !(m->opl_ctrl & 0x40)) {
        double p = (256 - m->opl_timer1) * 80.0;
        if (now_us(m) - m->opl_t1_start >= p) st |= 0x40;
    }
    if ((m->opl_ctrl & 2) && !(m->opl_ctrl & 0x20)) {
        double p = (256 - m->opl_timer2) * 320.0;
        if (now_us(m) - m->opl_t2_start >= p) st |= 0x20;
    }
    if (st) st |= 0x80;
    return st | 0x06;  /* low bits as on a real OPL2 */
}

static void opl_write_data(Machine *m, uint8_t v) {
    uint8_t r = m->opl_index;
    if (r == 2) m->opl_timer1 = v;
    else if (r == 3) m->opl_timer2 = v;
    else if (r == 4) {
        if (v & 0x80) { m->opl_ctrl &= 0x03; m->opl_t1_start = m->opl_t2_start = now_us(m); return; }
        if ((v & 1) && !(m->opl_ctrl & 1)) m->opl_t1_start = now_us(m);
        if ((v & 2) && !(m->opl_ctrl & 2)) m->opl_t2_start = now_us(m);
        m->opl_ctrl = v;
        return;
    }
    opl_sync(m);
    OPL3_WriteReg(&m->opl, r, v);
}

/* ------------------------------------------------------------------ VGA */
static uint8_t vga_rd(void *h, uint32_t a);
static void vga_wr(void *h, uint32_t a, uint8_t v);

static void vga_update_mapping(Machine *m) {
    int unchained = !(m->seq[4] & 0x08);
    int graphics = m->vmode == 0x13 || m->vmode == 0x0D || m->vmode == 0x12 || m->vmode == 0x10 || m->vmode == 0x0E;
    int planar = graphics && (unchained || m->vmode != 0x13);
    if (planar != m->planar) {
        machine_logf(m, 1, "VGA planar mode %s (mode %02X seq4=%02X)\n", planar ? "ON" : "off", m->vmode, m->seq[4]);
        if (planar && m->vmode == 0x13) {
            /* convert chained content to planes */
            for (uint32_t i = 0; i < 65536; i++)
                m->vram[(i & 3) * 65536 + (i >> 2)] = m->mem[0xA0000 + i];
        }
        m->planar = planar;
    }
    m->cpu.vga_hooked = planar;
}

static uint8_t vga_rd(void *h, uint32_t a) {
    Machine *m = h;
    uint32_t off = a - 0xA0000;
    for (int p = 0; p < 4; p++) m->latch[p] = m->vram[p * 65536 + off];
    if (m->gc[5] & 0x08) { /* read mode 1: color compare */
        uint8_t res = 0xFF;
        for (int p = 0; p < 4; p++) {
            if (!(m->gc[7] & (1 << p))) continue;
            uint8_t cc = (m->gc[2] & (1 << p)) ? 0xFF : 0x00;
            res &= (uint8_t)~(m->latch[p] ^ cc);
        }
        return res;
    }
    return m->latch[m->gc[4] & 3];
}

static void vga_wr(void *h, uint32_t a, uint8_t v) {
    Machine *m = h;
    uint32_t off = a - 0xA0000;
    uint8_t mask = m->seq[2] & 0x0F;
    int wmode = m->gc[5] & 3;
    int rot = m->gc[3] & 7, func = (m->gc[3] >> 3) & 3;
    uint8_t bm = m->gc[8];
    if (wmode == 0 || wmode == 3) v = (uint8_t)((v >> rot) | (v << (8 - rot)));
    if (wmode == 3) { bm &= v; }
    for (int p = 0; p < 4; p++) {
        if (!(mask & (1 << p))) continue;
        uint8_t d;
        if (wmode == 1) { m->vram[p * 65536 + off] = m->latch[p]; continue; }
        if (wmode == 0) d = (m->gc[1] & (1 << p)) ? ((m->gc[0] & (1 << p)) ? 0xFF : 0) : v;
        else if (wmode == 2) d = (v & (1 << p)) ? 0xFF : 0;
        else d = (m->gc[0] & (1 << p)) ? 0xFF : 0;
        switch (func) {
        case 1: d &= m->latch[p]; break;
        case 2: d |= m->latch[p]; break;
        case 3: d ^= m->latch[p]; break;
        }
        d = (uint8_t)((d & bm) | (m->latch[p] & ~bm));
        m->vram[p * 65536 + off] = d;
    }
}

static void vga_dac_update(Machine *m, int i) {
    uint8_t r = m->dac[i][0] & 63, g = m->dac[i][1] & 63, b = m->dac[i][2] & 63;
    m->palette_rgba[i] = 0xFF000000u | ((uint32_t)((b << 2) | (b >> 4)) << 16) |
                         ((uint32_t)((g << 2) | (g >> 4)) << 8) | (uint32_t)((r << 2) | (r >> 4));
}

void vga_set_dac(Machine *m, int i, uint8_t r, uint8_t g, uint8_t b) {
    m->dac[i][0] = r; m->dac[i][1] = g; m->dac[i][2] = b;
    vga_dac_update(m, i);
}

static void vga_out(Machine *m, uint16_t port, uint8_t v) {
    switch (port) {
    case 0x3C0:
        if (!m->attr_flip) m->attr_idx = v;
        else m->attr[m->attr_idx & 0x1F] = v;
        m->attr_flip ^= 1;
        break;
    case 0x3C2: m->misc = v; break;
    case 0x3C4: m->seq_idx = v & 7; break;
    case 0x3C5:
        m->seq[m->seq_idx] = v;
        if (m->seq_idx == 4) vga_update_mapping(m);
        break;
    case 0x3C6: m->dac_mask = v; break;
    case 0x3C7: m->dac_ridx = v; m->dac_rcomp = 0; m->dac_state = 0; break;
    case 0x3C8: m->stat_dac++; m->dac_widx = v; m->dac_wcomp = 0; m->dac_state = 3; break;
    case 0x3C9:
        m->dac[m->dac_widx][m->dac_wcomp] = v & 63;
        if (++m->dac_wcomp == 3) { vga_dac_update(m, m->dac_widx); m->dac_wcomp = 0; m->dac_widx++; }
        break;
    case 0x3CE: m->gc_idx = v & 15; break;
    case 0x3CF: m->gc[m->gc_idx] = v; break;
    case 0x3D4: case 0x3B4: m->crtc_idx = v & 31; break;
    case 0x3D5: case 0x3B5: m->crtc[m->crtc_idx] = v; break;
    default: break;
    }
}

static uint8_t vga_in(Machine *m, uint16_t port) {
    switch (port) {
    case 0x3C0: return m->attr_idx;
    case 0x3C1: return m->attr[m->attr_idx & 0x1F];
    case 0x3C2: return 0x10;
    case 0x3C4: return m->seq_idx;
    case 0x3C5: return m->seq[m->seq_idx];
    case 0x3C6: return m->dac_mask;
    case 0x3C7: return m->dac_state;
    case 0x3C8: return m->dac_widx;
    case 0x3C9: {
        uint8_t v = m->dac[m->dac_ridx][m->dac_rcomp];
        if (++m->dac_rcomp == 3) { m->dac_rcomp = 0; m->dac_ridx++; }
        return v;
    }
    case 0x3CC: return m->misc;
    case 0x3CE: return m->gc_idx;
    case 0x3CF: return m->gc[m->gc_idx];
    case 0x3D4: return m->crtc_idx;
    case 0x3D5: return m->crtc[m->crtc_idx];
    case 0x3DA: case 0x3BA: {
        m->stat_3da++;
        m->attr_flip = 0;
        double t = fmod(now_us(m), VGA_FRAME_US);
        uint8_t r = 0;
        if (t >= VGA_FRAME_US - VGA_RETRACE_US) r |= 0x09;
        else {
            /* horizontal blanking toggles at 31.5 kHz */
            double h = fmod(now_us(m), 31.78);
            if (h > 25.4) r |= 0x01;
        }
        return r;
    }
    default: return 0xFF;
    }
}

/* ------------------------------------------------------------------ ports */
static uint8_t port_in(void *h, uint16_t port) {
    Machine *m = h;
    switch (port) {
    case 0x20: return m->pic_irr;
    case 0x21: return m->pic_imr;
    case 0x40: case 0x41: case 0x42: return pit_read(m, port - 0x40);
    case 0x60: return m->kb_data;
    case 0x61: {
        /* bit 4 toggles with refresh, bit 5 = timer 2 output */
        uint8_t v = m->port61 & 0x0F;
        if (fmod(now_us(m), 30.0) < 15.0) v |= 0x10;
        if (m->port61 & 1) {
            double per = pit_period_us(m, 2);
            if (fmod(now_us(m) - m->pit_start_us[2], per) < per / 2) v |= 0x20;
        }
        return v;
    }
    case 0x64: return 0x14;
    case 0x201: {
        m->joy_reads++;
        io_delay(m, 1.0); /* an ISA I/O read takes about a microsecond */
        uint8_t v = 0xF0;
        int cal = m->joy_autocal && m->joy_cal_step < 9;
        uint8_t btn = cal ? (uint8_t)m->joy_cal_btn : m->joy_buttons;
        for (int b = 0; b < 4; b++) if (btn & (1 << b)) v &= (uint8_t)~(0x10 << b);
        /* The one-shot timers are modelled in port reads rather than time, so
         * the game's counting loops give the same result at any CPU speed
         * (about 500 reads at centre, like a 386 on an ISA bus). */
        uint64_t reads = m->joy_reads - m->joy_reads_at_trigger;
        for (int a = 0; a < 4; a++) {
            float pos = m->joy_axis[a];
            if (cal && a == 0) pos = m->joy_cal_x;
            if (cal && a == 1) pos = m->joy_cal_y;
            if (pos < -1) pos = -1;
            if (pos > 1) pos = 1;
            if (a >= 2 && m->joy_axis[a] < -1.5f) continue; /* absent axis */
            uint64_t len = 20 + (uint64_t)((pos + 1.0f) * 500.0f);
            if (reads < len) v |= (uint8_t)(1 << a);
        }
        return v;
    }
    case 0x388: case 0x228: case 0x220: case 0x222: return opl_status(m);
    case 0x389: case 0x229: case 0x221: case 0x223: return 0xFF;
    case 0x226: case 0x22A: case 0x22C: case 0x22E: return 0xFF; /* no SB DSP */
    case 0x70: case 0x71: return 0;
    }
    if (port >= 0x3B0 && port <= 0x3DF) return vga_in(m, port);
    machine_logf(m, 2, "IN  %04X\n", port);
    return 0xFF;
}

static void port_out(void *h, uint16_t port, uint8_t v) {
    Machine *m = h;
    switch (port) {
    case 0x20:
        if (v & 0x10) { m->pic_icw_step = 1; m->pic_isr = 0; return; }
        if (v == 0x20) pic_eoi(m);
        else if ((v & 0xE0) == 0x60) { m->pic_isr &= (uint8_t)~(1 << (v & 7)); pic_update(m); }
        return;
    case 0x21:
        if (m->pic_icw_step) { if (++m->pic_icw_step > 3) m->pic_icw_step = 0; return; }
        m->pic_imr = v; pic_update(m); return;
    case 0x40: case 0x41: case 0x42: case 0x43: pit_write(m, port, v); return;
    case 0x61:
        opl_sync(m);
        m->port61 = v;
        m->spk_on = (v & 3) == 3;
        return;
    case 0x201:
        m->joy_trigger_us = now_us(m); m->joy_triggers++; m->joy_reads_at_trigger = m->joy_reads;
        if (m->joy_triggers == 1) { m->joy_cal_t = m->time_us; m->joy_cal_trig = 1; }
        return;
    case 0x388: case 0x228: case 0x220: case 0x222: m->opl_index = v;
        return;
    case 0x389: case 0x229: case 0x221: case 0x223: opl_write_data(m, v); return;
    case 0x80: case 0xA0: case 0xA1: return;
    }
    if (port >= 0x3B0 && port <= 0x3DF) { vga_out(m, port, v); return; }
    if (port <= 0x0F || (port >= 0x81 && port <= 0x8F)) return; /* DMA */
    machine_logf(m, 2, "OUT %04X,%02X\n", port, v);
}

/* ------------------------------------------------------------------ input */
static void kbq_put(Machine *m, int scancode, int ext, int down);

void machine_key(Machine *m, int scancode, int ext, int down) {
    int code = scancode | (ext ? 0x100 : 0);
    int modifier = scancode == 0x2A || scancode == 0x36 || scancode == 0x1D || scancode == 0x38 || scancode == 0x3A || scancode == 0x45 || scancode == 0x46;
    if (down && !modifier) { m->typ_code = code; m->typ_next_us = m->time_us + m->typ_delay_us; }
    else if (!down && code == m->typ_code) m->typ_code = 0;
    kbq_put(m, scancode, ext, down);
}

static void kbq_put(Machine *m, int scancode, int ext, int down) {
    int n = 0;
    uint8_t codes[2];
    if (ext) codes[n++] = 0xE0;
    codes[n++] = (uint8_t)(scancode | (down ? 0 : 0x80));
    for (int i = 0; i < n; i++) {
        if (((m->kbq_tail + 1) & 255) == m->kbq_head) return;
        m->kbq[m->kbq_tail] = codes[i];
        m->kbq_tail = (m->kbq_tail + 1) & 255;
    }
}

/* SWOTL keeps its own cursor and only uses the driver's position deltas, so
 * the driver position is not clamped to the screen: clamping would make the
 * two drift apart at the edges. */
static void mouse_clip(Machine *m) {
    if (m->mouse_x < -30000) m->mouse_x = -30000;
    if (m->mouse_x > 30000) m->mouse_x = 30000;
    if (m->mouse_y < -30000) m->mouse_y = -30000;
    if (m->mouse_y > 30000) m->mouse_y = 30000;
}
void machine_mouse_move(Machine *m, int dx, int dy) {
    m->mouse_x += dx; m->mouse_y += dy;
    m->mouse_mickey_x += dx * 2; m->mouse_mickey_y += dy * 2;
    mouse_clip(m);
}
void machine_mouse_abs(Machine *m, double fx, double fy) {
    int nx = m->mouse_minx + (int)(fx * (m->mouse_maxx - m->mouse_minx + 1));
    int ny = m->mouse_miny + (int)(fy * (m->mouse_maxy - m->mouse_miny + 1));
    m->mouse_mickey_x += (nx - m->mouse_x) * 2; m->mouse_mickey_y += (ny - m->mouse_y) * 2;
    m->mouse_x = nx; m->mouse_y = ny;
    mouse_clip(m);
}
void machine_mouse_buttons(Machine *m, int buttons) {
    for (int b = 0; b < 3; b++) {
        int was = (m->mouse_buttons >> b) & 1, now = (buttons >> b) & 1;
        if (!was && now) { m->mouse_press_cnt[b]++; m->mouse_press_x[b] = m->mouse_x; m->mouse_press_y[b] = m->mouse_y; }
        if (was && !now) { m->mouse_rel_cnt[b]++; m->mouse_rel_x[b] = m->mouse_x; m->mouse_rel_y[b] = m->mouse_y; }
    }
    m->mouse_buttons = buttons;
}
void machine_joystick(Machine *m, float x, float y, float x2, float y2, int buttons) {
    m->joy_axis[0] = x; m->joy_axis[1] = y; m->joy_axis[2] = x2; m->joy_axis[3] = y2;
    m->joy_buttons = (uint8_t)buttons;
}

/* ------------------------------------------------------------------ render */
static const uint8_t font8x8[256][8] = {
#include "font8x8.inc"
};

int machine_render(Machine *m, uint32_t *rgba, int *w, int *h) {
    if (m->vmode == 0x13) {
        *w = 320; *h = 200;
        if (!m->planar) {
            const uint8_t *src = m->mem + 0xA0000;
            for (int i = 0; i < 64000; i++) rgba[i] = m->palette_rgba[src[i] & m->dac_mask];
        } else {
            uint32_t start = ((uint32_t)m->crtc[0x0C] << 8) | m->crtc[0x0D];
            uint32_t stride = (uint32_t)m->crtc[0x13] * 2;
            if (!stride) stride = 80;
            int lines = (m->crtc[9] & 0x1F) ? 200 : 400;
            *h = lines;
            for (int y = 0; y < lines; y++)
                for (int x = 0; x < 320; x++) {
                    uint32_t off = (start + y * stride + (x >> 2)) & 0xFFFF;
                    rgba[y * 320 + x] = m->palette_rgba[m->vram[(x & 3) * 65536 + off] & m->dac_mask];
                }
        }
        return 1;
    }
    if (m->vmode == 0x0D || m->vmode == 0x0E || m->vmode == 0x10 || m->vmode == 0x12) {
        int W = m->vmode == 0x0D ? 320 : 640;
        int H = m->vmode == 0x12 ? 480 : (m->vmode == 0x10 ? 350 : 200);
        *w = W; *h = H;
        uint32_t start = ((uint32_t)m->crtc[0x0C] << 8) | m->crtc[0x0D];
        uint32_t stride = W / 8;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                uint32_t off = (start + y * stride + (x >> 3)) & 0xFFFF;
                int bit = 7 - (x & 7), ci = 0;
                for (int p = 0; p < 4; p++) ci |= ((m->vram[p * 65536 + off] >> bit) & 1) << p;
                uint8_t dacidx = m->attr[ci & 15] & 0x3F;
                rgba[y * W + x] = m->palette_rgba[dacidx];
            }
        return 1;
    }
    /* text mode 80x25 rendered with 8x8 font -> 640x200 */
    *w = 640; *h = 200;
    static const uint8_t ega[16][3] = {
        {0,0,0},{0,0,42},{0,42,0},{0,42,42},{42,0,0},{42,0,42},{42,21,0},{42,42,42},
        {21,21,21},{21,21,63},{21,63,21},{21,63,63},{63,21,21},{63,21,63},{63,63,21},{63,63,63}};
    for (int row = 0; row < 25; row++)
        for (int col = 0; col < 80; col++) {
            uint8_t ch = m->mem[0xB8000 + (row * 80 + col) * 2];
            uint8_t at = m->mem[0xB8000 + (row * 80 + col) * 2 + 1];
            for (int yy = 0; yy < 8; yy++) {
                uint8_t bits = font8x8[ch][yy];
                for (int xx = 0; xx < 8; xx++) {
                    int ci = (bits & (0x80 >> xx)) ? (at & 15) : (at >> 4) & 7;
                    uint32_t col32 = 0xFF000000u | ((uint32_t)(ega[ci][2] * 4) << 16) | ((uint32_t)(ega[ci][1] * 4) << 8) | (uint32_t)(ega[ci][0] * 4);
                    rgba[(row * 8 + yy) * 640 + col * 8 + xx] = col32;
                }
            }
        }
    return 1;
}

const uint8_t *machine_font8x8(void) { return &font8x8[0][0]; }

int machine_mouse_active(Machine *m) { return m->time_us - m->mouse_last_poll_us < 300000.0; }

/* ------------------------------------------------------------------ run */
/* The game asks to centre the stick, then upper-left, then lower-right, each
 * confirmed with the fire button. Answer that automatically, paced by the
 * game's own joystick polling. */
static void joy_autocal_update(Machine *m) {
    static const float tx[3] = {0, -1, 1}, ty[3] = {0, -1, 1};
    if (!m->joy_autocal || m->joy_cal_step >= 9 || !m->joy_triggers) return;
    int target = m->joy_cal_step / 3, phase = m->joy_cal_step % 3;
    double el = m->time_us - m->joy_cal_t;
    uint64_t polls = m->joy_triggers - m->joy_cal_trig;
    m->joy_cal_x = tx[target]; m->joy_cal_y = ty[target];
    int advance = 0;
    if (phase == 0) { m->joy_cal_btn = 0; advance = polls >= 8 && el > 300000; }
    else if (phase == 1) { m->joy_cal_btn = 1; advance = polls >= 4 && el > 150000; }
    else { m->joy_cal_btn = 0; advance = polls >= 8 && el > 400000; }
    if (advance) {
        m->joy_cal_step++; m->joy_cal_t = m->time_us; m->joy_cal_trig = m->joy_triggers;
        if (m->joy_cal_step == 9) machine_logf(m, 0, "joystick calibration answered automatically\n");
    }
}

static void service_events(Machine *m) {
    joy_autocal_update(m);
    /* typematic: repeat the held key at the BIOS rate (default 10.9/s after
     * 500 ms) instead of the host's, so keyboard flying feels as in 1991 */
    if (m->typ_code && m->time_us >= m->typ_next_us) {
        int queued = (m->kbq_tail - m->kbq_head) & 255;
        if (queued < 4) kbq_put(m, m->typ_code & 0xFF, (m->typ_code & 0x100) != 0, 1);
        m->typ_next_us += 1e6 / m->typ_rate_hz;
        if (m->typ_next_us < m->time_us) m->typ_next_us = m->time_us + 1e6 / m->typ_rate_hz;
    }
    while (m->time_us >= m->next_irq0_us) {
        if (m->pic_irr & 1) { m->lost_ticks++; machine_logf(m, 2, "lost timer tick at %04X:%04X\n", m->cpu.s[CS], m->cpu.ip); }
        m->ticks_raised++;
        pic_raise(m, 0);
        m->next_irq0_us += pit_period_us(m, 0);
    }
    if (m->kbq_head != m->kbq_tail && m->time_us >= m->kb_next_us &&
        !(m->pic_irr & 2) && !(m->pic_isr & 2)) {
        m->kb_data = m->kbq[m->kbq_head];
        m->kbq_head = (m->kbq_head + 1) & 255;
        pic_raise(m, 1);
        m->kb_next_us = m->time_us + 1000.0;
    }
}

static int sig_match(const uint8_t *p, const uint8_t *sig, const uint8_t *any, size_t n) {
    for (size_t i = 0; i < n; i++) if (!any[i] && p[i] != sig[i]) return 0;
    return 1;
}

void machine_run(Machine *m, double us) {
    double end = m->time_us + us;
    CPU *c = &m->cpu;
    while (m->time_us < end && !m->exited) {
        service_events(m);
        deliver_irq(m);
        double next = end;
        if (m->next_irq0_us < next) next = m->next_irq0_us;
        if (m->kbq_head != m->kbq_tail && m->kb_next_us < next && m->kb_next_us > m->time_us) next = m->kb_next_us;
        if (c->halted) {
            if (!(c->fl & F_IF)) { machine_logf(m, 0, "CPU halted with interrupts disabled at %04X:%04X\n", c->s[CS], c->ip); m->exited = 1; break; }
            m->time_us = next > m->time_us ? next : m->time_us + 1;
            continue;
        }
        double span = next - m->time_us;
        int n = (int)(span * m->ips);
        if (n < 16) n = 16;
        if (n > 4000) n = 4000;
        m->slice_t0 = m->time_us; m->slice_ic0 = c->icount; m->in_slice = 1;
        m->slice_deadline = next;
        cpu_run(c, n);
        m->time_us = now_us(m);
        m->in_slice = 0;
        if (c->fault) { c->fault = 0; }
    }
    opl_sync(m);
    /* find the VGA driver's frame-present routine (RTSVGA.OVL) to count frames */
    if (m->time_us - m->sig_scan_us > 1e6) {
        /* push bp; mov bp,sp; push ds; mov cs:[xxxx],ds; mov word cs:[xxxx],0;
         * push si; push di; cld. The xxxx offsets differ between game
         * versions (1991: 330C/330E; the later release in LucasArts Air Combat
         * Classics: 35DC/35DE). */
        static const uint8_t sig[] = {0x55, 0x8B, 0xEC, 0x1E, 0x2E, 0x8C, 0x1E, 0x00, 0x00, 0x2E, 0xC7, 0x06, 0x00, 0x00, 0x00, 0x00, 0x56, 0x57, 0xFC};
        static const uint8_t any[sizeof sig] = {0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0};
        m->sig_scan_us = m->time_us;
        if (m->hires) hires_scan(m->hires, m, machine_in_3d(m));
        uint32_t w = m->cpu.watch_lin;
        if (!w || !sig_match(m->mem + w, sig, any, sizeof sig)) {
            m->cpu.watch_lin = 0;
            for (uint32_t a = 0x500; a < 0xA0000 - sizeof sig; a++)
                if (m->mem[a] == 0x55 && sig_match(m->mem + a, sig, any, sizeof sig)) { m->cpu.watch_lin = a; break; }
        }
    }
}

uint64_t machine_frames(Machine *m) { return m->cpu.watch_hits; }

static void on_present(void *host, CPU *c) {
    Machine *m = host;
    if (m->hires) hires_present(m->hires, m, c);
}

static void on_breakpoint(void *host, CPU *c) {
    Machine *m = host;
    if (m->hires) hires_breakpoint(m->hires, m, c);
}

void machine_set_hires(Machine *m, int on) {
    if (on && !m->hires) {
        m->hires = hires_create();
        if (m->hires) hires_scan(m->hires, m, 0);
    } else if (!on && m->hires) {
        hires_detach(m->hires, m);
        hires_destroy(m->hires);
        m->hires = NULL;
    }
}

int machine_render_hires(Machine *m, int n, uint32_t *out) {
    return m->hires ? hires_compose(m->hires, m, n, out) : 0;
}

const char *machine_hires_state(Machine *m) {
    return m->hires ? hires_state(m->hires) : "";
}

int machine_in_3d(Machine *m) {
    if (m->cpu.watch_hits != m->frames_seen) { m->frames_seen = m->cpu.watch_hits; m->last_frame_us = m->time_us; }
    return m->frames_seen > 0 && m->time_us - m->last_frame_us < 500000.0;
}

/* ------------------------------------------------------------------ create */
static const uint8_t vga_default_16[16][3] = {
    {0,0,0},{0,0,42},{0,42,0},{0,42,42},{42,0,0},{42,0,42},{42,21,0},{42,42,42},
    {21,21,21},{21,21,63},{21,63,21},{21,63,63},{63,21,21},{63,21,63},{63,63,21},{63,63,63}};

void vga_set_mode(Machine *m, int mode, int clear) {
    m->vmode = mode & 0x7F;
    memset(m->seq, 0, sizeof m->seq);
    memset(m->gc, 0, sizeof m->gc);
    m->seq[2] = 0x0F;
    m->seq[4] = (m->vmode == 0x13) ? 0x0E : 0x06;
    m->gc[5] = (m->vmode == 0x13) ? 0x40 : 0x00;
    m->gc[6] = 0x05;
    m->gc[7] = 0x0F;
    m->gc[8] = 0xFF;
    memset(m->crtc, 0, sizeof m->crtc);
    m->crtc[0x13] = 0x28;
    m->crtc[0x09] = (m->vmode == 0x13 || m->vmode == 0x0D) ? 0x41 : 0x4F;
    m->crtc[0x14] = 0x40; m->crtc[0x17] = 0xA3;
    for (int i = 0; i < 16; i++) m->attr[i] = (uint8_t)(i < 8 ? i : i + 0x30);
    if (m->vmode == 0x0D || m->vmode == 0x0E) for (int i = 0; i < 16; i++) m->attr[i] = (uint8_t)(i < 8 ? i : i + 8);
    m->attr[0x10] = (m->vmode == 0x13) ? 0x41 : 0x01;
    m->dac_mask = 0xFF;
    /* default palette */
    for (int i = 0; i < 16; i++) vga_set_dac(m, i, vga_default_16[i][0], vga_default_16[i][1], vga_default_16[i][2]);
    if (m->vmode == 0x0D || m->vmode == 0x0E || m->vmode == 0x10 || m->vmode == 0x12 || m->vmode < 4)
        for (int i = 0; i < 16; i++) { int j = i < 8 ? i : i + 8; (void)j; }
    for (int i = 16; i < 32; i++) { uint8_t g = (uint8_t)((i - 16) * 63 / 15); vga_set_dac(m, i, g, g, g); }
    for (int i = 32; i < 256; i++) {
        int k = (i - 32) % 24, grp = (i - 32) / 24;
        double hue = k / 24.0 * 6.0;
        double rr = fabs(fmod(hue + 0.0, 6.0) - 3.0) - 1.0, gg = 2.0 - fabs(fmod(hue + 4.0, 6.0) - 2.0), bb = 2.0 - fabs(fmod(hue + 2.0, 6.0) - 2.0);
        (void)gg; (void)bb;
        double r = fmin(fmax(rr, 0), 1), g = fmin(fmax(2.0 - fabs(fmod(hue, 6.0) - 2.0), 0), 1), b = fmin(fmax(2.0 - fabs(fmod(hue, 6.0) - 4.0), 0), 1);
        double inten = grp < 3 ? 1.0 : (grp < 6 ? 0.45 : 0.25), sat = (grp % 3 == 0) ? 1.0 : (grp % 3 == 1 ? 0.5 : 0.25);
        if (i >= 248) { vga_set_dac(m, i, 0, 0, 0); continue; }
        vga_set_dac(m, i, (uint8_t)(63 * inten * (1 - sat + sat * r)), (uint8_t)(63 * inten * (1 - sat + sat * g)), (uint8_t)(63 * inten * (1 - sat + sat * b)));
    }
    if (m->vmode == 0x10 || m->vmode == 0x12 || m->vmode < 4) {
        /* 16-colour modes map attribute values straight into DAC 0-63 (EGA style) */
        for (int i = 0; i < 64; i++) {
            int r = ((i >> 2) & 1) * 42 + ((i >> 5) & 1) * 21, g = ((i >> 1) & 1) * 42 + ((i >> 4) & 1) * 21, b = (i & 1) * 42 + ((i >> 3) & 1) * 21;
            vga_set_dac(m, i, (uint8_t)r, (uint8_t)g, (uint8_t)b);
        }
        for (int i = 0; i < 16; i++) m->attr[i] = (uint8_t)(i < 8 ? (i == 6 ? 0x14 : i) : i + 0x30);
    }
    if (clear) {
        if (m->vmode == 0x13 || m->vmode >= 0x0D) { memset(m->mem + 0xA0000, 0, 0x10000); memset(m->vram, 0, 4 * 65536); }
        else for (int i = 0; i < 80 * 25; i++) { m->mem[0xB8000 + i * 2] = ' '; m->mem[0xB8000 + i * 2 + 1] = 7; }
    }
    m->planar = -1;
    vga_update_mapping(m);
    m->mem[0x449] = (uint8_t)m->vmode;
    machine_logf(m, 1, "set video mode %02X\n", mode);
}

Machine *machine_create(const char *gamedir, const char *exe, const char *args) {
    Machine *m = calloc(1, sizeof *m);
    m->mem = calloc(1, MEM_SIZE);
    m->vram = calloc(4, 65536);
    snprintf(m->gamedir, sizeof m->gamedir, "%s", gamedir);
    m->log = stderr;
    m->log_level = 0;
    m->ips = 20.0;
    m->audio_rate = 0;
    m->cpu.mem = m->mem;
    m->cpu.host = m;
    m->cpu.in8 = port_in;
    m->cpu.out8 = port_out;
    m->cpu.vga_rd = vga_rd;
    m->cpu.vga_wr = vga_wr;
    cpu_reset(&m->cpu);
    m->cpu.watch_hook = on_present;
    m->cpu.bp_hook = on_breakpoint;
    m->pic_imr = 0xB8; /* timer, keyboard, cascade, mouse unmasked */
    m->pit_rw[0] = 3; m->pit_mode[0] = 3;
    m->next_irq0_us = pit_period_us(m, 0);
    m->joy_axis[2] = m->joy_axis[3] = -2.0f; /* second stick absent */
    m->joy_autocal = 1;
    m->typ_rate_hz = 30.0;
    m->typ_delay_us = 250000.0;
    m->mouse_present = 1;
    m->mouse_maxx = 639; m->mouse_maxy = 199;
    m->mouse_x = 320; m->mouse_y = 100;
    OPL3_Reset(&m->opl, 44100);
    hle_init(m);
    vga_set_mode(m, 3, 1);
    if (hle_load_program(m, exe, args) != 0) {
        machine_destroy(m);
        return NULL;
    }
    return m;
}

void machine_set_audio_rate(Machine *m, int rate) {
    m->audio_rate = rate;
    OPL3_Reset(&m->opl, (uint32_t)rate);
    m->audio_gen_pos = m->time_us * rate / 1e6;
}

void machine_destroy(Machine *m) {
    if (!m) return;
    for (int i = 0; i < MAX_FILES; i++) if (m->files[i]) fclose(m->files[i]);
    if (m->hires) hires_destroy(m->hires);
    free(m->vram);
    free(m->mem);
    free(m);
}
