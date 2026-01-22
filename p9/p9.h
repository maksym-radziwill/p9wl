/*
 * p9.h - 9P2000 Protocol Header
 *
 * Defines 9P message types, connection structure,
 * and protocol operations with optional TLS support.
 */

#ifndef P9_H
#define P9_H

#include <stdint.h>
#include <pthread.h>
#include <openssl/ssl.h>

#include "p9_tls.h"

/* 9P Protocol Constants */
#define P9_MSIZE    65536
#define P9_NOTAG    ((uint16_t)~0)
#define P9_NOFID    ((uint32_t)~0)
#define P9_PORT     10000       /* Standard 9P port (plaintext) */

/* 9P Message Types */
enum {
    Tversion = 100,
    Rversion,
    Tauth = 102,
    Rauth,
    Tattach = 104,
    Rattach,
    Terror = 106,  /* illegal */
    Rerror,
    Tflush = 108,
    Rflush,
    Twalk = 110,
    Rwalk,
    Topen = 112,
    Ropen,
    Tcreate = 114,
    Rcreate,
    Tread = 116,
    Rread,
    Twrite = 118,
    Rwrite,
    Tclunk = 120,
    Rclunk,
    Tremove = 122,
    Rremove,
    Tstat = 124,
    Rstat,
    Twstat = 126,
    Rwstat,
};

/* Open modes */
#define OREAD   0
#define OWRITE  1
#define ORDWR   2
#define OEXEC   3
#define OTRUNC  0x10

/* Byte order helpers (little-endian) */
#define GET16(p) ((uint16_t)(p)[0] | ((uint16_t)(p)[1] << 8))
#define GET32(p) ((uint32_t)(p)[0] | ((uint32_t)(p)[1] << 8) | \
                  ((uint32_t)(p)[2] << 16) | ((uint32_t)(p)[3] << 24))
#define GET64(p) ((uint64_t)GET32(p) | ((uint64_t)GET32((p)+4) << 32))

#define PUT16(p, v) do { (p)[0] = (uint8_t)(v); (p)[1] = (uint8_t)((v) >> 8); } while(0)
#define PUT32(p, v) do { (p)[0] = (uint8_t)(v); (p)[1] = (uint8_t)((v) >> 8); \
                         (p)[2] = (uint8_t)((v) >> 16); (p)[3] = (uint8_t)((v) >> 24); } while(0)
#define PUT64(p, v) do { PUT32(p, v); PUT32((p)+4, (v) >> 32); } while(0)

/* 9P Connection Structure - single authoritative definition with TLS support */
struct p9conn {
    int fd;                    /* Socket file descriptor */
    SSL *ssl;                  /* TLS connection (NULL if plaintext) */

    uint8_t *buf;              /* Message buffer */
    uint32_t msize;            /* Maximum message size */
    uint16_t tag;              /* Next tag to use */

    uint32_t root_fid;         /* Root fid from attach */
    uint32_t next_fid;         /* Next fid to allocate */

    pthread_mutex_t lock;      /* Lock for RPC operations */

    /* Error flags (set by protocol handlers) */
    int unknown_id_error;      /* Set when "unknown id" error received */
    int draw_error;            /* Set when draw protocol error received */
    int window_deleted;        /* Set when "window deleted" error received */
};

/* Connection Management */

/* Connect to 9P server with optional TLS
 * tls_cfg can be NULL for plaintext connection
 * Returns 0 on success, -1 on error
 */
int p9_connect(struct p9conn *p9, const char *host, int port,
               struct tls_config *tls_cfg);

/* Disconnect and free resources */
void p9_disconnect(struct p9conn *p9);

/* Check if connection should be terminated (e.g., window deleted)
 * Returns non-zero if shutdown is needed
 */
int p9_should_shutdown(struct p9conn *p9);

/* Low-level I/O (uses TLS if enabled) */
int p9_write_full(struct p9conn *p9, const uint8_t *buf, int len);
int p9_read_full(struct p9conn *p9, uint8_t *buf, int n);

/* 9P Protocol Operations */

/* Version negotiation - must be called first */
int p9_version(struct p9conn *p9);

/* Attach to filesystem root */
int p9_attach(struct p9conn *p9, uint32_t fid, const char *aname);

/* Walk path from fid to newfid */
int p9_walk(struct p9conn *p9, uint32_t fid, uint32_t newfid,
            int nwname, const char **wnames);

/* Open file */
int p9_open(struct p9conn *p9, uint32_t fid, uint8_t mode, uint32_t *iounit);

/* Read from file */
int p9_read(struct p9conn *p9, uint32_t fid, uint64_t offset,
            uint32_t count, uint8_t *data);

/* Write to file (synchronous) */
int p9_write(struct p9conn *p9, uint32_t fid, uint64_t offset,
             const uint8_t *data, uint32_t count);

/* Close fid */
int p9_clunk(struct p9conn *p9, uint32_t fid);

/* High-level file operations */

/* Read entire file contents
 * path: relative path from root (e.g., "snarf", "draw/new")
 * data: output buffer (caller-provided)
 * bufsize: size of buffer
 * Returns bytes read on success, -1 on error
 */
int p9_read_file(struct p9conn *p9, const char *path, char *data, size_t bufsize);

/* Write data to file
 * path: relative path from root
 * data: data to write
 * len: length of data
 * Returns 0 on success, -1 on error
 */
int p9_write_file(struct p9conn *p9, const char *path, const char *data, size_t len);

/* Pipelined write operations (for high-throughput) */

/* Send Twrite without waiting for response
 * Returns count on success, -1 on send error
 */
int p9_write_send(struct p9conn *p9, uint32_t fid, uint64_t offset,
                  const uint8_t *data, uint32_t count);

/* Receive Rwrite response from pipelined write
 * Returns bytes written by server, -1 on error
 */
int p9_write_recv(struct p9conn *p9);

/* Internal: RPC with/without lock */
int p9_rpc(struct p9conn *p9, int txlen, int expected_type);
int p9_rpc_locked(struct p9conn *p9, int txlen, int expected_type);

#endif /* P9_H */
