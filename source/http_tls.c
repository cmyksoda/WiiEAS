/**
 * Minimal HTTPS GET for the Wii: libogc net_* + mbedTLS.
 * VERIFY_NONE — no CA bundle ships on the Wii.
 */
#include "http_tls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#include <network.h>
#include <ogc/if_config.h>
#include <ogcsys.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/filio.h>

#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/x509_crt.h>

static void set_err(char *err, size_t err_len, const char *msg)
{
	if (!err || err_len == 0)
		return;
	snprintf(err, err_len, "%s", msg);
}

/* The Wii clock is console-local with no zone info, but alert epochs are UTC.
 * Record (Date: header - time(NULL)) each fetch to reconstruct real UTC. */
static s64 s_utc_offset;
static int s_utc_offset_known;

s64 http_utc_offset(void)
{
	return s_utc_offset_known ? s_utc_offset : 0;
}

/* Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm). */
static s64 days_from_civil(int y, int m, int d)
{
	y -= m <= 2;
	int era = (y >= 0 ? y : y - 399) / 400;
	unsigned yoe = (unsigned)(y - era * 400);
	unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
	unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return (s64)era * 146097 + (s64)doe - 719468;
}

/* Scan response headers for "Date: Sun, 09 Aug 2026 20:15:33 GMT" (RFC 7231
 * fixdate — the only form modern servers emit) and record the UTC offset. */
static void note_date_header(const char *hdrs, const char *hdr_end)
{
	static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
	const char *p = hdrs;

	while (p < hdr_end) {
		const char *eol = memchr(p, '\n', (size_t)(hdr_end - p));
		if ((size_t)(hdr_end - p) > 5 && strncasecmp(p, "Date:", 5) == 0) {
			int day, year, hh, mm, ss;
			char mon[4];
			if (sscanf(p + 5, " %*[A-Za-z], %d %3s %d %d:%d:%d",
			           &day, mon, &year, &hh, &mm, &ss) == 6 &&
			    year >= 2020 && year <= 2100) {
				const char *m = strstr(months, mon);
				if (m && ((m - months) % 3) == 0) {
					int month = (int)(m - months) / 3 + 1;
					s64 epoch = days_from_civil(year, month, day) * 86400
					          + (s64)hh * 3600 + (s64)mm * 60 + ss;
					s_utc_offset = epoch - (s64)time(NULL);
					s_utc_offset_known = 1;
				}
			}
			return;
		}
		if (!eol)
			break;
		p = eol + 1;
	}
}

static int wii_tls_send(void *ctx, const unsigned char *buf, size_t len)
{
	s32 fd = *(s32 *)ctx;
	int ret = net_write(fd, (void *)buf, (u32)len);
	if (ret < 0) {
		if (ret == -EAGAIN || ret == -EWOULDBLOCK) {
			fd_set wfds;
			FD_ZERO(&wfds);
			FD_SET(fd, &wfds);
			struct timeval tv = { 0, 5000 };
			net_select(fd + 1, NULL, &wfds, NULL, &tv);
			return MBEDTLS_ERR_SSL_WANT_WRITE;
		}
		return MBEDTLS_ERR_NET_SEND_FAILED;
	}
	return ret;
}

static int wii_tls_recv(void *ctx, unsigned char *buf, size_t len)
{
	s32 fd = *(s32 *)ctx;
	{
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(fd, &rfds);
		struct timeval stv = { 20, 0 };
		if (net_select(fd + 1, &rfds, NULL, NULL, &stv) <= 0)
			return MBEDTLS_ERR_NET_RECV_FAILED;
	}
	int ret = net_read(fd, buf, (u32)len);
	if (ret < 0) {
		if (ret == -EAGAIN || ret == -EWOULDBLOCK)
			return MBEDTLS_ERR_SSL_WANT_READ;
		return MBEDTLS_ERR_NET_RECV_FAILED;
	}
	return ret;
}

int net_bringup(char *ip_out, size_t ip_out_len)
{
	char ip[16], mask[16], gw[16];
	s32 ret = if_config(ip, mask, gw, true, 20);
	if (ret < 0)
		return -1;
	if (ip_out && ip_out_len)
		snprintf(ip_out, ip_out_len, "%s", ip);
	return 0;
}

static int resolve_host(const char *host, struct sockaddr_in *addr, char *err, size_t err_len)
{
	memset(addr, 0, sizeof(*addr));
	addr->sin_family = AF_INET;
	if (inet_aton(host, &addr->sin_addr) != 0)
		return 0;

	struct hostent *he = net_gethostbyname((char *)host);
	if (!he) {
		set_err(err, err_len, "DNS failed");
		return -1;
	}
	if (he->h_length > (int)sizeof(addr->sin_addr)) {
		set_err(err, err_len, "DNS: bad address length");
		return -1;
	}
	memcpy(&addr->sin_addr, he->h_addr, (size_t)he->h_length);
	return 0;
}

int https_get(const char *host, int port, const char *path,
              void *body_buf, size_t body_cap, size_t *body_len,
              char *err, size_t err_len)
{
	if (body_len)
		*body_len = 0;
	if (!host || !path || !body_buf || body_cap < 1)
		return -1;

	struct sockaddr_in addr;
	if (resolve_host(host, &addr, err, err_len) < 0)
		return -1;
	addr.sin_port = htons((u16)port);

	s32 sock = net_socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		set_err(err, err_len, "socket() failed");
		return -1;
	}

	{
		int cr = net_connect(sock, (struct sockaddr *)&addr, sizeof(addr));
		if (cr < 0) {
			if (cr == -EINPROGRESS || cr == -EAGAIN) {
				fd_set wfds;
				FD_ZERO(&wfds);
				FD_SET(sock, &wfds);
				struct timeval tv = { 15, 0 };
				if (net_select(sock + 1, NULL, &wfds, NULL, &tv) <= 0) {
					set_err(err, err_len, "connect() timed out");
					net_close(sock);
					return -1;
				}
			} else {
				set_err(err, err_len, "connect() failed");
				net_close(sock);
				return -1;
			}
		}
	}
	/* libogc leaves the socket non-blocking after connect — restore blocking. */
	{
		u32 nb = 0;
		net_ioctl(sock, FIONBIO, &nb);
	}

	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context ctr_drbg;
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;

	mbedtls_entropy_init(&entropy);
	mbedtls_ctr_drbg_init(&ctr_drbg);
	mbedtls_ssl_init(&ssl);
	mbedtls_ssl_config_init(&conf);

	int http_status = -1;
	int ret = 0;
	int failed = 0;

	do {
		const char *pers = "wiieas_car";
		if ((ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
		                                 (const unsigned char *)pers, strlen(pers))) != 0) {
			set_err(err, err_len, "TLS: DRBG seed failed");
			failed = 1;
			break;
		}
		if ((ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
		                                       MBEDTLS_SSL_TRANSPORT_STREAM,
		                                       MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
			set_err(err, err_len, "TLS: config_defaults failed");
			failed = 1;
			break;
		}
		/* No CA bundle on Wii — VERIFY_NONE. */
		mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
		mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);

		if ((ret = mbedtls_ssl_setup(&ssl, &conf)) != 0) {
			set_err(err, err_len, "TLS: ssl_setup failed");
			failed = 1;
			break;
		}
		mbedtls_ssl_set_hostname(&ssl, host);
		mbedtls_ssl_set_bio(&ssl, &sock, wii_tls_send, wii_tls_recv, NULL);

		do {
			ret = mbedtls_ssl_handshake(&ssl);
		} while (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);

		if (ret != 0) {
			char ebuf[96];
			mbedtls_strerror(ret, ebuf, sizeof(ebuf));
			if (err && err_len)
				snprintf(err, err_len, "TLS handshake: %s", ebuf);
			failed = 1;
			break;
		}

		char host_hdr[256];
		if (port == 443)
			snprintf(host_hdr, sizeof(host_hdr), "%s", host);
		else
			snprintf(host_hdr, sizeof(host_hdr), "%s:%d", host, port);

		char req[512];
		int req_len = snprintf(req, sizeof(req),
			"GET %s HTTP/1.0\r\n"
			"Host: %s\r\n"
			"User-Agent: WiiEAS/0.1 (Nintendo Wii; homebrew)\r\n"
			"Accept: */*\r\n"
			"Connection: close\r\n"
			"\r\n",
			path[0] ? path : "/", host_hdr);
		if (req_len <= 0 || req_len >= (int)sizeof(req)) {
			set_err(err, err_len, "request too large");
			failed = 1;
			break;
		}

		const unsigned char *ptr = (const unsigned char *)req;
		int remaining = req_len;
		while (remaining > 0) {
			ret = mbedtls_ssl_write(&ssl, ptr, (size_t)remaining);
			if (ret == MBEDTLS_ERR_SSL_WANT_WRITE)
				continue;
			if (ret < 0) {
				set_err(err, err_len, "TLS write failed");
				failed = 1;
				break;
			}
			ptr += ret;
			remaining -= ret;
		}
		if (failed)
			break;

		/* Read full response into a staging buffer, then split headers/body.
		 * Staging is heap-allocated so large MP3s don't blow the stack. */
		size_t raw_cap = body_cap + 4096;
		if (raw_cap < 8192)
			raw_cap = 8192;
		char *raw = (char *)malloc(raw_cap);
		if (!raw) {
			set_err(err, err_len, "out of memory");
			failed = 1;
			break;
		}
		size_t raw_len = 0;
		unsigned char chunk[2048];
		while (1) {
			ret = mbedtls_ssl_read(&ssl, chunk, sizeof(chunk));
			if (ret == MBEDTLS_ERR_SSL_WANT_READ)
				continue;
			if (ret <= 0)
				break;
			if (raw_len + (size_t)ret >= raw_cap) {
				/* Truncate rather than overflow. */
				size_t room = raw_cap - 1 - raw_len;
				if (room > 0) {
					memcpy(raw + raw_len, chunk, room);
					raw_len += room;
				}
				break;
			}
			memcpy(raw + raw_len, chunk, (size_t)ret);
			raw_len += (size_t)ret;
		}
		raw[raw_len] = '\0';
		mbedtls_ssl_close_notify(&ssl);

		if (raw_len >= 12 && strncmp(raw, "HTTP/", 5) == 0) {
			const char *sp = strchr(raw, ' ');
			if (sp)
				http_status = atoi(sp + 1);
		}

		const char *sep = strstr(raw, "\r\n\r\n");
		const char *body = NULL;
		size_t blen = 0;
		if (sep) {
			body = sep + 4;
			blen = raw_len - (size_t)(body - raw);
		} else {
			sep = strstr(raw, "\n\n");
			if (sep) {
				body = sep + 2;
				blen = raw_len - (size_t)(body - raw);
			}
		}

		if (sep)
			note_date_header(raw, sep);

		if (body && blen > 0) {
			size_t copy = blen;
			if (copy > body_cap - 1)
				copy = body_cap - 1;
			memcpy(body_buf, body, copy);
			((char *)body_buf)[copy] = '\0';
			if (body_len)
				*body_len = copy;
		} else {
			((char *)body_buf)[0] = '\0';
			if (body_len)
				*body_len = 0;
		}

		free(raw);
	} while (0);

	mbedtls_ssl_free(&ssl);
	mbedtls_ssl_config_free(&conf);
	mbedtls_ctr_drbg_free(&ctr_drbg);
	mbedtls_entropy_free(&entropy);
	net_close(sock);

	if (failed)
		return -1;
	return http_status;
}

int https_get_url(const char *url,
                  void *body_buf, size_t body_cap, size_t *body_len,
                  char *err, size_t err_len)
{
	if (!url || strncmp(url, "https://", 8) != 0) {
		set_err(err, err_len, "only https:// supported");
		return -1;
	}
	const char *p = url + 8;
	char host[256];
	int port = 443;
	const char *slash = strchr(p, '/');
	const char *colon = NULL;
	size_t host_len;

	if (slash)
		host_len = (size_t)(slash - p);
	else
		host_len = strlen(p);

	for (size_t i = 0; i < host_len; i++) {
		if (p[i] == ':') {
			colon = p + i;
			break;
		}
	}
	if (colon) {
		size_t hlen = (size_t)(colon - p);
		if (hlen >= sizeof(host))
			hlen = sizeof(host) - 1;
		memcpy(host, p, hlen);
		host[hlen] = '\0';
		port = atoi(colon + 1);
	} else {
		if (host_len >= sizeof(host))
			host_len = sizeof(host) - 1;
		memcpy(host, p, host_len);
		host[host_len] = '\0';
	}

	const char *path = slash ? slash : "/";
	return https_get(host, port, path, body_buf, body_cap, body_len, err, err_len);
}
