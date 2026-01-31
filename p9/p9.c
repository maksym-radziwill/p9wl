/*
 * p9.c - 9P2000 Protocol Implementation with TLS Support
 *
 * Refactored:
 * - Extracted p9_walk_open() helper for common walk+open pattern
 * - Simplified p9_read_file/p9_write_file using the helper
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include <wlr/util/log.h>

#include "p9.h"
#include "p9_tls.h"

/* Write full buffer - TLS or plaintext */
int p9_write_full(struct p9conn *p9, const uint8_t *buf, int len) {
    if (p9->ssl) {
        int r = tls_write_full(p9->ssl, buf, len);
        if (r < 0) {
            wlr_log(WLR_ERROR, "Connection lost - exiting");
            exit(1);
        }
        return r;
    }

    int total = 0;
    while (total < len) {
        ssize_t w = write(p9->fd, buf + total, len - total);
        if (w < 0) {
            if (errno == EINTR) continue;
            wlr_log(WLR_ERROR, "9P write error: %s - exiting", strerror(errno));
            exit(1);
        }
        if (w == 0) {
            wlr_log(WLR_ERROR, "9P write: connection closed - exiting");
            exit(1);
        }
        total += w;
    }
    return total;
}

/* Read exactly n bytes - TLS or plaintext */
int p9_read_full(struct p9conn *p9, uint8_t *buf, int n) {
    if (p9->ssl) {
        int r = tls_read_full(p9->ssl, buf, n);
        if (r < 0) {
            wlr_log(WLR_ERROR, "Connection lost - exiting");
            exit(1);
        }
        return r;
    }

    int total = 0;
    while (total < n) {
        ssize_t r = read(p9->fd, buf + total, n - total);
        if (r < 0) {
            if (errno == EINTR) continue;
            wlr_log(WLR_ERROR, "9P read error: %s - exiting", strerror(errno));
            exit(1);
        }
        if (r == 0) {
            wlr_log(WLR_ERROR, "9P read: connection closed - exiting");
            exit(1);
        }
        total += r;
    }
    return total;
}

/* Handle 9P error response, set appropriate flags */
static void handle_9p_error(struct p9conn *p9, const char *errmsg) {
    wlr_log(WLR_ERROR, "9P error: %s", errmsg);

    if (strstr(errmsg, "unknown id") != NULL) {
        p9->unknown_id_error = 1;
    }

    if (strstr(errmsg, "window deleted") != NULL) {
        wlr_log(WLR_INFO, "Window deleted - signaling shutdown");
        p9->window_deleted = 1;
    }

    if (strstr(errmsg, "short") != NULL) {
        p9->draw_error = 1;
        wlr_log(WLR_ERROR, "Draw protocol error - will reset");
    }
}

/* Send a 9P message and receive response (caller must hold lock) */
int p9_rpc_locked(struct p9conn *p9, int txlen, int expected_type) {
    uint8_t *buf = p9->buf;

    PUT32(buf, txlen);

    if (p9_write_full(p9, buf, txlen) != txlen) {
        return -1;
    }

    if (p9_read_full(p9, buf, 4) != 4) {
        return -1;
    }

    uint32_t rxlen = GET32(buf);
    if (rxlen < 7 || rxlen > p9->msize) {
        wlr_log(WLR_ERROR, "9P invalid response length: %u", rxlen);
        return -1;
    }

    if (p9_read_full(p9, buf + 4, rxlen - 4) != (int)(rxlen - 4)) {
        return -1;
    }

    int type = buf[4];
    if (type == Rerror) {
        uint16_t elen = GET16(buf + 7);
        char errmsg[256];
        int copylen = (elen < 255) ? elen : 255;
        memcpy(errmsg, buf + 9, copylen);
        errmsg[copylen] = '\0';
        
        handle_9p_error(p9, errmsg);
        return -1;
    }

    if (type != expected_type) {
        wlr_log(WLR_ERROR, "9P unexpected response: got %d, expected %d",
                type, expected_type);
        return -1;
    }

    return rxlen;
}

int p9_rpc(struct p9conn *p9, int txlen, int expected_type) {
    pthread_mutex_lock(&p9->lock);
    int r = p9_rpc_locked(p9, txlen, expected_type);
    pthread_mutex_unlock(&p9->lock);
    return r;
}

/* Tversion */
int p9_version(struct p9conn *p9) {
    uint8_t *buf = p9->buf;
    const char *version = "9P2000";
    int vlen = strlen(version);

    pthread_mutex_lock(&p9->lock);
    buf[4] = Tversion;
    PUT16(buf + 5, P9_NOTAG);
    PUT32(buf + 7, p9->msize);
    PUT16(buf + 11, vlen);
    memcpy(buf + 13, version, vlen);

    int rxlen = p9_rpc_locked(p9, 13 + vlen, Rversion);
    if (rxlen < 0) {
        pthread_mutex_unlock(&p9->lock);
        return -1;
    }

    p9->msize = GET32(buf + 7);
    pthread_mutex_unlock(&p9->lock);

    wlr_log(WLR_INFO, "9P version OK, msize=%u", p9->msize);
    return 0;
}

/* Tattach */
int p9_attach(struct p9conn *p9, uint32_t fid, const char *aname) {
    uint8_t *buf = p9->buf;

    /* Use P9USER env var, fallback to glenda */
    const char *uname = getenv("P9USER");
    if (!uname || !*uname) uname = "glenda";
    int ulen = strlen(uname);
    int alen = aname ? strlen(aname) : 0;

    wlr_log(WLR_INFO, "9P attach: uname='%s'", uname);

    pthread_mutex_lock(&p9->lock);
    buf[4] = Tattach;
    PUT16(buf + 5, p9->tag++);
    PUT32(buf + 7, fid);
    PUT32(buf + 11, P9_NOFID);  /* afid - no auth */
    PUT16(buf + 15, ulen);
    memcpy(buf + 17, uname, ulen);
    PUT16(buf + 17 + ulen, alen);
    if (alen > 0) memcpy(buf + 19 + ulen, aname, alen);

    int r = p9_rpc_locked(p9, 19 + ulen + alen, Rattach);
    pthread_mutex_unlock(&p9->lock);

    if (r < 0) return -1;
    wlr_log(WLR_INFO, "9P attached as '%s'", uname);
    return 0;
}

/* Twalk */
int p9_walk(struct p9conn *p9, uint32_t fid, uint32_t newfid,
            int nwname, const char **wnames) {
    uint8_t *buf = p9->buf;

    pthread_mutex_lock(&p9->lock);
    int off = 7;
    buf[4] = Twalk;
    PUT16(buf + 5, p9->tag++);
    PUT32(buf + off, fid); off += 4;
    PUT32(buf + off, newfid); off += 4;
    PUT16(buf + off, nwname); off += 2;

    for (int i = 0; i < nwname; i++) {
        int len = strlen(wnames[i]);
        PUT16(buf + off, len); off += 2;
        memcpy(buf + off, wnames[i], len); off += len;
    }

    int r = p9_rpc_locked(p9, off, Rwalk);
    pthread_mutex_unlock(&p9->lock);
    return r >= 0 ? 0 : -1;
}

/* Topen - returns iounit via pointer (0 from server means use msize-24) */
int p9_open(struct p9conn *p9, uint32_t fid, uint8_t mode, uint32_t *iounit) {
    uint8_t *buf = p9->buf;

    pthread_mutex_lock(&p9->lock);
    buf[4] = Topen;
    PUT16(buf + 5, p9->tag++);
    PUT32(buf + 7, fid);
    buf[11] = mode;

    int r = p9_rpc_locked(p9, 12, Ropen);
    if (r >= 0 && iounit) {
        /* Ropen: size[4] type[1] tag[2] qid[13] iounit[4] */
        /* iounit is at offset 7 + 13 = 20 */
        *iounit = GET32(buf + 20);
        if (*iounit == 0) {
            *iounit = p9->msize - 24;  /* Default per 9P spec */
        }
        wlr_log(WLR_INFO, "9P open fid %u: iounit=%u", fid, *iounit);
    }
    pthread_mutex_unlock(&p9->lock);
    return r >= 0 ? 0 : -1;
}

/* Tread */
int p9_read(struct p9conn *p9, uint32_t fid, uint64_t offset,
            uint32_t count, uint8_t *data) {
    uint8_t *buf = p9->buf;

    pthread_mutex_lock(&p9->lock);
    buf[4] = Tread;
    PUT16(buf + 5, p9->tag++);
    PUT32(buf + 7, fid);
    PUT64(buf + 11, offset);
    PUT32(buf + 19, count);

    int rxlen = p9_rpc_locked(p9, 23, Rread);
    if (rxlen < 0) {
        pthread_mutex_unlock(&p9->lock);
        return -1;
    }

    uint32_t rcount = GET32(buf + 7);
    if (rcount > count) rcount = count;
    if (data && rcount > 0) {
        memcpy(data, buf + 11, rcount);
    }
    pthread_mutex_unlock(&p9->lock);

    return rcount;
}

/* Twrite (synchronous) */
int p9_write(struct p9conn *p9, uint32_t fid, uint64_t offset,
             const uint8_t *data, uint32_t count) {
    uint8_t *buf = p9->buf;

    pthread_mutex_lock(&p9->lock);
    if (count + 23 > p9->msize) {
        count = p9->msize - 23;
    }

    buf[4] = Twrite;
    PUT16(buf + 5, p9->tag++);
    PUT32(buf + 7, fid);
    PUT64(buf + 11, offset);
    PUT32(buf + 19, count);
    memcpy(buf + 23, data, count);

    int rxlen = p9_rpc_locked(p9, 23 + count, Rwrite);
    if (rxlen < 0) {
        pthread_mutex_unlock(&p9->lock);
        return -1;
    }

    int written = GET32(buf + 7);
    pthread_mutex_unlock(&p9->lock);
    return written;
}

/* Tclunk */
int p9_clunk(struct p9conn *p9, uint32_t fid) {
    uint8_t *buf = p9->buf;

    pthread_mutex_lock(&p9->lock);
    buf[4] = Tclunk;
    PUT16(buf + 5, p9->tag++);
    PUT32(buf + 7, fid);

    int r = p9_rpc_locked(p9, 11, Rclunk);
    pthread_mutex_unlock(&p9->lock);
    return r >= 0 ? 0 : -1;
}

/* ============== Helper: walk + open in one call ============== */

/*
 * Walk to a single-component path and open it.
 * On success, returns 0 and sets *fid_out.
 * On failure, returns -1 (fid is automatically clunked on open failure).
 */
static int p9_walk_open(struct p9conn *p9, const char *path, 
                        uint8_t mode, uint32_t *fid_out) {
    uint32_t fid = p9->next_fid++;
    const char *wnames[] = { path };
    
    if (p9_walk(p9, p9->root_fid, fid, 1, wnames) < 0) {
        wlr_log(WLR_ERROR, "p9_walk_open: walk to '%s' failed", path);
        return -1;
    }
    
    if (p9_open(p9, fid, mode, NULL) < 0) {
        wlr_log(WLR_ERROR, "p9_walk_open: open '%s' failed", path);
        p9_clunk(p9, fid);
        return -1;
    }
    
    *fid_out = fid;
    return 0;
}

/* ============== High-level file operations ============== */

int p9_read_file(struct p9conn *p9, const char *path, char *data, size_t bufsize) {
    uint32_t fid;
    
    if (p9_walk_open(p9, path, OREAD, &fid) < 0) {
        return -1;
    }
    
    /* Read contents */
    int total = 0;
    uint64_t offset = 0;
    while ((size_t)total < bufsize - 1) {
        int n = p9_read(p9, fid, offset, bufsize - 1 - total, 
                        (uint8_t*)data + total);
        if (n < 0) {
            wlr_log(WLR_ERROR, "p9_read_file: read '%s' failed", path);
            p9_clunk(p9, fid);
            return -1;
        }
        if (n == 0) break;  /* EOF */
        total += n;
        offset += n;
    }
    
    data[total] = '\0';
    p9_clunk(p9, fid);
    
    wlr_log(WLR_DEBUG, "p9_read_file: read %d bytes from '%s'", total, path);
    return total;
}

int p9_write_file(struct p9conn *p9, const char *path, const char *data, size_t len) {
    uint32_t fid;
    
    if (p9_walk_open(p9, path, OWRITE, &fid) < 0) {
        return -1;
    }
    
    /* Write contents */
    uint64_t offset = 0;
    size_t remaining = len;
    while (remaining > 0) {
        int n = p9_write(p9, fid, offset, (const uint8_t*)data + offset, remaining);
        if (n < 0) {
            wlr_log(WLR_ERROR, "p9_write_file: write '%s' failed", path);
            p9_clunk(p9, fid);
            return -1;
        }
        offset += n;
        remaining -= n;
    }
    
    p9_clunk(p9, fid);
    
    wlr_log(WLR_DEBUG, "p9_write_file: wrote %zu bytes to '%s'", len, path);
    return 0;
}

/* Pipelined write - send Twrite without waiting for response */
int p9_write_send(struct p9conn *p9, uint32_t fid, uint64_t offset,
                  const uint8_t *data, uint32_t count) {
    uint8_t header[23];

    if (count + 23 > p9->msize) {
        count = p9->msize - 23;
    }

    uint32_t txlen = 23 + count;

    PUT32(header, txlen);
    header[4] = Twrite;
    PUT16(header + 5, p9->tag++);
    PUT32(header + 7, fid);
    PUT64(header + 11, offset);
    PUT32(header + 19, count);

    /* Send header then data */
    if (p9_write_full(p9, header, 23) != 23) return -1;
    if (p9_write_full(p9, data, count) != (int)count) return -1;

    return count;
}

/* Collect one Rwrite response from pipelined writes */
int p9_write_recv(struct p9conn *p9) {
    uint8_t *buf = p9->buf;

    /* Read length */
    if (p9_read_full(p9, buf, 4) != 4) return -1;
    uint32_t rxlen = GET32(buf);
    if (rxlen < 7 || rxlen > p9->msize) return -1;

    /* Read rest of message */
    if (p9_read_full(p9, buf + 4, rxlen - 4) != (int)(rxlen - 4)) return -1;

    int type = buf[4];
    if (type == Rerror) {
        uint16_t elen = GET16(buf + 7);
        char errmsg[256];
        int copylen = (elen < 255) ? elen : 255;
        memcpy(errmsg, buf + 9, copylen);
        errmsg[copylen] = '\0';
        
        handle_9p_error(p9, errmsg);
        return -1;
    }

    if (type != Rwrite) {
        wlr_log(WLR_ERROR, "9P unexpected response: got %d, expected Rwrite", type);
        return -1;
    }

    return GET32(buf + 7);
}

/* Check if connection should be terminated */
int p9_should_shutdown(struct p9conn *p9) {
    return p9->window_deleted;
}

/* Connect to 9P server with optional TLS */
int p9_connect(struct p9conn *p9, const char *host, int port,
               struct tls_config *tls_cfg) {
    memset(p9, 0, sizeof(*p9));
    pthread_mutex_init(&p9->lock, NULL);
    p9->fd = -1;
    p9->ssl = NULL;

    /* Create socket */
    p9->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (p9->fd < 0) {
        wlr_log(WLR_ERROR, "socket: %s", strerror(errno));
        return -1;
    }

    /* Disable Nagle for lower latency */
    int one = 1;
    setsockopt(p9->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    /* Parse and connect to address */
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };

    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        wlr_log(WLR_ERROR, "Invalid address: %s", host);
        close(p9->fd);
        return -1;
    }

    wlr_log(WLR_INFO, "Connecting to %s:%d...", host, port);

    if (connect(p9->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        wlr_log(WLR_ERROR, "connect %s:%d: %s", host, port, strerror(errno));
        close(p9->fd);
        return -1;
    }

    wlr_log(WLR_INFO, "TCP connection established");

    /* Establish TLS if configured */
    if (tls_cfg && (tls_cfg->cert_file || tls_cfg->cert_fingerprint ||
                    tls_cfg->insecure)) {
        if (tls_connect(p9->fd, &p9->ssl, tls_cfg) < 0) {
            wlr_log(WLR_ERROR, "TLS connection failed");
            close(p9->fd);
            return -1;
        }
    } else if (tls_cfg == NULL || (!tls_cfg->cert_file &&
                                    !tls_cfg->cert_fingerprint &&
                                    !tls_cfg->insecure)) {
        wlr_log(WLR_INFO, "Using plaintext connection (no TLS)");
    }

    /* Allocate message buffer */
    p9->msize = P9_MSIZE;
    p9->tag = 1;
    p9->buf = malloc(p9->msize);
    if (!p9->buf) {
        wlr_log(WLR_ERROR, "Failed to allocate message buffer");
        if (p9->ssl) tls_disconnect(p9->ssl);
        close(p9->fd);
        return -1;
    }

    p9->root_fid = 0;
    p9->next_fid = 1;

    /* 9P version handshake */
    if (p9_version(p9) < 0) {
        wlr_log(WLR_ERROR, "9P version handshake failed");
        free(p9->buf);
        if (p9->ssl) tls_disconnect(p9->ssl);
        close(p9->fd);
        return -1;
    }

    /* Attach to root */
    if (p9_attach(p9, p9->root_fid, NULL) < 0) {
        wlr_log(WLR_ERROR, "9P attach failed");
        free(p9->buf);
        if (p9->ssl) tls_disconnect(p9->ssl);
        close(p9->fd);
        return -1;
    }

    return 0;
}

/* Disconnect from 9P server */
void p9_disconnect(struct p9conn *p9) {
    if (p9->ssl) {
        tls_disconnect(p9->ssl);
        p9->ssl = NULL;
    }
    if (p9->fd >= 0) {
        close(p9->fd);
        p9->fd = -1;
    }
    if (p9->buf) {
        free(p9->buf);
        p9->buf = NULL;
    }
    pthread_mutex_destroy(&p9->lock);
}
