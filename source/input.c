/*
 * Wiimote / Nunchuk / Classic / GC input.
 * Channels 0-3 are all formatted and OR-ed so Dolphin's remote is seen.
 */
#include "input.h"

#include <string.h>
#include <wiiuse/wpad.h>
#include <ogc/pad.h>
#include <grrlib.h>

#define STICK_DZ_MAG 0.45f
#define GC_STICK_DZ  32

static int s_home_held;
static u8  s_fmt_done[4];   /* format applied since this channel last connected */

/* Held D-pad bits for an analogue stick, or 0 inside the deadzone. */
static u32 stick_dir(float mag, float ang)
{
	if (mag < STICK_DZ_MAG)
		return 0;
	if (ang >= 315.0f || ang < 45.0f)
		return WPAD_BUTTON_UP;
	if (ang < 135.0f)
		return WPAD_BUTTON_RIGHT;
	if (ang < 225.0f)
		return WPAD_BUTTON_DOWN;
	return WPAD_BUTTON_LEFT;
}

static u32 expansion_edges_chan0(void)
{
	static u32 was;
	expansion_t exp;
	u32 is = 0;

	/* WPAD_Expansion leaves exp untouched with nothing connected — zero it so
	 * a garbage type can't feed phantom D-pad edges (WPAD_EXP_NONE == 0). */
	memset(&exp, 0, sizeof(exp));
	WPAD_Expansion(0, &exp);

	if (exp.type == WPAD_EXP_NUNCHUK)
		is = stick_dir(exp.nunchuk.js.mag, exp.nunchuk.js.ang);
	else if (exp.type == WPAD_EXP_CLASSIC)
		is = stick_dir(exp.classic.ljs.mag, exp.classic.ljs.ang);

	u32 down = is & ~was;
	was = is;
	return down;
}

static u32 gc_stick_edges(void)
{
	static u32 was;
	s8 x = PAD_StickX(0), y = PAD_StickY(0);
	u32 is = 0;

	if (y >  GC_STICK_DZ) is |= PAD_BUTTON_UP;
	if (y < -GC_STICK_DZ) is |= PAD_BUTTON_DOWN;
	if (x < -GC_STICK_DZ) is |= PAD_BUTTON_LEFT;
	if (x >  GC_STICK_DZ) is |= PAD_BUTTON_RIGHT;

	u32 down = is & ~was;
	was = is;
	return down;
}

static void map_wpad(u32 bits, u32 *a)
{
	if (bits & WPAD_BUTTON_LEFT)  *a |= IN_LEFT;
	if (bits & WPAD_BUTTON_RIGHT) *a |= IN_RIGHT;
	if (bits & WPAD_BUTTON_UP)    *a |= IN_UP;
	if (bits & WPAD_BUTTON_DOWN)  *a |= IN_DOWN;
	if (bits & WPAD_BUTTON_A)     *a |= IN_A;
	if (bits & WPAD_BUTTON_B)     *a |= IN_B;
	if (bits & WPAD_BUTTON_PLUS)  *a |= IN_PLUS;
	if (bits & WPAD_BUTTON_MINUS) *a |= IN_MINUS;
	if (bits & WPAD_BUTTON_HOME)  *a |= IN_HOME;
	if (bits & WPAD_BUTTON_1)     *a |= IN_1;
	if (bits & WPAD_BUTTON_2)     *a |= IN_2;

	if (bits & WPAD_CLASSIC_BUTTON_LEFT)  *a |= IN_LEFT;
	if (bits & WPAD_CLASSIC_BUTTON_RIGHT) *a |= IN_RIGHT;
	if (bits & WPAD_CLASSIC_BUTTON_UP)    *a |= IN_UP;
	if (bits & WPAD_CLASSIC_BUTTON_DOWN)  *a |= IN_DOWN;
	if (bits & WPAD_CLASSIC_BUTTON_A)     *a |= IN_A;
	if (bits & WPAD_CLASSIC_BUTTON_B)     *a |= IN_B;
	if (bits & WPAD_CLASSIC_BUTTON_PLUS)  *a |= IN_PLUS;
	if (bits & WPAD_CLASSIC_BUTTON_MINUS) *a |= IN_MINUS;
	if (bits & WPAD_CLASSIC_BUTTON_HOME)  *a |= IN_HOME;
	if (bits & WPAD_CLASSIC_BUTTON_X)     *a |= IN_2;
	if (bits & WPAD_CLASSIC_BUTTON_Y)     *a |= IN_1;
}

/* Per-channel, not WPAD_CHAN_ALL — SetDataFormat with -1 is quirky on some
 * libogc builds. */
static void apply_wpad_format(int ch)
{
	u32 w = 640, h = 480;
	if (rmode) {
		w = rmode->fbWidth;
		h = rmode->xfbHeight;
	}
	WPAD_SetDataFormat(ch, WPAD_FMT_BTNS_ACC_IR);
	WPAD_SetVRes(ch, w, h);
}

void input_init(void)
{
	WPAD_Init();
	for (int ch = 0; ch < 4; ch++) {
		apply_wpad_format(ch);
		s_fmt_done[ch] = 0;
	}
	PAD_Init();
}

void input_reinit(void)
{
	for (int ch = 0; ch < 4; ch++) {
		apply_wpad_format(ch);
		s_fmt_done[ch] = 0;
	}
}

void input_shutdown(void)
{
	WPAD_Shutdown();
}

u32 input_poll(void)
{
	u32 actions = 0;
	s_home_held = 0;

	WPAD_ScanPads();
	PAD_ScanPads();

	u32 w_down = 0;
	u32 w_held = 0;
	for (int ch = 0; ch < 4; ch++) {
		u32 t = 0;
		if (WPAD_Probe(ch, &t) == WPAD_ERR_NONE) {
			/* Remotes associate asynchronously — SetDataFormat at init is a
			 * no-op, so re-apply the first frame a channel reports in. */
			if (!s_fmt_done[ch]) {
				apply_wpad_format(ch);
				s_fmt_done[ch] = 1;
			}
		} else {
			s_fmt_done[ch] = 0;
		}
		w_down |= WPAD_ButtonsDown(ch);
		w_held |= WPAD_ButtonsHeld(ch);
	}
	/* Stick edges only on chan 0 expansion. */
	w_down |= expansion_edges_chan0();

	map_wpad(w_down, &actions);
	if (w_held & (WPAD_BUTTON_HOME | WPAD_CLASSIC_BUTTON_HOME))
		s_home_held = 1;

	u16 gd = (u16)(PAD_ButtonsDown(0) | gc_stick_edges());
	u16 gh = PAD_ButtonsHeld(0);

	if (gd & PAD_BUTTON_LEFT)  actions |= IN_LEFT;
	if (gd & PAD_BUTTON_RIGHT) actions |= IN_RIGHT;
	if (gd & PAD_BUTTON_UP)    actions |= IN_UP;
	if (gd & PAD_BUTTON_DOWN)  actions |= IN_DOWN;
	if (gd & PAD_BUTTON_A)     actions |= IN_A;
	if (gd & PAD_BUTTON_B)     actions |= IN_B;
	/* Pages on Y/X and L/R — IN_MINUS is never consumed by main.c. */
	if (gd & PAD_BUTTON_Y)     actions |= IN_1;
	if (gd & PAD_BUTTON_X)     actions |= IN_2;
	if (gd & PAD_TRIGGER_L)    actions |= IN_1;
	if (gd & PAD_TRIGGER_R)    actions |= IN_2;
	if (gd & PAD_BUTTON_START) actions |= IN_PLUS;
	if (gh & PAD_BUTTON_START) s_home_held = 1;

	return actions;
}

int input_home_held(void)
{
	return s_home_held;
}
