/* TLS 1.2 client for the 3DS: libctru sockets under mbedTLS.
 *
 * The console's own sslc is deliberately unused. We need to pick the cipher
 * list, carry our own trust anchors, and reuse the same crypto for the access
 * point handshake and the AES-CTR audio files later on.
 */
#ifndef TLS3DS_H
#define TLS3DS_H

#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct tls_conn tls_conn;

/* Optional socket metadata trace; never includes payloads or keys. The callback
 * runs synchronously and must not reenter TLS. Pass NULL to disable it.
 * errno=0 in a trace means no syscall failure, not the thread's stale errno.
 * Logging adds overhead: compare timings with tracing disabled if necessary. */
typedef void (*tls_trace_fn)(const char *line);
void tls_set_trace(tls_trace_fn callback);

/* Everything the probe wants to report, filled in even when the connection
 * fails -- the stage that is still zero is the one that broke. */
typedef struct {
	double   dns_ms;
	double   connect_ms;
	double   handshake_ms;
	u32      verify_flags;       /* 0 = accepted; UINT32_MAX = unavailable */
	bool     resumed;            /* server retained the offered TLS 1.2 master secret */
	char     version[16];        /* "TLSv1.2" */
	char     ciphersuite[48];
	char     peer_cn[96];
	int      handshake_bytes_in;
	int      handshake_bytes_out;
} tls_stats;

/* Parses the linked CA bundle and seeds the DRBG. socInit() and psInit() must
 * already have succeeded. Returns 0, or a negative mbedTLS error.
 * A failed initialization releases all contexts and may be retried. */
int  tls_global_init(void);
/* Call before socExit(). Waits for any timed-out SOC DNS lookup to finish. */
void tls_global_exit(void);

/* How many anchors tls_global_init() accepted; 0 means every chain will fail. */
int  tls_anchor_count(void);

/* Connects, verifies the chain against the bundle and against `host`, and
 * returns a ready connection, or NULL with `err` set. `stats` may be NULL.
 *
 * timeout_ms must be positive: DNS and TCP share that budget; each TLS I/O
 * wait has the same timeout. Writes try nonblocking send first, waiting only
 * on backpressure and without restarting the deadline after each retry.
 * TCP polls use slices of at most 100 ms within the original DNS/TCP budget.
 * Completion is checked with getpeername() even when a slice has no event:
 * SOC may retain EINPROGRESS in SO_ERROR after reporting POLLOUT.
 * A timed-out DNS lookup cannot be cancelled by SOC;
 * new connections fail while it is still pending, rather than spawning threads.
 * Calls to this module must be serialized.
 *
 * A cached session is offered to the server, which may reject it and perform
 * a full handshake. stats->resumed reports confirmed TLS 1.2 resumption only. */
tls_conn *tls_connect(const char *host, u16 port, int timeout_ms,
                      tls_stats *stats, char *err, size_t errlen);

int  tls_write(tls_conn *c, const void *buf, size_t len);   /* writes all of it */
int  tls_read(tls_conn *c, void *buf, size_t len);          /* 0 = closed */
void tls_close(tls_conn *c);

/* Cuts every network wait short while `on` (TLS here, and the access
 * point's in ap3ds.c): polls run in slices of TLS_ABORT_SLICE_MS and fail
 * once it is set, so a worker in the middle of a request returns within a
 * slice instead of after its whole timeout. For leaving the session, so its
 * workers can be joined quickly; set it back to false afterwards. DNS is
 * not cut: its thread uses the sockets and must end on its own. */
#define TLS_ABORT_SLICE_MS 100
void tls_set_abort(bool on);
bool tls_aborting(void);

/* Renders an mbedTLS error as text; in the shipping build, as a bare number. */
void tls_strerror(int code, char *out, size_t outlen);

#endif
