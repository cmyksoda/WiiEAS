#ifndef WIIEAS_AUDIO_PLAYER_H
#define WIIEAS_AUDIO_PLAYER_H

#include <gctypes.h>
#include <stddef.h>
#include "car_api.h"

void audio_init(void);
void audio_shutdown(void);

/*
 * Hard stop: silences the voice AND releases the decoded clip. This is the
 * "leaving this alert" call — the next play of it starts from a fresh download.
 */
void audio_stop(void);

/*
 * Pause: silences the voice but keeps the decoded clip and the play position.
 * audio_play_alert() on the same alert then resumes instantly — no network
 * round trip, no decode — so a mis-pressed A costs nothing.
 */
void audio_pause(void);

/* True while ASND is playing. */
int audio_is_playing(void);

/*
 * True when this alert's audio is already decoded and sitting in memory, i.e.
 * playing it will be instant. Only one clip is cached at a time.
 */
int audio_alert_cached(const CarAlert *alert);

/*
 * Called between the long blocking phases of a cold play (after the download,
 * before the decode) so the UI can update its status line. May be NULL.
 */
typedef void (*audio_progress_fn)(const char *msg);

/*
 * Resume the cached clip if this is the alert we already have, otherwise
 * download + decode + play it (releasing any other alert's clip first).
 * Returns 0 on success, -1 on failure (err filled if provided).
 */
int audio_play_alert(const CarAlert *alert, audio_progress_fn progress,
                     char *err, size_t err_len);

#endif
