#ifndef WIIEAS_HTTP_TLS_H
#define WIIEAS_HTTP_TLS_H

#include <gctypes.h>
#include <stddef.h>

/* Bring up libogc networking (DHCP). Returns 0 on success. */
int net_bringup(char *ip_out, size_t ip_out_len);

/* HTTPS GET of host:port/path into body_buf (headers stripped, trailing NUL
 * appended). Returns HTTP status, or -1 on transport/TLS failure (err set). */
int https_get(const char *host, int port, const char *path,
              void *body_buf, size_t body_cap, size_t *body_len,
              char *err, size_t err_len);

/* Convenience: parse https://host[:port]/path and GET it. */
int https_get_url(const char *url,
                  void *body_buf, size_t body_cap, size_t *body_len,
                  char *err, size_t err_len);

/* Seconds to add to time(NULL) for real UTC, from the last response's Date:
 * header. 0 until a fetch has succeeded. */
s64 http_utc_offset(void);

#endif
