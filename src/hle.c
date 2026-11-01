/* SWOTL Enhanced - BIOS and DOS services implemented on the host.
 * GPL-3.0-or-later. */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "hle.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define mkdir_host(p) _mkdir(p)
#else
#include <unistd.h>
#define mkdir_host(p) mkdir(p, 0755)
#endif

#define ROM_SEG 0xF000
#define STUB_TICK 0x0400
#define STUB_KBD  0x0410
#define STUB_EOI  0x0420
#define STUB_IRET 0x0430
#define FONT_OFF  0xFA6E

/* First memory block. The environment takes 0x20 paragraphs, so the program
 * PSP lands at 0x01C1 and the load segment at 0x01D1. The load segment ends
 * up in the overlay fixups. Some copies of the game have a modified
 * NOTCAMP.OVL whose start-up code only behaves correctly at certain load
 * segments: with the old value 0x0132 the "CHOOSE A SIDE" screen was skipped
 * and the menu text showed stray '_' marks. At 0x01D1 these copies work as
 * in DOSBox; unmodified copies work at any load segment. */
#define MCB_FIRST 0x019F
#define MEM_TOP   0xA000

/* ----------------------------------------------------------- helpers */
static inline uint8_t rd8(Machine *m, uint16_t s, uint16_t o) { return m->mem[cpu_lin(&m->cpu, s, o)]; }
static inline uint16_t rd16(Machine *m, uint16_t s, uint16_t o) { return (uint16_t)(rd8(m, s, o) | (rd8(m, s, (uint16_t)(o + 1)) << 8)); }
static inline void wr8(Machine *m, uint16_t s, uint16_t o, uint8_t v) { mem_wb(&m->cpu, cpu_lin(&m->cpu, s, o), v); }
static inline void wr16(Machine *m, uint16_t s, uint16_t o, uint16_t v) { wr8(m, s, o, (uint8_t)v); wr8(m, s, (uint16_t)(o + 1), (uint8_t)(v >> 8)); }

#define R(x) (c->r[x])
#define AL ((uint8_t)c->r[AX])
#define AHr ((uint8_t)(c->r[AX] >> 8))
#define BLr ((uint8_t)c->r[BX])
#define BHr ((uint8_t)(c->r[BX] >> 8))
#define CLr ((uint8_t)c->r[CX])
#define CHr ((uint8_t)(c->r[CX] >> 8))
#define DLr ((uint8_t)c->r[DX])
#define DHr ((uint8_t)(c->r[DX] >> 8))
static inline void setAL(CPU *c, uint8_t v) { c->r[AX] = (uint16_t)((c->r[AX] & 0xFF00) | v); }
static inline void setAH(CPU *c, uint8_t v) { c->r[AX] = (uint16_t)((c->r[AX] & 0x00FF) | (v << 8)); }
static inline void setBL(CPU *c, uint8_t v) { c->r[BX] = (uint16_t)((c->r[BX] & 0xFF00) | v); }
static inline void setBH(CPU *c, uint8_t v) { c->r[BX] = (uint16_t)((c->r[BX] & 0x00FF) | (v << 8)); }
static inline void setCL(CPU *c, uint8_t v) { c->r[CX] = (uint16_t)((c->r[CX] & 0xFF00) | v); }
static inline void setCH(CPU *c, uint8_t v) { c->r[CX] = (uint16_t)((c->r[CX] & 0x00FF) | (v << 8)); }
static inline void setDL(CPU *c, uint8_t v) { c->r[DX] = (uint16_t)((c->r[DX] & 0xFF00) | v); }
static inline void setDH(CPU *c, uint8_t v) { c->r[DX] = (uint16_t)((c->r[DX] & 0x00FF) | (v << 8)); }

/* flags saved on the stack by INT: SS:SP -> IP, CS, FLAGS */
static void stack_flag(Machine *m, uint16_t flag, int on) {
    CPU *c = &m->cpu;
    uint16_t f = rd16(m, c->s[SS], (uint16_t)(c->r[SP] + 4));
    f = on ? (f | flag) : (f & (uint16_t)~flag);
    wr16(m, c->s[SS], (uint16_t)(c->r[SP] + 4), f);
}
static void set_cf(Machine *m, int on) { stack_flag(m, F_CF, on); }
static void dos_error(Machine *m, uint16_t err) { m->cpu.r[AX] = err; m->last_error = err; set_cf(m, 1); }
/* re-execute the trap later (blocking call); enable interrupts like the BIOS would */
static void block_retry(Machine *m) {
    CPU *c = &m->cpu;
    c->ip -= 2;
    c->fl |= F_IF;
    c->halted = 1;
}

static void read_asciiz(Machine *m, uint16_t s, uint16_t o, char *out, size_t n) {
    size_t i = 0;
    for (; i + 1 < n; i++) {
        char ch = (char)rd8(m, s, (uint16_t)(o + i));
        if (!ch) break;
        out[i] = ch;
    }
    out[i] = 0;
}

/* ----------------------------------------------------------- host paths */
/* simpler, correct matcher used everywhere */
static int match_part(const char *p, const char *n, int len) {
    int pi = 0, ni = 0;
    for (int k = 0; k < len; k++) {
        char pc = p[pi], nc = n[ni];
        if (pc == '*') return 1;
        if (!pc) return nc == 0;
        if (pc == '?') { pi++; if (nc) ni++; continue; }
        if (toupper((unsigned char)pc) != toupper((unsigned char)nc)) return 0;
        pi++; ni++;
    }
    return n[ni] == 0;
}
static int dos_match(const char *pat, const char *name) {
    char pn[16], pe[8], nn[64], ne[64];
    const char *d = strchr(pat, '.');
    if (d) { snprintf(pn, sizeof pn, "%.*s", (int)(d - pat), pat); snprintf(pe, sizeof pe, "%s", d + 1); }
    else { snprintf(pn, sizeof pn, "%s", pat); pe[0] = 0; }
    const char *e = strrchr(name, '.');
    if (e && e != name) { snprintf(nn, sizeof nn, "%.*s", (int)(e - name), name); snprintf(ne, sizeof ne, "%s", e + 1); }
    else { snprintf(nn, sizeof nn, "%s", name); ne[0] = 0; }
    if (strlen(nn) > 8 || strlen(ne) > 3) return 0;
    if (!d && !strcmp(pn, "*")) return 1;
    return match_part(pn, nn, 8) && match_part(pe, ne, 3);
}

/* Resolve a DOS path to a host path. Components are matched case-insensitively.
 * *exists is set if the final component exists. Returns 0 on success, -1 if a
 * directory component is missing. */
int host_resolve(Machine *m, const char *dospath, char *out, size_t outsz, int *exists) {
    char path[300];
    const char *p = dospath;
    if (p[0] && p[1] == ':') p += 2;
    if (*p == '\\' || *p == '/') snprintf(path, sizeof path, "%s", p + 1);
    else if (m->cwd[0]) snprintf(path, sizeof path, "%s\\%s", m->cwd, p);
    else snprintf(path, sizeof path, "%s", p);

    char comps[16][64]; int nc = 0;
    char *tok = path, *q;
    for (;;) {
        q = tok;
        while (*q && *q != '\\' && *q != '/') q++;
        char save = *q; *q = 0;
        if (*tok && strcmp(tok, ".")) {
            if (!strcmp(tok, "..")) { if (nc) nc--; }
            else if (nc < 16) snprintf(comps[nc++], 64, "%s", tok);
        }
        if (!save) break;
        tok = q + 1;
    }
    snprintf(out, outsz, "%s", m->gamedir);
    if (exists) *exists = 1;
    for (int i = 0; i < nc; i++) {
        char found[256] = "";
        DIR *dir = opendir(out);
        if (dir) {
            struct dirent *de;
            while ((de = readdir(dir))) {
#ifdef _WIN32
                if (!_stricmp(de->d_name, comps[i])) { snprintf(found, sizeof found, "%s", de->d_name); break; }
#else
                if (!strcasecmp(de->d_name, comps[i])) { snprintf(found, sizeof found, "%s", de->d_name); break; }
#endif
            }
            closedir(dir);
        }
        size_t l = strlen(out);
        if (!found[0]) {
            if (exists) *exists = 0;
            if (i < nc - 1) return -1;
            for (char *s = comps[i]; *s; s++) *s = (char)toupper((unsigned char)*s);
            snprintf(out + l, outsz - l, "/%s", comps[i]);
        } else {
            snprintf(out + l, outsz - l, "/%s", found);
        }
    }
    return 0;
}

/* ----------------------------------------------------------- memory (MCB) */
static void mcb_set(Machine *m, uint16_t seg, char type, uint16_t owner, uint16_t size) {
    wr8(m, seg, 0, (uint8_t)type); wr16(m, seg, 1, owner); wr16(m, seg, 3, size);
}
static void mcb_coalesce(Machine *m) {
    uint16_t seg = MCB_FIRST;
    for (;;) {
        char type = (char)rd8(m, seg, 0);
        uint16_t owner = rd16(m, seg, 1), size = rd16(m, seg, 3);
        if (type == 'Z') break;
        uint16_t next = (uint16_t)(seg + size + 1);
        if (!owner && rd16(m, next, 1) == 0) {
            char ntype = (char)rd8(m, next, 0);
            mcb_set(m, seg, ntype, 0, (uint16_t)(size + rd16(m, next, 3) + 1));
            continue;
        }
        seg = next;
        if (type != 'M') break;
    }
}
static uint16_t mcb_alloc(Machine *m, uint16_t paras, uint16_t owner, uint16_t *largest) {
    mcb_coalesce(m);
    uint16_t seg = MCB_FIRST, best = 0;
    *largest = 0;
    for (;;) {
        char type = (char)rd8(m, seg, 0);
        uint16_t own = rd16(m, seg, 1), size = rd16(m, seg, 3);
        if (!own) {
            if (size > *largest) *largest = size;
            if (size >= paras) {
                if (size > paras) {
                    uint16_t nseg = (uint16_t)(seg + paras + 1);
                    mcb_set(m, nseg, type, 0, (uint16_t)(size - paras - 1));
                    mcb_set(m, seg, 'M', owner, paras);
                } else mcb_set(m, seg, type, owner, size);
                best = (uint16_t)(seg + 1);
                return best;
            }
        }
        if (type == 'Z') break;
        seg = (uint16_t)(seg + size + 1);
    }
    return 0;
}
static int mcb_free(Machine *m, uint16_t seg) {
    uint16_t mcb = (uint16_t)(seg - 1);
    char t = (char)rd8(m, mcb, 0);
    if (t != 'M' && t != 'Z') return -1;
    wr16(m, mcb, 1, 0);
    mcb_coalesce(m);
    return 0;
}
static int mcb_resize(Machine *m, uint16_t seg, uint16_t paras, uint16_t *maxp) {
    uint16_t mcb = (uint16_t)(seg - 1);
    char t = (char)rd8(m, mcb, 0);
    if (t != 'M' && t != 'Z') return 9;
    mcb_coalesce(m);
    uint16_t size = rd16(m, mcb, 3), owner = rd16(m, mcb, 1);
    if (paras <= size) {
        if (paras < size) {
            uint16_t nseg = (uint16_t)(mcb + paras + 1);
            mcb_set(m, nseg, t, 0, (uint16_t)(size - paras - 1));
            mcb_set(m, mcb, 'M', owner, paras);
            mcb_coalesce(m);
        }
        return 0;
    }
    uint16_t avail = size;
    if (t == 'M') {
        uint16_t next = (uint16_t)(mcb + size + 1);
        if (rd16(m, next, 1) == 0) avail = (uint16_t)(size + rd16(m, next, 3) + 1);
    }
    if (paras > avail) { *maxp = avail; return 8; }
    uint16_t next = (uint16_t)(mcb + size + 1);
    char ntype = (char)rd8(m, next, 0);
    uint16_t total = avail;
    if (total - paras >= 1) {
        mcb_set(m, mcb, 'M', owner, paras);
        mcb_set(m, (uint16_t)(mcb + paras + 1), ntype, 0, (uint16_t)(total - paras - 1));
    } else mcb_set(m, mcb, ntype, owner, total);
    return 0;
}

/* ----------------------------------------------------------- keyboard */
static const char kb_norm[0x3A] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', 8, 9,
    'q','w','e','r','t','y','u','i','o','p','[',']', 13, 0,
    'a','s','d','f','g','h','j','k','l',';','\'','`', 0, '\\',
    'z','x','c','v','b','n','m',',','.','/', 0, '*', 0, ' '};
static const char kb_shift[0x3A] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', 8, 0,
    'Q','W','E','R','T','Y','U','I','O','P','{','}', 13, 0,
    'A','S','D','F','G','H','J','K','L',':','"','~', 0, '|',
    'Z','X','C','V','B','N','M','<','>','?', 0, '*', 0, ' '};

static int prefix_e0;

static void kb_push(Machine *m, uint16_t code) {
    uint16_t head = rd16(m, 0x40, 0x1A), tail = rd16(m, 0x40, 0x1C);
    uint16_t nt = (uint16_t)(tail + 2);
    if (nt >= 0x3E) nt = 0x1E;
    if (nt == head) return;
    wr16(m, 0x40, tail, code);
    wr16(m, 0x40, 0x1C, nt);
}

static void bios_kbd_irq(Machine *m) {
    uint8_t sc = m->kb_data;
    if (sc == 0xE0) { prefix_e0 = 1; return; }
    int ext = prefix_e0; prefix_e0 = 0;
    int brk = sc & 0x80; uint8_t k = sc & 0x7F;
    uint8_t fl = rd8(m, 0x40, 0x17);
    uint8_t bit = 0;
    switch (k) {
    case 0x2A: if (!ext) bit = 0x02; break;
    case 0x36: bit = 0x01; break;
    case 0x1D: bit = 0x04; break;
    case 0x38: bit = 0x08; break;
    }
    if (bit) { fl = brk ? (uint8_t)(fl & ~bit) : (uint8_t)(fl | bit); wr8(m, 0x40, 0x17, fl); return; }
    if (k == 0x3A || k == 0x45 || k == 0x46) {
        if (!brk) { uint8_t t = k == 0x3A ? 0x40 : (k == 0x45 ? 0x20 : 0x10); wr8(m, 0x40, 0x17, fl ^ t); }
        return;
    }
    if (brk) return;
    int shift = fl & 3, ctrl = fl & 4, alt = fl & 8, caps = fl & 0x40;
    uint8_t ascii = 0, scan = k;
    if (k >= 0x3B && k <= 0x44) { /* F1-F10 */
        if (alt) scan = (uint8_t)(k - 0x3B + 0x68);
        else if (ctrl) scan = (uint8_t)(k - 0x3B + 0x5E);
        else if (shift) scan = (uint8_t)(k - 0x3B + 0x54);
    } else if (k == 0x57 || k == 0x58) {
        scan = (uint8_t)(k == 0x57 ? 0x85 : 0x86);
    } else if (k >= 0x47 && k <= 0x53) {
        if (ext || !(fl & 0x20)) {
            ascii = ext ? 0xE0 : 0;
            if (k == 0x4A) ascii = '-';
            else if (k == 0x4E) ascii = '+';
            if (ext) ascii = 0;
        } else {
            static const char np[] = "789-456+1230.";
            ascii = (uint8_t)np[k - 0x47];
        }
    } else if (k < 0x3A) {
        if (alt) {
            ascii = 0;
            if (k >= 0x02 && k <= 0x0D) scan = (uint8_t)(k - 0x02 + 0x78);
        } else if (ctrl) {
            char ch = kb_norm[k];
            if (ch >= 'a' && ch <= 'z') ascii = (uint8_t)(ch - 'a' + 1);
            else if (k == 0x1C) ascii = 10;
            else if (k == 0x0E) ascii = 127;
            else if (k == 0x01) ascii = 27;
            else return;
        } else {
            char ch = shift ? kb_shift[k] : kb_norm[k];
            if (caps && isalpha((unsigned char)ch)) ch = (char)(shift ? tolower((unsigned char)ch) : toupper((unsigned char)ch));
            ascii = (uint8_t)ch;
            if (ext && k == 0x35) ascii = '/';
            if (ext && k == 0x1C) ascii = 13;
        }
    } else return;
    kb_push(m, (uint16_t)((scan << 8) | ascii));
}

/* ----------------------------------------------------------- INT 10h */
static void put_char_gfx(Machine *m, uint8_t ch, uint8_t color) {
    int col = rd8(m, 0x40, 0x50), row = rd8(m, 0x40, 0x51);
    const uint8_t *font = machine_font8x8() + ch * 8;
    if (m->vmode == 0x13 && !m->planar) {
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                uint32_t a = 0xA0000 + (uint32_t)(row * 8 + y) * 320 + col * 8 + x;
                if (font[y] & (0x80 >> x)) m->mem[a] = color;
                else m->mem[a] = 0;
            }
    }
}

static void tty_out(Machine *m, uint8_t ch, uint8_t color) {
    int col = rd8(m, 0x40, 0x50), row = rd8(m, 0x40, 0x51);
    int cols = m->vmode == 0x13 ? 40 : 80, rows = 25;
    if (m->log_level >= 1) machine_logf(m, 1, "TTY '%c'\n", ch >= 32 ? ch : '.');
    if (ch == 13) col = 0;
    else if (ch == 10) row++;
    else if (ch == 8) { if (col) col--; }
    else if (ch == 7) {}
    else {
        if (m->vmode < 4 || m->vmode == 7) {
            m->mem[0xB8000 + (row * 80 + col) * 2] = ch;
        } else {
            wr8(m, 0x40, 0x50, (uint8_t)col); wr8(m, 0x40, 0x51, (uint8_t)row);
            put_char_gfx(m, ch, color ? color : 15);
        }
        col++;
        if (col >= cols) { col = 0; row++; }
    }
    if (row >= rows) {
        row = rows - 1;
        if (m->vmode < 4) {
            memmove(m->mem + 0xB8000, m->mem + 0xB8000 + 160, 160 * 24);
            for (int i = 0; i < 80; i++) { m->mem[0xB8000 + 24 * 160 + i * 2] = ' '; m->mem[0xB8000 + 24 * 160 + i * 2 + 1] = 7; }
        } else if (m->vmode == 0x13 && !m->planar) {
            memmove(m->mem + 0xA0000, m->mem + 0xA0000 + 320 * 8, 320 * 192);
            memset(m->mem + 0xA0000 + 320 * 192, 0, 320 * 8);
        }
    }
    wr8(m, 0x40, 0x50, (uint8_t)col); wr8(m, 0x40, 0x51, (uint8_t)row);
}

static void int10(Machine *m) {
    CPU *c = &m->cpu;
    switch (AHr) {
    case 0x00: vga_set_mode(m, AL, !(AL & 0x80)); break;
    case 0x01: break;
    case 0x02: wr8(m, 0x40, 0x50, DLr); wr8(m, 0x40, 0x51, DHr); break;
    case 0x03: setDL(c, rd8(m, 0x40, 0x50)); setDH(c, rd8(m, 0x40, 0x51)); c->r[CX] = 0x0607; break;
    case 0x05: break;
    case 0x06: case 0x07: {
        if (m->vmode < 4) {
            int lines = AL, top = CHr, left = CLr, bot = DHr, right = DLr; uint8_t attr = BHr;
            if (!lines || lines > bot - top) lines = bot - top + 1;
            for (int n = 0; n < lines; n++) {
                if (AHr == 6) for (int r = top; r < bot; r++) for (int col = left; col <= right; col++) { m->mem[0xB8000 + (r * 80 + col) * 2] = m->mem[0xB8000 + ((r + 1) * 80 + col) * 2]; m->mem[0xB8000 + (r * 80 + col) * 2 + 1] = m->mem[0xB8000 + ((r + 1) * 80 + col) * 2 + 1]; }
                else for (int r = bot; r > top; r--) for (int col = left; col <= right; col++) { m->mem[0xB8000 + (r * 80 + col) * 2] = m->mem[0xB8000 + ((r - 1) * 80 + col) * 2]; m->mem[0xB8000 + (r * 80 + col) * 2 + 1] = m->mem[0xB8000 + ((r - 1) * 80 + col) * 2 + 1]; }
                int clr = AHr == 6 ? bot : top;
                for (int col = left; col <= right; col++) { m->mem[0xB8000 + (clr * 80 + col) * 2] = ' '; m->mem[0xB8000 + (clr * 80 + col) * 2 + 1] = attr; }
            }
        }
        break;
    }
    case 0x08: {
        int col = rd8(m, 0x40, 0x50), row = rd8(m, 0x40, 0x51);
        if (m->vmode < 4) c->r[AX] = rd16(m, 0xB800, (uint16_t)((row * 80 + col) * 2));
        else c->r[AX] = 0;
        break;
    }
    case 0x09: case 0x0A: {
        int col = rd8(m, 0x40, 0x50), row = rd8(m, 0x40, 0x51);
        for (int i = 0; i < R(CX); i++) {
            if (m->vmode < 4) {
                m->mem[0xB8000 + (row * 80 + col + i) * 2] = AL;
                if (AHr == 9) m->mem[0xB8000 + (row * 80 + col + i) * 2 + 1] = BLr;
            } else {
                wr8(m, 0x40, 0x50, (uint8_t)(col + i));
                put_char_gfx(m, AL, BLr);
            }
        }
        wr8(m, 0x40, 0x50, (uint8_t)col);
        break;
    }
    case 0x0B: break;
    case 0x0C:
        if (m->vmode == 0x13 && !m->planar && R(CX) < 320 && R(DX) < 200) m->mem[0xA0000 + R(DX) * 320 + R(CX)] = AL;
        break;
    case 0x0D:
        if (m->vmode == 0x13 && !m->planar && R(CX) < 320 && R(DX) < 200) setAL(c, m->mem[0xA0000 + R(DX) * 320 + R(CX)]);
        break;
    case 0x0E: tty_out(m, AL, BLr); break;
    case 0x0F: setAL(c, (uint8_t)m->vmode); setAH(c, m->vmode == 0x13 ? 40 : 80); setBH(c, 0); break;
    case 0x10:
        switch (AL) {
        case 0x00: if (BLr < 16) m->attr[BLr] = BHr; break;
        case 0x01: m->attr[0x11] = BHr; break;
        case 0x02: for (int i = 0; i < 17; i++) m->attr[i] = rd8(m, c->s[ES], (uint16_t)(R(DX) + i)); break;
        case 0x03: break;
        case 0x07: setBH(c, m->attr[BLr & 31]); break;
        case 0x10: vga_set_dac(m, BLr, DHr, CHr, CLr); break;
        case 0x12:
            for (int i = 0; i < R(CX); i++) {
                uint16_t o = (uint16_t)(R(DX) + i * 3);
                vga_set_dac(m, (R(BX) + i) & 255, rd8(m, c->s[ES], o), rd8(m, c->s[ES], (uint16_t)(o + 1)), rd8(m, c->s[ES], (uint16_t)(o + 2)));
            }
            break;
        case 0x15: setDH(c, m->dac[BLr][0]); setCH(c, m->dac[BLr][1]); setCL(c, m->dac[BLr][2]); break;
        case 0x17:
            for (int i = 0; i < R(CX); i++) {
                uint16_t o = (uint16_t)(R(DX) + i * 3);
                int idx = (R(BX) + i) & 255;
                wr8(m, c->s[ES], o, m->dac[idx][0]); wr8(m, c->s[ES], (uint16_t)(o + 1), m->dac[idx][1]); wr8(m, c->s[ES], (uint16_t)(o + 2), m->dac[idx][2]);
            }
            break;
        case 0x1A: setBH(c, 0); setBL(c, 0); break;
        default: machine_logf(m, 1, "int10 AX=%04X unhandled\n", R(AX)); break;
        }
        break;
    case 0x11:
        if (AL == 0x30) {
            c->s[ES] = ROM_SEG; c->r[BP] = FONT_OFF; c->r[CX] = 8; setDL(c, 24);
        }
        break;
    case 0x12:
        if (BLr == 0x10) { setBH(c, 0); setBL(c, 3); c->r[CX] = 0x0009; }
        break;
    case 0x1A:
        if (AL == 0) { setAL(c, 0x1A); c->r[BX] = 0x0008; }
        break;
    case 0x1B: setAL(c, 0); break; /* functionality info: not supported */
    case 0x1C: setAL(c, 0); break;
    default:
        machine_logf(m, 1, "int10 AH=%02X unhandled\n", AHr);
        break;
    }
}

/* ----------------------------------------------------------- INT 16h */
static void int16(Machine *m) {
    CPU *c = &m->cpu;
    uint16_t head = rd16(m, 0x40, 0x1A), tail = rd16(m, 0x40, 0x1C);
    switch (AHr) {
    case 0x00: case 0x10: {
        if (head == tail) { block_retry(m); return; }
        uint16_t k = rd16(m, 0x40, head);
        head = (uint16_t)(head + 2); if (head >= 0x3E) head = 0x1E;
        wr16(m, 0x40, 0x1A, head);
        if (AHr == 0x00 && (k & 0xFF) == 0xE0) k &= 0xFF00;
        c->r[AX] = k;
        break;
    }
    case 0x01: case 0x11:
        if (head == tail) { stack_flag(m, F_ZF, 1); /* let time pass for polling loops */ }
        else { uint16_t k = rd16(m, 0x40, head); if (AHr == 1 && (k & 0xFF) == 0xE0) k &= 0xFF00; c->r[AX] = k; stack_flag(m, F_ZF, 0); }
        break;
    case 0x02: case 0x12: setAL(c, rd8(m, 0x40, 0x17)); if (AHr == 0x12) setAH(c, 0); break;
    case 0x03: break;
    case 0x05: kb_push(m, R(CX)); setAL(c, 0); break;
    default: machine_logf(m, 1, "int16 AH=%02X unhandled\n", AHr); break;
    }
}

/* ----------------------------------------------------------- INT 33h */
static void int33(Machine *m) {
    CPU *c = &m->cpu;
    static int cnt[64]; if (R(AX) < 64 && ++cnt[R(AX)] % 100000 == 1) machine_logf(m, 1, "int33 fn %02X count %d\n", R(AX), cnt[R(AX)]);
    if (R(AX) == 3 || R(AX) == 5 || R(AX) == 6 || R(AX) == 0x0B) m->mouse_last_poll_us = m->time_us;
    else machine_logf(m, 1, "int33 AX=%04X BX=%04X CX=%04X DX=%04X\n", R(AX), R(BX), R(CX), R(DX));
    switch (R(AX)) {
    case 0x00: case 0x21:
        R(AX) = 0xFFFF; R(BX) = 2;
        m->mouse_shown = 0; m->mouse_minx = 0; m->mouse_maxx = 639; m->mouse_miny = 0; m->mouse_maxy = 199;
        m->mouse_x = 320; m->mouse_y = 100;
        break;
    case 0x01: m->mouse_shown++; if (m->mouse_shown > 1) m->mouse_shown = 1; break;
    case 0x02: m->mouse_shown--; break;
    case 0x03: R(BX) = (uint16_t)m->mouse_buttons; R(CX) = (uint16_t)m->mouse_x; R(DX) = (uint16_t)m->mouse_y; break;
    case 0x04: m->mouse_x = (int16_t)R(CX); m->mouse_y = (int16_t)R(DX); break;
    case 0x05: { int b = R(BX) & 3; R(AX) = (uint16_t)m->mouse_buttons; R(BX) = (uint16_t)m->mouse_press_cnt[b]; R(CX) = (uint16_t)m->mouse_press_x[b]; R(DX) = (uint16_t)m->mouse_press_y[b]; m->mouse_press_cnt[b] = 0; break; }
    case 0x06: { int b = R(BX) & 3; R(AX) = (uint16_t)m->mouse_buttons; R(BX) = (uint16_t)m->mouse_rel_cnt[b]; R(CX) = (uint16_t)m->mouse_rel_x[b]; R(DX) = (uint16_t)m->mouse_rel_y[b]; m->mouse_rel_cnt[b] = 0; break; }
    case 0x07: m->mouse_minx = (int16_t)R(CX); m->mouse_maxx = (int16_t)R(DX); if (m->mouse_minx > m->mouse_maxx) { int t = m->mouse_minx; m->mouse_minx = m->mouse_maxx; m->mouse_maxx = t; } break;
    case 0x08: m->mouse_miny = (int16_t)R(CX); m->mouse_maxy = (int16_t)R(DX); if (m->mouse_miny > m->mouse_maxy) { int t = m->mouse_miny; m->mouse_miny = m->mouse_maxy; m->mouse_maxy = t; } break;
    case 0x0B: R(CX) = (uint16_t)m->mouse_mickey_x; R(DX) = (uint16_t)m->mouse_mickey_y; m->mouse_mickey_x = m->mouse_mickey_y = 0; break;
    case 0x0C: case 0x14: machine_logf(m, 0, "mouse: event handler requested (mask %04X) - not supported\n", R(CX)); break;
    case 0x24: R(BX) = 0x0626; R(CX) = 0x0400; break;
    default: machine_logf(m, 1, "int33 AX=%04X unhandled\n", R(AX)); break;
    }
}

/* ----------------------------------------------------------- INT 21h */
static int alloc_handle(Machine *m) {
    for (int i = 5; i < MAX_FILES; i++) if (!m->files[i]) return i;
    return -1;
}

static void fill_find(Machine *m, const char *name, struct stat *st, int isdir) {
    uint16_t s = m->dta_seg, o = m->dta_off;
    wr8(m, s, (uint16_t)(o + 0x15), isdir ? 0x10 : 0x20);
    struct tm *tm = localtime(&st->st_mtime);
    uint16_t t = 0, d = 0x21;
    if (tm) {
        t = (uint16_t)((tm->tm_hour << 11) | (tm->tm_min << 5) | (tm->tm_sec / 2));
        d = (uint16_t)(((tm->tm_year - 80) << 9) | ((tm->tm_mon + 1) << 5) | tm->tm_mday);
    }
    wr16(m, s, (uint16_t)(o + 0x16), t);
    wr16(m, s, (uint16_t)(o + 0x18), d);
    wr16(m, s, (uint16_t)(o + 0x1A), (uint16_t)st->st_size);
    wr16(m, s, (uint16_t)(o + 0x1C), (uint16_t)((uint32_t)st->st_size >> 16));
    int i = 0;
    for (; name[i] && i < 12; i++) wr8(m, s, (uint16_t)(o + 0x1E + i), (uint8_t)toupper((unsigned char)name[i]));
    wr8(m, s, (uint16_t)(o + 0x1E + i), 0);
}

static int name_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int find_next(Machine *m) {
    DIR *dir = opendir(m->find_dir);
    if (!dir) return 18;
    char *names[1024]; int n = 0;
    struct dirent *de;
    while ((de = readdir(dir)) && n < 1024) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (dos_match(m->find_pattern, de->d_name)) { size_t L = strlen(de->d_name) + 1; names[n] = malloc(L); memcpy(names[n], de->d_name, L); n++; }
    }
    closedir(dir);
    qsort(names, (size_t)n, sizeof names[0], name_cmp);
    int rc = 18;
    while (m->find_index < n) {
        const char *nm = names[m->find_index++];
        char full[1400];
        snprintf(full, sizeof full, "%s/%s", m->find_dir, nm);
        struct stat st;
        if (stat(full, &st)) continue;
        int isdir = S_ISDIR(st.st_mode);
        if (isdir && !(m->find_attr & 0x10)) continue;
        fill_find(m, nm, &st, isdir);
        rc = 0;
        break;
    }
    for (int i = 0; i < n; i++) free(names[i]);
    return rc;
}

/* DOS character input: extended keys (arrows, F-keys) are returned as a 0
 * followed by the scan code on the next call. Returns -1 if no key. */
static int dos_getch(Machine *m) {
    if (m->dos_ext_pending) { int sc = m->dos_ext_pending; m->dos_ext_pending = 0; return sc; }
    uint16_t head = rd16(m, 0x40, 0x1A), tail = rd16(m, 0x40, 0x1C);
    if (head == tail) return -1;
    uint16_t k = rd16(m, 0x40, head);
    head = (uint16_t)(head + 2); if (head >= 0x3E) head = 0x1E;
    wr16(m, 0x40, 0x1A, head);
    uint8_t ascii = (uint8_t)k;
    if (ascii == 0 || ascii == 0xE0) { m->dos_ext_pending = k >> 8; return 0; }
    return ascii;
}

static void int21(Machine *m) {
    CPU *c = &m->cpu;
    uint8_t ah = AHr;
    char path[260], host[1400];
    int exists;
    set_cf(m, 0);
    switch (ah) {
    case 0x01: case 0x07: case 0x08: {
        int ch = dos_getch(m);
        if (ch < 0) { block_retry(m); return; }
        setAL(c, (uint8_t)ch);
        break;
    }
    case 0x02: tty_out(m, DLr, 7); break;
    case 0x06:
        if (DLr == 0xFF) {
            int ch = dos_getch(m);
            if (ch < 0) { setAL(c, 0); stack_flag(m, F_ZF, 1); }
            else { setAL(c, (uint8_t)ch); stack_flag(m, F_ZF, 0); }
        } else tty_out(m, DLr, 7);
        break;
    case 0x09: {
        for (uint16_t o = R(DX);; o++) {
            uint8_t ch = rd8(m, c->s[DS], o);
            if (ch == '$') break;
            tty_out(m, ch, 7);
            if (m->log_level < 1) fputc(ch, stderr);
        }
        break;
    }
    case 0x0B: {
        uint16_t head = rd16(m, 0x40, 0x1A), tail = rd16(m, 0x40, 0x1C);
        setAL(c, (head == tail && !m->dos_ext_pending) ? 0 : 0xFF);
        break;
    }
    case 0x0C: {
        wr16(m, 0x40, 0x1A, rd16(m, 0x40, 0x1C));
        m->dos_ext_pending = 0;
        uint8_t f = AL;
        if (f == 1 || f == 6 || f == 7 || f == 8 || f == 0x0A) { setAH(c, f); int21(m); }
        break;
    }
    case 0x0D: break;
    case 0x0E: setAL(c, 26); break;
    case 0x19: setAL(c, 2); break;
    case 0x1B: case 0x1C: /* allocation info: 4 sectors/cluster, 512 bytes, plenty of clusters */
        setAL(c, 4); R(CX) = 512; R(DX) = 0xFFF0;
        c->s[DS] = 0xF000; R(BX) = 0x0440; /* media id byte in ROM */
        break;
    case 0x1A: m->dta_seg = c->s[DS]; m->dta_off = R(DX); break;
    case 0x25: wr16(m, 0, (uint16_t)(AL * 4), R(DX)); wr16(m, 0, (uint16_t)(AL * 4 + 2), c->s[DS]);
        machine_logf(m, 1, "set vector %02X -> %04X:%04X\n", AL, c->s[DS], R(DX));
        break;
    case 0x2A: {
        time_t t = time(NULL); struct tm *tm = localtime(&t);
        R(CX) = (uint16_t)(tm->tm_year + 1900); setDH(c, (uint8_t)(tm->tm_mon + 1)); setDL(c, (uint8_t)tm->tm_mday); setAL(c, (uint8_t)tm->tm_wday);
        break;
    }
    case 0x2B: case 0x2D: setAL(c, 0); break;
    case 0x2C: {
        time_t t = time(NULL); struct tm *tm = localtime(&t);
        setCH(c, (uint8_t)tm->tm_hour); setCL(c, (uint8_t)tm->tm_min); setDH(c, (uint8_t)tm->tm_sec);
        setDL(c, (uint8_t)((uint64_t)(m->time_us / 10000) % 100));
        break;
    }
    case 0x2F: c->s[ES] = m->dta_seg; R(BX) = m->dta_off; break;
    case 0x30: R(AX) = 0x0005; R(BX) = 0; R(CX) = 0; break;
    case 0x33: if (AL == 0 || AL == 2) setDL(c, 0); else if (AL == 5) setDL(c, 3); break;
    case 0x35: R(BX) = rd16(m, 0, (uint16_t)(AL * 4)); c->s[ES] = rd16(m, 0, (uint16_t)(AL * 4 + 2)); break;
    case 0x36: R(AX) = 32; R(BX) = 0x4000; R(CX) = 512; R(DX) = 0xFFFF; break;
    case 0x37: if (AL == 0) setDL(c, '/'); break;
    case 0x38: {
        /* country info: US defaults */
        uint8_t info[34] = {0};
        info[2] = '$'; info[4] = ','; info[6] = '.'; info[8] = '-'; info[10] = ':'; info[15] = 2;
        for (int i = 0; i < 34; i++) wr8(m, c->s[DS], (uint16_t)(R(DX) + i), info[i]);
        R(BX) = 1;
        break;
    }
    case 0x39: case 0x3A: case 0x3B: {
        read_asciiz(m, c->s[DS], R(DX), path, sizeof path);
        if (host_resolve(m, path, host, sizeof host, &exists)) { dos_error(m, 3); break; }
        if (ah == 0x39) { if (mkdir_host(host)) dos_error(m, 5); }
        else if (ah == 0x3A) { dos_error(m, 5); }
        else {
            struct stat st;
            if (!exists || stat(host, &st) || !S_ISDIR(st.st_mode)) { dos_error(m, 3); break; }
            /* compute new DOS cwd */
            char tmp[260]; const char *p = path;
            if (p[0] && p[1] == ':') p += 2;
            if (*p == '\\' || *p == '/') snprintf(tmp, sizeof tmp, "%s", p + 1);
            else if (m->cwd[0]) snprintf(tmp, sizeof tmp, "%s\\%s", m->cwd, p);
            else snprintf(tmp, sizeof tmp, "%s", p);
            for (char *q = tmp; *q; q++) { if (*q == '/') *q = '\\'; *q = (char)toupper((unsigned char)*q); }
            size_t l = strlen(tmp); while (l && tmp[l - 1] == '\\') tmp[--l] = 0;
            snprintf(m->cwd, sizeof m->cwd, "%s", tmp);
        }
        break;
    }
    case 0x3C: case 0x3D: case 0x5B: {
        read_asciiz(m, c->s[DS], R(DX), path, sizeof path);
        if (host_resolve(m, path, host, sizeof host, &exists)) { dos_error(m, 3); break; }
        int h = alloc_handle(m);
        if (h < 0) { dos_error(m, 4); break; }
        FILE *f = NULL;
        if (ah == 0x3D) {
            if (!exists) { machine_logf(m, 1, "open '%s' -> not found\n", path); dos_error(m, 2); break; }
            int mode = AL & 7;
            f = fopen(host, mode == 0 ? "rb" : "r+b");
            if (!f && mode != 0) f = fopen(host, "rb");
        } else {
            if (ah == 0x5B && exists) { dos_error(m, 80); break; }
            f = fopen(host, "w+b");
        }
        if (!f) { dos_error(m, ah == 0x3D ? 2 : 5); break; }
        m->files[h] = f;
        snprintf(m->filenames[h], sizeof m->filenames[h], "%s", path);
        machine_logf(m, 1, "%s '%s' -> handle %d\n", ah == 0x3D ? "open" : "create", path, h);
        R(AX) = (uint16_t)h;
        break;
    }
    case 0x3E: {
        int h = R(BX);
        if (h < 5) break;
        if (h >= MAX_FILES || !m->files[h]) { dos_error(m, 6); break; }
        fclose(m->files[h]); m->files[h] = NULL;
        break;
    }
    case 0x3F: {
        int h = R(BX);
        uint16_t n = R(CX);
        if (h == 0) { /* stdin: return nothing */ R(AX) = 0; break; }
        if (h >= MAX_FILES || !m->files[h]) { dos_error(m, 6); break; }
        uint8_t buf[65536];
        size_t got = fread(buf, 1, n, m->files[h]);
        for (size_t i = 0; i < got; i++) wr8(m, c->s[DS], (uint16_t)(R(DX) + i), buf[i]);
        machine_logf(m, 2, "read h%d %u -> %u\n", h, n, (unsigned)got);
        R(AX) = (uint16_t)got;
        break;
    }
    case 0x40: {
        int h = R(BX);
        uint16_t n = R(CX);
        if (h == 1 || h == 2) {
            for (uint16_t i = 0; i < n; i++) { uint8_t ch = rd8(m, c->s[DS], (uint16_t)(R(DX) + i)); tty_out(m, ch, 7); fputc(ch, stderr); }
            R(AX) = n; break;
        }
        if (h < 5) { R(AX) = n; break; }
        if (h >= MAX_FILES || !m->files[h]) { dos_error(m, 6); break; }
        if (n == 0) {
            /* truncate at current position */
            long pos = ftell(m->files[h]);
            fflush(m->files[h]);
#ifdef _WIN32
            _chsize(_fileno(m->files[h]), pos);
#else
            if (ftruncate(fileno(m->files[h]), pos)) {}
#endif
            R(AX) = 0; break;
        }
        uint8_t buf[65536];
        for (uint16_t i = 0; i < n; i++) buf[i] = rd8(m, c->s[DS], (uint16_t)(R(DX) + i));
        size_t put = fwrite(buf, 1, n, m->files[h]);
        fflush(m->files[h]);
        R(AX) = (uint16_t)put;
        break;
    }
    case 0x41: {
        read_asciiz(m, c->s[DS], R(DX), path, sizeof path);
        if (host_resolve(m, path, host, sizeof host, &exists) || !exists) { dos_error(m, 2); break; }
        if (remove(host)) dos_error(m, 5);
        break;
    }
    case 0x42: {
        int h = R(BX);
        if (h >= MAX_FILES || !m->files[h]) { if (h < 5) { R(AX) = 0; R(DX) = 0; break; } dos_error(m, 6); break; }
        long off = (long)(int32_t)(((uint32_t)R(CX) << 16) | R(DX));
        int wh = AL == 0 ? SEEK_SET : (AL == 1 ? SEEK_CUR : SEEK_END);
        if (fseek(m->files[h], off, wh)) { dos_error(m, 25); break; }
        long pos = ftell(m->files[h]);
        R(AX) = (uint16_t)pos; R(DX) = (uint16_t)((uint32_t)pos >> 16);
        break;
    }
    case 0x43: {
        read_asciiz(m, c->s[DS], R(DX), path, sizeof path);
        if (host_resolve(m, path, host, sizeof host, &exists) || !exists) { dos_error(m, 2); break; }
        if (AL == 0) { struct stat st; stat(host, &st); R(CX) = S_ISDIR(st.st_mode) ? 0x10 : 0x20; }
        break;
    }
    case 0x44:
        switch (AL) {
        case 0x00: {
            int h = R(BX);
            if (h < 5) R(DX) = (uint16_t)(h == 0 ? 0x80D3 : (h <= 2 ? 0x80D3 : 0x80C0));
            else if (h < MAX_FILES && m->files[h]) R(DX) = 0x0002;
            else { dos_error(m, 6); break; }
            R(AX) = R(DX);
            break;
        }
        case 0x01: break;
        case 0x06: setAL(c, 0xFF); break;
        case 0x07: setAL(c, 0xFF); break;
        case 0x08: R(AX) = 1; break;
        case 0x09: R(DX) = 0; break;
        case 0x0E: setAL(c, 0); break;
        default: dos_error(m, 1); break;
        }
        break;
    case 0x45: case 0x46: {
        int h = R(BX);
        if (h < 5) { R(AX) = (uint16_t)h; break; }
        if (h >= MAX_FILES || !m->files[h]) { dos_error(m, 6); break; }
        int nh = ah == 0x45 ? alloc_handle(m) : R(CX);
        if (nh < 0 || nh >= MAX_FILES) { dos_error(m, 4); break; }
        if (ah == 0x46 && m->files[nh]) { fclose(m->files[nh]); m->files[nh] = NULL; }
        char host2[1400];
        host_resolve(m, m->filenames[h], host2, sizeof host2, &exists);
        FILE *f = fopen(host2, "r+b");
        if (!f) f = fopen(host2, "rb");
        if (!f) { dos_error(m, 4); break; }
        fseek(f, ftell(m->files[h]), SEEK_SET);
        m->files[nh] = f;
        snprintf(m->filenames[nh], sizeof m->filenames[nh], "%s", m->filenames[h]);
        R(AX) = (uint16_t)nh;
        break;
    }
    case 0x47: {
        char tmp[130];
        snprintf(tmp, sizeof tmp, "%s", m->cwd);
        for (size_t i = 0; i <= strlen(tmp); i++) wr8(m, c->s[DS], (uint16_t)(R(SI) + i), (uint8_t)tmp[i]);
        R(AX) = 0x0100;
        break;
    }
    case 0x48: {
        uint16_t largest;
        uint16_t seg = mcb_alloc(m, R(BX), m->psp_seg, &largest);
        machine_logf(m, 1, "alloc %04X paras -> %04X (largest %04X)\n", R(BX), seg, largest);
        if (!seg) { R(BX) = largest; dos_error(m, 8); break; }
        R(AX) = seg;
        break;
    }
    case 0x49:
        machine_logf(m, 1, "free %04X\n", c->s[ES]);
        if (mcb_free(m, c->s[ES])) dos_error(m, 9);
        break;
    case 0x4A: {
        uint16_t maxp = 0;
        int rc = mcb_resize(m, c->s[ES], R(BX), &maxp);
        machine_logf(m, 1, "resize %04X to %04X paras -> rc %d (max %04X)\n", c->s[ES], R(BX), rc, maxp);
        if (rc) { R(BX) = maxp; dos_error(m, (uint16_t)rc); }
        break;
    }
    case 0x4B:
        read_asciiz(m, c->s[DS], R(DX), path, sizeof path);
        machine_logf(m, 0, "EXEC '%s' AL=%d not supported\n", path, AL);
        dos_error(m, 2);
        break;
    case 0x00: case 0x4C:
        m->exited = 1; m->exit_code = ah == 0 ? 0 : AL;
        machine_logf(m, 0, "program exited with code %d\n", m->exit_code);
        c->halted = 1; c->fl &= ~F_IF;
        break;
    case 0x4D: R(AX) = 0; break;
    case 0x4E: case 0x4F: {
        if (ah == 0x4E) {
            read_asciiz(m, c->s[DS], R(DX), path, sizeof path);
            char *bs = strrchr(path, '\\'), *fs = strrchr(path, '/');
            if (fs > bs) bs = fs;
            char dirpart[260] = "";
            const char *pat = path;
            if (bs) { snprintf(dirpart, sizeof dirpart, "%.*s", (int)(bs - path), path); pat = bs + 1; }
            else if (path[0] && path[1] == ':') { pat = path + 2; snprintf(dirpart, sizeof dirpart, "%.2s", path); }
            if (host_resolve(m, dirpart[0] ? dirpart : ".", m->find_dir, sizeof m->find_dir, &exists) || !exists) { dos_error(m, 3); break; }
            snprintf(m->find_pattern, sizeof m->find_pattern, "%s", pat);
            m->find_index = 0;
            m->find_attr = CLr;
        }
        int rc = find_next(m);
        machine_logf(m, 1, "find%s '%s' -> %d\n", ah == 0x4E ? "first" : "next", m->find_pattern, rc);
        if (rc) dos_error(m, ah == 0x4E ? 2 : 18);
        break;
    }
    case 0x50: m->psp_seg = R(BX); break;
    case 0x51: case 0x62: R(BX) = m->psp_seg; break;
    case 0x54: setAL(c, 0); break;
    case 0x56: {
        char p2[260], h2[1400];
        read_asciiz(m, c->s[DS], R(DX), path, sizeof path);
        read_asciiz(m, c->s[ES], R(DI), p2, sizeof p2);
        if (host_resolve(m, path, host, sizeof host, &exists) || !exists) { dos_error(m, 2); break; }
        host_resolve(m, p2, h2, sizeof h2, &exists);
        if (rename(host, h2)) dos_error(m, 5);
        break;
    }
    case 0x57:
        if (AL == 0) { R(CX) = 0; R(DX) = 0x1721; }
        break;
    case 0x58: if (AL == 0) R(AX) = 0; break;
    case 0x59: R(AX) = m->last_error; R(BX) = 0x0101; setCH(c, 1); break;
    case 0x29: { /* parse filename into FCB: minimal */
        setAL(c, 0);
        break;
    }
    case 0x63: case 0x65: case 0x66: case 0x67: case 0x68: dos_error(m, 1); break;
    default:
        machine_logf(m, 0, "int21 AH=%02X unhandled (AX=%04X)\n", ah, R(AX));
        dos_error(m, 1);
        break;
    }
}

/* ----------------------------------------------------------- misc INTs */
static void int15(Machine *m) {
    CPU *c = &m->cpu;
    switch (AHr) {
    case 0x84:
        if (R(DX) == 0) {
            uint8_t v = 0xF0;
            for (int b = 0; b < 4; b++) if (m->joy_buttons & (1 << b)) v &= (uint8_t)~(0x10 << b);
            setAL(c, v);
        } else {
            R(AX) = (uint16_t)((m->joy_axis[0] + 1) * 127); R(BX) = (uint16_t)((m->joy_axis[1] + 1) * 127);
            R(CX) = 0; R(DX) = 0;
        }
        break;
    case 0x86: break;
    case 0x88: R(AX) = 0; break;
    case 0x4F: set_cf(m, 1); return;
    case 0x90: case 0x91: setAH(c, 0); break;
    default: setAH(c, 0x86); set_cf(m, 1); return;
    }
    set_cf(m, 0);
}

static void int1a(Machine *m) {
    CPU *c = &m->cpu;
    switch (AHr) {
    case 0x00: R(CX) = rd16(m, 0x40, 0x6E); R(DX) = rd16(m, 0x40, 0x6C); setAL(c, rd8(m, 0x40, 0x70)); wr8(m, 0x40, 0x70, 0); break;
    case 0x01: wr16(m, 0x40, 0x6E, R(CX)); wr16(m, 0x40, 0x6C, R(DX)); break;
    case 0x02: case 0x04: {
        time_t t = time(NULL); struct tm *tm = localtime(&t);
#define BCD(x) (uint8_t)((((x) / 10) << 4) | ((x) % 10))
        if (AHr == 2) { setCH(c, BCD(tm->tm_hour)); setCL(c, BCD(tm->tm_min)); setDH(c, BCD(tm->tm_sec)); setDL(c, 0); }
        else { setCH(c, BCD((tm->tm_year + 1900) / 100)); setCL(c, BCD(tm->tm_year % 100)); setDH(c, BCD(tm->tm_mon + 1)); setDL(c, BCD(tm->tm_mday)); }
        set_cf(m, 0);
        break;
    }
    default: break;
    }
}

static void hle_dispatch(void *host, CPU *c, uint8_t num) {
    Machine *m = host;
    m->int_count[num]++;
    switch (num) {
    case 0x08: { /* BIOS timer tick */
        uint32_t t = (uint32_t)rd16(m, 0x40, 0x6C) | ((uint32_t)rd16(m, 0x40, 0x6E) << 16);
        t++;
        if (t >= 0x1800B0) { t = 0; wr8(m, 0x40, 0x70, 1); }
        wr16(m, 0x40, 0x6C, (uint16_t)t); wr16(m, 0x40, 0x6E, (uint16_t)(t >> 16));
        break;
    }
    case 0x09: bios_kbd_irq(m); break;
    case 0x10: int10(m); break;
    case 0x11: R(AX) = rd16(m, 0x40, 0x10); break;
    case 0x12: R(AX) = 640; break;
    case 0x13: if (AHr == 0) { setAH(c, 0); set_cf(m, 0); } else { setAH(c, 1); set_cf(m, 1); } break;
    case 0x14: case 0x17: setAH(c, 0); break;
    case 0x15: int15(m); break;
    case 0x16: int16(m); break;
    case 0x1A: int1a(m); break;
    case 0x20: m->exited = 1; c->halted = 1; c->fl &= ~F_IF; break;
    case 0x21: int21(m); break;
    case 0x2F:
        if (R(AX) == 0x1600) setAL(c, 0);
        else if (R(AX) == 0x4300) setAL(c, 0);
        break;
    case 0x33: int33(m); break;
    case 0x00: machine_logf(m, 0, "divide error at %04X:%04X\n", rd16(m, c->s[SS], (uint16_t)(c->r[SP] + 2)), rd16(m, c->s[SS], c->r[SP])); break;
    case 0x06: machine_logf(m, 0, "invalid opcode at %04X:%04X\n", rd16(m, c->s[SS], (uint16_t)(c->r[SP] + 2)), rd16(m, c->s[SS], c->r[SP])); m->exited = 1; c->halted = 1; break;
    default:
        machine_logf(m, 1, "unhandled int %02X AX=%04X\n", num, R(AX));
        break;
    }
}

/* ----------------------------------------------------------- init/load */
void hle_init(Machine *m) {
    CPU *c = &m->cpu;
    c->hle = hle_dispatch;
    uint8_t *rom = m->mem + 0xF0000;
    for (int n = 0; n < 256; n++) {
        rom[n * 4] = 0xF1; rom[n * 4 + 1] = (uint8_t)n; rom[n * 4 + 2] = 0xCF; rom[n * 4 + 3] = 0x90;
        uint16_t off = (uint16_t)(n * 4);
        if (n == 0x08) off = STUB_TICK;
        else if (n == 0x09) off = STUB_KBD;
        else if ((n >= 0x0A && n <= 0x0F) || (n >= 0x70 && n <= 0x77)) off = STUB_EOI;
        else if (n == 0x1C || n == 0x1B || n == 0x23 || n == 0x24 || n == 0x67 || (n >= 0x34 && n <= 0x3F) || n == 0x05) off = STUB_IRET;
        wr16(m, 0, (uint16_t)(n * 4), off);
        wr16(m, 0, (uint16_t)(n * 4 + 2), ROM_SEG);
    }
    static const uint8_t tick[] = {0xF1, 0x08, 0xCD, 0x1C, 0x50, 0xB0, 0x20, 0xE6, 0x20, 0x58, 0xCF};
    static const uint8_t kbd[] = {0xF1, 0x09, 0x50, 0xB0, 0x20, 0xE6, 0x20, 0x58, 0xCF};
    static const uint8_t eoi[] = {0x50, 0xB0, 0x20, 0xE6, 0x20, 0x58, 0xCF};
    memcpy(rom + STUB_TICK, tick, sizeof tick);
    memcpy(rom + STUB_KBD, kbd, sizeof kbd);
    memcpy(rom + STUB_EOI, eoi, sizeof eoi);
    rom[STUB_IRET] = 0xCF;
    rom[0x0440] = 0xF8; /* media descriptor: fixed disk */
    memcpy(rom + FONT_OFF, machine_font8x8(), 128 * 8);
    memcpy(rom + 0xF000, machine_font8x8() + 128 * 8, 128 * 8);
    wr16(m, 0, 0x1F * 4, 0xF000); wr16(m, 0, 0x1F * 4 + 2, ROM_SEG);
    memcpy(rom + 0xFFF5, "01/01/92", 8);
    rom[0xFFFE] = 0xFC;
    /* BIOS data area */
    wr16(m, 0x40, 0x10, 0x0063 | 0x0020); /* 1 floppy, mouse, 80x25 colour */
    wr16(m, 0x40, 0x13, 640);
    wr16(m, 0x40, 0x1A, 0x1E); wr16(m, 0x40, 0x1C, 0x1E);
    wr16(m, 0x40, 0x80, 0x1E); wr16(m, 0x40, 0x82, 0x3E);
    wr8(m, 0x40, 0x49, 3); wr16(m, 0x40, 0x4A, 80);
    wr16(m, 0x40, 0x63, 0x3D4);
    wr8(m, 0x40, 0x84, 24);
    wr16(m, 0x40, 0x85, 8);
    wr8(m, 0x40, 0x96, 0x10); /* 101-key keyboard */
    /* memory arena */
    mcb_set(m, MCB_FIRST, 'Z', 0, (uint16_t)(MEM_TOP - MCB_FIRST - 1));
    snprintf(m->cwd, sizeof m->cwd, "%s", "");
}

int hle_load_program(Machine *m, const char *exe, const char *args) {
    CPU *c = &m->cpu;
    char host[1400]; int exists;
    host_resolve(m, exe, host, sizeof host, &exists);
    FILE *f = fopen(host, "rb");
    if (!f) { machine_logf(m, 0, "cannot open %s\n", host); return -1; }
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)size);
    if (fread(data, 1, (size_t)size, f) != (size_t)size) { fclose(f); free(data); return -1; }
    fclose(f);

    /* environment */
    uint16_t largest;
    uint16_t env = mcb_alloc(m, 0x20, 8, &largest);
    char envbuf[512]; int el = 0;
    const char *vars[] = {"COMSPEC=C:\\COMMAND.COM", "PATH=C:\\", "BLASTER=A220 I7 D1 T3", NULL};
    for (int i = 0; vars[i]; i++) { int l = (int)strlen(vars[i]) + 1; memcpy(envbuf + el, vars[i], (size_t)l); el += l; }
    envbuf[el++] = 0;
    envbuf[el++] = 1; envbuf[el++] = 0;
    char prog[64];
    snprintf(prog, sizeof prog, "C:\\%s", exe);
    for (char *p = prog; *p; p++) *p = (char)toupper((unsigned char)*p);
    memcpy(envbuf + el, prog, strlen(prog) + 1); el += (int)strlen(prog) + 1;
    for (int i = 0; i < el; i++) wr8(m, env, (uint16_t)i, (uint8_t)envbuf[i]);
    m->env_seg = env;

    /* program block: take the largest */
    mcb_alloc(m, 0xFFFF, 0, &largest);
    uint16_t psp = mcb_alloc(m, largest, 0, &largest);
    wr16(m, (uint16_t)(psp - 1), 1, psp); /* owner = itself */
    wr16(m, (uint16_t)(env - 1), 1, psp);
    m->psp_seg = psp;
    uint16_t blk_size = rd16(m, (uint16_t)(psp - 1), 3);

    /* PSP */
    for (int i = 0; i < 256; i++) wr8(m, psp, (uint16_t)i, 0);
    wr8(m, psp, 0, 0xCD); wr8(m, psp, 1, 0x20);
    wr16(m, psp, 2, (uint16_t)(psp + blk_size));
    wr8(m, psp, 5, 0x9A); /* far call to DOS (not used) */
    wr16(m, psp, 0x0A, rd16(m, 0, 0x22 * 4)); wr16(m, psp, 0x0C, rd16(m, 0, 0x22 * 4 + 2));
    wr16(m, psp, 0x16, psp);
    for (int i = 0; i < 20; i++) wr8(m, psp, (uint16_t)(0x18 + i), (uint8_t)(i < 5 ? (i < 3 ? 1 : i - 1) : 0xFF));
    wr16(m, psp, 0x2C, env);
    wr16(m, psp, 0x32, 20); wr16(m, psp, 0x34, 0x18); wr16(m, psp, 0x36, psp);
    wr8(m, psp, 0x50, 0xCD); wr8(m, psp, 0x51, 0x21); wr8(m, psp, 0x52, 0xCB);
    char cmd[128];
    snprintf(cmd, sizeof cmd, " %s", args ? args : "");
    int cl = (int)strlen(cmd); if (cl > 126) cl = 126;
    wr8(m, psp, 0x80, (uint8_t)cl);
    for (int i = 0; i < cl; i++) wr8(m, psp, (uint16_t)(0x81 + i), (uint8_t)cmd[i]);
    wr8(m, psp, (uint16_t)(0x81 + cl), 0x0D);
    m->dta_seg = psp; m->dta_off = 0x80;

    uint16_t load = (uint16_t)(psp + 0x10);
    if (size > 2 && data[0] == 'M' && data[1] == 'Z') {
        uint16_t *h = (uint16_t *)data;
        uint32_t img = (uint32_t)(h[2] - 1) * 512 + (h[1] ? h[1] : 512);
        uint32_t hdr = (uint32_t)h[4] * 16;
        uint32_t len = img - hdr;
        if (img > (uint32_t)size) len = (uint32_t)size - hdr;
        memcpy(m->mem + (uint32_t)load * 16, data + hdr, len);
        for (int i = 0; i < h[3]; i++) {
            uint16_t ro = *(uint16_t *)(data + h[12] + i * 4), rs = *(uint16_t *)(data + h[12] + i * 4 + 2);
            uint32_t a = (uint32_t)(rs + load) * 16 + ro;
            uint16_t v = (uint16_t)(m->mem[a] | (m->mem[a + 1] << 8));
            v += load;
            m->mem[a] = (uint8_t)v; m->mem[a + 1] = (uint8_t)(v >> 8);
        }
        c->s[SS] = (uint16_t)(h[7] + load); c->r[SP] = h[8];
        c->s[CS] = (uint16_t)(h[11] + load); c->ip = h[10];
        machine_logf(m, 0, "loaded %s: %u bytes at %04X, entry %04X:%04X, PSP %04X, block %04X paras\n", exe, len, load, c->s[CS], c->ip, psp, blk_size);
    } else {
        memcpy(m->mem + (uint32_t)psp * 16 + 0x100, data, (size_t)size);
        c->s[CS] = c->s[SS] = psp; c->ip = 0x100; c->r[SP] = 0xFFFE;
    }
    free(data);
    c->s[DS] = c->s[ES] = psp;
    c->r[AX] = 0; c->r[BX] = 0; c->r[CX] = 0xFF; c->r[DX] = psp;
    c->r[BP] = 0x091C; c->r[SI] = c->ip; c->r[DI] = c->r[SP];
    c->fl = 0x0202;
    return 0;
}
