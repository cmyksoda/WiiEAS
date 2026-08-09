#ifndef WIIEAS_HTTP_TLS_H
#define WIIEAS_HTTP_TLS_H

#include <gctypes.h>
#include <stddef.h>

/* Bring up libogc networking (DHCP). Returns 0 on success. */
int net_bringup(char *ip_out, size_t ip_out_len);

/*
 * HTTPS GET of host:port/path into body_buf.
 * body_cap is capacity of body_buf; *body_len receives body byte count.
 * On success returns HTTP status (e.g. 200). On transport/TLS failure returns -1
 * and writes a short message into err (if non-NULL).
 *
 * body is raw response body (headers stripped). Not null-terminated unless
 * the body itself is text and you leave room; we always append a trailing NUL
 * if body_cap > *body_len.
 */
int https_get(const char *host, int port, const char *path,
              void *body_buf, size_t body_cap, size_t *body_len,
              char *err, size_t err_len);

/* Convenience: parse https://host[:port]/path and GET it. */
int https_get_url(const char *url,
                  void *body_buf, size_t body_cap, size_t *body_len,
                  char *err, size_t err_len);

/*
 * Seconds to ADD to time(NULL) to get real UTC, learned from the Date: header
 * of the most recent HTTPS response (the Wii clock is local time with no zone
 * info). 0 until a fetch has succeeded.
 */
s64 http_utc_offset(void);

#endif
