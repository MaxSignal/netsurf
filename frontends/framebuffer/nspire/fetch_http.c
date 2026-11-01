/*
 * Copyright 2026 The NetSurf Browser Project
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * NetSurf is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * \file
 * HTTP and HTTPS fetcher for the TI-Nspire.
 *
 * There is no libcurl and no socket layer under Ndless; this talks HTTP/1.1
 * itself over the USB network's non-blocking TCP (net/nnet.h), with TLS
 * from mbedTLS (tls.c) for https. Everything is polled from the fetch
 * scheduler.
 *
 *  - name lookups through the computer's DNS (or the calculator's own
 *    "pc" for the computer itself)
 *  - persistent connections, reused per scheme/host/port, and TLS session
 *    resumption: a TLS handshake costs the calculator seconds
 *  - chunked transfer coding, gzip/deflate content coding (zlib)
 *  - cookies, basic authentication, redirects, 304, POST (urlencoded and
 *    multipart), certificate errors reported with the server's chain
 */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <zlib.h>
#include <mbedtls/base64.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include "utils/errors.h"
#include "utils/log.h"
#include "utils/messages.h"
#include "utils/nsoption.h"
#include "utils/nsurl.h"
#include "utils/useragent.h"
#include "utils/ring.h"
#include "netsurf/ssl_certs.h"
#include "content/fetch.h"
#include "content/fetchers.h"
#include "content/urldb.h"

#include "framebuffer/nspire/nspire.h"
#include "framebuffer/nspire/sys.h"
#include "framebuffer/nspire/net/nnet.h"

#define CONNECT_TIMEOUT_MS 30000
#define IDLE_TIMEOUT_MS 60000
#define POOL_IDLE_MS 30000
#define POOL_MAX 6
#define LINE_MAX_LEN 16384
#define RECV_CHUNK 4096
#define SESSION_CACHE 8

enum conn_state {
	CONN_RESOLVING,
	CONN_CONNECTING,
	CONN_HANDSHAKE,
	CONN_OPEN,
	CONN_FAILED,
};

struct conn {
	struct conn *next;
	char *host;
	uint16_t port;
	bool tls;
	bool verify;
	enum conn_state state;
	uint32_t ip;
	uint32_t since;
	nnet_tcp *tcp;
	mbedtls_ssl_context *ssl;
	int tls_error;		/* mbedTLS error of a failed handshake */
	uint32_t verify_flags;
	bool reused;
	/* decrypted bytes read but not consumed */
	uint8_t buf[RECV_CHUNK];
	size_t buf_len, buf_off;
};

enum phase {
	PH_CONNECT,
	PH_SEND,
	PH_STATUS,
	PH_HEADERS,
	PH_BODY,
	PH_DONE,
};

enum framing {
	BODY_NONE,
	BODY_LENGTH,
	BODY_CHUNKED,
	BODY_CLOSE,
};

enum chunk_state {
	CH_SIZE,
	CH_DATA,
	CH_DATA_END,
	CH_TRAILER,
};

struct http_fetch {
	struct http_fetch *r_next, *r_prev;	/* ring */

	struct fetch *parent;
	nsurl *url;
	char *host;
	uint16_t port;
	bool https;
	bool only_2xx;
	bool downgrade_tls;
	bool is_post;
	char **headers;

	bool started;
	bool aborted;
	bool locked;
	bool done;		/* a final message was sent */

	struct conn *conn;
	int attempts;
	enum phase phase;
	uint32_t activity;

	uint8_t *req;
	size_t req_len, req_off;

	char *line;
	size_t line_len;
	bool line_cr;

	long code;
	bool got_response_bytes;
	enum framing framing;
	unsigned long long remaining;
	enum chunk_state chunk;
	bool keepalive;
	bool http10;
	char *location;
	char *realm;
	bool have_length;
	unsigned long long content_length;
	unsigned long long received;

	int coding;		/* 0 identity, 1 gzip/deflate */
	bool inflating;
	z_stream zs;
};

static struct http_fetch *ring;
static struct conn *pool;		/* idle connections */
static lwc_string *scheme_http, *scheme_https;

static struct {
	char host[64];
	uint16_t port;
	mbedtls_ssl_session session;
	bool used;
	uint32_t when;
} sessions[SESSION_CACHE];

/* ------------------------------------------------------------------------ */
/* connections */

static void conn_free(struct conn *c)
{
	if (c == NULL)
		return;
	if (c->ssl != NULL) {
		mbedtls_ssl_free(c->ssl);
		free(c->ssl);
	}
	if (c->tcp != NULL)
		nnet_tcp_close(c->tcp);
	free(c->host);
	free(c);
}

static int session_slot(const char *host, uint16_t port, bool create)
{
	int i, oldest = 0;

	for (i = 0; i < SESSION_CACHE; i++) {
		if (sessions[i].used && sessions[i].port == port &&
		    strcmp(sessions[i].host, host) == 0)
			return i;
	}
	if (!create || strlen(host) >= sizeof(sessions[0].host))
		return -1;
	for (i = 0; i < SESSION_CACHE; i++) {
		if (!sessions[i].used)
			return i;
		if ((int32_t)(sessions[i].when - sessions[oldest].when) < 0)
			oldest = i;
	}
	return oldest;
}

static void session_save(struct conn *c)
{
	int i = session_slot(c->host, c->port, true);

	if (i < 0)
		return;
	if (sessions[i].used)
		mbedtls_ssl_session_free(&sessions[i].session);
	mbedtls_ssl_session_init(&sessions[i].session);
	if (mbedtls_ssl_get_session(c->ssl, &sessions[i].session) != 0) {
		mbedtls_ssl_session_free(&sessions[i].session);
		sessions[i].used = false;
		return;
	}
	strcpy(sessions[i].host, c->host);
	sessions[i].port = c->port;
	sessions[i].used = true;
	sessions[i].when = nspire_ms();
}

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
	struct conn *c = ctx;
	int n = nnet_tcp_write(c->tcp, buf, len > 0x7FFF ? 0x7FFF : (int)len);

	if (n < 0)
		return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
	if (n == 0)
		return MBEDTLS_ERR_SSL_WANT_WRITE;
	return n;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
	struct conn *c = ctx;
	int n = nnet_tcp_read(c->tcp, buf, len > 0x7FFF ? 0x7FFF : (int)len);

	if (n < 0)
		return 0;	/* closed */
	if (n == 0)
		return MBEDTLS_ERR_SSL_WANT_READ;
	return n;
}

static struct conn *conn_new(const char *host, uint16_t port, bool tls, bool verify)
{
	struct conn *c = calloc(1, sizeof(*c));

	if (c == NULL)
		return NULL;
	c->host = strdup(host);
	if (c->host == NULL) {
		free(c);
		return NULL;
	}
	c->port = port;
	c->tls = tls;
	c->verify = verify;
	c->state = CONN_RESOLVING;
	c->since = nspire_ms();
	return c;
}

static bool conn_start_tls(struct conn *c)
{
	const mbedtls_ssl_config *conf = nspire_tls_config(c->verify);
	int i;

	if (conf == NULL)
		return false;
	c->ssl = malloc(sizeof(*c->ssl));
	if (c->ssl == NULL)
		return false;
	mbedtls_ssl_init(c->ssl);
	if (mbedtls_ssl_setup(c->ssl, conf) != 0 ||
	    mbedtls_ssl_set_hostname(c->ssl, c->host) != 0)
		return false;
	mbedtls_ssl_set_bio(c->ssl, c, bio_send, bio_recv, NULL);
	i = session_slot(c->host, c->port, false);
	if (i >= 0)
		mbedtls_ssl_set_session(c->ssl, &sessions[i].session);
	return true;
}

/* Move a connection along; false when it has failed. */
static bool conn_step(struct conn *c)
{
	int r;
	uint32_t now = nspire_ms();

	switch (c->state) {
	case CONN_RESOLVING:
		r = nnet_resolve(c->host, &c->ip);
		if (r < 0) {
			nspire_log("http: cannot resolve %s\n", c->host);
			c->state = CONN_FAILED;
			return false;
		}
		if (r == 0) {
			if ((uint32_t)(now - c->since) > CONNECT_TIMEOUT_MS) {
				c->state = CONN_FAILED;
				return false;
			}
			return true;
		}
		c->tcp = nnet_tcp_connect(c->ip, c->port);
		if (c->tcp == NULL) {
			c->state = CONN_FAILED;
			return false;
		}
		c->state = CONN_CONNECTING;
		c->since = now;
		/* fall through */
	case CONN_CONNECTING:
		r = nnet_tcp_state(c->tcp);
		if (r == NNET_TCP_CLOSED) {
			c->state = CONN_FAILED;
			return false;
		}
		if (r == NNET_TCP_CONNECTING) {
			if ((uint32_t)(now - c->since) > CONNECT_TIMEOUT_MS) {
				c->state = CONN_FAILED;
				return false;
			}
			return true;
		}
		if (!c->tls) {
			c->state = CONN_OPEN;
			return true;
		}
		if (!conn_start_tls(c)) {
			c->tls_error = -1;
			c->state = CONN_FAILED;
			return false;
		}
		c->state = CONN_HANDSHAKE;
		c->since = now;
		/* fall through */
	case CONN_HANDSHAKE:
		r = mbedtls_ssl_handshake(c->ssl);
		if (r == 0) {
			c->state = CONN_OPEN;
			nspire_log("tls: %s: %s %s in %lu ms\n", c->host,
				   mbedtls_ssl_get_version(c->ssl),
				   mbedtls_ssl_get_ciphersuite(c->ssl),
				   (unsigned long)(nspire_ms() - c->since));
			session_save(c);
			return true;
		}
		if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE ||
		    r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
			if ((uint32_t)(now - c->since) > CONNECT_TIMEOUT_MS * 2) {
				c->state = CONN_FAILED;
				return false;
			}
			return true;
		}
		c->tls_error = r;
		c->verify_flags = mbedtls_ssl_get_verify_result(c->ssl);
		nspire_log("tls: %s: handshake failed -0x%04x, verify flags %08lx\n",
			   c->host, (unsigned)-r, (unsigned long)c->verify_flags);
		c->state = CONN_FAILED;
		return false;
	case CONN_OPEN:
		return true;
	case CONN_FAILED:
	default:
		return false;
	}
}

/* Bytes from the connection: >0 read, 0 nothing yet, -1 closed. */
static int conn_read(struct conn *c, uint8_t *buf, size_t len)
{
	int n;

	if (c->buf_off < c->buf_len) {
		size_t avail = c->buf_len - c->buf_off;
		if (avail > len)
			avail = len;
		memcpy(buf, c->buf + c->buf_off, avail);
		c->buf_off += avail;
		return (int)avail;
	}
	if (!c->tls)
		return nnet_tcp_read(c->tcp, buf, (int)(len > 0x7FFF ? 0x7FFF : len));
	n = mbedtls_ssl_read(c->ssl, buf, len);
	if (n > 0)
		return n;
	if (n == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
		/* TLS 1.3: the ticket comes after the handshake */
		session_save(c);
		return 0;
	}
	if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
		return 0;
	return -1;	/* close notify, reset, or error */
}

/* Bytes to the connection: >=0 written, -1 closed. */
static int conn_write(struct conn *c, const uint8_t *buf, size_t len)
{
	int n;

	if (!c->tls)
		return nnet_tcp_write(c->tcp, buf, (int)(len > 0x7FFF ? 0x7FFF : len));
	n = mbedtls_ssl_write(c->ssl, buf, len > 4096 ? 4096 : len);
	if (n >= 0)
		return n;
	if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
		return 0;
	return -1;
}

static struct conn *pool_take(const char *host, uint16_t port, bool tls, bool verify)
{
	struct conn **pp, *c;

	for (pp = &pool; (c = *pp) != NULL; pp = &c->next) {
		if (c->port == port && c->tls == tls && c->verify == verify &&
		    strcasecmp(c->host, host) == 0) {
			*pp = c->next;
			c->next = NULL;
			if (nnet_tcp_state(c->tcp) != NNET_TCP_OPEN) {
				conn_free(c);
				return NULL;
			}
			c->reused = true;
			return c;
		}
	}
	return NULL;
}

static void pool_put(struct conn *c)
{
	struct conn *p;
	int n = 0;

	if (c->state != CONN_OPEN || nnet_tcp_state(c->tcp) != NNET_TCP_OPEN ||
	    c->buf_off < c->buf_len) {
		conn_free(c);
		return;
	}
	c->since = nspire_ms();
	c->next = pool;
	pool = c;
	/* keep the pool small */
	for (p = pool; p != NULL; p = p->next) {
		if (++n == POOL_MAX && p->next != NULL) {
			struct conn *rest = p->next;
			p->next = NULL;
			while (rest != NULL) {
				struct conn *next = rest->next;
				conn_free(rest);
				rest = next;
			}
			break;
		}
	}
}

static void pool_expire(void)
{
	struct conn **pp = &pool, *c;
	uint32_t now = nspire_ms();

	while ((c = *pp) != NULL) {
		/* a server closing an idle connection shows as data or EOF */
		if ((uint32_t)(now - c->since) > POOL_IDLE_MS ||
		    nnet_tcp_state(c->tcp) != NNET_TCP_OPEN ||
		    nnet_tcp_available(c->tcp) > 0) {
			*pp = c->next;
			conn_free(c);
		} else {
			pp = &c->next;
		}
	}
}

/* ------------------------------------------------------------------------ */
/* messages to the fetch machinery */

static void send_msg(struct http_fetch *f, fetch_msg *msg)
{
	f->locked = true;
	fetch_send_callback(msg, f->parent);
	f->locked = false;
}

static void send_final(struct http_fetch *f, fetch_msg_type type, const char *error)
{
	fetch_msg msg;

	if (f->done || f->aborted)
		return;
	NSLOG(netsurf, INFO, "http: %s -> %ld, message %d (%s), %llu bytes",
	      nsurl_access(f->url), f->code, (int)type, error ? error : "",
	      f->received);
	f->done = true;
	msg.type = type;
	msg.data.error = error;
	send_msg(f, &msg);
}

static void send_header(struct http_fetch *f, const char *line, size_t len)
{
	fetch_msg msg;

	msg.type = FETCH_HEADER;
	msg.data.header_or_data.buf = (const uint8_t *)line;
	msg.data.header_or_data.len = len;
	send_msg(f, &msg);
}

static void send_data(struct http_fetch *f, const uint8_t *data, size_t len)
{
	fetch_msg msg;

	if (len == 0 || f->aborted)
		return;
	msg.type = FETCH_DATA;
	msg.data.header_or_data.buf = data;
	msg.data.header_or_data.len = len;
	send_msg(f, &msg);
}

static ssl_cert_err cert_error(uint32_t flags, int depth)
{
	if (flags & MBEDTLS_X509_BADCERT_EXPIRED)
		return SSL_CERT_ERR_TOO_OLD;
	if (flags & MBEDTLS_X509_BADCERT_FUTURE)
		return SSL_CERT_ERR_TOO_YOUNG;
	if (flags & MBEDTLS_X509_BADCERT_CN_MISMATCH)
		return depth == 0 ? SSL_CERT_ERR_HOSTNAME_MISMATCH : SSL_CERT_ERR_OK;
	if (flags & MBEDTLS_X509_BADCERT_REVOKED)
		return SSL_CERT_ERR_REVOKED;
	if (flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED)
		return SSL_CERT_ERR_BAD_ISSUER;
	if (flags & (MBEDTLS_X509_BADCERT_BAD_MD | MBEDTLS_X509_BADCERT_BAD_PK |
		     MBEDTLS_X509_BADCERT_BAD_KEY))
		return SSL_CERT_ERR_BAD_SIG;
	if (flags != 0)
		return SSL_CERT_ERR_UNKNOWN;
	return SSL_CERT_ERR_OK;
}

/* The server's chain, for the certificate error page. */
static void send_certs(struct http_fetch *f, struct conn *c)
{
	struct cert_chain chain;
	const mbedtls_x509_crt *crt;
	fetch_msg msg;
	size_t depth = 0;

	if (c->ssl == NULL)
		return;
	crt = mbedtls_ssl_get_peer_cert(c->ssl);
	memset(&chain, 0, sizeof(chain));
	for (; crt != NULL && depth < MAX_CERT_DEPTH; crt = crt->next, depth++) {
		chain.certs[depth].der = crt->raw.p;
		chain.certs[depth].der_length = crt->raw.len;
		chain.certs[depth].err = SSL_CERT_ERR_OK;
	}
	if (depth == 0) {
		chain.certs[0].err = cert_error(c->verify_flags, 0);
		if (chain.certs[0].err == SSL_CERT_ERR_OK)
			chain.certs[0].err = SSL_CERT_ERR_UNKNOWN;
		chain.certs[0].der = NULL;
		chain.certs[0].der_length = 0;
		depth = 1;
	} else {
		chain.certs[0].err = cert_error(c->verify_flags, 0);
		if (c->verify_flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED)
			chain.certs[depth - 1].err = depth == 1 ?
				SSL_CERT_ERR_SELF_SIGNED : SSL_CERT_ERR_BAD_ISSUER;
	}
	chain.depth = depth;
	msg.type = FETCH_CERTS;
	msg.data.chain = &chain;
	send_msg(f, &msg);
}

/* ------------------------------------------------------------------------ */
/* the request */

struct buf {
	char *data;
	size_t len, size;
	bool failed;
};

static void buf_add(struct buf *b, const void *data, size_t len)
{
	if (b->failed)
		return;
	if (b->len + len + 1 > b->size) {
		size_t size = (b->size ? b->size * 2 : 1024);
		char *n;
		while (size < b->len + len + 1)
			size *= 2;
		n = realloc(b->data, size);
		if (n == NULL) {
			b->failed = true;
			return;
		}
		b->data = n;
		b->size = size;
	}
	memcpy(b->data + b->len, data, len);
	b->len += len;
	b->data[b->len] = '\0';
}

static void buf_str(struct buf *b, const char *s)
{
	buf_add(b, s, strlen(s));
}

static void buf_printf(struct buf *b, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
static void buf_printf(struct buf *b, const char *fmt, ...)
{
	char tmp[512];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if ((size_t)n < sizeof(tmp)) {
		buf_add(b, tmp, (size_t)n);
	} else {
		char *big = malloc((size_t)n + 1);
		if (big == NULL) {
			b->failed = true;
			return;
		}
		va_start(ap, fmt);
		vsnprintf(big, (size_t)n + 1, fmt, ap);
		va_end(ap);
		buf_add(b, big, (size_t)n);
		free(big);
	}
}

static bool has_header(char **headers, const char *name)
{
	size_t n = strlen(name);
	int i;

	for (i = 0; headers[i] != NULL; i++)
		if (strncasecmp(headers[i], name, n) == 0 && headers[i][n] == ':')
			return true;
	return false;
}

static void add_multipart(struct buf *body, const struct fetch_multipart_data *part,
			  const char *boundary)
{
	for (; part != NULL; part = part->next) {
		buf_printf(body, "--%s\r\n", boundary);
		if (part->file) {
			FILE *fp = part->rawfile != NULL ? fopen(part->rawfile, "rb") : NULL;
			const char *name = part->value != NULL ? part->value : "";
			buf_printf(body, "Content-Disposition: form-data; name=\"%s\"; "
				   "filename=\"%s\"\r\n"
				   "Content-Type: application/octet-stream\r\n\r\n",
				   part->name, name);
			if (fp != NULL) {
				char chunk[1024];
				size_t n;
				while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0)
					buf_add(body, chunk, n);
				fclose(fp);
			}
			buf_str(body, "\r\n");
		} else {
			buf_printf(body, "Content-Disposition: form-data; name=\"%s\"\r\n\r\n",
				   part->name);
			buf_str(body, part->value != NULL ? part->value : "");
			buf_str(body, "\r\n");
		}
	}
	buf_printf(body, "--%s--\r\n", boundary);
}

static bool build_request(struct http_fetch *f, const char *post_urlenc,
			  const struct fetch_multipart_data *post_multipart)
{
	struct buf req = { 0 }, body = { 0 };
	char *path = NULL;
	size_t path_len;
	char *cookie;
	const char *auth;
	char boundary[48];
	bool default_port = (f->https && f->port == 443) || (!f->https && f->port == 80);
	int i;

	if (nsurl_get(f->url, NSURL_PATH | NSURL_QUERY, &path, &path_len) != NSERROR_OK)
		return false;

	if (post_urlenc != NULL) {
		f->is_post = true;
		buf_str(&body, post_urlenc);
	} else if (post_multipart != NULL) {
		f->is_post = true;
		snprintf(boundary, sizeof(boundary), "----NetSurfNspire%08lx%08lx",
			 (unsigned long)nspire_entropy(), (unsigned long)nspire_ms());
		add_multipart(&body, post_multipart, boundary);
	}

	buf_printf(&req, "%s %s HTTP/1.1\r\n", f->is_post ? "POST" : "GET",
		   path_len ? path : "/");
	free(path);
	if (default_port)
		buf_printf(&req, "Host: %s\r\n", f->host);
	else
		buf_printf(&req, "Host: %s:%u\r\n", f->host, (unsigned)f->port);
	buf_printf(&req, "User-Agent: %s\r\n", user_agent_string());
	if (!has_header(f->headers, "Accept"))
		buf_str(&req, "Accept: */*\r\n");
	buf_str(&req, "Accept-Encoding: gzip, deflate\r\n");
	if (nsoption_charp(accept_language) != NULL && nsoption_charp(accept_language)[0])
		buf_printf(&req, "Accept-Language: %s, *;q=0.1\r\n",
			   nsoption_charp(accept_language));
	if (nsoption_charp(accept_charset) != NULL && nsoption_charp(accept_charset)[0])
		buf_printf(&req, "Accept-Charset: %s, *;q=0.1\r\n",
			   nsoption_charp(accept_charset));
	if (nsoption_bool(do_not_track))
		buf_str(&req, "DNT: 1\r\n");
	buf_str(&req, "Connection: keep-alive\r\n");

	cookie = urldb_get_cookie(f->url, true);
	if (cookie != NULL) {
		buf_printf(&req, "Cookie: %s\r\n", cookie);
		free(cookie);
	}

	auth = urldb_get_auth_details(f->url, NULL);
	if (auth != NULL) {
		size_t olen = 0;
		unsigned char *b64;
		size_t alen = strlen(auth);
		mbedtls_base64_encode(NULL, 0, &olen, (const unsigned char *)auth, alen);
		b64 = malloc(olen + 1);
		if (b64 != NULL &&
		    mbedtls_base64_encode(b64, olen + 1, &olen,
					  (const unsigned char *)auth, alen) == 0) {
			b64[olen] = '\0';
			buf_printf(&req, "Authorization: Basic %s\r\n", b64);
		}
		free(b64);
	}

	if (f->is_post) {
		if (post_multipart != NULL)
			buf_printf(&req, "Content-Type: multipart/form-data; boundary=%s\r\n",
				   boundary);
		else if (!has_header(f->headers, "Content-Type"))
			buf_str(&req, "Content-Type: application/x-www-form-urlencoded\r\n");
		buf_printf(&req, "Content-Length: %lu\r\n", (unsigned long)body.len);
	}

	for (i = 0; f->headers[i] != NULL; i++)
		buf_printf(&req, "%s\r\n", f->headers[i]);
	buf_str(&req, "\r\n");
	if (body.len)
		buf_add(&req, body.data, body.len);
	free(body.data);

	if (req.failed || body.failed) {
		free(req.data);
		return false;
	}
	f->req = (uint8_t *)req.data;
	f->req_len = req.len;
	return true;
}

/* ------------------------------------------------------------------------ */
/* fetcher operations */

static bool http_initialise(lwc_string *scheme)
{
	(void)scheme;
	return true;
}

static void http_finalise(lwc_string *scheme)
{
	int i;

	if (scheme != scheme_https)
		return;
	while (pool != NULL) {
		struct conn *c = pool;
		pool = c->next;
		conn_free(c);
	}
	for (i = 0; i < SESSION_CACHE; i++) {
		if (sessions[i].used)
			mbedtls_ssl_session_free(&sessions[i].session);
		sessions[i].used = false;
	}
	nspire_tls_finalise();
}

static bool http_acceptable(const nsurl *url)
{
	return nsurl_has_component(url, NSURL_HOST);
}

static void *http_setup(struct fetch *parent, nsurl *url, bool only_2xx,
			bool downgrade_tls, const char *post_urlenc,
			const struct fetch_multipart_data *post_multipart,
			const char **headers)
{
	struct http_fetch *f;
	lwc_string *scheme, *host, *port;
	int i, n;

	f = calloc(1, sizeof(*f));
	if (f == NULL)
		return NULL;
	f->parent = parent;
	f->url = nsurl_ref(url);
	f->only_2xx = only_2xx;
	f->downgrade_tls = downgrade_tls;

	scheme = nsurl_get_component(url, NSURL_SCHEME);
	f->https = scheme != NULL && lwc_string_length(scheme) == 5 &&
		strncasecmp(lwc_string_data(scheme), "https", 5) == 0;
	if (scheme != NULL)
		lwc_string_unref(scheme);

	host = nsurl_get_component(url, NSURL_HOST);
	if (host == NULL)
		goto failed;
	f->host = strndup(lwc_string_data(host), lwc_string_length(host));
	lwc_string_unref(host);
	if (f->host == NULL)
		goto failed;
	/* [v6 literal]: not reachable over the IPv4 link anyway */

	f->port = f->https ? 443 : 80;
	port = nsurl_get_component(url, NSURL_PORT);
	if (port != NULL) {
		long p = strtol(lwc_string_data(port), NULL, 10);
		if (p > 0 && p < 65536)
			f->port = (uint16_t)p;
		lwc_string_unref(port);
	}

	for (n = 0; headers[n] != NULL; n++)
		;
	f->headers = calloc((size_t)n + 1, sizeof(char *));
	if (f->headers == NULL)
		goto failed;
	for (i = 0; i < n; i++) {
		f->headers[i] = strdup(headers[i]);
		if (f->headers[i] == NULL)
			goto failed;
	}

	if (!build_request(f, post_urlenc, post_multipart))
		goto failed;

	f->line = malloc(LINE_MAX_LEN);
	if (f->line == NULL)
		goto failed;

	return f;

failed:
	if (f->headers != NULL) {
		for (i = 0; f->headers[i] != NULL; i++)
			free(f->headers[i]);
		free(f->headers);
	}
	free(f->req);
	free(f->host);
	nsurl_unref(f->url);
	free(f);
	return NULL;
}

static bool http_start(void *ctx)
{
	struct http_fetch *f = ctx;

	f->started = true;
	f->phase = PH_CONNECT;
	f->activity = nspire_ms();
	RING_INSERT(ring, f);
	return true;
}

static void http_abort(void *ctx)
{
	struct http_fetch *f = ctx;

	/* the poll loop cleans up */
	f->aborted = true;
}

static void http_free(void *ctx)
{
	struct http_fetch *f = ctx;
	int i;

	if (f->conn != NULL)
		conn_free(f->conn);
	if (f->inflating)
		inflateEnd(&f->zs);
	for (i = 0; f->headers[i] != NULL; i++)
		free(f->headers[i]);
	free(f->headers);
	free(f->req);
	free(f->line);
	free(f->location);
	free(f->realm);
	free(f->host);
	nsurl_unref(f->url);
	free(f);
}

/* ------------------------------------------------------------------------ */
/* the response */

static void reset_response(struct http_fetch *f)
{
	f->phase = PH_STATUS;
	f->line_len = 0;
	f->line_cr = false;
	f->code = 0;
	f->framing = BODY_CLOSE;
	f->keepalive = true;
	f->http10 = false;
	f->have_length = false;
	f->coding = 0;
	free(f->location);
	f->location = NULL;
	free(f->realm);
	f->realm = NULL;
}

static void parse_header(struct http_fetch *f, char *line, size_t len)
{
	char *v = strchr(line, ':');

	if (v == NULL)
		return;
	v++;
	while (*v == ' ' || *v == '\t')
		v++;

#define IS(name) (strncasecmp(line, name ":", sizeof(name)) == 0)
	if (IS("Location")) {
		free(f->location);
		f->location = strdup(v);
	} else if (IS("Content-Length")) {
		f->content_length = strtoull(v, NULL, 10);
		f->have_length = true;
	} else if (IS("Transfer-Encoding")) {
		if (strcasestr(v, "chunked") != NULL)
			f->framing = BODY_CHUNKED;
	} else if (IS("Content-Encoding")) {
		if (strcasestr(v, "gzip") != NULL || strcasestr(v, "deflate") != NULL)
			f->coding = 1;
	} else if (IS("Connection")) {
		if (strcasestr(v, "close") != NULL)
			f->keepalive = false;
		else if (strcasestr(v, "keep-alive") != NULL)
			f->keepalive = true;
	} else if (IS("WWW-Authenticate")) {
		char *r = strcasestr(v, "realm=\"");
		if (r != NULL) {
			char *e;
			r += 7;
			e = strchr(r, '"');
			free(f->realm);
			f->realm = e ? strndup(r, (size_t)(e - r)) : strdup(r);
		}
	} else if (IS("Set-Cookie")) {
		/* urldb wants the header as sent, line end included */
		size_t n = strlen(v);
		char *h = malloc(n + 3);
		if (h != NULL) {
			memcpy(h, v, n);
			memcpy(h + n, "\r\n", 3);
			fetch_set_cookie(f->parent, h);
			free(h);
		}
	}
#undef IS
	(void)len;
}

/* After the headers: what to tell the fetch machinery. true: carry on
 * with the body. */
static bool headers_complete(struct http_fetch *f)
{
	fetch_msg msg;

	if (f->framing != BODY_CHUNKED)
		f->framing = f->have_length ? BODY_LENGTH : BODY_CLOSE;
	if (f->framing == BODY_LENGTH)
		f->remaining = f->content_length;
	if (f->code == 204 || f->code == 304 || (f->code >= 100 && f->code < 200))
		f->framing = BODY_NONE;
	if (f->framing == BODY_LENGTH && f->remaining == 0)
		f->framing = BODY_NONE;
	if (f->framing == BODY_CLOSE)
		f->keepalive = false;
	f->chunk = CH_SIZE;

	fetch_set_http_code(f->parent, f->code);

	if (f->code == 304 && !f->is_post) {
		send_final(f, FETCH_NOTMODIFIED, NULL);
		return false;
	}
	if (f->code >= 300 && f->code < 400 && f->location != NULL) {
		msg.type = FETCH_REDIRECT;
		msg.data.redirect = f->location;
		f->done = true;
		send_msg(f, &msg);
		return false;
	}
	if (f->code == 401) {
		msg.type = FETCH_AUTH;
		msg.data.auth.realm = f->realm != NULL ? f->realm : "";
		f->done = true;
		send_msg(f, &msg);
		return false;
	}
	if (f->only_2xx && (f->code < 200 || f->code > 299)) {
		send_final(f, FETCH_ERROR, messages_get("Not2xx"));
		return false;
	}
	if (f->coding) {
		memset(&f->zs, 0, sizeof(f->zs));
		/* 15 + 32: zlib or gzip, recognised by the header */
		if (inflateInit2(&f->zs, 15 + 32) == Z_OK)
			f->inflating = true;
	}
	return true;
}

static void deliver(struct http_fetch *f, const uint8_t *data, size_t len)
{
	/* not on the stack: the calculator's is small, and the content
	 * handlers run below this */
	static uint8_t out[4096];
	int r;

	f->received += len;
	if (!f->inflating) {
		send_data(f, data, len);
		return;
	}
	f->zs.next_in = (Bytef *)data;
	f->zs.avail_in = (uInt)len;
	do {
		f->zs.next_out = out;
		f->zs.avail_out = sizeof(out);
		r = inflate(&f->zs, Z_NO_FLUSH);
		if (r == Z_DATA_ERROR && f->zs.total_out == 0 && f->coding == 1) {
			/* "deflate" sent raw, without the zlib header */
			inflateEnd(&f->zs);
			memset(&f->zs, 0, sizeof(f->zs));
			if (inflateInit2(&f->zs, -15) != Z_OK) {
				f->inflating = false;
				return;
			}
			f->coding = 2;
			f->zs.next_in = (Bytef *)data;
			f->zs.avail_in = (uInt)len;
			continue;
		}
		send_data(f, out, sizeof(out) - f->zs.avail_out);
		if (f->aborted)
			return;
		if (r != Z_OK && r != Z_BUF_ERROR) {
			/* end of stream, or garbage after it */
			f->zs.avail_in = 0;
			break;
		}
	} while (f->zs.avail_in > 0 || f->zs.avail_out == 0);
}

/* A complete response: tell the fetch machinery and recycle the
 * connection when it is still usable. */
static void finish(struct http_fetch *f)
{
	f->phase = PH_DONE;
	if (f->conn != NULL) {
		if (f->keepalive && !f->http10)
			pool_put(f->conn);
		else
			conn_free(f->conn);
		f->conn = NULL;
	}
	send_final(f, FETCH_FINISHED, NULL);
}

/* Consume received bytes. */
static void consume(struct http_fetch *f, uint8_t *data, size_t len)
{
	size_t i = 0;

	while (i < len && !f->aborted && f->phase != PH_DONE && !f->done) {
		if (f->phase == PH_STATUS || f->phase == PH_HEADERS ||
		    (f->phase == PH_BODY && f->framing == BODY_CHUNKED &&
		     f->chunk != CH_DATA)) {
			/* a line */
			char c = (char)data[i++];
			if (c == '\n') {
				char *line = f->line;
				f->line[f->line_len] = '\0';
				if (f->line_len > 0 && f->line[f->line_len - 1] == '\r')
					f->line[--f->line_len] = '\0';
				if (f->phase == PH_STATUS) {
					int maj = 1, min = 1;
					long code = 0;
					if (f->line_len == 0) {
						f->line_len = 0;
						continue;	/* stray CRLF */
					}
					if (sscanf(line, "HTTP/%d.%d %ld", &maj, &min, &code) < 3 ||
					    code < 100 || code > 999) {
						send_final(f, FETCH_ERROR, "Bad HTTP response");
						return;
					}
					f->code = code;
					f->http10 = maj == 1 && min == 0;
					if (f->http10)
						f->keepalive = false;
					f->phase = PH_HEADERS;
				} else if (f->phase == PH_HEADERS) {
					if (f->line_len == 0) {
						if (f->code >= 100 && f->code < 200 && f->code != 101) {
							/* 100 Continue etc.: the real one follows */
							reset_response(f);
							continue;
						}
						if (!headers_complete(f)) {
							f->phase = PH_DONE;
							if (f->conn != NULL) {
								conn_free(f->conn);
								f->conn = NULL;
							}
							return;
						}
						f->phase = PH_BODY;
						if (f->framing == BODY_NONE) {
							finish(f);
							return;
						}
					} else {
						parse_header(f, line, f->line_len);
						/* as libcurl hands them over: with the CRLF */
						memcpy(line + f->line_len, "\r\n", 3);
						send_header(f, line, f->line_len + 2);
						if (f->aborted)
							return;
					}
				} else {	/* chunk framing */
					if (f->chunk == CH_SIZE) {
						f->remaining = strtoull(line, NULL, 16);
						if (f->remaining == 0)
							f->chunk = CH_TRAILER;
						else
							f->chunk = CH_DATA;
					} else if (f->chunk == CH_DATA_END) {
						f->chunk = CH_SIZE;
					} else if (f->chunk == CH_TRAILER) {
						if (f->line_len == 0) {
							finish(f);
							return;
						}
					}
				}
				f->line_len = 0;
			} else if (f->line_len < LINE_MAX_LEN - 3) {
				f->line[f->line_len++] = c;
			}
			continue;
		}

		/* body bytes */
		{
			size_t n = len - i;
			if (f->framing == BODY_LENGTH || f->framing == BODY_CHUNKED) {
				if (n > f->remaining)
					n = (size_t)f->remaining;
			}
			deliver(f, data + i, n);
			i += n;
			if (f->framing == BODY_LENGTH || f->framing == BODY_CHUNKED) {
				f->remaining -= n;
				if (f->remaining == 0) {
					if (f->framing == BODY_LENGTH) {
						finish(f);
						return;
					}
					f->chunk = CH_DATA_END;
				}
			}
		}
	}
}

/* A failure before anything usable arrived. */
static void fail_connect(struct http_fetch *f)
{
	struct conn *c = f->conn;
	uint32_t waited = c != NULL ? nspire_ms() - c->since : 0;

	if (c != NULL && c->tls && c->tls_error != 0) {
		if (c->tls_error == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
			send_certs(f, c);
			if (!f->aborted)
				send_final(f, FETCH_CERT_ERR, NULL);
		} else {
			send_final(f, FETCH_SSL_ERR, NULL);
		}
	} else if (nnet_state() != NNET_READY) {
		send_final(f, FETCH_ERROR,
			   "The USB network is not ready: see the status bar");
	} else if (c != NULL && c->tcp == NULL && waited < CONNECT_TIMEOUT_MS) {
		send_final(f, FETCH_ERROR, "Server not found");
	} else if (waited < CONNECT_TIMEOUT_MS) {
		send_final(f, FETCH_ERROR, "The server refused the connection");
	} else {
		send_final(f, FETCH_TIMEDOUT, messages_get("Timeout"));
	}
}

/* Work on one fetch. true when it is finished with. */
static bool step(struct http_fetch *f)
{
	static uint8_t buf[RECV_CHUNK];
	uint32_t now = nspire_ms();
	int n, rounds;

	if (f->aborted || f->done)
		return true;

	if (f->phase == PH_CONNECT) {
		if (f->conn == NULL) {
			bool verify = f->https && !urldb_get_cert_permissions(f->url);
			if (f->https && nspire_tls_config(verify) == NULL) {
				send_final(f, FETCH_SSL_ERR, NULL);
				return true;
			}
			if (f->attempts == 0)
				f->conn = pool_take(f->host, f->port, f->https, verify);
			if (f->conn == NULL)
				f->conn = conn_new(f->host, f->port, f->https, verify);
			if (f->conn == NULL) {
				send_final(f, FETCH_ERROR, messages_get("NoMemory"));
				return true;
			}
			f->attempts++;
		}
		if (!conn_step(f->conn)) {
			fail_connect(f);
			return true;
		}
		if (f->conn->state != CONN_OPEN)
			return false;
		f->phase = PH_SEND;
		f->req_off = 0;
		reset_response(f);
		f->phase = PH_SEND;
		f->got_response_bytes = false;
		f->activity = now;
	}

	if (f->phase == PH_SEND) {
		while (f->req_off < f->req_len) {
			n = conn_write(f->conn, f->req + f->req_off, f->req_len - f->req_off);
			if (n < 0)
				goto broken;
			if (n == 0)
				break;
			f->req_off += (size_t)n;
			f->activity = now;
		}
		if (f->req_off < f->req_len)
			return false;
		nnet_tcp_flush(f->conn->tcp);
		f->phase = PH_STATUS;
	}

	/* read what there is; bounded so other fetches get their turn */
	for (rounds = 0; rounds < 16 && !f->aborted && !f->done; rounds++) {
		n = conn_read(f->conn, buf, sizeof(buf));
		if (n == 0)
			break;
		if (n < 0) {
			/* closed */
			if (f->phase == PH_BODY && f->framing == BODY_CLOSE) {
				f->keepalive = false;
				finish(f);
				return true;
			}
			goto broken;
		}
		f->got_response_bytes = true;
		f->activity = now;
		consume(f, buf, (size_t)n);
		if (f->phase == PH_DONE)
			return true;
	}
	if (f->aborted || f->done)
		return true;

	if ((uint32_t)(now - f->activity) > IDLE_TIMEOUT_MS) {
		send_final(f, FETCH_TIMEDOUT, messages_get("Timeout"));
		return true;
	}
	return false;

broken:
	/* A reused connection the server had closed: once more on a new one */
	if (f->conn->reused && !f->got_response_bytes && f->attempts < 3) {
		conn_free(f->conn);
		f->conn = NULL;
		f->phase = PH_CONNECT;
		return false;
	}
	if (f->phase == PH_BODY && f->framing == BODY_LENGTH && f->received > 0) {
		/* short body: hand over what came, as curl does */
		f->keepalive = false;
		finish(f);
		return true;
	}
	if (!f->got_response_bytes) {
		send_final(f, FETCH_ERROR, "The connection was closed");
	} else {
		send_final(f, FETCH_ERROR, messages_get("FetchFailedToFinish"));
	}
	return true;
}

static void http_poll(lwc_string *scheme)
{
	static bool polling;
	struct http_fetch *f, *save = NULL;

	(void)scheme;
	/* a callback may run the scheduler again; the receive buffers are
	 * static, so no second pass inside the first */
	if (polling)
		return;
	polling = true;
	nspire_net_poll();
	pool_expire();

	while (ring != NULL) {
		f = ring;
		RING_REMOVE(ring, f);
		if (f->locked) {
			RING_INSERT(save, f);
			continue;
		}
		if (step(f)) {
			if (f->conn != NULL && !f->aborted && !f->done) {
				/* not reached */
				conn_free(f->conn);
				f->conn = NULL;
			}
			fetch_remove_from_queues(f->parent);
			fetch_free(f->parent);
		} else {
			RING_INSERT(save, f);
		}
	}
	ring = save;
	polling = false;
}

nserror nspire_fetch_http_register(void)
{
	static const struct fetcher_operation_table ops = {
		.initialise = http_initialise,
		.acceptable = http_acceptable,
		.setup = http_setup,
		.start = http_start,
		.abort = http_abort,
		.free = http_free,
		.poll = http_poll,
		.finalise = http_finalise,
	};
	nserror err;

	if (lwc_intern_string("http", 4, &scheme_http) != lwc_error_ok ||
	    lwc_intern_string("https", 5, &scheme_https) != lwc_error_ok)
		return NSERROR_NOMEM;
	err = fetcher_add(lwc_string_ref(scheme_http), &ops);
	if (err != NSERROR_OK)
		return err;
	return fetcher_add(lwc_string_ref(scheme_https), &ops);
}
