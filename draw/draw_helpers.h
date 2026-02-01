/*
 * draw_helpers.h - Common helpers for Plan 9 draw protocol
 *
 * Inline functions for building draw commands and common patterns.
 *
 * IMPORTANT: This header requires TILE_SIZE to be defined before inclusion.
 * Include types.h first, or define TILE_SIZE explicitly.
 */

#ifndef DRAW_HELPERS_H
#define DRAW_HELPERS_H

#include <stdint.h>
#include <string.h>

/* Require TILE_SIZE to be defined (typically via types.h) */
#ifndef TILE_SIZE
#error "TILE_SIZE must be defined before including draw_helpers.h - include types.h first"
#endif

/* Byte order macros (assuming these exist elsewhere, included for completeness) */
#ifndef PUT32
#define PUT32(p, v) do { \
    (p)[0] = (v) & 0xFF; \
    (p)[1] = ((v) >> 8) & 0xFF; \
    (p)[2] = ((v) >> 16) & 0xFF; \
    (p)[3] = ((v) >> 24) & 0xFF; \
} while(0)
#endif

/* ============== Draw Command Builders ============== */

/*
 * Build a 'd' (draw/blit) command. Returns command size (45 bytes).
 * 
 * dst_id: destination image
 * src_id: source image  
 * mask_id: mask image (use opaque_id for no transparency)
 * dx1,dy1,dx2,dy2: destination rectangle
 * sx,sy: source origin
 * mx,my: mask origin (usually 0,0)
 */
static inline int cmd_draw(uint8_t *buf,
                           uint32_t dst_id, uint32_t src_id, uint32_t mask_id,
                           int dx1, int dy1, int dx2, int dy2,
                           int sx, int sy, int mx, int my) {
    buf[0] = 'd';
    PUT32(buf + 1, dst_id);
    PUT32(buf + 5, src_id);
    PUT32(buf + 9, mask_id);
    PUT32(buf + 13, dx1);
    PUT32(buf + 17, dy1);
    PUT32(buf + 21, dx2);
    PUT32(buf + 25, dy2);
    PUT32(buf + 29, sx);
    PUT32(buf + 33, sy);
    PUT32(buf + 37, mx);
    PUT32(buf + 41, my);
    return 45;
}

/*
 * Build a 'd' command for simple copy (mask at origin).
 */
static inline int cmd_copy(uint8_t *buf,
                           uint32_t dst_id, uint32_t src_id, uint32_t mask_id,
                           int dx1, int dy1, int dx2, int dy2,
                           int sx, int sy) {
    return cmd_draw(buf, dst_id, src_id, mask_id, dx1, dy1, dx2, dy2, sx, sy, 0, 0);
}

/*
 * Build a 'd' command for solid fill (replicated 1x1 source).
 */
static inline int cmd_fill(uint8_t *buf,
                           uint32_t dst_id, uint32_t color_id, uint32_t mask_id,
                           int x1, int y1, int x2, int y2) {
    return cmd_draw(buf, dst_id, color_id, mask_id, x1, y1, x2, y2, 0, 0, 0, 0);
}

/*
 * Build a 'Y' (compressed load) command header. Returns header size (21 bytes).
 * Caller must append compressed data after this.
 */
static inline int cmd_load_hdr(uint8_t *buf, uint32_t img_id,
                               int x1, int y1, int x2, int y2) {
    buf[0] = 'Y';
    PUT32(buf + 1, img_id);
    PUT32(buf + 5, x1);
    PUT32(buf + 9, y1);
    PUT32(buf + 13, x2);
    PUT32(buf + 17, y2);
    return 21;
}

/*
 * Build a 'y' (uncompressed load) command header. Returns header size (21 bytes).
 * Caller must append raw pixel data after this.
 */
static inline int cmd_loadraw_hdr(uint8_t *buf, uint32_t img_id,
                                  int x1, int y1, int x2, int y2) {
    buf[0] = 'y';
    PUT32(buf + 1, img_id);
    PUT32(buf + 5, x1);
    PUT32(buf + 9, y1);
    PUT32(buf + 13, x2);
    PUT32(buf + 17, y2);
    return 21;
}

/*
 * Build a 'v' (flush) command. Returns 1.
 */
static inline int cmd_flush(uint8_t *buf) {
    buf[0] = 'v';
    return 1;
}

/* ============== Tile Utilities ============== */

/*
 * Compute tile bounds, clamped to frame dimensions.
 *
 * tx, ty:    tile coordinates (0-indexed)
 * frame_w, frame_h: frame dimensions in pixels
 * x1, y1:    output - top-left pixel coordinate
 * w, h:      output - tile width and height (may be < TILE_SIZE at edges)
 */
static inline void tile_bounds(int tx, int ty, int frame_w, int frame_h,
                               int *x1, int *y1, int *w, int *h) {
    *x1 = tx * TILE_SIZE;
    *y1 = ty * TILE_SIZE;
    int x2 = *x1 + TILE_SIZE;
    int y2 = *y1 + TILE_SIZE;
    if (x2 > frame_w) x2 = frame_w;
    if (y2 > frame_h) y2 = frame_h;
    *w = x2 - *x1;
    *h = y2 - *y1;
}

/*
 * Check if a tile has changed between two buffers.
 *
 * curr, prev: pixel buffers to compare (XRGB32 format)
 * stride:     buffer stride in pixels
 * x1, y1:     top-left corner of tile
 * w, h:       tile dimensions
 *
 * Returns non-zero if any pixel differs.
 */
static inline int tile_changed(uint32_t *curr, uint32_t *prev, int stride,
                               int x1, int y1, int w, int h) {
    for (int y = 0; y < h; y++) {
        if (memcmp(&curr[(y1 + y) * stride + x1],
                   &prev[(y1 + y) * stride + x1], w * 4) != 0) {
            return 1;
        }
    }
    return 0;
}

/* ============== Scroll Rectangle Calculation ============== */

/*
 * Scroll rectangle geometry for copy operations.
 *
 * When scrolling, we need three rectangles:
 * - src: where pixels come from in the source buffer
 * - dst: where pixels land in the destination buffer
 * - exp: exposed region that needs fresh content (tile-aligned)
 */
struct scroll_rects {
    int src_x1, src_y1, src_x2, src_y2;  /* Source rectangle */
    int dst_x1, dst_y1, dst_x2, dst_y2;  /* Destination rectangle */
    int exp_x1, exp_y1, exp_x2, exp_y2;  /* Exposed region (tile-aligned) */
    int valid;  /* Set to 0 if scroll is degenerate */
};

/*
 * Compute source, destination, and exposed rectangles for a scroll.
 *
 * rx1,ry1,rx2,ry2: region bounds
 * dx,dy: scroll delta (positive = content moves right/down)
 * r: output structure with computed rectangles
 *
 * Sets r->valid to 0 if the scroll is degenerate (region too small
 * or scroll distance exceeds region size).
 */
static inline void compute_scroll_rects(int rx1, int ry1, int rx2, int ry2,
                                        int dx, int dy, struct scroll_rects *r) {
    int rw = rx2 - rx1;
    int rh = ry2 - ry1;
    int abs_dx = dx < 0 ? -dx : dx;
    int abs_dy = dy < 0 ? -dy : dy;
    
    /* Check for degenerate scroll */
    if (rw < 16 || rh < 16 || abs_dx >= rw || abs_dy >= rh) {
        r->valid = 0;
        return;
    }
    r->valid = 1;
    
    /* Destination rectangle (where pixels land) */
    r->dst_x1 = rx1;
    r->dst_x2 = rx2;
    r->dst_y1 = ry1;
    r->dst_y2 = ry2;
    
    /* Source rectangle (where pixels come from) */
    r->src_x1 = rx1;
    r->src_x2 = rx2;
    r->src_y1 = ry1;
    r->src_y2 = ry2;
    
    /* Adjust for vertical scroll */
    if (dy < 0) {
        r->src_y1 = ry1 + abs_dy;
        r->dst_y2 = ry2 - abs_dy;
    } else if (dy > 0) {
        r->src_y2 = ry2 - abs_dy;
        r->dst_y1 = ry1 + abs_dy;
    }
    
    /* Adjust for horizontal scroll */
    if (dx < 0) {
        r->src_x1 = rx1 + abs_dx;
        r->dst_x2 = rx2 - abs_dx;
    } else if (dx > 0) {
        r->src_x2 = rx2 - abs_dx;
        r->dst_x1 = rx1 + abs_dx;
    }
    
    /* Exposed region (tile-aligned for proper invalidation) */
    r->exp_x1 = r->exp_x2 = 0;
    r->exp_y1 = r->exp_y2 = 0;
    
    if (dy < 0) {
        r->exp_y1 = (r->dst_y2 / TILE_SIZE) * TILE_SIZE;
        r->exp_y2 = ry2;
    } else if (dy > 0) {
        r->exp_y1 = ry1;
        r->exp_y2 = ((r->dst_y1 + TILE_SIZE - 1) / TILE_SIZE) * TILE_SIZE;
    }
    
    if (dx < 0) {
        r->exp_x1 = (r->dst_x2 / TILE_SIZE) * TILE_SIZE;
        r->exp_x2 = rx2;
    } else if (dx > 0) {
        r->exp_x1 = rx1;
        r->exp_x2 = ((r->dst_x1 + TILE_SIZE - 1) / TILE_SIZE) * TILE_SIZE;
    }
}

/* ============== XDG Surface Validation ============== */

/*
 * Check if an XDG toplevel's surface chain is valid.
 */
#define XDG_VALID(xdg) \
    ((xdg) && (xdg)->base && (xdg)->base->surface)

/*
 * Check if an XDG toplevel's surface is mapped.
 */
#define XDG_MAPPED(xdg) \
    (XDG_VALID(xdg) && (xdg)->base->surface->mapped)

/*
 * Get surface from XDG toplevel (NULL if invalid).
 */
#define XDG_SURFACE(xdg) \
    (XDG_VALID(xdg) ? (xdg)->base->surface : NULL)

#endif /* DRAW_HELPERS_H */
