/* Headless test runner: runs the game with a scripted input timeline and
 * writes screenshots as PPM. Usage:
 *   headless <gamedir> <total_ms> <script> [loglevel] [args]
 * Script lines:  <ms> key <hex scancode> [ext]   (press, release after 80 ms)
 *                <ms> down|up <hex scancode> [ext]
 *                <ms> type <text>
 *                <ms> shot <name>
 *                <ms> mouse <x0..1> <y0..1>
 *                <ms> click <buttons> */
#include "../src/machine.h"
#include "../src/hle.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

typedef struct { double t; char cmd[16]; char arg[128]; } Ev;

static int ascii_to_scan(char ch, int *shift) {
    static const char *row = "\x1b" "1234567890-=\b\tqwertyuiop[]\r\0asdfghjkl;'`\0\\zxcvbnm,./";
    *shift = 0;
    if (isupper((unsigned char)ch)) { *shift = 1; ch = (char)tolower((unsigned char)ch); }
    if (ch == ' ') return 0x39;
    for (int i = 0; i < 0x35; i++) if (row[i] == ch && ch) return i + 1;
    return 0;
}

static void shot(Machine *m, const char *name) {
    static uint32_t buf[640 * 480];
    int w, h;
    machine_render(m, buf, &w, &h);
    char fn[256]; snprintf(fn, sizeof fn, "%s.ppm", name);
    FILE *f = fopen(fn, "wb");
    fprintf(f, "P6 %d %d 255\n", w, h);
    for (int i = 0; i < w * h; i++) { uint32_t p = buf[i]; fputc(p & 255, f); fputc((p >> 8) & 255, f); fputc((p >> 16) & 255, f); }
    fclose(f);
    fprintf(stderr, "[shot] %s (%dx%d) mode %02X t=%.0fms CS:IP=%04X:%04X\n", fn, w, h, m->vmode, m->time_us / 1000, m->cpu.s[CS], m->cpu.ip);
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage\n"); return 1; }
    const char *args = argc > 5 ? argv[5] : "V A 5 Y N Y 16";
    Machine *m = machine_create(argv[1], "SWOTL.EXE", args);
    if (!m) return 1;
    if (argc > 4) m->log_level = atoi(argv[4]);
    double total = atof(argv[2]);
    FILE *wav = NULL; long wav_frames = 0;
    if (getenv("SWOTL_WAV")) { wav = fopen(getenv("SWOTL_WAV"), "wb"); machine_set_audio_rate(m, 44100); fseek(wav, 44, SEEK_SET); }
    Ev ev[512]; int nev = 0;
    FILE *sf = fopen(argv[3], "r");
    char line[256];
    while (sf && fgets(line, sizeof line, sf) && nev < 512) {
        if (line[0] == '#' || line[0] == '\n') continue;
        Ev *e = &ev[nev];
        e->arg[0] = 0;
        int n = 0;
        if (sscanf(line, "%lf %15s %n", &e->t, e->cmd, &n) >= 2) {
            snprintf(e->arg, sizeof e->arg, "%s", line + n);
            size_t l = strlen(e->arg); while (l && (e->arg[l - 1] == '\n' || e->arg[l - 1] == '\r')) e->arg[--l] = 0;
            nev++;
        }
    }
    if (sf) fclose(sf);
    int ei = 0;
    double step = 5.0; /* ms */
    for (double t = 0; t < total && !m->exited; t += step) {
        while (ei < nev && ev[ei].t <= t) {
            Ev *e = &ev[ei++];
            if (!strcmp(e->cmd, "shot")) shot(m, e->arg);
            else if (!strcmp(e->cmd, "fps")) {
                /* sample the framebuffer every 2 ms for N ms and count changes */
                double dur = atof(e->arg); uint32_t last = 0; int changes = 0; uint32_t b0 = m->cpu.vram_blits, by0 = m->cpu.vram_bytes;
                for (double tt = 0; tt < dur; tt += 2) {
                    machine_run(m, 2000);
                    uint32_t h = 2166136261u;
                    for (int i = 0; i < 64000; i += 7) h = (h ^ m->mem[0xA0000 + i]) * 16777619u;
                    if (h != last) changes++;
                    last = h;
                }
                t += dur;
                fprintf(stderr, "[fps] %d frame changes, %u blits in %.0f ms, %.0f KB (mips %.0f)\n", changes, m->cpu.vram_blits - b0, dur, (m->cpu.vram_bytes - by0) / 1024.0, m->ips);
            }
            else if (!strcmp(e->cmd, "mips")) m->ips = atof(e->arg);
            else if (!strcmp(e->cmd, "typ")) { double r, d; sscanf(e->arg, "%lf %lf", &r, &d); m->typ_rate_hz = r; m->typ_delay_us = d * 1000; }
            else if (!strcmp(e->cmd, "stats")) {
                uint64_t f0 = machine_frames(m), j0 = m->joy_triggers, w0 = m->cpu.vram_writes, s0 = m->stat_3da, d0 = m->stat_dac, i16 = m->int_count[0x16];
                double dur = atof(e->arg); machine_run(m, dur * 1000); t += dur;
                fprintf(stderr, "[stats] %.0fms: vram %llu bytes/s, 3DA %llu/s, DAC %llu/s, int16 %llu/s joy %.1f/s FRAMES %.1f/s\n", dur, (unsigned long long)((m->cpu.vram_writes - w0) * 1000 / dur), (unsigned long long)((m->stat_3da - s0) * 1000 / dur), (unsigned long long)((m->stat_dac - d0) * 1000 / dur), (unsigned long long)((m->int_count[0x16] - i16) * 1000 / dur), (m->joy_triggers - j0) * 1000.0 / dur, (machine_frames(m) - f0) * 1000.0 / dur);
            }
            else if (!strcmp(e->cmd, "ticks")) { fprintf(stderr, "[ticks] t=%.0f raised=%llu lost=%llu wall=%.2fs\n", m->time_us/1000, (unsigned long long)m->ticks_raised, (unsigned long long)m->lost_ticks, (double)clock()/CLOCKS_PER_SEC); }
            else if (!strcmp(e->cmd, "joy4")) { float x, y, x2, y2; int b = 0; sscanf(e->arg, "%f %f %f %f %d", &x, &y, &x2, &y2, &b); machine_joystick(m, x, y, x2, y2, b); }
            else if (!strcmp(e->cmd, "joy")) { float x, y; int b = 0; sscanf(e->arg, "%f %f %d", &x, &y, &b); machine_joystick(m, x, y, -2, -2, b); }
            else if (!strcmp(e->cmd, "key") || !strcmp(e->cmd, "down") || !strcmp(e->cmd, "up")) {
                unsigned sc; char ext[8] = "";
                sscanf(e->arg, "%x %7s", &sc, ext);
                int x = !strcmp(ext, "ext");
                if (strcmp(e->cmd, "up")) machine_key(m, (int)sc, x, 1);
                if (!strcmp(e->cmd, "key")) {
                    /* schedule release by running a little first */
                    machine_run(m, 80000);
                    t += 80;
                    machine_key(m, (int)sc, x, 0);
                } else if (!strcmp(e->cmd, "up")) machine_key(m, (int)sc, x, 0);
            } else if (!strcmp(e->cmd, "type")) {
                for (char *p = e->arg; *p; p++) {
                    int sh; int sc = ascii_to_scan(*p, &sh);
                    if (!sc) continue;
                    if (sh) machine_key(m, 0x2A, 0, 1);
                    machine_key(m, sc, 0, 1); machine_run(m, 60000); machine_key(m, sc, 0, 0);
                    if (sh) machine_key(m, 0x2A, 0, 0);
                    machine_run(m, 60000); t += 120;
                }
            } else if (!strcmp(e->cmd, "mouse")) {
                double x, y; sscanf(e->arg, "%lf %lf", &x, &y); machine_mouse_abs(m, x, y);
            } else if (!strcmp(e->cmd, "click")) {
                int b = atoi(e->arg); machine_mouse_buttons(m, b); machine_run(m, 100000); t += 100; machine_mouse_buttons(m, 0);
            } else if (!strcmp(e->cmd, "mbtn")) {
                machine_mouse_buttons(m, atoi(e->arg));
            } else if (!strcmp(e->cmd, "rel")) {
                int dx, dy; sscanf(e->arg, "%d %d", &dx, &dy); machine_mouse_move(m, dx, dy);
            } else if (!strcmp(e->cmd, "dump")) {
                FILE *df = fopen(e->arg, "wb"); fwrite(m->mem, 1, 0x100000, df); fclose(df);
            } else if (!strcmp(e->cmd, "mickey")) {
                int dx, dy; sscanf(e->arg, "%d %d", &dx, &dy); m->mouse_mickey_x += dx; m->mouse_mickey_y += dy;
            }
        }
        machine_run(m, step * 1000);
        if (wav) { int16_t ab[4096 * 2]; int n; while ((n = machine_audio_pull(m, ab, 4096)) > 0) { fwrite(ab, 4, (size_t)n, wav); wav_frames += n; } }
    }
    if (wav) {
        uint32_t data = (uint32_t)wav_frames * 4, riff = 36 + data, sr = 44100, br = 44100 * 4, fmt = 16; uint16_t pcm = 1, ch = 2, ba = 4, bps = 16;
        fseek(wav, 0, SEEK_SET);
        fwrite("RIFF", 1, 4, wav); fwrite(&riff, 4, 1, wav); fwrite("WAVEfmt ", 1, 8, wav); fwrite(&fmt, 4, 1, wav);
        fwrite(&pcm, 2, 1, wav); fwrite(&ch, 2, 1, wav); fwrite(&sr, 4, 1, wav); fwrite(&br, 4, 1, wav); fwrite(&ba, 2, 1, wav); fwrite(&bps, 2, 1, wav);
        fwrite("data", 1, 4, wav); fwrite(&data, 4, 1, wav); fclose(wav);
        fprintf(stderr, "wav: %ld frames\n", wav_frames);
    }
    fprintf(stderr, "done at t=%.0fms CS:IP=%04X:%04X exited=%d icount=%llu\n", m->time_us / 1000, m->cpu.s[CS], m->cpu.ip, m->exited, (unsigned long long)m->cpu.icount);
    fprintf(stderr, "joy reads %llu triggers %llu\n", (unsigned long long)m->joy_reads, (unsigned long long)m->joy_triggers);
    fprintf(stderr, "mouse_active=%d in_3d=%d\n", machine_mouse_active(m), machine_in_3d(m));
    for (int v = 8; v <= 9; v++) fprintf(stderr, "vec %02X = %04X:%04X\n", v, m->mem[v*4+2] | m->mem[v*4+3] << 8, m->mem[v*4] | m->mem[v*4+1] << 8);
    fprintf(stderr, "ints:");
    for (int i = 0; i < 256; i++) if (m->int_count[i]) fprintf(stderr, " %02X:%llu", i, (unsigned long long)m->int_count[i]);
    fprintf(stderr, "\n");
    shot(m, "final");
    machine_destroy(m);
    return 0;
}
