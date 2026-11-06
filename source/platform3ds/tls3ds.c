#include "tls3ds.h"
#include "ca_bundle.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#ifdef MBEDTLS_ERROR_C
#include "mbedtls/error.h"
#endif

/* Spotify's hosts all offer ECDHE-RSA-AES128-GCM-SHA256 and nothing here needs
 * to interoperate with anything else, so the list stays short: a smaller
 * ClientHello, and no CBC or SHA-1 suites to fall back onto. ChaCha20 leads
 * because the ARM11 has no AES instructions, so it is the cheaper choice when
 * a server will take it. */
static const int SUITES[] = {
	MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
	MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
	MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
	MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
	MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
	MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
	0
};

/* X25519 first for the same reason, then the NIST curves Spotify actually
 * selects. */
static const uint16_t GROUPS[] = {
	MBEDTLS_SSL_IANA_TLS_GROUP_X25519,
	MBEDTLS_SSL_IANA_TLS_GROUP_SECP256R1,
	MBEDTLS_SSL_IANA_TLS_GROUP_SECP384R1,
	MBEDTLS_SSL_IANA_TLS_GROUP_NONE
};

struct tls_conn {
	mbedtls_ssl_context      ssl;
	mbedtls_ssl_config       conf;
	int                      fd;
	int                      timeout_ms;
	char                     host[128];
};

static struct {
	bool                     initialized;
	bool                     ready;
	mbedtls_entropy_context  entropy;
	mbedtls_ctr_drbg_context drbg;
	mbedtls_x509_crt         anchors;
	int                      anchor_count;
	/* A small ring of session tickets, keyed by host. Spotify's client talks
	 * to a handful of hosts in bursts; with a single slot the art fetches and
	 * API calls kept evicting each other's ticket, forcing a full ECDHE on
	 * every metadata chunk. Four slots cover the working set. */
#define TLS_TICKET_SLOTS 4
	mbedtls_ssl_session      ticket[TLS_TICKET_SLOTS];
	char                     ticket_host[TLS_TICKET_SLOTS][128];
	bool                     have_ticket[TLS_TICKET_SLOTS];
	int                      ticket_next;
} g;

/* SOC has no cancellable resolver. Keep at most one outstanding lookup, with
 * owned arguments, and join it before socExit() can tear down the service. */
static struct {
	Thread thread;
	struct addrinfo *result;
	int rc;
	char service[8];
	char *host;
} dns;

static tls_trace_fn trace_callback;

void tls_set_trace(tls_trace_fn callback)
{
	trace_callback = callback;
}

static void trace(const char *fmt, ...)
{
	if (!trace_callback)
		return;
	int saved_errno = errno;
	char line[384];
	va_list args;
	va_start(args, fmt);
	vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);
	trace_callback(line);
	errno = saved_errno;
}

static double elapsed_ms(TickCounter *t)
{
	/* Update advances the reference in libctru. Sample a copy so repeated
	 * deadline checks still measure from the original start. */
	TickCounter sample = *t;
	osTickCounterUpdate(&sample);
	return osTickCounterRead(&sample);
}

static int trace_poll(const char *host, const char *stage,
                      struct pollfd *p, int timeout_ms)
{
	TickCounter t;
	osTickCounterStart(&t);
	int rc = poll(p, 1, timeout_ms);
	int error = rc < 0 ? errno : 0;
	trace("host=%s fd=%d stage=%s op=poll rc=%d errno=%d events=0x%X "
	      "revents=0x%X timeout_ms=%d elapsed_ms=%.3f\n",
	      host, p->fd, stage, rc, error, p->events, p->revents,
	      timeout_ms, elapsed_ms(&t));
	if (rc < 0)
		errno = error;
	return rc;
}

static void dns_worker(void *arg)
{
	(void)arg;
	struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
	dns.rc = getaddrinfo(dns.host, dns.service, &hints, &dns.result);
}

static void dns_clear(void)
{
	threadFree(dns.thread);
	if (dns.result)
		freeaddrinfo(dns.result);
	free(dns.host);
	memset(&dns, 0, sizeof(dns));
}

/* The matching slot, or the next victim when the host has none yet. */
static int ticket_slot(const char *host, bool *found)
{
	for (int i = 0; i < TLS_TICKET_SLOTS; i++)
		if (g.have_ticket[i] && strcmp(g.ticket_host[i], host) == 0) {
			*found = true;
			return i;
		}
	*found = false;
	int slot = g.ticket_next;
	g.ticket_next = (g.ticket_next + 1) % TLS_TICKET_SLOTS;
	return slot;
}

void tls_strerror(int code, char *out, size_t outlen)
{
	if (code == MBEDTLS_ERR_NET_SEND_FAILED || code == MBEDTLS_ERR_NET_RECV_FAILED) {
		snprintf(out, outlen, "socket %s failed (-0x%04X); see socket trace",
		         code == MBEDTLS_ERR_NET_SEND_FAILED ? "send" : "receive",
		         (unsigned)-code);
		return;
	}
#ifdef MBEDTLS_ERROR_C
	mbedtls_strerror(code, out, outlen);
	if (out[0])
		return;
#endif
	snprintf(out, outlen, "mbedtls %d (-0x%04X)", code, (unsigned)-code);
}

int tls_anchor_count(void) { return g.anchor_count; }

int tls_global_init(void)
{
	if (g.ready)
		return 0;

	mbedtls_entropy_init(&g.entropy);
	mbedtls_ctr_drbg_init(&g.drbg);
	mbedtls_x509_crt_init(&g.anchors);
	for (int i = 0; i < TLS_TICKET_SLOTS; i++)
		mbedtls_ssl_session_init(&g.ticket[i]);
	g.initialized = true;

	static const char PERS[] = "3ds-spotify";
	int rc = mbedtls_ctr_drbg_seed(&g.drbg, mbedtls_entropy_func, &g.entropy,
	                               (const unsigned char *)PERS, sizeof(PERS) - 1);
	if (rc)
		goto fail;

	/* A partial failure is normal and fine: parse() returns the number of
	 * certificates it had to skip, and only a total failure is fatal. */
	rc = mbedtls_x509_crt_parse(&g.anchors,
	                            (const unsigned char *)spotify_ca_bundle,
	                            spotify_ca_bundle_len);
	if (rc < 0)
		goto fail;
	for (const mbedtls_x509_crt *c = &g.anchors; c; c = c->next)
		if (c->raw.p)
			g.anchor_count++;
	if (g.anchor_count == 0) {
		rc = MBEDTLS_ERR_X509_INVALID_FORMAT;
		goto fail;
	}

	g.ready = true;
	return 0;
fail:
	tls_global_exit();
	return rc;
}

void tls_global_exit(void)
{
	if (dns.thread) {
		threadJoin(dns.thread, U64_MAX);
		dns_clear();
	}
	if (!g.initialized)
		return;
	for (int i = 0; i < TLS_TICKET_SLOTS; i++)
		mbedtls_ssl_session_free(&g.ticket[i]);
	mbedtls_x509_crt_free(&g.anchors);
	mbedtls_ctr_drbg_free(&g.drbg);
	mbedtls_entropy_free(&g.entropy);
	memset(&g, 0, sizeof(g));
}

/* ------------------------------------------------------------------ sockets */

/* libctru's SOC has no usable SO_RCVTIMEO. Nonblocking sockets ensure that a
 * readiness notification cannot turn into an unbounded send/recv. */
static int remaining_ms(TickCounter *t, int timeout_ms)
{
	double left = timeout_ms - elapsed_ms(t);
	return left > 0 ? (int)left : 0;
}

static volatile bool aborting;

void tls_set_abort(bool on)
{
	aborting = on;
	__sync_synchronize();
}

bool tls_aborting(void)
{
	return aborting;
}

/* poll() in slices, so tls_set_abort can cut the wait short: -1 then. */
static int poll_abortable(const char *host, const char *stage,
                          struct pollfd *p, int timeout_ms)
{
	TickCounter t;
	osTickCounterStart(&t);
	for (;;) {
		if (aborting)
			return -1;
		int left = remaining_ms(&t, timeout_ms);
		int slice = left < TLS_ABORT_SLICE_MS ? left : TLS_ABORT_SLICE_MS;
		int n = trace_poll(host, stage, p, slice);
		if (n != 0 || left <= TLS_ABORT_SLICE_MS)
			return n;
	}
}

static int wait_io(tls_conn *c, short events)
{
	struct pollfd p = { .fd = c->fd, .events = events };
	int n = poll_abortable(c->host, "tls", &p, c->timeout_ms);
	if (n == 0)
		return MBEDTLS_ERR_SSL_TIMEOUT;
	if (n < 0)
		return MBEDTLS_ERR_NET_RECV_FAILED;
	if (p.revents & (POLLERR | POLLNVAL))
		return MBEDTLS_ERR_NET_RECV_FAILED;
	if (!(p.revents & (events | POLLHUP)))
		return MBEDTLS_ERR_NET_RECV_FAILED;
	return 0;
}

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
	tls_conn *c = ctx;
	TickCounter deadline;
	osTickCounterStart(&deadline);
	for (;;) {
		TickCounter t;
		osTickCounterStart(&t);
		int n = send(c->fd, buf, len, 0);
		int error = n < 0 ? errno : 0;
		trace("host=%s fd=%d op=send requested=%lu rc=%d errno=%d elapsed_ms=%.3f\n",
		      c->host, c->fd, (unsigned long)len, n, error, elapsed_ms(&t));
		if (n >= 0)
			return n;
		if (error != EAGAIN && error != EWOULDBLOCK && error != EINTR)
			return MBEDTLS_ERR_NET_SEND_FAILED;
		int left = remaining_ms(&deadline, c->timeout_ms);
		if (left == 0)
			return MBEDTLS_ERR_SSL_TIMEOUT;
		struct pollfd p = { .fd = c->fd, .events = POLLOUT };
		int rc = poll_abortable(c->host, "tls-send", &p, left);
		if (rc == 0 || remaining_ms(&deadline, c->timeout_ms) == 0)
			return MBEDTLS_ERR_SSL_TIMEOUT;
		if (rc < 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL)))
			return MBEDTLS_ERR_NET_SEND_FAILED;
	}
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
	tls_conn *c = ctx;
	int rc = wait_io(c, POLLIN);
	if (rc)
		return rc;
	TickCounter t;
	osTickCounterStart(&t);
	int n = recv(c->fd, buf, len, 0);
	int error = n < 0 ? errno : 0;
	trace("host=%s fd=%d op=recv requested=%lu rc=%d errno=%d elapsed_ms=%.3f\n",
	      c->host, c->fd, (unsigned long)len, n, error, elapsed_ms(&t));
	if (n == 0)
		return MBEDTLS_ERR_SSL_CONN_EOF;
	if (n < 0 && (error == EAGAIN || error == EWOULDBLOCK || error == EINTR))
		return MBEDTLS_ERR_SSL_WANT_READ;
	if (n < 0)
		return MBEDTLS_ERR_NET_RECV_FAILED;
	return n;
}

static int dial(const char *host, u16 port, int timeout_ms,
                tls_stats *st, char *err, size_t errlen)
{
	TickCounter t;
	osTickCounterStart(&t);
	trace("host=%s port=%u op=dial timeout_ms=%d\n", host, port, timeout_ms);

	if (dns.thread) {
		if (R_FAILED(threadJoin(dns.thread, 0))) {
			snprintf(err, errlen, "DNS resolver still busy after timeout");
			return -1;
		}
		dns_clear();
	}
	dns.host = strdup(host);
	if (!dns.host) {
		snprintf(err, errlen, "out of memory resolving %s", host);
		return -1;
	}
	snprintf(dns.service, sizeof(dns.service), "%u", port);
	dns.thread = threadCreate(dns_worker, NULL, 16 * 1024, 0x30, -2, false);
	if (!dns.thread) {
		free(dns.host);
		dns.host = NULL;
		snprintf(err, errlen, "could not start DNS resolver");
		return -1;
	}
	Result joined = threadJoin(dns.thread,
	                          (u64)remaining_ms(&t, timeout_ms) * 1000000);
	if (st)
		st->dns_ms = elapsed_ms(&t);
	if (R_FAILED(joined)) {
		snprintf(err, errlen, "DNS timeout/wait failed for %s (0x%08lX)",
		         host, (unsigned long)joined);
		return -1;
	}
	struct addrinfo *res = dns.result;
	dns.result = NULL;
	int dns_rc = dns.rc;
	dns_clear();
	if (dns_rc != 0 || !res) {
		if (res)
			freeaddrinfo(res);
		snprintf(err, errlen, "DNS failed for %s (%d)", host, dns_rc);
		return -1;
	}
	const struct sockaddr_in *address = (const struct sockaddr_in *)res->ai_addr;
	const unsigned char *ip = (const unsigned char *)&address->sin_addr;
	trace("host=%s op=dns rc=%d ip=%u.%u.%u.%u elapsed_ms=%.3f\n",
	      host, dns_rc, ip[0], ip[1], ip[2], ip[3], elapsed_ms(&t));

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		freeaddrinfo(res);
		snprintf(err, errlen, "socket() failed, errno %d", errno);
		return -1;
	}

	TickCounter tcp;
	osTickCounterStart(&tcp);
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
		snprintf(err, errlen, "fcntl() failed, errno %d", errno);
		goto fail;
	}
	int left = remaining_ms(&t, timeout_ms);
	if (left == 0) {
		snprintf(err, errlen, "DNS/TCP timeout for %s:%u", host, port);
		goto fail;
	}
	TickCounter operation;
	osTickCounterStart(&operation);
	int connect_rc = connect(fd, res->ai_addr, res->ai_addrlen);
	int connect_error = connect_rc < 0 ? errno : 0;
	trace("host=%s fd=%d op=connect rc=%d errno=%d elapsed_ms=%.3f\n",
	      host, fd, connect_rc, connect_error, elapsed_ms(&operation));
	if (connect_rc < 0) {
		if (connect_error != EINPROGRESS && connect_error != EWOULDBLOCK &&
		    connect_error != EALREADY) {
			snprintf(err, errlen, "connect() to %s:%u failed, errno %d",
			         host, port, connect_error);
			goto fail;
		}
		for (;;) {
			if (aborting) {
				snprintf(err, errlen, "cancelled connecting to %s:%u",
				         host, port);
				goto fail;
			}
			left = remaining_ms(&t, timeout_ms);
			if (left == 0) {
				snprintf(err, errlen, "TCP timeout for %s:%u", host, port);
				goto fail;
			}
			struct pollfd p = { .fd = fd, .events = POLLOUT };
			/* SOC may delay a long poll despite an established peer.
			 * Sample the peer after each slice, not just on POLLOUT. */
			int slice = left < 100 ? left : 100;
			int n = trace_poll(host, "tcp", &p, slice);
			if (remaining_ms(&t, timeout_ms) == 0) {
				snprintf(err, errlen, "TCP timeout for %s:%u", host, port);
				goto fail;
			}
			if (n < 0 || (p.revents & POLLNVAL)) {
				snprintf(err, errlen, "TCP poll failed for %s:%u, errno %d",
				         host, port, errno);
				goto fail;
			}
			int error = 0;
			socklen_t size = sizeof(error);
			osTickCounterStart(&operation);
			int option_rc = getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size);
			int option_error = option_rc < 0 ? errno : 0;
			trace("host=%s fd=%d op=SO_ERROR rc=%d errno=%d raw_soc=%d "
			      "elapsed_ms=%.3f\n", host, fd, option_rc, option_error,
			      error, elapsed_ms(&operation));
			if (option_rc < 0) {
				snprintf(err, errlen, "TCP SO_ERROR failed, errno %d", option_error);
				goto fail;
			}
			/* SOC's SO_ERROR payload uses negative SOC codes, not errno.
			 * -26 is EINPROGRESS and can remain after POLLOUT. Do not
			 * accept it as success without confirming the peer. */
			const int soc_inprogress = -26;
			if ((error && error != soc_inprogress) ||
			    (p.revents & (POLLERR | POLLHUP))) {
				snprintf(err, errlen,
				         "connect() to %s:%u failed, SOC error %d (poll %d)",
				         host, port, error, p.revents);
				goto fail;
			}
			struct sockaddr_storage peer;
			size = sizeof(peer);
			osTickCounterStart(&operation);
			int peer_rc = getpeername(fd, (struct sockaddr *)&peer, &size);
			int peer_error = peer_rc < 0 ? errno : 0;
			trace("host=%s fd=%d op=getpeername rc=%d errno=%d elapsed_ms=%.3f\n",
			      host, fd, peer_rc, peer_error, elapsed_ms(&operation));
			if (peer_rc == 0) {
				trace("host=%s fd=%d op=tcp_confirmed poll_rc=%d "
				      "revents=0x%X total_ms=%.3f\n",
				      host, fd, n, p.revents, elapsed_ms(&tcp));
				break;
			}
			if (peer_error != ENOTCONN) {
				snprintf(err, errlen, "TCP getpeername failed, errno %d", peer_error);
				goto fail;
			}
			/* A writable notification alone is not proof of connection.
			 * Avoid spinning if SOC keeps returning pending readiness. */
			if (n > 0 && remaining_ms(&t, timeout_ms) > 0)
				svcSleepThread(1000000);
		}
	}
	osTickCounterUpdate(&tcp);
	if (st)
		st->connect_ms = osTickCounterRead(&tcp);
	trace("host=%s fd=%d op=tcp_ready elapsed_ms=%.3f\n",
	      host, fd, osTickCounterRead(&tcp));

	freeaddrinfo(res);
	return fd;
fail:
	osTickCounterUpdate(&tcp);
	if (st)
		st->connect_ms = osTickCounterRead(&tcp);
	close(fd);
	freeaddrinfo(res);
	return -1;
}

/* ---------------------------------------------------------------- handshake */

tls_conn *tls_connect(const char *host, u16 port, int timeout_ms,
                      tls_stats *stats, char *err, size_t errlen)
{
	tls_stats local = {0};
	tls_stats *st = stats ? stats : &local;
	memset(st, 0, sizeof(*st));
	if (errlen)
		err[0] = '\0';

	if (!g.ready) {
		snprintf(err, errlen, "tls_global_init() was not called");
		return NULL;
	}
	if (timeout_ms <= 0) {
		snprintf(err, errlen, "timeout_ms must be positive");
		return NULL;
	}

	tls_conn *c = calloc(1, sizeof(*c));
	if (!c) {
		snprintf(err, errlen, "out of memory");
		return NULL;
	}
	c->timeout_ms = timeout_ms;
	snprintf(c->host, sizeof(c->host), "%s", host);

	c->fd = dial(host, port, timeout_ms, st, err, errlen);
	if (c->fd < 0) {
		free(c);
		return NULL;
	}

	mbedtls_ssl_init(&c->ssl);
	mbedtls_ssl_config_init(&c->conf);

	int rc = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT,
	                                     MBEDTLS_SSL_TRANSPORT_STREAM,
	                                     MBEDTLS_SSL_PRESET_DEFAULT);
	if (rc)
		goto fail;

	mbedtls_ssl_conf_min_tls_version(&c->conf, MBEDTLS_SSL_VERSION_TLS1_2);
	mbedtls_ssl_conf_max_tls_version(&c->conf, MBEDTLS_SSL_VERSION_TLS1_2);
	mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
	mbedtls_ssl_conf_ca_chain(&c->conf, &g.anchors, NULL);
	mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &g.drbg);
	mbedtls_ssl_conf_ciphersuites(&c->conf, SUITES);
	mbedtls_ssl_conf_groups(&c->conf, GROUPS);
	mbedtls_ssl_conf_session_tickets(&c->conf, MBEDTLS_SSL_SESSION_TICKETS_ENABLED);
	/* 16 KB in, 4 KB out, as configured; asking for more would be refused. */
	mbedtls_ssl_conf_read_timeout(&c->conf, timeout_ms);

	if ((rc = mbedtls_ssl_setup(&c->ssl, &c->conf)))
		goto fail;
	/* Without SNI, Spotify's edge serves the wrong certificate; this call also
	 * arms the hostname check against the leaf. */
	if ((rc = mbedtls_ssl_set_hostname(&c->ssl, host)))
		goto fail;
	bool found = false;
	const int slot = ticket_slot(host, &found);
	if (found && (rc = mbedtls_ssl_set_session(&c->ssl, &g.ticket[slot])))
		goto fail;

	mbedtls_ssl_set_bio(&c->ssl, c, bio_send, bio_recv, NULL);

	TickCounter t;
	osTickCounterStart(&t);
	trace("host=%s fd=%d op=handshake_begin ticket_offered=%d\n",
	      host, c->fd, found);
	while ((rc = mbedtls_ssl_handshake(&c->ssl)) != 0) {
		if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE)
			break;
	}
	osTickCounterUpdate(&t);
	st->handshake_ms = osTickCounterRead(&t);
	trace("host=%s fd=%d op=handshake_end rc=%d elapsed_ms=%.3f verify=0x%08lX\n",
	      host, c->fd, rc, st->handshake_ms,
	      (unsigned long)mbedtls_ssl_get_verify_result(&c->ssl));

	if (rc) {
		/* A chain that did not verify is the interesting case, so report the
		 * flags rather than just "handshake failed". A console with an unset
		 * clock (fresh battery, never configured) fails every certificate
		 * with FUTURE/EXPIRED: say so plainly, it is the most common
		 * setup mistake and "handshake error" sends people nowhere. */
		st->verify_flags = mbedtls_ssl_get_verify_result(&c->ssl);
		char reason[128];
		tls_strerror(rc, reason, sizeof(reason));
		if (st->verify_flags != UINT32_MAX &&
		    (st->verify_flags & (MBEDTLS_X509_BADCERT_FUTURE |
		                         MBEDTLS_X509_BADCERT_EXPIRED)))
			snprintf(err, errlen,
			         "certificado fuera de fecha: revisa la FECHA y HORA "
			         "de la consola en Configuracion (verify 0x%08lX)",
			         (unsigned long)st->verify_flags);
		else if (st->verify_flags == UINT32_MAX)
			snprintf(err, errlen, "handshake: %s (verify unavailable)", reason);
		else
			snprintf(err, errlen, "handshake: %s (verify 0x%08lX)",
			         reason, (unsigned long)st->verify_flags);
		goto drop;
	}

	st->verify_flags = mbedtls_ssl_get_verify_result(&c->ssl);
	snprintf(st->version, sizeof(st->version), "%s",
	         mbedtls_ssl_get_version(&c->ssl));
	snprintf(st->ciphersuite, sizeof(st->ciphersuite), "%s",
	         mbedtls_ssl_get_ciphersuite(&c->ssl));

	const mbedtls_x509_crt *peer = mbedtls_ssl_get_peer_cert(&c->ssl);
	if (peer)
		mbedtls_x509_dn_gets(st->peer_cn, sizeof(st->peer_cn), &peer->subject);

	/* Keep the ticket for the next connection to this host. The slot lookup
	 * already ran before the handshake: a match is refreshed in place, a miss
	 * lands on the ring's next victim. */
	mbedtls_ssl_session session;
	mbedtls_ssl_session_init(&session);
	rc = mbedtls_ssl_get_session(&c->ssl, &session);
	if (rc == 0) {
		/* TLS 1.2 resumption retains the master secret; a rejected ticket
		 * produces a new one. mbedTLS 3.6 has no public resumed accessor. */
		st->resumed = found &&
			memcmp(session.MBEDTLS_PRIVATE(master),
			       g.ticket[slot].MBEDTLS_PRIVATE(master),
			       sizeof(session.MBEDTLS_PRIVATE(master))) == 0;
	}
	mbedtls_ssl_session_free(&g.ticket[slot]);
	mbedtls_ssl_session_init(&g.ticket[slot]);
	if (rc == 0) {
		g.ticket[slot] = session;
		snprintf(g.ticket_host[slot], sizeof(g.ticket_host[slot]), "%s", host);
		g.have_ticket[slot] = true;
	} else {
		mbedtls_ssl_session_free(&session);
		g.have_ticket[slot] = false;
	}

	return c;

fail: {
	char reason[128];
	tls_strerror(rc, reason, sizeof(reason));
	snprintf(err, errlen, "setup: %s", reason);
}
drop:
	mbedtls_ssl_free(&c->ssl);
	mbedtls_ssl_config_free(&c->conf);
	close(c->fd);
	free(c);
	return NULL;
}

int tls_write(tls_conn *c, const void *buf, size_t len)
{
	const unsigned char *p = buf;
	size_t sent = 0;
	while (sent < len) {
		int n = mbedtls_ssl_write(&c->ssl, p + sent, len - sent);
		if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
			continue;
		if (n <= 0)
			return n;
		sent += n;
	}
	return (int)sent;
}

int tls_read(tls_conn *c, void *buf, size_t len)
{
	for (;;) {
		int n = mbedtls_ssl_read(&c->ssl, buf, len);
		if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
			continue;
		if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
			return 0;
		return n;
	}
}

void tls_close(tls_conn *c)
{
	if (!c)
		return;
	mbedtls_ssl_close_notify(&c->ssl);
	mbedtls_ssl_free(&c->ssl);
	mbedtls_ssl_config_free(&c->conf);
	close(c->fd);
	free(c);
}
