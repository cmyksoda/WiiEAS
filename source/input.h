#ifndef WIIEAS_INPUT_H
#define WIIEAS_INPUT_H

#include <gctypes.h>

/* Edge-triggered actions for one frame. */
enum {
	IN_LEFT   = 1 << 0,
	IN_RIGHT  = 1 << 1,
	IN_UP     = 1 << 2,
	IN_DOWN   = 1 << 3,
	IN_A      = 1 << 4,
	IN_B      = 1 << 5,
	IN_PLUS   = 1 << 6,
	IN_MINUS  = 1 << 7,
	IN_HOME   = 1 << 8,
	IN_1      = 1 << 9,
	IN_2      = 1 << 10,
};

void input_init(void);
/* Call again after VIDEO/GRRLIB are up (some stacks need pads post-video). */
void input_reinit(void);
void input_shutdown(void);

/* Scan all controllers; returns bitfield of new presses this frame. */
u32 input_poll(void);

/* Non-zero while HOME / Classic HOME / GC Start is held on any pad. */
int input_home_held(void);

/*
 * Diagnostics for the status bezel:
 *   out_pad  — PAD_ButtonsHeld(0)
 *   out_wpad — WPAD_ButtonsHeld(0)
 *   out_act  — last actions bitfield from input_poll
 */
void input_debug_raw(u16 *out_pad, u32 *out_wpad, u32 *out_act);

/* Non-zero if any WPAD channel reports a connected remote this frame. */
int input_wiimote_connected(void);

#endif
