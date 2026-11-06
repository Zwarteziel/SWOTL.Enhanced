/* SWOTL Enhanced - emulated PC: memory, BIOS/DOS services, timer, keyboard,
 * VGA, AdLib/OPL2, joystick and mouse. GPL-3.0-or-later. */
#ifndef SWOTL_MACHINE_H
#define SWOTL_MACHINE_H
#include <stdint.h>
#include <stdio.h>
#include "cpu.h"
#include "../third_party/opl3.h"

#define MAX_FILES 40
#define AUDIO_RING 32768

struct Hires;

typedef struct Machine {
    CPU cpu;
    uint8_t *mem;
    char gamedir[1024];
    int log_level;
    FILE *log;

    /* time (virtual, microseconds) */
    double time_us;
    double ips;              /* instructions per microsecond */
    double slice_t0;         /* time at start of the current CPU slice */
    uint64_t slice_ic0;
    int in_slice;
    double slice_deadline;

    /* PIC */
    uint8_t pic_imr, pic_irr, pic_isr, pic_icw_step;

    /* PIT */
    uint16_t pit_reload[3];
    uint8_t pit_mode[3], pit_rw[3], pit_wstate[3], pit_rstate[3];
    uint16_t pit_latch[3]; int pit_latched[3];
    double pit_start_us[3];
    double next_irq0_us;
    uint8_t port61;

    /* keyboard */
    uint8_t kbq[256]; int kbq_head, kbq_tail;
    uint8_t kb_data;
    double kb_next_us;

    /* VGA */
    int vmode;
    uint8_t dac[256][3];
    uint8_t dac_widx, dac_ridx, dac_wcomp, dac_rcomp, dac_mask, dac_state;
    uint8_t seq_idx, seq[8];
    uint8_t gc_idx, gc[16];
    uint8_t crtc_idx, crtc[32];
    uint8_t attr_idx, attr[32]; int attr_flip;
    uint8_t misc;
    uint8_t latch[4];
    uint8_t *vram;           /* 4 planes x 64K, used when unchained */
    int planar;
    int frame_count;
    uint32_t palette_rgba[256];

    /* mouse */
    int mouse_present;
    int mouse_x, mouse_y, mouse_buttons, mouse_shown;
    int mouse_minx, mouse_maxx, mouse_miny, mouse_maxy;
    int mouse_press_cnt[3], mouse_rel_cnt[3];
    int mouse_press_x[3], mouse_press_y[3], mouse_rel_x[3], mouse_rel_y[3];
    int mouse_mickey_x, mouse_mickey_y;
    double mouse_last_poll_us;   /* when the game last asked for the mouse */

    /* joystick (axes in -1..1) */
    float joy_axis[4];
    uint8_t joy_buttons;     /* bit set = pressed */
    double joy_trigger_us;
    uint64_t joy_reads, joy_triggers, joy_reads_at_trigger;
    /* automatic answer to the game's start-up joystick calibration */
    int joy_autocal, joy_cal_step;
    double joy_cal_t; uint64_t joy_cal_trig;
    float joy_cal_x, joy_cal_y; int joy_cal_btn;

    /* OPL2 */
    opl3_chip opl;
    uint8_t opl_index;
    uint8_t opl_timer1, opl_timer2, opl_ctrl;
    double opl_t1_start, opl_t2_start;
    int16_t audio_ring[AUDIO_RING * 2];
    uint32_t audio_wpos, audio_rpos;
    double audio_rate;
    double audio_gen_pos;    /* samples generated (fractional bookkeeping) */
    /* PC speaker */
    int spk_on;

    /* DOS */
    FILE *files[MAX_FILES];
    char filenames[MAX_FILES][260];
    uint16_t psp_seg, env_seg;
    uint16_t dta_seg, dta_off;
    char cwd[128];           /* DOS current dir, without drive, e.g. "" or "FE" */
    uint16_t last_error;
    uint8_t dos_ext_pending;  /* scan code owed after a 0 from INT 21h input */
    int exited, exit_code;
    /* find first/next */
    char find_pattern[260];
    char find_dir[1024];
    int find_index;
    uint8_t find_attr;

    /* stats */
    uint64_t int_count[256];
    uint64_t lost_ticks, ticks_raised;
    uint64_t stat_3da, stat_dac;
    double sig_scan_us;      /* last search for the frame-present routine */
    uint64_t frames_seen; double last_frame_us;

    /* BIOS-style key repeat (typematic) for the most recently pressed key */
    int typ_code;            /* scancode | 0x100 for E0 keys, 0 = none */
    double typ_next_us, typ_rate_hz, typ_delay_us;

    /* experimental high-resolution 3D view (NULL = off) */
    struct Hires *hires;
} Machine;

Machine *machine_create(const char *gamedir, const char *exe, const char *args);
void machine_destroy(Machine *m);
/* run for 'us' microseconds of emulated time */
void machine_run(Machine *m, double us);

/* input (XT set-1 scancodes; for extended keys pass 0xE0 prefix as 'ext') */
void machine_key(Machine *m, int scancode, int ext, int down);
void machine_mouse_move(Machine *m, int dx, int dy);
void machine_mouse_abs(Machine *m, double fx, double fy); /* 0..1 */
void machine_mouse_buttons(Machine *m, int buttons);
void machine_joystick(Machine *m, float x, float y, float x2, float y2, int buttons);

/* render current display into 640x400 or 320x200 RGBA; returns 1 if valid */
int machine_render(Machine *m, uint32_t *rgba, int *w, int *h);
/* pull audio frames (stereo s16); returns frames written */
int machine_audio_pull(Machine *m, int16_t *out, int frames);

void machine_logf(Machine *m, int level, const char *fmt, ...);
/* 1 while the game is using the mouse (menus, map), 0 in flight */
int machine_mouse_active(Machine *m);
/* number of 3D frames the game has presented so far (VGA driver present calls) */
uint64_t machine_frames(Machine *m);
/* 1 while the 3D view is running (flight or replay): the game presented a
 * 3D frame in the last half second. The game also polls the mouse in flight
 * (mouse flying), so mouse polling cannot tell menus from flight. */
int machine_in_3d(Machine *m);

/* Experimental high-resolution 3D view: on/off. */
void machine_set_hires(Machine *m, int on);
/* Draws the screen at 320*n x 200*n into out when the 3D view can be redrawn
 * at that resolution; returns 0 otherwise (use machine_render). */
int machine_render_hires(Machine *m, int n, uint32_t *out);
/* "" when working or off, otherwise a short reason */
const char *machine_hires_state(Machine *m);

#endif
