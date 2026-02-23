/*
 * draw_cmd.h - Plan 9 draw protocol command helpers
 *
 * Inline helpers for building Plan 9 draw protocol commands. Each function
 * writes a complete command to a buffer and returns the number of bytes
 * written.
 *
 * Plan 9 Draw Protocol Overview:
 *
 *   The draw device accepts single-letter commands followed by binary data.
 *   All multi-byte integers are little-endian. Coordinates are signed 32-bit.
 *
 *   Commands used by this module:
 *
 *     'd' (draw):   Copy src to dst through mask
 *     'b' (alloc):  Allocate a new image
 *     'f' (free):   Free an image
 *     'v' (flush):  Flush pending operations to display
 *     'n' (name):   Look up image by name
 *     'y' (load):   Load uncompressed pixel data
 *     'Y' (loadc):  Load compressed pixel data
 *
 * Image IDs:
 *
 *   Images are referenced by 32-bit IDs. ID 0 is the screen.
 *   Clients allocate their own IDs starting from 1.
 *
 * Channel Formats:
 *
 *   Pixel formats are encoded as 32-bit channel descriptors:
 *     CHAN_XRGB32 (0x68081828): 8 bits each for padding, R, G, B
 *     CHAN_ARGB32 (0x48081828): 8 bits each for alpha, R, G, B
 *     CHAN_GREY1  (0x00000031): 1-bit grayscale (for masks)
 *
 * Usage:
 *
 *   uint8_t cmd[64];
 *   int len = draw_cmd(cmd, dst_id, src_id, mask_id, x1, y1, x2, y2);
 *   p9_write(p9, data_fid, 0, cmd, len);
 */

#ifndef DRAW_CMD_H
#define DRAW_CMD_H

#include <stdint.h>
#include <string.h>

/* ============== Byte Order Macros ============== */

/* Write 32-bit little-endian value to buffer */
#ifndef PUT32
#define PUT32(p, v) do { \
    (p)[0] = (v) & 0xFF; \
    (p)[1] = ((v) >> 8) & 0xFF; \
    (p)[2] = ((v) >> 16) & 0xFF; \
    (p)[3] = ((v) >> 24) & 0xFF; \
} while(0)
#endif

/* ============== Draw Commands ============== */

/*
 * Emit 'd' (draw) command: copy src to dst through mask.
 *
 * Copies pixels from src to dst, using mask for transparency.
 * Source and mask points default to (0,0).
 *
 * buf:  output buffer (must have 45 bytes available)
 * dst:  destination image ID
 * src:  source image ID
 * mask: mask image ID (use opaque_id for no transparency)
 * x1, y1: destination rectangle top-left
 * x2, y2: destination rectangle bottom-right (exclusive)
 *
 * Wire format: d dst[4] src[4] mask[4] r[16] sp[8] mp[8]
 *
 * Returns bytes written (always 45).
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
 * Emit 'd' command with explicit source point offset.
 *
 * Like draw_cmd() but allows specifying where in the source image
 * to read from. Used for scrolling and region copies.
 *
 * buf:       output buffer (must have 45 bytes available)
 * dst:       destination image ID
 * src:       source image ID
 * mask:      mask image ID
 * x1, y1:    destination rectangle top-left
 * x2, y2:    destination rectangle bottom-right
 * sp_x, sp_y: source point (where to read from in src)
 *
 * Returns bytes written (always 45).
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

/* ============== Image Management Commands ============== */

/*
 * Emit 'b' (allocate) command for a new image.
 *
 * Creates a new image with the specified properties.
 *
 * buf:   output buffer (must have 55 bytes available)
 * id:    image ID to assign (client-chosen, must be unique)
 * chan:  channel format (CHAN_XRGB32, CHAN_ARGB32, CHAN_GREY1)
 * repl:  if non-zero, image is replicated (tiled) when drawn
 * x1, y1: image rectangle top-left (usually 0,0)
 * x2, y2: image rectangle bottom-right (width = x2-x1, height = y2-y1)
 * color: initial fill color (ARGB format)
 *
 * For replicated images (repl=1), clipr is set to huge bounds so
 * the image tiles infinitely. Used for solid color fills.
 *
 * Wire format: b id[4] screenid[4] refresh[1] chan[4] repl[1] r[16] clipr[16] color[4]
 *
 * Returns bytes written (always 55).
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
 * Emit 'f' (free) command to release an image.
 *
 * Frees the image and its server-side resources.
 * The ID can be reused after this command.
 *
 * buf: output buffer (must have 5 bytes available)
 * id:  image ID to free
 *
 * Wire format: f id[4]
 *
 * Returns bytes written (always 5).
 */
static inline int free_image_cmd(uint8_t *buf, uint32_t id) {
    buf[0] = 'f';
    PUT32(buf + 1, id);
    return 5;
}

/*
 * Emit 'n' (name lookup) command.
 *
 * Looks up an image by name (e.g., window name in rio).
 * If found, assigns the specified ID to reference it.
 *
 * buf:     output buffer (must have 6 + namelen bytes available)
 * id:      image ID to assign if lookup succeeds
 * name:    name string to look up (not null-terminated in wire format)
 * namelen: length of name in bytes (max 255)
 *
 * Wire format: n id[4] namelen[1] name[namelen]
 *
 * Returns bytes written (6 + namelen).
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

/* ============== Display Commands ============== */

/*
 * Emit 'v' (flush) command.
 *
 * Flushes all pending draw operations to the display.
 * Required after a batch of commands to make them visible.
 *
 * buf: output buffer (must have 1 byte available)
 *
 * Wire format: v
 *
 * Returns bytes written (always 1).
 */
static inline int flush_cmd(uint8_t *buf) {
    buf[0] = 'v';
    return 1;
}

/* ============== Pixel Data Commands ============== */

/*
 * Emit 'y' (uncompressed load) command header.
 *
 * Writes header for loading raw pixel data into an image.
 * Caller must append (x2-x1) * (y2-y1) * bytes_per_pixel
 * of pixel data after the header.
 *
 * buf:       output buffer (must have 21 bytes + pixel data available)
 * id:        target image ID
 * x1, y1:    target rectangle top-left
 * x2, y2:    target rectangle bottom-right
 *
 * Wire format: y id[4] r.min.x[4] r.min.y[4] r.max.x[4] r.max.y[4] data[...]
 *
 * Returns header bytes written (always 21).
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
 * Emit 'Y' (compressed load) command header.
 *
 * Writes header for loading LZ77-compressed pixel data into an image.
 * Caller must append compressed data after the header.
 * See compress.h for compression functions.
 *
 * buf:       output buffer (must have 21 bytes + compressed data available)
 * id:        target image ID
 * x1, y1:    target rectangle top-left
 * x2, y2:    target rectangle bottom-right
 *
 * Wire format: Y id[4] r.min.x[4] r.min.y[4] r.max.x[4] r.max.y[4] data[...]
 *
 * Returns header bytes written (always 21).
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



/* ============== Channel Format Constants ============== */

/*
 * Pixel channel format descriptors.
 *
 * These encode the pixel format for Plan 9 images.
 * Format: each nibble describes one channel's depth.
 */
#define CHAN_XRGB32     0x68081828  /* x8r8g8b8 - 32bpp with padding */
#define CHAN_ARGB32     0x48081828  /* a8r8g8b8 - 32bpp with alpha */
#define CHAN_GREY1      0x00000031  /* k1 - 1-bit grayscale (masks) */

#endif /* DRAW_CMD_H */
