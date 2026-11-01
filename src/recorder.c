/* SWOTL Enhanced - diagnostic recorder. GPL-3.0-or-later. */
#include "recorder.h"
#include <stdlib.h>
#include <string.h>

static void w32(FILE *f, uint32_t v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); fputc((v >> 16) & 255, f); fputc(v >> 24, f); }
static void w16(FILE *f, uint16_t v) { fputc(v & 255, f); fputc(v >> 8, f); }
static void cc(FILE *f, const char *s) { fwrite(s, 1, 4, f); }

int rec_open(Recorder *r, const char *avi_path, const char *log_path, int w, int h, int fps) {
    memset(r, 0, sizeof *r);
    r->avi = fopen(avi_path, "wb");
    if (!r->avi) return -1;
    r->log = fopen(log_path, "w");
    r->w = w; r->h = h;
    uint32_t fsz = (uint32_t)(w * h * 3);
    FILE *f = r->avi;
    cc(f, "RIFF"); w32(f, 0); cc(f, "AVI ");
    cc(f, "LIST"); w32(f, 4 + 8 + 56 + 8 + 4 + 8 + 56 + 8 + 40); cc(f, "hdrl");
    cc(f, "avih"); w32(f, 56);
    w32(f, 1000000 / fps); w32(f, fsz * fps); w32(f, 0); w32(f, 0x10);
    w32(f, 0); /* total frames, patched */
    w32(f, 0); w32(f, 1); w32(f, fsz); w32(f, (uint32_t)w); w32(f, (uint32_t)h);
    w32(f, 0); w32(f, 0); w32(f, 0); w32(f, 0);
    cc(f, "LIST"); w32(f, 4 + 8 + 56 + 8 + 40); cc(f, "strl");
    cc(f, "strh"); w32(f, 56);
    cc(f, "vids"); cc(f, "DIB "); w32(f, 0); w16(f, 0); w16(f, 0); w32(f, 0);
    w32(f, 1); w32(f, (uint32_t)fps); w32(f, 0);
    w32(f, 0); /* length, patched */
    w32(f, fsz); w32(f, 0xFFFFFFFF); w32(f, 0);
    w16(f, 0); w16(f, 0); w16(f, (uint16_t)w); w16(f, (uint16_t)h);
    cc(f, "strf"); w32(f, 40);
    w32(f, 40); w32(f, (uint32_t)w); w32(f, (uint32_t)h); w16(f, 1); w16(f, 24); w32(f, 0); w32(f, fsz);
    w32(f, 0); w32(f, 0); w32(f, 0); w32(f, 0);
    cc(f, "LIST"); w32(f, 0); /* patched */
    r->movi_start = ftell(f);
    cc(f, "movi");
    return 0;
}

void rec_frame(Recorder *r, const uint32_t *rgba) {
    if (!r->avi) return;
    if (r->frames == r->index_cap) {
        r->index_cap = r->index_cap ? r->index_cap * 2 : 1024;
        r->index = realloc(r->index, r->index_cap * sizeof *r->index);
    }
    r->index[r->frames++] = (uint32_t)(ftell(r->avi) - r->movi_start);
    uint32_t fsz = (uint32_t)(r->w * r->h * 3);
    cc(r->avi, "00db"); w32(r->avi, fsz);
    static uint8_t line[640 * 3];
    for (int y = r->h - 1; y >= 0; y--) {          /* bottom-up BGR */
        const uint32_t *src = rgba + y * r->w;
        for (int x = 0; x < r->w; x++) {
            uint32_t p = src[x];
            line[x * 3] = (uint8_t)(p >> 16); line[x * 3 + 1] = (uint8_t)(p >> 8); line[x * 3 + 2] = (uint8_t)p;
        }
        fwrite(line, 1, (size_t)r->w * 3, r->avi);
    }
}

void rec_close(Recorder *r) {
    if (!r->avi) return;
    FILE *f = r->avi;
    long movi_end = ftell(f);
    uint32_t fsz = (uint32_t)(r->w * r->h * 3);
    cc(f, "idx1"); w32(f, r->frames * 16);
    for (uint32_t i = 0; i < r->frames; i++) { cc(f, "00db"); w32(f, 0x10); w32(f, r->index[i]); w32(f, fsz); }
    long end = ftell(f);
    fseek(f, 4, SEEK_SET); w32(f, (uint32_t)(end - 8));
    fseek(f, 12 + 8 + 4 + 8 + 16, SEEK_SET); w32(f, r->frames);       /* avih total frames */
    fseek(f, 12 + 8 + 4 + 8 + 56 + 12 + 8 + 32, SEEK_SET); w32(f, r->frames); /* strh length */
    fseek(f, r->movi_start - 4, SEEK_SET); w32(f, (uint32_t)(movi_end - r->movi_start));
    fclose(f);
    if (r->log) fclose(r->log);
    free(r->index);
    memset(r, 0, sizeof *r);
}
