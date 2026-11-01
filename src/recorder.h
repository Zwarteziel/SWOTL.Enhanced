/* SWOTL Enhanced - diagnostic recorder: uncompressed AVI (320x200, 30 fps)
 * plus a text log of the input and timing state per frame. GPL-3.0-or-later. */
#ifndef SWOTL_RECORDER_H
#define SWOTL_RECORDER_H
#include <stdio.h>
#include <stdint.h>

typedef struct {
    FILE *avi, *log;
    long movi_start;          /* file offset of the 'movi' fourcc */
    uint32_t frames;
    uint32_t *index;          /* offsets of frames relative to movi_start */
    uint32_t index_cap;
    int w, h;
    double t0;
} Recorder;

int  rec_open(Recorder *r, const char *avi_path, const char *log_path, int w, int h, int fps);
/* rgba: w*h pixels 0xAABBGGRR */
void rec_frame(Recorder *r, const uint32_t *rgba);
void rec_close(Recorder *r);
#endif
