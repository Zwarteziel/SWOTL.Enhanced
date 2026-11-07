/* SWOTL Enhanced - experimental high-resolution 3D view. GPL-3.0-or-later.
 *
 * The game draws its 3D view at 320x200 into a RAM buffer and copies it to
 * the screen through the cockpit mask. This module watches the drawing
 * routines of the game (ground polygons, roads, objects), recomputes each
 * projected point with more precision by running the game's own projection
 * routine a second time on the side with a larger focal length, and redraws
 * those shapes at N times the resolution. Everything it does not know (the
 * cockpit, aircraft, smoke) stays as the game drew it, scaled up. */
#ifndef SWOTL_HIRES_H
#define SWOTL_HIRES_H
#include <stdint.h>
#include "cpu.h"

struct Machine;
typedef struct Hires Hires;

Hires *hires_create(void);
void hires_destroy(Hires *h);

/* Looks for the game's 3D routines in memory and sets the CPU breakpoints.
 * Returns 1 when found. When the game is flying (in_3d) and they are not
 * there, the game release is marked as not supported. */
int hires_scan(Hires *h, struct Machine *m, int in_3d);
/* Removes the breakpoints. */
void hires_detach(Hires *h, struct Machine *m);
/* Called by the machine at each frame present of the VGA driver. */
void hires_present(Hires *h, struct Machine *m, CPU *c);
/* Called when the present call returns. */
void hires_present_done(Hires *h, struct Machine *m, CPU *c);
/* Called by the CPU at a breakpoint. */
void hires_breakpoint(Hires *h, struct Machine *m, CPU *c);

/* Draws the current screen at 320*n x 200*n RGBA into out. Returns 1 if the
 * 3D view was redrawn, 0 if there is nothing to redraw (out is untouched). */
int hires_compose(Hires *h, struct Machine *m, int n, uint32_t *out);
/* Prints internal state (tests). */
#include <stdio.h>
void hires_debug(Hires *h, struct Machine *m, FILE *out);
/* Short state text: "", "waiting for flight", "game version not supported". */
const char *hires_state(Hires *h);

#endif
