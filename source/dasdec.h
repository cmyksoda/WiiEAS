#ifndef WIIEAS_DASDEC_H
#define WIIEAS_DASDEC_H

#include <gctypes.h>
#include "car_api.h"

/*
 * Authenticated DASDEC CG look (from real encoder screenshots):
 *   black outside the red frame, deep blue-violet inside, white mono text.
 */
#define DASDEC_OUTSIDE  0x000000FF  /* black beyond the red border */
#define DASDEC_BG       0x272945FF  /* ~RGB(39,41,69) interior field */
#define DASDEC_BORDER   0xE01010FF  /* solid red outline */
#define DASDEC_TEXT     0xFFFFFFFF
#define DASDEC_DIM      0xB0B0B0FF
/* Chrome on the black bezel: same family as the box purple, a touch lighter so it reads. */
#define DASDEC_CHROME   0x6A6C9AFF

int  dasdec_init(void);
void dasdec_shutdown(void);

/*
 * Seconds to add to time(NULL) to get real UTC (from http_utc_offset()).
 * Alert epochs are UTC but the Wii clock is local time; without this the
 * details panel's relative times are shifted by the local UTC offset.
 */
void dasdec_set_utc_offset(s64 seconds);

/* Rebuild page layout for the given alert text. Call when alert changes.
 * A NULL alert means we have genuinely fetched and there is nothing active. */
void dasdec_set_alert(const CarAlert *alert, int index, int total);

/*
 * Put a single centred line in the box instead of an alert — for states where
 * we don't yet know what's out there ("LOADING..."), which must not be
 * mistaken for a confirmed empty list.
 */
void dasdec_set_message(const char *msg);

void dasdec_next_page(void);
void dasdec_prev_page(void);
/* Call once per rendered frame. Auto-advances multi-page alerts (~5 s/page). */
void dasdec_auto_page_tick(void);

int  dasdec_page(void);       /* 0-based */
int  dasdec_page_count(void);

/*
 * status_line: top bezel line — empty when there's nothing to report.
 * auto_play: adds AUTO to the bottom bezel line alongside the alert index.
 * show_details: non-zero draws the info panel (sent / expires / meta).
 */
void dasdec_draw(const char *status_line, int auto_play, int show_details);

#endif
