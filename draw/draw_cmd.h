/*
 * draw_cmd.h - Plan 9 draw protocol command helpers
 *
 * Inline helpers to reduce repetition when building draw commands.
 * All functions return the number of bytes written.
 */

#ifndef DRAW_CMD_H
#define DRAW_CMD_H

#include <stdint.h>
#include <string.h>

/* Byte order macros (Plan 9 uses little-endian) */
#ifndef PUT32
#define PUT32(p, v) do { \
    (p)[0] = (v) & 0xFF; \
    (p)[1] = ((v) >> 8) & 0xFF; \
    (p)[2] = ((v) >> 16) & 0xFF; \
    (p)[3] = ((v) >> 24) & 0xFF; \
} while(0)
#endif

/*
 * Emit 'd' (draw) command: copy src to dst through mask
 * Format: d dst[4] src[4] mask[4] r.min.x[4] r.min.y[4] r.max.x[4] r.max.y[4] sp[8] mp[8]
 * Returns: bytes written (always 45)
 */
static inline int draw_cmd(uint8_t *buf, uint32_t dst, uint32_t src, uint32_t mask,
                           int x1, int y1, int x2, int y2) {
    int off = 0;
    buf[off++] = 'd';
    PUT32(buf + off, dst); off += 4;
    PUT32(buf + off, src); off += 4;
    PUT32(buf + off, mask); off += 4;
    PUT32(buf + off, x1); off += 4;
    PUT32(buf + off, y1); off += 4;
    PUT32(buf + off, x2); off += 4;
    PUT32(buf + off, y2); off += 4;
    PUT32(buf + off, 0); off += 4;  /* sp.x */
    PUT32(buf + off, 0); off += 4;  /* sp.y */
    PUT32(buf + off, 0); off += 4;  /* mp.x */
    PUT32(buf + off, 0); off += 4;  /* mp.y */
    return off;  /* 45 bytes */
}

/*
 * Emit 'd' command with source point offset
 */
static inline int draw_cmd_sp(uint8_t *buf, uint32_t dst, uint32_t src, uint32_t mask,
                              int x1, int y1, int x2, int y2, int sp_x, int sp_y) {
    int off = 0;
    buf[off++] = 'd';
    PUT32(buf + off, dst); off += 4;
    PUT32(buf + off, src); off += 4;
    PUT32(buf + off, mask); off += 4;
    PUT32(buf + off, x1); off += 4;
    PUT32(buf + off, y1); off += 4;
    PUT32(buf + off, x2); off += 4;
    PUT32(buf + off, y2); off += 4;
    PUT32(buf + off, sp_x); off += 4;
    PUT32(buf + off, sp_y); off += 4;
    PUT32(buf + off, 0); off += 4;  /* mp.x */
    PUT32(buf + off, 0); off += 4;  /* mp.y */
    return off;
}

/*
 * Emit 'b' (allocate) command for a new image
 * Format: b id[4] screenid[4] refresh[1] chan[4] repl[1] r[16] clipr[16] color[4]
 * Returns: bytes written (always 55)
 */
static inline int alloc_image_cmd(uint8_t *buf, uint32_t id, uint32_t chan, int repl,
                                  int x1, int y1, int x2, int y2, uint32_t color) {
    int off = 0;
    buf[off++] = 'b';
    PUT32(buf + off, id); off += 4;
    PUT32(buf + off, 0); off += 4;    /* screenid = 0 */
    buf[off++] = 0;                    /* refresh = 0 */
    PUT32(buf + off, chan); off += 4;
    buf[off++] = repl ? 1 : 0;
    /* rect r */
    PUT32(buf + off, x1); off += 4;
    PUT32(buf + off, y1); off += 4;
    PUT32(buf + off, x2); off += 4;
    PUT32(buf + off, y2); off += 4;
    /* clipr - for repl=1, use huge rect; otherwise same as r */
    if (repl) {
        PUT32(buf + off, (uint32_t)-0x3FFFFFFF); off += 4;
        PUT32(buf + off, (uint32_t)-0x3FFFFFFF); off += 4;
        PUT32(buf + off, 0x3FFFFFFF); off += 4;
        PUT32(buf + off, 0x3FFFFFFF); off += 4;
    } else {
        PUT32(buf + off, x1); off += 4;
        PUT32(buf + off, y1); off += 4;
        PUT32(buf + off, x2); off += 4;
        PUT32(buf + off, y2); off += 4;
    }
    PUT32(buf + off, color); off += 4;
    return off;  /* 55 bytes */
}

/*
 * Emit 'f' (free) command
 * Format: f id[4]
 * Returns: bytes written (always 5)
 */
static inline int free_image_cmd(uint8_t *buf, uint32_t id) {
    buf[0] = 'f';
    PUT32(buf + 1, id);
    return 5;
}

/*
 * Emit 'v' (flush) command
 * Returns: bytes written (always 1)
 */
static inline int flush_cmd(uint8_t *buf) {
    buf[0] = 'v';
    return 1;
}

/*
 * Emit 'n' (name lookup) command
 * Format: n id[4] namelen[1] name[namelen]
 * Returns: bytes written
 */
static inline int name_cmd(uint8_t *buf, uint32_t id, const char *name, int namelen) {
    int off = 0;
    buf[off++] = 'n';
    PUT32(buf + off, id); off += 4;
    buf[off++] = (uint8_t)namelen;
    memcpy(buf + off, name, namelen);
    off += namelen;
    return off;
}

/*
 * Emit 'y' (uncompressed write) command header
 * Format: y id[4] r.min.x[4] r.min.y[4] r.max.x[4] r.max.y[4] data[...]
 * Returns: header bytes written (always 21), caller appends pixel data
 */
static inline int write_raw_header(uint8_t *buf, uint32_t id,
                                   int x1, int y1, int x2, int y2) {
    int off = 0;
    buf[off++] = 'y';
    PUT32(buf + off, id); off += 4;
    PUT32(buf + off, x1); off += 4;
    PUT32(buf + off, y1); off += 4;
    PUT32(buf + off, x2); off += 4;
    PUT32(buf + off, y2); off += 4;
    return off;  /* 21 bytes */
}

/*
 * Emit 'Y' (compressed write) command header
 * Format: Y id[4] r.min.x[4] r.min.y[4] r.max.x[4] r.max.y[4] data[...]
 * Returns: header bytes written (always 21), caller appends compressed data
 */
static inline int write_compressed_header(uint8_t *buf, uint32_t id,
                                          int x1, int y1, int x2, int y2) {
    int off = 0;
    buf[off++] = 'Y';
    PUT32(buf + off, id); off += 4;
    PUT32(buf + off, x1); off += 4;
    PUT32(buf + off, y1); off += 4;
    PUT32(buf + off, x2); off += 4;
    PUT32(buf + off, y2); off += 4;
    return off;  /* 21 bytes */
}

/*
 * Emit 's' (scroll/copy within image) command
 * Format: s dst[4] src[4] mask[4] r[16] sp[8] mp[8]
 * Note: This copies a region within an image (used for scrolling)
 */
static inline int scroll_cmd(uint8_t *buf, uint32_t dst, uint32_t src, uint32_t mask,
                             int x1, int y1, int x2, int y2, int sp_x, int sp_y) {
    int off = 0;
    buf[off++] = 's';
    PUT32(buf + off, dst); off += 4;
    PUT32(buf + off, src); off += 4;
    PUT32(buf + off, mask); off += 4;
    PUT32(buf + off, x1); off += 4;
    PUT32(buf + off, y1); off += 4;
    PUT32(buf + off, x2); off += 4;
    PUT32(buf + off, y2); off += 4;
    PUT32(buf + off, sp_x); off += 4;
    PUT32(buf + off, sp_y); off += 4;
    PUT32(buf + off, 0); off += 4;  /* mp.x */
    PUT32(buf + off, 0); off += 4;  /* mp.y */
    return off;
}

/* Channel format constants */
#define CHAN_XRGB32     0x68081828  /* x8r8g8b8 */
#define CHAN_ARGB32     0x48081828  /* a8r8g8b8 */
#define CHAN_GREY1      0x00000031  /* k1 */

#endif /* DRAW_CMD_H */
