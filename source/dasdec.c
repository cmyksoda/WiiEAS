/**
 * DASDEC-style character generator.
 *
 * Real CG: black outside a red rectangular frame, deep blue-violet interior,
 * centered white Luxi Mono Bold text, page counter at the bottom of the box.
 * App chrome (AUTO / status / help) is tiny text on the black bezel only.
 */
#include "dasdec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#include <grrlib.h>
#include "font_ttf.h"

#define MAX_LINES_PER_PAGE  10
#define MAX_PAGES           32
#define MAX_LINE_CHARS      80
#define MAX_LINES_TOTAL     (MAX_LINES_PER_PAGE * MAX_PAGES)

#define FONT_SIZE_BODY      20
#define FONT_SIZE_HDR       18
#define FONT_SIZE_PAGE      16
#define FONT_SIZE_PANEL     16
#define FONT_SIZE_CHROME    12
/*
 * Page dwell in *frames*, not wall-clock ms. ticks_to_millisecs can run hot on
 * Dolphin and made a "5000 ms" hold feel like 1–2 s. At 60 Hz, 300 ≈ 5 s;
 * at 50 Hz PAL it's 6 s — both fine.
 */
#define PAGE_HOLD_FRAMES    300

/* Layout constants — keep wrap and draw in lockstep so text never leaves the box. */
#define LAYOUT_MARGIN_X     36.0f
/*
 * App chrome sits on the black bezel: the status line on the top edge, the
 * index / AUTO / AUDIO line on the bottom edge, each the same distance from
 * its own edge so the pair reads as symmetrical top-to-bottom.
 *
 * The pad is set by CRT overscan, not by looks. A tube typically eats 5-8% of
 * each edge; on 480 lines that's 24-38 px, and the old 14 px from the bottom
 * edge put the status line — including "Downloading..." — straight into the
 * part of the picture a CRT never shows. 32 px clears 5% comfortably and most
 * of 8%. Raising it further is fine down to ~60: past that LAYOUT_MARGIN_Y
 * squeezes the box below the 360 px that ten body lines need.
 */
#define LAYOUT_CHROME_PAD   32.0f   /* screen edge  -> chrome glyphs */
#define LAYOUT_CHROME_GAP   8.0f    /* chrome glyphs -> red frame */
/* Equal top and bottom margins keep the CG box centred between the two lines. */
#define LAYOUT_MARGIN_Y     (LAYOUT_CHROME_PAD + (f32)FONT_SIZE_CHROME \
                             + LAYOUT_CHROME_GAP)
#define LAYOUT_BORDER_THICK 8.0f
#define LAYOUT_INNER_PAD    20.0f

typedef struct {
	char lines[MAX_LINES_TOTAL][MAX_LINE_CHARS];
	int  line_count;
	int  page;
	int  page_count;
	int  index;   /* 1-based display index among active alerts */
	int  total;
	int  has_alert;
	int  max_text_w; /* pixel width used for wrapping */
	char type[CAR_TYPE_LEN];
	char severity[CAR_SEV_LEN];
	char callsign[CAR_CALL_LEN];
	char originator[CAR_ORIG_LEN];
	char hash[CAR_HASH_LEN + 1]; /* full 40-char CAR hash */
	s64  start_epoch;
	s64  end_epoch;
	u32  page_frames; /* frames spent on current page (for auto-advance) */
} DasdecState;

static GRRLIB_ttfFont *s_font;
static DasdecState s;

/* Seconds to add to time(NULL) to get UTC — see dasdec_set_utc_offset(). */
static s64 s_utc_offset;

void dasdec_set_utc_offset(s64 seconds)
{
	s_utc_offset = seconds;
}

static void clear_layout(void)
{
	memset(&s, 0, sizeof(s));
	s.page_count = 1;
}

int dasdec_init(void)
{
	clear_layout();
	s_font = GRRLIB_LoadTTF(font_ttf, (s32)font_ttf_size);
	return s_font ? 0 : -1;
}

void dasdec_shutdown(void)
{
	if (s_font) {
		GRRLIB_FreeTTF(s_font);
		s_font = NULL;
	}
}

static int measure(const char *str, unsigned size)
{
	if (!s_font || !str || !str[0])
		return 0;
	return (int)GRRLIB_WidthTTF(s_font, str, size);
}

/* Usable text width inside the red frame (must match dasdec_draw). */
static int text_area_width(void)
{
	int fb = 640;
	if (rmode)
		fb = (int)rmode->fbWidth;
	int w = fb - (int)(LAYOUT_MARGIN_X * 2.0f)
	           - (int)(LAYOUT_BORDER_THICK * 2.0f)
	           - (int)(LAYOUT_INNER_PAD * 2.0f);
	if (w < 200)
		w = 200;
	return w;
}

static void push_line(const char *line)
{
	if (s.line_count >= MAX_LINES_TOTAL)
		return;
	snprintf(s.lines[s.line_count], MAX_LINE_CHARS, "%s", line);
	s.line_count++;
}

/*
 * Pixel-aware word wrap so glyphs never spill past the red border.
 * Soft-wrap on spaces; hard-break overlong tokens by character width.
 */
static void wrap_text(const char *text)
{
	s.line_count = 0;
	s.max_text_w = text_area_width();
	if (!text || !text[0])
		return;

	const int max_w = s.max_text_w;
	char line[MAX_LINE_CHARS];
	line[0] = '\0';
	size_t line_len = 0;

	const char *p = text;
	while (*p && s.line_count < MAX_LINES_TOTAL) {
		/* Newlines force a break. */
		if (*p == '\n' || *p == '\r') {
			push_line(line);
			line[0] = '\0';
			line_len = 0;
			if (*p == '\r' && p[1] == '\n')
				p++;
			p++;
			continue;
		}

		/* Skip ordinary spaces at the start of a line. */
		if (isspace((unsigned char)*p)) {
			if (line_len == 0) {
				p++;
				continue;
			}
		}

		/* Collect next word (or single non-space run). */
		const char *wstart = p;
		while (*p && !isspace((unsigned char)*p) && *p != '\n' && *p != '\r')
			p++;
		size_t wlen = (size_t)(p - wstart);
		if (wlen == 0) {
			p++;
			continue;
		}
		if (wlen >= MAX_LINE_CHARS)
			wlen = MAX_LINE_CHARS - 1;

		char word[MAX_LINE_CHARS];
		memcpy(word, wstart, wlen);
		word[wlen] = '\0';

		/* Try "line + space + word" (or just word if line empty). */
		char trial[MAX_LINE_CHARS];
		if (line_len == 0)
			snprintf(trial, sizeof(trial), "%s", word);
		else
			snprintf(trial, sizeof(trial), "%s %s", line, word);

		if (measure(trial, FONT_SIZE_BODY) <= max_w) {
			snprintf(line, sizeof(line), "%s", trial);
			line_len = strlen(line);
			continue;
		}

		/* Doesn't fit: flush current line first. */
		if (line_len > 0) {
			push_line(line);
			line[0] = '\0';
			line_len = 0;
		}

		/* Word alone may still be too wide — hard-break by glyph width. */
		if (measure(word, FONT_SIZE_BODY) <= max_w) {
			snprintf(line, sizeof(line), "%s", word);
			line_len = strlen(line);
			continue;
		}

		size_t i = 0;
		while (i < wlen && s.line_count < MAX_LINES_TOTAL) {
			size_t take = 1;
			while (i + take < wlen) {
				char chunk[MAX_LINE_CHARS];
				size_t n = take + 1;
				if (n >= MAX_LINE_CHARS)
					break;
				memcpy(chunk, word + i, n);
				chunk[n] = '\0';
				if (measure(chunk, FONT_SIZE_BODY) > max_w)
					break;
				take++;
			}
			char chunk[MAX_LINE_CHARS];
			memcpy(chunk, word + i, take);
			chunk[take] = '\0';
			push_line(chunk);
			i += take;
		}
	}

	if (line_len > 0)
		push_line(line);

	if (s.line_count == 0)
		push_line("(no message text)");

	s.page_count = (s.line_count + MAX_LINES_PER_PAGE - 1) / MAX_LINES_PER_PAGE;
	if (s.page_count < 1)
		s.page_count = 1;
	if (s.page_count > MAX_PAGES)
		s.page_count = MAX_PAGES;
	s.page = 0;
}

void dasdec_set_message(const char *msg)
{
	clear_layout();
	s.max_text_w = text_area_width();
	s.has_alert = 0;
	push_line((msg && msg[0]) ? msg : "");
	s.page_count = 1;
}

void dasdec_set_alert(const CarAlert *alert, int index, int total)
{
	clear_layout();
	s.index = index;
	s.total = total;
	s.page_frames = 0;
	s.has_alert = 0;
	s.max_text_w = text_area_width();

	if (!alert) {
		push_line("NO ACTIVE ALERTS");
		s.page_count = 1;
		return;
	}

	s.has_alert = 1;
	snprintf(s.type, sizeof(s.type), "%s", alert->type);
	snprintf(s.severity, sizeof(s.severity), "%s", alert->severity);
	snprintf(s.callsign, sizeof(s.callsign), "%s", alert->callsign);
	snprintf(s.originator, sizeof(s.originator), "%s", alert->originator);
	s.start_epoch = alert->start_epoch;
	s.end_epoch = alert->end_epoch;
	if (alert->hash[0])
		snprintf(s.hash, sizeof(s.hash), "%s", alert->hash);

	wrap_text(alert->translation);
}

void dasdec_next_page(void)
{
	if (s.page_count <= 1)
		return;
	s.page = (s.page + 1) % s.page_count;
	s.page_frames = 0;
}

void dasdec_prev_page(void)
{
	if (s.page_count <= 1)
		return;
	s.page = (s.page + s.page_count - 1) % s.page_count;
	s.page_frames = 0;
}

void dasdec_auto_page_tick(void)
{
	if (s.page_count <= 1)
		return;
	s.page_frames++;
	if (s.page_frames >= PAGE_HOLD_FRAMES) {
		s.page = (s.page + 1) % s.page_count;
		s.page_frames = 0;
	}
}

int dasdec_page(void) { return s.page; }
int dasdec_page_count(void) { return s.page_count; }

static void draw_border_box(f32 x, f32 y, f32 w, f32 h, f32 thick, u32 color)
{
	GRRLIB_Rectangle(x, y, w, thick, color, true);
	GRRLIB_Rectangle(x, y + h - thick, w, thick, color, true);
	GRRLIB_Rectangle(x, y, thick, h, color, true);
	GRRLIB_Rectangle(x + w - thick, y, thick, h, color, true);
}

/* Centre text in [box_left, box_right]; clamp so wide strings never start left of box_left. */
static int center_x(const char *str, unsigned size, int box_left, int box_right)
{
	if (!s_font || !str)
		return box_left;
	int tw = measure(str, size);
	int span = box_right - box_left;
	if (tw >= span)
		return box_left;
	return box_left + (span - tw) / 2;
}

static void format_epoch_utc(s64 epoch, char *buf, size_t n)
{
	if (epoch <= 0) {
		snprintf(buf, n, "(unknown)");
		return;
	}
	time_t t = (time_t)epoch;
	struct tm *tm = gmtime(&t);
	if (!tm) {
		snprintf(buf, n, "%lld", (long long)epoch);
		return;
	}
	strftime(buf, n, "%Y-%m-%d  %H:%M UTC", tm);
}

static void format_relative(s64 epoch, int as_expiry, char *buf, size_t n)
{
	buf[0] = '\0';
	if (epoch <= 0 || n < 8)
		return;

	/*
	 * Alert epochs are UTC, but the Wii clock is the console's *local* time —
	 * comparing them raw shifted everything by the local UTC offset, so an
	 * alert sent moments ago read as "in 5 hours". s_utc_offset (learned from
	 * the HTTP Date: header, pushed in by main after each fetch) corrects the
	 * clock back to real UTC.
	 */
	s64 now = (s64)time(NULL) + s_utc_offset;
	if (now < 1577836800 || now > 4102444800)
		return;

	/*
	 * Signed seconds until epoch — negative means it already happened.
	 * Computing this the other way round for "Sent" inverted the wording:
	 * an alert sent 8 minutes ago rendered as "in 8 minutes".
	 */
	s64 diff = epoch - now;
	int past = diff < 0;
	if (past)
		diff = -diff;

	const char *unit;
	long amount;
	if (diff < 60) {
		amount = (long)diff;
		unit = amount == 1 ? "second" : "seconds";
	} else if (diff < 3600) {
		amount = (long)(diff / 60);
		unit = amount == 1 ? "minute" : "minutes";
	} else if (diff < 86400) {
		amount = (long)(diff / 3600);
		unit = amount == 1 ? "hour" : "hours";
	} else {
		amount = (long)(diff / 86400);
		unit = amount == 1 ? "day" : "days";
	}

	if (as_expiry) {
		if (past)
			snprintf(buf, n, "expired %ld %s ago", amount, unit);
		else
			snprintf(buf, n, "in %ld %s", amount, unit);
	} else {
		if (past)
			snprintf(buf, n, "%ld %s ago", amount, unit);
		else
			snprintf(buf, n, "in %ld %s", amount, unit);
	}
}

/*
 * Two-column row: labels share one pixel column, values share another.
 * Using %-Ns with TTF was misaligned (glyph advances ≠ fixed cells; size
 * changes shift the value column). Measure "Originator " once for the gap.
 */
static int s_value_x;

static void panel_row(int left, int right, int y, unsigned size,
                      const char *label, const char *value, u32 color)
{
	if (label && label[0])
		GRRLIB_PrintfTTF(left, y, s_font, label, size, color);

	const char *val = (value && value[0]) ? value : "---";
	char buf[96];
	snprintf(buf, sizeof(buf), "%s", val);

	/* Clip value to stay inside the panel right edge. */
	while (buf[0] && measure(buf, size) > (right - s_value_x))
		buf[strlen(buf) - 1] = '\0';

	GRRLIB_PrintfTTF(s_value_x, y, s_font, buf, size, color);
}

static void draw_details_panel(void)
{
	const f32 scr_w = (f32)rmode->fbWidth;
	const f32 scr_h = (f32)rmode->xfbHeight;

	GRRLIB_Rectangle(0.0f, 0.0f, scr_w, scr_h, 0x000000AA, true);

	const f32 panel_w = scr_w - 48.0f;
	const f32 panel_h = 320.0f;
	const f32 panel_x = (scr_w - panel_w) * 0.5f;
	const f32 panel_y = (scr_h - panel_h) * 0.5f - 6.0f;
	const f32 thick = 6.0f;
	const unsigned body = 15; /* every row same size — incl. ID */

	GRRLIB_Rectangle(panel_x, panel_y, panel_w, panel_h, DASDEC_BG, true);
	draw_border_box(panel_x, panel_y, panel_w, panel_h, thick, DASDEC_BORDER);

	if (!s_font)
		return;

	const int left = (int)(panel_x + thick + 18.0f);
	const int right = (int)(panel_x + panel_w - thick - 18.0f);
	int y = (int)(panel_y + thick + 14.0f);
	const int line_h = (int)body + 6;

	/* Fixed value column = width of longest label + gap (pixel-accurate). */
	s_value_x = left + measure("Originator", body) + 16;
	if (s_value_x > right - 40)
		s_value_x = left + 120;

	GRRLIB_PrintfTTF(left, y, s_font, "ALERT DETAILS", FONT_SIZE_HDR, DASDEC_CHROME);
	y += line_h + 2;
	GRRLIB_Rectangle((f32)left, (f32)y, (f32)(right - left), 2.0f, DASDEC_BORDER, true);
	y += 10;

	if (!s.has_alert) {
		GRRLIB_PrintfTTF(left, y, s_font, "No alert selected.", body, DASDEC_TEXT);
		y = (int)(panel_y + panel_h - thick - 28.0f);
		GRRLIB_PrintfTTF(left, y, s_font, "+ / B  close", FONT_SIZE_CHROME, DASDEC_DIM);
		return;
	}

	char absbuf[40];
	char relbuf[40];

	panel_row(left, right, y, body, "Type", s.type, DASDEC_TEXT);
	y += line_h;
	panel_row(left, right, y, body, "Severity", s.severity, DASDEC_TEXT);
	y += line_h;
	panel_row(left, right, y, body, "Callsign", s.callsign, DASDEC_TEXT);
	y += line_h;
	panel_row(left, right, y, body, "Originator", s.originator, DASDEC_TEXT);
	y += line_h + 4;

	format_epoch_utc(s.start_epoch, absbuf, sizeof(absbuf));
	format_relative(s.start_epoch, 0, relbuf, sizeof(relbuf));
	panel_row(left, right, y, body, "Sent", absbuf, DASDEC_TEXT);
	y += line_h;
	if (relbuf[0]) {
		char ind[48];
		snprintf(ind, sizeof(ind), "(%s)", relbuf);
		panel_row(left, right, y, body, NULL, ind, DASDEC_DIM);
		y += line_h;
	}

	format_epoch_utc(s.end_epoch, absbuf, sizeof(absbuf));
	format_relative(s.end_epoch, 1, relbuf, sizeof(relbuf));
	panel_row(left, right, y, body, "Expires", absbuf, DASDEC_TEXT);
	y += line_h;
	if (relbuf[0]) {
		char ind[48];
		snprintf(ind, sizeof(ind), "(%s)", relbuf);
		panel_row(left, right, y, body, NULL, ind, DASDEC_DIM);
		y += line_h;
	}

	y += 4;
	/* Same size + white as every other title row; full hash. */
	if (s.hash[0])
		panel_row(left, right, y, body, "ID", s.hash, DASDEC_TEXT);

	/*
	 * 28 px above the border, not 18: GRRLIB's TTF path puts the *baseline*
	 * around y + size, so at 18 px the glyph bottoms grazed the red border and
	 * the line looked clipped on hardware.
	 */
	y = (int)(panel_y + panel_h - thick - 28.0f);
	GRRLIB_PrintfTTF(left, y, s_font, "+ / B  close", FONT_SIZE_CHROME, DASDEC_DIM);
}

void dasdec_draw(const char *status_line, int auto_play, int show_details)
{
	const f32 scr_w = (f32)rmode->fbWidth;
	const f32 scr_h = (f32)rmode->xfbHeight;

	/* Outside the red frame is pure black (real DASDEC CG over black). */
	GRRLIB_FillScreen(DASDEC_OUTSIDE);

	const f32 margin_x = LAYOUT_MARGIN_X;
	const f32 margin_y = LAYOUT_MARGIN_Y;
	const f32 thick = LAYOUT_BORDER_THICK;
	const f32 pad = LAYOUT_INNER_PAD;
	const f32 box_x = margin_x;
	const f32 box_y = margin_y;
	const f32 box_w = scr_w - margin_x * 2.0f;
	const f32 box_h = scr_h - margin_y * 2.0f;

	/* Fill interior blue first, then red frame on top. */
	GRRLIB_Rectangle(box_x, box_y, box_w, box_h, DASDEC_BG, true);
	draw_border_box(box_x, box_y, box_w, box_h, thick, DASDEC_BORDER);

	if (!s_font)
		return;

	const int inner_l = (int)(box_x + thick + pad);
	const int inner_r = (int)(box_x + box_w - thick - pad);
	const int text_top = (int)(box_y + thick + pad);
	const int page_y = (int)(box_y + box_h - thick - pad - FONT_SIZE_PAGE);

	/* Centered body text for the current page (authentic DASDEC). */
	const int line_h = FONT_SIZE_BODY + 8;
	const int first = s.page * MAX_LINES_PER_PAGE;
	int lines_this = 0;
	for (int i = 0; i < MAX_LINES_PER_PAGE; i++) {
		if (first + i >= s.line_count)
			break;
		lines_this++;
	}
	/* Vertically centre the block in the usable area above the page counter. */
	const int text_bottom = page_y - 8;
	const int block_h = lines_this > 0 ? (lines_this * line_h) : line_h;
	int body_y0 = text_top;
	if (text_bottom > text_top + block_h)
		body_y0 = text_top + (text_bottom - text_top - block_h) / 2;

	for (int i = 0; i < MAX_LINES_PER_PAGE; i++) {
		int li = first + i;
		if (li >= s.line_count)
			break;
		if (s.lines[li][0] == '\0')
			continue;
		int y = body_y0 + i * line_h;
		/* Hard clip: never draw into/through the page counter or border. */
		if (y + FONT_SIZE_BODY > page_y - 4)
			break;
		int x = center_x(s.lines[li], FONT_SIZE_BODY, inner_l, inner_r);
		GRRLIB_PrintfTTF(x, y, s_font, s.lines[li], FONT_SIZE_BODY, DASDEC_TEXT);
	}

	/* Page counter — bottom centre *inside* the blue box, classic "1/3". */
	char pagebuf[24];
	snprintf(pagebuf, sizeof(pagebuf), "%d/%d", s.page + 1, s.page_count);
	{
		int px = center_x(pagebuf, FONT_SIZE_PAGE, inner_l, inner_r);
		GRRLIB_PrintfTTF(px, page_y, s_font, pagebuf, FONT_SIZE_PAGE, DASDEC_TEXT);
	}

	/*
	 * App chrome on the black bezel — one centred line at each edge, purple
	 * (same family as the box fill) so they don't shout over the CG.
	 *   top:     status  (empty unless there's something to report — the
	 *            controls legend that used to idle here lives in the README)
	 *   bottom:  1/30  AUTO
	 *
	 * No AUDIO flag: the status line says PLAYING when playback starts, which
	 * is the same information a beat earlier.
	 */
	const int chrome_top_y = (int)LAYOUT_CHROME_PAD;
	const int chrome_bot_y = (int)(scr_h - LAYOUT_CHROME_PAD
	                               - (f32)FONT_SIZE_CHROME);
	const int scr_l = 0;
	const int scr_r = (int)scr_w;

	char line1[48];
	if (s.total > 0) {
		if (auto_play)
			snprintf(line1, sizeof(line1), "%d/%d   AUTO", s.index, s.total);
		else
			snprintf(line1, sizeof(line1), "%d/%d", s.index, s.total);
	} else if (auto_play) {
		snprintf(line1, sizeof(line1), "AUTO");
	} else {
		line1[0] = '\0';
	}

	if (status_line && status_line[0]) {
		int x = center_x(status_line, FONT_SIZE_CHROME, scr_l, scr_r);
		GRRLIB_PrintfTTF(x, chrome_top_y, s_font, status_line, FONT_SIZE_CHROME,
		                 DASDEC_CHROME);
	}

	if (line1[0]) {
		int x = center_x(line1, FONT_SIZE_CHROME, scr_l, scr_r);
		GRRLIB_PrintfTTF(x, chrome_bot_y, s_font, line1, FONT_SIZE_CHROME,
		                 DASDEC_CHROME);
	}

	if (show_details)
		draw_details_panel();
}
