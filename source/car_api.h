#ifndef WIIEAS_CAR_API_H
#define WIIEAS_CAR_API_H

#include <gctypes.h>
#include <stddef.h>

#define CAR_MAX_ALERTS      64
#define CAR_HASH_LEN        40
#define CAR_TYPE_LEN        8
#define CAR_SEV_LEN         16
#define CAR_ORIG_LEN        8
#define CAR_CALL_LEN        16
#define CAR_TRANS_LEN       1600
#define CAR_AUDIO_URL_LEN   256

typedef struct {
	int  id;
	char hash[CAR_HASH_LEN + 1];
	char type[CAR_TYPE_LEN];
	char severity[CAR_SEV_LEN];
	char originator[CAR_ORIG_LEN];
	char callsign[CAR_CALL_LEN];
	s64  start_epoch;
	s64  end_epoch;
	char translation[CAR_TRANS_LEN];
	char audio_url[CAR_AUDIO_URL_LEN];
} CarAlert;

typedef struct {
	CarAlert alerts[CAR_MAX_ALERTS];
	int      count;
} CarAlertList;

/* Fetch https://alerts.globaleas.org/api/v1/alerts/active into out.
 * Returns 0 on success, -1 on failure (err filled if provided). */
int car_fetch_active(CarAlertList *out, char *err, size_t err_len);

/* Download an alert's MP3 (audio_url) into buf. Returns bytes written, or -1. */
int car_fetch_audio(const CarAlert *alert, void *buf, size_t cap, size_t *out_len,
                    char *err, size_t err_len);

#endif
