/*
 * p9.h - 9P2000 Protocol Implementation
 *
 * Provides a client implementation of the 9P2000 protocol for connecting
 * to Plan 9 file servers. Supports both plaintext and TLS connections.
 *
 * 9P2000 Overview:
 *
 *   9P is a simple, message-based protocol for accessing remote files.
 *   Each operation is a request/response pair (T-message/R-message).
 *   Files are identified by "fids" - client-chosen 32-bit handles.
 *
 *   Connection lifecycle:
 *     1. TCP connect (optionally wrap with TLS)
 *     2. Tversion/Rversion - negotiate protocol version and message size
 *     3. Tattach/Rattach - authenticate and get root fid
 *     4. Twalk/Rwalk - navigate to files
 *     5. Topen/Ropen - open files for I/O
 *     6. Tread/Twrite - read/write data
 *     7. Tclunk/Rclunk - release fids
 *
 * Wire Format:
 *
 *   All messages start with: size[4] type[1] tag[2]
 *   Multi-byte integers are little-endian.
 *   Strings are length-prefixed: len[2] data[len]
 *
 * Thread Safety:
 *
 *   Each p9conn has a mutex that protects RPC operations. The synchronous
 *   functions (p9_read, p9_write, etc.) acquire this lock automatically.
 *   For pipelined writes, the caller must manage concurrency.
 *
 * Error Handling:
 *
 *   Connection errors (socket failures, TLS errors) are fatal - the
 *   p9_read_full() and p9_write_full() functions call exit(1) on error.
 *   This is appropriate for p9wl where connection loss is unrecoverable.
 *
 *   Protocol errors (Rerror responses) set flags in p9conn and return -1:
 *     - unknown_id_error: draw image ID not found
 *     - draw_error: error containing "short" (e.g., short write)
 *     - window_deleted: rio window was closed
 *
 * Usage:
 *
 *   struct p9conn p9;
 *   struct tls_config tls = { .cert_file = "server.pem" };
 *
 *   if (p9_connect(&p9, "192.168.1.1", P9_TLS_PORT, &tls) < 0) {
 *       // handle error
 *   }
 *
 *   // Read a file
 *   char buf[1024];
 *   int n = p9_read_file(&p9, "snarf", buf, sizeof(buf));
 *
 *   // Low-level operations
 *   uint32_t fid = p9.next_fid++;
 *   const char *path[] = { "draw", "new" };
 *   p9_walk(&p9, p9.root_fid, fid, 2, path);
 *   p9_open(&p9, fid, ORDWR, NULL);
 *   p9_read(&p9, fid, 0, sizeof(buf), (uint8_t*)buf);
 *   p9_clunk(&p9, fid);
 *
 *   p9_disconnect(&p9);
 */

#ifndef P9_H
#define P9_H

#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <openssl/ssl.h>

#include "p9_tls.h"

/* ============== Protocol Constants ============== */

/* Maximum message size (negotiated down via Tversion) */
#define P9_MSIZE    65536

/* Special tag value for Tversion (no tag multiplexing) */
#define P9_NOTAG    ((uint16_t)~0)

/* Special fid value meaning "no fid" (e.g., no auth fid in Tattach) */
#define P9_NOFID    ((uint32_t)~0)

/* Standard 9P port (plaintext) - TLS typically uses P9_TLS_PORT */
#define P9_PORT     10000

/* ============== 9P Message Types ============== */

/*
 * T-messages are requests from client to server.
 * R-messages are responses from server to client.
 * Each T-message type has a corresponding R-message type = T + 1.
 */
enum {
    Tversion = 100,     /* Negotiate protocol version */
    Rversion,
    Tauth = 102,        /* Authenticate (optional, not used here) */
    Rauth,
    Tattach = 104,      /* Attach to filesystem root */
    Rattach,
    Terror = 106,       /* Illegal - never sent */
    Rerror,             /* Error response (replaces any R-message) */
    Tflush = 108,       /* Cancel pending request */
    Rflush,
    Twalk = 110,        /* Navigate path from fid to newfid */
    Rwalk,
    Topen = 112,        /* Open file for I/O */
    Ropen,
    Tcreate = 114,      /* Create new file */
    Rcreate,
    Tread = 116,        /* Read from file */
    Rread,
    Twrite = 118,       /* Write to file */
    Rwrite,
    Tclunk = 120,       /* Release fid */
    Rclunk,
    Tremove = 122,      /* Remove file */
    Rremove,
    Tstat = 124,        /* Get file metadata */
    Rstat,
    Twstat = 126,       /* Set file metadata */
    Rwstat,
};

/* ============== Open Modes ============== */

#define OREAD   0       /* Open for reading */
#define OWRITE  1       /* Open for writing */
#define ORDWR   2       /* Open for read/write */
#define OEXEC   3       /* Open for execute */
#define OTRUNC  0x10    /* Truncate file on open (OR with above) */

/* ============== Wire Format Macros ============== */

/*
 * Read little-endian integers from buffer.
 * p must be a uint8_t* pointer.
 */
#define GET16(p) ((uint16_t)(p)[0] | ((uint16_t)(p)[1] << 8))
#define GET32(p) ((uint32_t)(p)[0] | ((uint32_t)(p)[1] << 8) | \
                  ((uint32_t)(p)[2] << 16) | ((uint32_t)(p)[3] << 24))
#define GET64(p) ((uint64_t)GET32(p) | ((uint64_t)GET32((p)+4) << 32))

/*
 * Write little-endian integers to buffer.
 * p must be a uint8_t* pointer, v is the value.
 */
#define PUT16(p, v) do { (p)[0] = (uint8_t)(v); (p)[1] = (uint8_t)((v) >> 8); } while(0)
#define PUT32(p, v) do { (p)[0] = (uint8_t)(v); (p)[1] = (uint8_t)((v) >> 8); \
                         (p)[2] = (uint8_t)((v) >> 16); (p)[3] = (uint8_t)((v) >> 24); } while(0)
#define PUT64(p, v) do { PUT32(p, v); PUT32((p)+4, (v) >> 32); } while(0)

/* ============== Connection Structure ============== */

/*
 * 9P connection state.
 *
 * Holds socket, TLS state, message buffer, and fid allocation.
 * Initialize with p9_connect(), cleanup with p9_disconnect().
 */
struct p9conn {
    int fd;                    /* Socket file descriptor */
    SSL *ssl;                  /* TLS connection (NULL if plaintext) */

    uint8_t *buf;              /* Message buffer (msize bytes) */
    uint32_t msize;            /* Maximum message size (negotiated) */
    uint16_t tag;              /* Next tag to use (auto-incremented) */

    uint32_t root_fid;         /* Root fid from attach (typically 0) */
    uint32_t next_fid;         /* Next fid to allocate (caller increments) */

    pthread_mutex_t lock;      /* Lock for RPC operations */

    /* Error flags - set by protocol handlers, checked by caller.
     * atomic_int for safe cross-thread visibility (drain → send). */
    atomic_int unknown_id_error;  /* "unknown id" error (draw image not found) */
    atomic_int draw_error;        /* Error containing "short" (e.g., short write) */
    atomic_int window_deleted;    /* "window deleted" error (rio closed window) */
};

/* ============== Connection Management ============== */

/*
 * Connect to a 9P server with optional TLS.
 *
 * Establishes TCP connection (with TCP_NODELAY for lower latency),
 * optionally wraps with TLS, then performs 9P version negotiation
 * and attach. On success, the connection is ready for file operations.
 *
 * p9:      connection structure to initialize
 * host:    server IPv4 address in dotted-decimal form (no DNS resolution)
 * port:    server port (P9_PORT for plaintext, P9_TLS_PORT for TLS)
 * tls_cfg: TLS configuration, or NULL for plaintext
 *
 * Returns 0 on success, -1 on error.
 *
 * On success:
 *   - p9->fd is the socket
 *   - p9->ssl is set if TLS enabled
 *   - p9->root_fid is attached to filesystem root
 *   - p9->next_fid is ready for allocation (starts at 1)
 */
int p9_connect(struct p9conn *p9, const char *host, int port,
               struct tls_config *tls_cfg);

/*
 * Disconnect from 9P server and free resources.
 *
 * Closes TLS connection (if any), closes socket, frees message buffer,
 * and destroys mutex. Safe to call after a failed p9_connect().
 *
 * p9: connection to disconnect
 */
void p9_disconnect(struct p9conn *p9);

/*
 * Check if connection should be terminated.
 *
 * Returns non-zero if the window_deleted flag is set, indicating
 * the rio window was closed and the compositor should exit.
 *
 * p9: connection to check
 *
 * Returns non-zero if shutdown requested, 0 otherwise.
 */
int p9_should_shutdown(struct p9conn *p9);

/* ============== Low-Level I/O ============== */

/*
 * Write exactly len bytes to the connection.
 *
 * Uses TLS if enabled, otherwise raw socket. Retries on EINTR.
 * FATAL: calls exit(1) on any error or connection close and
 * does not return.
 *
 * p9:  connection
 * buf: data to write
 * len: number of bytes
 *
 * Returns len on success.
 */
int p9_write_full(struct p9conn *p9, const uint8_t *buf, int len);

/*
 * Read exactly n bytes from the connection.
 *
 * Uses TLS if enabled, otherwise raw socket. Retries on EINTR.
 * FATAL: calls exit(1) on any error or connection close and
 * does not return.
 *
 * p9:  connection
 * buf: buffer to read into
 * n:   number of bytes to read
 *
 * Returns n on success.
 */
int p9_read_full(struct p9conn *p9, uint8_t *buf, int n);

/* ============== 9P Protocol Operations ============== */

/*
 * Perform 9P version negotiation.
 *
 * Sends Tversion with requested msize, receives Rversion with
 * server's msize. Updates p9->msize to negotiated value.
 * Called automatically by p9_connect().
 *
 * Acquires p9->lock internally (uses p9_rpc_locked).
 *
 * p9: connection (must have socket/TLS ready)
 *
 * Returns 0 on success, -1 on error.
 */
int p9_version(struct p9conn *p9);

/*
 * Attach to filesystem root.
 *
 * Sends Tattach to get a fid referencing the root directory.
 * Uses P9USER environment variable for username (default: "glenda").
 * Called automatically by p9_connect().
 *
 * Acquires p9->lock internally (uses p9_rpc_locked).
 *
 * p9:    connection
 * fid:   fid to assign to root (typically 0)
 * aname: attach name, or NULL for default
 *
 * Returns 0 on success, -1 on error.
 */
int p9_attach(struct p9conn *p9, uint32_t fid, const char *aname);

/*
 * Walk a path from one fid to another.
 *
 * Navigates from fid through path components to create newfid.
 * If nwname is 0, clones fid to newfid without navigation.
 *
 * p9:     connection
 * fid:    starting fid (must reference a directory)
 * newfid: fid to create (caller allocates via p9->next_fid++)
 * nwname: number of path components
 * wnames: array of path component strings
 *
 * Returns 0 on success, -1 on error.
 *
 * Example:
 *   const char *path[] = { "draw", "new" };
 *   p9_walk(&p9, p9.root_fid, fid, 2, path);
 */
int p9_walk(struct p9conn *p9, uint32_t fid, uint32_t newfid,
            int nwname, const char **wnames);

/*
 * Open a file for I/O.
 *
 * Opens the file referenced by fid with the specified mode.
 * The fid must have been created by a prior Twalk.
 *
 * p9:     connection
 * fid:    fid to open (from Twalk)
 * mode:   open mode (OREAD, OWRITE, ORDWR, optionally | OTRUNC)
 * iounit: output - maximum I/O size, or NULL to ignore
 *         (if server returns 0, defaults to msize-24)
 *
 * Returns 0 on success, -1 on error.
 */
int p9_open(struct p9conn *p9, uint32_t fid, uint8_t mode, uint32_t *iounit);

/*
 * Read data from an open file.
 *
 * Reads up to count bytes starting at offset. The actual number
 * of bytes read may be less than requested (EOF or server limit).
 *
 * p9:     connection
 * fid:    open fid (from Topen)
 * offset: byte offset in file
 * count:  maximum bytes to read
 * data:   buffer to read into (must have count bytes available)
 *
 * Returns bytes read (0 at EOF), or -1 on error.
 */
int p9_read(struct p9conn *p9, uint32_t fid, uint64_t offset,
            uint32_t count, uint8_t *data);

/*
 * Write data to an open file (synchronous).
 *
 * Writes count bytes starting at offset. Waits for server response.
 * If count exceeds msize-23, it is clamped to fit in a single 9P
 * message; the caller is responsible for chunking larger writes.
 * For high-throughput scenarios, use p9_write_send/p9_write_recv.
 *
 * p9:     connection
 * fid:    open fid (from Topen with OWRITE or ORDWR)
 * offset: byte offset in file
 * data:   data to write
 * count:  number of bytes (clamped to msize-23 if too large)
 *
 * Returns bytes written by server, or -1 on error.
 */
int p9_write(struct p9conn *p9, uint32_t fid, uint64_t offset,
             const uint8_t *data, uint32_t count);

/*
 * Release a fid.
 *
 * Tells the server to forget this fid. The fid cannot be used
 * after clunking. Should be called when done with a file.
 *
 * p9:  connection
 * fid: fid to release
 *
 * Returns 0 on success, -1 on error.
 */
int p9_clunk(struct p9conn *p9, uint32_t fid);

/*
 * Get file metadata via Tstat.
 *
 * Sends Tstat and extracts qid.vers from the response. Useful for
 * detecting file changes without reading the full contents (e.g.,
 * rio's /dev/snarf increments qid.vers on each write).
 *
 * p9:       connection
 * fid:      fid referencing the file (need not be open)
 * qid_vers: output - qid version number, or NULL to ignore
 *
 * Returns 0 on success, -1 on error.
 */
int p9_stat(struct p9conn *p9, uint32_t fid, uint32_t *qid_vers);

/* ============== High-Level File Operations ============== */

/*
 * Read entire file contents into a buffer.
 *
 * Convenience function that walks, opens, reads, and clunks.
 * Null-terminates the output buffer.
 *
 * Only supports a single path component relative to root (e.g.,
 * "snarf" works, but "draw/new" does not - use p9_walk for
 * multi-component paths).
 *
 * p9:      connection
 * path:    single path component relative to root (e.g., "snarf")
 * data:    output buffer (caller-provided)
 * bufsize: size of buffer (reads at most bufsize-1 bytes)
 *
 * Returns bytes read on success, -1 on error.
 */
int p9_read_file(struct p9conn *p9, const char *path, char *data, size_t bufsize);

/*
 * Write data to an existing file.
 *
 * Convenience function that walks, opens (OWRITE), writes all data
 * in chunked RPCs, and clunks. The file must already exist; this
 * does not create files or truncate on open.
 *
 * Only supports a single path component relative to root.
 *
 * p9:   connection
 * path: single path component relative to root
 * data: data to write
 * len:  number of bytes to write
 *
 * Returns 0 on success, -1 on error.
 */
int p9_write_file(struct p9conn *p9, const char *path, const char *data, size_t len);

/* ============== Pipelined Write Operations ============== */

/*
 * Send Twrite without waiting for response.
 *
 * For high-throughput scenarios (e.g., frame sending), multiple
 * Twrite messages can be sent before collecting responses.
 * Call p9_write_recv() to collect each response.
 *
 * If count exceeds msize-23, it is clamped to fit in a single
 * message. The return value reflects the actual bytes sent.
 *
 * Note: Caller must ensure proper ordering and not exceed
 * server's request queue. Does not acquire connection lock.
 *
 * p9:     connection
 * fid:    open fid
 * offset: byte offset in file
 * data:   data to write
 * count:  number of bytes (clamped to msize-23 if too large)
 *
 * Returns bytes actually sent (may be less than count), or -1 on
 * send error.
 */
int p9_write_send(struct p9conn *p9, uint32_t fid, uint64_t offset,
                  const uint8_t *data, uint32_t count);

/*
 * Receive Rwrite response from pipelined write.
 *
 * Collects one response from a prior p9_write_send().
 * Must be called once for each p9_write_send().
 *
 * p9: connection
 *
 * Returns bytes written by server, or -1 on error.
 */
int p9_write_recv(struct p9conn *p9);

/* ============== Internal RPC Functions ============== */

/*
 * Send request and receive response (with lock).
 *
 * Acquires p9->lock, sends message, waits for response.
 * Message must be built in p9->buf before calling, starting at
 * buf[4] (type) - bytes 0-3 are reserved for the size field and
 * written automatically.
 *
 * p9:            connection
 * txlen:         total length of request message (including size field)
 * expected_type: expected R-message type
 *
 * Returns response length on success, -1 on Rerror, unexpected
 * response type, or I/O error.
 */
int p9_rpc(struct p9conn *p9, int txlen, int expected_type);

/*
 * Send request and receive response (caller holds lock).
 *
 * Like p9_rpc() but assumes caller already holds p9->lock.
 *
 * p9:            connection
 * txlen:         total length of request message (including size field)
 * expected_type: expected R-message type
 *
 * Returns response length on success, -1 on Rerror, unexpected
 * response type, or I/O error.
 */
int p9_rpc_locked(struct p9conn *p9, int txlen, int expected_type);

#endif /* P9_H */
