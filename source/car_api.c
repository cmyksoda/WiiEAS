/**
 * Central Alert Repository client.
 * API: https://alerts.globaleas.org/swagger/v1/swagger.json
 *   GET /api/v1/alerts/active  -> AlertEntity[]
 * Audio: https://vroom.gwes-cdn.net/<hash>.mp3
 */
#include "car_api.h"
#include "http_tls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* CAR's array is not guaranteed newest-first (the website re-sorts client-
 * side); present newest → oldest by start time, then id. */
static int alert_newer_first(const void *a, const void *b)
{
	const CarAlert *x = (const CarAlert *)a;
	const CarAlert *y = (const CarAlert *)b;
	if (x->start_epoch != y->start_epoch)
		return (x->start_epoch < y->start_epoch) ? 1 : -1; /* higher epoch first */
	if (x->id != y->id)
		return (x->id < y->id) ? 1 : -1;
	return 0;
}

static void sort_alerts_newest_first(CarAlertList *list)
{
	if (!list || list->count < 2)
		return;
	qsort(list->alerts, (size_t)list->count, sizeof(CarAlert), alert_newer_first);
}

#define CAR_HOST "alerts.globaleas.org"
#define CAR_PATH "/api/v1/alerts/active"
#define CAR_JSON_CAP (256 * 1024)

/* ---- minimal JSON field helpers (no full parser needed for this schema) ---- */

static const char *skip_ws(const char *p)
{
	while (*p && isspace((unsigned char)*p))
		p++;
	return p;
}

/* Find "key" : value starting at or after p. Returns pointer to value start, or NULL. */
static const char *find_key(const char *p, const char *end, const char *key)
{
	size_t klen = strlen(key);
	while (p < end) {
		const char *q = (const char *)memchr(p, '"', (size_t)(end - p));
		if (!q)
			return NULL;
		if ((size_t)(end - q) > klen + 2 &&
		    q[1 + klen] == '"' &&
		    memcmp(q + 1, key, klen) == 0) {
			const char *v = skip_ws(q + 1 + klen + 1);
			if (*v == ':')
				return skip_ws(v + 1);
		}
		p = q + 1;
	}
	return NULL;
}

/* Decode a JSON string at *pp (must start with "). Writes into out (NUL-term).
 * Advances *pp past the closing quote. Returns 0 on success. */
static int decode_json_string(const char **pp, const char *end, char *out, size_t out_cap)
{
	const char *p = *pp;
	if (p >= end || *p != '"')
		return -1;
	p++;
	size_t o = 0;
	while (p < end && *p != '"') {
		unsigned char c = (unsigned char)*p++;
		if (c == '\\' && p < end) {
			unsigned char e = (unsigned char)*p++;
			switch (e) {
			case '"':  c = '"'; break;
			case '\\': c = '\\'; break;
			case '/':  c = '/'; break;
			case 'b':  c = '\b'; break;
			case 'f':  c = '\f'; break;
			case 'n':  c = '\n'; break;
			case 'r':  c = '\r'; break;
			case 't':  c = '\t'; break;
			case 'u': {
				/* \uXXXX — keep ASCII range, map common punctuation, else '?'. */
				if (p + 4 > end)
					return -1;
				unsigned cp = 0;
				for (int i = 0; i < 4; i++) {
					char h = *p++;
					cp <<= 4;
					if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
					else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
					else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
					else return -1;
				}
				if (cp == 0x00A0) c = ' ';
				else if (cp == 0x2013 || cp == 0x2014) c = '-';
				else if (cp == 0x2018 || cp == 0x2019) c = '\'';
				else if (cp == 0x201C || cp == 0x201D) c = '"';
				else if (cp < 0x80) c = (unsigned char)cp;
				else if (cp == 0x2026) {
					/* ellipsis as three dots if room */
					if (o + 3 < out_cap) {
						out[o++] = '.'; out[o++] = '.'; out[o++] = '.';
					}
					continue;
				} else
					c = '?';
				break;
			}
			default:
				c = e;
				break;
			}
		}
		if (o + 1 < out_cap)
			out[o++] = (char)c;
	}
	if (p >= end || *p != '"')
		return -1;
	p++;
	out[o] = '\0';
	*pp = p;
	return 0;
}

static int parse_json_int(const char **pp, const char *end, s64 *out)
{
	const char *p = skip_ws(*pp);
	if (p >= end)
		return -1;
	int neg = 0;
	if (*p == '-') {
		neg = 1;
		p++;
	}
	if (p >= end || !isdigit((unsigned char)*p))
		return -1;
	s64 v = 0;
	while (p < end && isdigit((unsigned char)*p)) {
		v = v * 10 + (*p - '0');
		p++;
	}
	*out = neg ? -v : v;
	*pp = p;
	return 0;
}

static void trim_callsign(char *s)
{
	/* API pads callsign with trailing spaces. */
	size_t n = strlen(s);
	while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t'))
		s[--n] = '\0';
}

/* Parse one AlertEntity object between { and }. */
static int parse_alert_object(const char *obj, const char *obj_end, CarAlert *a)
{
	memset(a, 0, sizeof(*a));

	const char *v;
	const char *p;

	v = find_key(obj, obj_end, "id");
	if (v) {
		s64 id = 0;
		p = v;
		if (parse_json_int(&p, obj_end, &id) == 0)
			a->id = (int)id;
	}

	v = find_key(obj, obj_end, "hash");
	if (v) {
		p = v;
		decode_json_string(&p, obj_end, a->hash, sizeof(a->hash));
	}

	v = find_key(obj, obj_end, "type");
	if (v) {
		p = v;
		decode_json_string(&p, obj_end, a->type, sizeof(a->type));
	}

	v = find_key(obj, obj_end, "severity");
	if (v) {
		p = v;
		decode_json_string(&p, obj_end, a->severity, sizeof(a->severity));
	}

	v = find_key(obj, obj_end, "originator");
	if (v) {
		p = v;
		decode_json_string(&p, obj_end, a->originator, sizeof(a->originator));
	}

	v = find_key(obj, obj_end, "callsign");
	if (v) {
		p = v;
		decode_json_string(&p, obj_end, a->callsign, sizeof(a->callsign));
		trim_callsign(a->callsign);
	}

	v = find_key(obj, obj_end, "startTimeEpoch");
	if (v) {
		p = v;
		parse_json_int(&p, obj_end, &a->start_epoch);
	}

	v = find_key(obj, obj_end, "endTimeEpoch");
	if (v) {
		p = v;
		parse_json_int(&p, obj_end, &a->end_epoch);
	}

	v = find_key(obj, obj_end, "translation");
	if (v) {
		p = v;
		decode_json_string(&p, obj_end, a->translation, sizeof(a->translation));
	}

	v = find_key(obj, obj_end, "audioUrl");
	if (v) {
		p = skip_ws(v);
		if (p < obj_end && *p == 'n') {
			/* null */
			a->audio_url[0] = '\0';
		} else {
			decode_json_string(&p, obj_end, a->audio_url, sizeof(a->audio_url));
		}
	}

	/* Require at least a hash or translation to count as a real alert. */
	return (a->hash[0] || a->translation[0]) ? 0 : -1;
}

static int parse_alert_array(const char *json, size_t json_len, CarAlertList *out)
{
	out->count = 0;
	const char *end = json + json_len;
	const char *p = skip_ws(json);
	if (p >= end || *p != '[')
		return -1;
	p++;

	while (p < end && out->count < CAR_MAX_ALERTS) {
		p = skip_ws(p);
		if (p >= end)
			break;
		if (*p == ']')
			break;
		if (*p == ',') {
			p++;
			continue;
		}
		if (*p != '{') {
			p++;
			continue;
		}
		/* Find matching close brace (strings may contain braces — track quotes). */
		const char *start = p;
		int depth = 0;
		int in_str = 0;
		int esc = 0;
		const char *q = p;
		for (; q < end; q++) {
			char c = *q;
			if (in_str) {
				if (esc)
					esc = 0;
				else if (c == '\\')
					esc = 1;
				else if (c == '"')
					in_str = 0;
				continue;
			}
			if (c == '"') {
				in_str = 1;
				continue;
			}
			if (c == '{')
				depth++;
			else if (c == '}') {
				depth--;
				if (depth == 0) {
					q++;
					break;
				}
			}
		}
		if (depth != 0)
			break;

		CarAlert a;
		if (parse_alert_object(start, q, &a) == 0)
			out->alerts[out->count++] = a;
		p = q;
	}
	return 0;
}

int car_fetch_active(CarAlertList *out, char *err, size_t err_len)
{
	if (!out)
		return -1;
	out->count = 0;

	char *json = (char *)malloc(CAR_JSON_CAP);
	if (!json) {
		if (err && err_len)
			snprintf(err, err_len, "out of memory");
		return -1;
	}

	size_t jlen = 0;
	int status = https_get(CAR_HOST, 443, CAR_PATH, json, CAR_JSON_CAP, &jlen, err, err_len);
	if (status < 0) {
		free(json);
		return -1;
	}
	if (status != 200) {
		if (err && err_len)
			snprintf(err, err_len, "HTTP %d from CAR", status);
		free(json);
		return -1;
	}

	if (parse_alert_array(json, jlen, out) != 0) {
		if (err && err_len)
			snprintf(err, err_len, "failed to parse alert JSON");
		free(json);
		return -1;
	}

	sort_alerts_newest_first(out);

	free(json);
	return 0;
}

int car_fetch_audio(const CarAlert *alert, void *buf, size_t cap, size_t *out_len,
                    char *err, size_t err_len)
{
	if (out_len)
		*out_len = 0;
	if (!alert || !buf || cap < 64)
		return -1;
	if (!alert->audio_url[0]) {
		if (err && err_len)
			snprintf(err, err_len, "no audio URL");
		return -1;
	}

	size_t blen = 0;
	int status = https_get_url(alert->audio_url, buf, cap, &blen, err, err_len);
	if (status < 0)
		return -1;
	if (status != 200) {
		if (err && err_len)
			snprintf(err, err_len, "audio HTTP %d", status);
		return -1;
	}
	if (out_len)
		*out_len = blen;
	return 0;
}
