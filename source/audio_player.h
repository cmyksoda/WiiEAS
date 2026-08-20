#ifndef WIIEAS_AUDIO_PLAYER_H
#define WIIEAS_AUDIO_PLAYER_H

#include <gctypes.h>
#include <stddef.h>
#include "car_api.h"

void audio_init(void);
void audio_shutdown(void);

/* Hard stop: silences the voice AND releases the decoded clip — the next
 * play starts from a fresh download. */
void audio_stop(void);

/* Silence the voice but keep the decoded clip and position; replaying the
 * same alert resumes instantly with no network or decode. */
void audio_pause(void);

/* True while ASND is playing. */
int audio_is_playing(void);

/* True when this alert is already decoded in memory (playing is instant).
 * Only one clip is cached at a time. */
int audio_alert_cached(const CarAlert *alert);

/* Called between the blocking phases of a cold play so the UI can update its
 * status line. May be NULL. */
typedef void (*audio_progress_fn)(const char *msg);

/* Resume the cached clip if it matches, else download + decode + play.
 * Returns 0 on success, -1 on failure (err filled if provided). */
int audio_play_alert(const CarAlert *alert, audio_progress_fn progress,
                     char *err, size_t err_len);

#endif
