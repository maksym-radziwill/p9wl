/*
 * scroll.c - Scroll detection and 9P scroll commands (refactored)
 *
 * Changes from original:
 * - Uses compute_scroll_rects() for all rectangle calculations
 * - Uses cmd_copy() for draw commands
 * - Simplified validation logic
 * - Extracted copy_tile_region() helper for repetitive tile copying
 */

#define _POSIX_C_SOURCE 200809L
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <wlr/util/log.h>

#include "scroll.h"
#include "send.h"
#include "compress.h"
#include "thread_pool.h"
#include "phase_correlate.h"
#include "draw/draw_helpers.h"
#include "types.h"
#include "p9/p9.h"
#include "draw/draw.h"

static struct thread_pool scroll_pool = {0};
static struct scroll_timing timing;

struct scroll_work {
    struct server *s;
    uint32_t *send_buf;
};

static struct scroll_work current_work;

static inline double get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

/*
 * Copy a rectangular region from src buffer to dst buffer.
 * Handles different strides for source and destination.
 */
static void copy_tile_region(uint32_t *dst, int dst_stride,
                             const uint32_t *src, int src_stride,
                             int src_x, int src_y, int w, int h) {
    for (int row = 0; row < h; row++) {
        memcpy(&dst[row * dst_stride],
               &src[(src_y + row) * src_stride + src_x],
               w * sizeof(uint32_t));
    }
}

/*
 * Process a single scroll region (called from thread pool).
 */
static void detect_region_scroll_worker(void *user_data, int reg_idx) {
    struct scroll_work *work = user_data;
    struct server *s = work->s;
    uint32_t *send_buf = work->send_buf;
    uint32_t *prev_buf = s->prev_framebuf;
    int width = s->width;
    
    /* Initialize region state */
    s->scroll_regions[reg_idx].detected = 0;
    s->scroll_regions[reg_idx].dx = 0;
    s->scroll_regions[reg_idx].dy = 0;
    
    int rx1 = s->scroll_regions[reg_idx].x1;
    int ry1 = s->scroll_regions[reg_idx].y1;
    int rx2 = s->scroll_regions[reg_idx].x2;
    int ry2 = s->scroll_regions[reg_idx].y2;
    
    /* Compute max scroll based on region dimensions */
    int max_scroll_x = (rx2 - rx1) / 2;
    int max_scroll_y = (ry2 - ry1) / 2;
    int max_scroll = max_scroll_x < max_scroll_y ? max_scroll_x : max_scroll_y;
    
    /* Detect scroll using phase correlation */
    struct phase_result result = phase_correlate_detect(
        send_buf, prev_buf, width, rx1, ry1, rx2, ry2, max_scroll);
    
    if (result.dx == 0 && result.dy == 0) return;
    
    int dx = result.dx, dy = result.dy;
    int abs_dx = dx < 0 ? -dx : dx;
    int abs_dy = dy < 0 ? -dy : dy;
    
    /* Reject scroll at boundaries - likely aliasing */
    if (abs_dx >= max_scroll_x || abs_dy >= max_scroll_y) return;
    
    wlr_log(WLR_INFO, "Region %d: FFT detected scroll dx=%d dy=%d", reg_idx, dx, dy);
    
    /* Compute rectangles using helper */
    struct scroll_rects rects;
    compute_scroll_rects(rx1, ry1, rx2, ry2, dx, dy, &rects);
    if (!rects.valid) return;
    
    /* Compare compressed sizes */
    int bytes_no_scroll = 0, bytes_with_scroll = 0;
    int tiles_identical_no = 0, tiles_identical_with = 0;
    uint8_t comp_buf[TILE_SIZE * TILE_SIZE * 4 + 256];
    
    int tx1 = rx1 / TILE_SIZE, ty1 = ry1 / TILE_SIZE;
    int tx2 = rx2 / TILE_SIZE, ty2 = ry2 / TILE_SIZE;
    
    for (int ty = ty1; ty < ty2; ty++) {
        for (int tx = tx1; tx < tx2; tx++) {
            int x1, y1, w, h;
            tile_bounds(tx, ty, s->width, s->height, &x1, &y1, &w, &h);
            if (w != TILE_SIZE || h != TILE_SIZE) continue;  /* Skip partial tiles */
            
            /* Check without scroll */
            if (!tile_changed(send_buf, prev_buf, width, x1, y1, w, h)) {
                tiles_identical_no++;
            } else {
                int size = compress_tile_adaptive(comp_buf, sizeof(comp_buf),
                                                   send_buf, width, prev_buf, width,
                                                   x1, y1, w, h);
                if (size < 0) size = -size;
                if (size == 0) size = w * h * 4;
                bytes_no_scroll += size;
            }
            
            /* Check with scroll */
            int src_x1 = x1 - dx, src_y1 = y1 - dy;
            
            /* Check if tile is in exposed region */
            int in_exposed = 0;
            if (dy != 0 && y1 >= rects.exp_y1 && y1 < rects.exp_y2) in_exposed = 1;
            if (dx != 0 && x1 >= rects.exp_x1 && x1 < rects.exp_x2) in_exposed = 1;
            
            if (!in_exposed && src_x1 >= 0 && src_y1 >= 0 &&
                src_x1 + w <= s->width && src_y1 + h <= s->height) {
                /* Compare against shifted previous frame */
                int identical = 1;
                for (int row = 0; row < h && identical; row++) {
                    if (memcmp(&send_buf[(y1 + row) * width + x1],
                               &prev_buf[(src_y1 + row) * width + src_x1], w * 4))
                        identical = 0;
                }
                
                if (identical) {
                    tiles_identical_with++;
                } else {
                    /* Copy tiles using helper */
                    uint32_t curr_tile[TILE_SIZE * TILE_SIZE];
                    uint32_t shifted[TILE_SIZE * TILE_SIZE];
                    copy_tile_region(curr_tile, TILE_SIZE, send_buf, width, x1, y1, w, h);
                    copy_tile_region(shifted, TILE_SIZE, prev_buf, width, src_x1, src_y1, w, h);
                    
                    int size = compress_tile_adaptive(comp_buf, sizeof(comp_buf),
                                                       curr_tile, TILE_SIZE,
                                                       shifted, TILE_SIZE,
                                                       0, 0, w, h);
                    if (size < 0) size = -size;
                    if (size == 0) size = w * h * 4;
                    bytes_with_scroll += size;
                }
            } else {
                /* Exposed area - copy using helper */
                uint32_t curr_tile[TILE_SIZE * TILE_SIZE];
                uint32_t prev_tile[TILE_SIZE * TILE_SIZE];
                copy_tile_region(curr_tile, TILE_SIZE, send_buf, width, x1, y1, w, h);
                copy_tile_region(prev_tile, TILE_SIZE, prev_buf, width, x1, y1, w, h);
                
                int size = compress_tile_adaptive(comp_buf, sizeof(comp_buf),
                                                   curr_tile, TILE_SIZE,
                                                   prev_tile, TILE_SIZE,
                                                   0, 0, w, h);
                if (size < 0) size = -size;
                if (size == 0) size = w * h * 4;
                bytes_with_scroll += size;
            }
        }
    }
    
    if (bytes_no_scroll == 0) return;
    if (bytes_with_scroll > bytes_no_scroll) {
        wlr_log(WLR_INFO, "Region %d: REJECTED - scroll costs %d more bytes",
                reg_idx, bytes_with_scroll - bytes_no_scroll);
        return;
    }
    
    s->scroll_regions[reg_idx].detected = 1;
    s->scroll_regions[reg_idx].dx = dx;
    s->scroll_regions[reg_idx].dy = dy;
    
    int saved = bytes_no_scroll - bytes_with_scroll;
    int pct = bytes_no_scroll > 0 ? (saved * 100 / bytes_no_scroll) : 0;
    wlr_log(WLR_INFO, "Region %d: ACCEPTED - saves %d bytes (%d%%)", reg_idx, saved, pct);
}

void scroll_init(void) {
    if (!scroll_pool.initialized) {
        pool_create(&scroll_pool, 0);
    }
}

void detect_scroll(struct server *s, uint32_t *send_buf) {
    if (!send_buf || !s->prev_framebuf) return;
    
    scroll_init();
    
    double t_start = get_time_us();
    memset(&timing, 0, sizeof(timing));
    
    /* Divide frame into grid with tile-aligned boundaries */
    int margin = TILE_SIZE;
    int cols = s->width / 256 > 0 ? s->width / 256 : 1;
    int rows = s->height / 256 > 0 ? s->height / 256 : 1;
    int cell_w = ((s->width - 2 * margin) / cols / TILE_SIZE) * TILE_SIZE;
    int cell_h = ((s->height - 2 * margin) / rows / TILE_SIZE) * TILE_SIZE;
    
    if (cell_w < TILE_SIZE) cell_w = TILE_SIZE;
    if (cell_h < TILE_SIZE) cell_h = TILE_SIZE;
    
    s->scroll_regions_x = cols;
    s->scroll_regions_y = rows;
    s->num_scroll_regions = 0;
    
    int max_x = (s->width / TILE_SIZE) * TILE_SIZE;
    int max_y = (s->height / TILE_SIZE) * TILE_SIZE;
    
    for (int ry = 0; ry < rows; ry++) {
        for (int rx = 0; rx < cols; rx++) {
            int x1 = (margin + rx * cell_w) / TILE_SIZE * TILE_SIZE;
            int y1 = (margin + ry * cell_h) / TILE_SIZE * TILE_SIZE;
            int x2 = (rx == cols - 1)
                ? ((s->width - margin) / TILE_SIZE) * TILE_SIZE
                : ((x1 + cell_w + TILE_SIZE - 1) / TILE_SIZE) * TILE_SIZE;
            int y2 = (ry == rows - 1)
                ? ((s->height - margin) / TILE_SIZE) * TILE_SIZE
                : ((y1 + cell_h + TILE_SIZE - 1) / TILE_SIZE) * TILE_SIZE;
            
            if (x2 > max_x) x2 = max_x;
            if (y2 > max_y) y2 = max_y;
            if (x2 - x1 < 64 || y2 - y1 < 64) continue;
            
            int idx = s->num_scroll_regions++;
            s->scroll_regions[idx].x1 = x1;
            s->scroll_regions[idx].y1 = y1;
            s->scroll_regions[idx].x2 = x2;
            s->scroll_regions[idx].y2 = y2;
            s->scroll_regions[idx].detected = 0;
            s->scroll_regions[idx].dx = 0;
            s->scroll_regions[idx].dy = 0;
        }
    }
    
    /* Process regions in parallel */
    if (s->num_scroll_regions > 0 && scroll_pool.initialized) {
        current_work.s = s;
        current_work.send_buf = send_buf;
        pool_process(&scroll_pool, detect_region_scroll_worker,
                     &current_work, s->num_scroll_regions);
    }
    
    /* Count detected scrolls */
    int detected_count = 0;
    for (int i = 0; i < s->num_scroll_regions; i++) {
        if (s->scroll_regions[i].detected) {
            detected_count++;
            timing.regions_detected++;
        }
    }
    
    timing.total_us = get_time_us() - t_start;
    timing.regions_processed = s->num_scroll_regions;
    
    if (detected_count > 0) {
        wlr_log(WLR_INFO, "Scroll detected in %d/%d regions (%.1fus)",
                detected_count, s->num_scroll_regions, timing.total_us);
    }
}

int apply_scroll_to_prevbuf(struct server *s) {
    int scrolled_count = 0;
    
    for (int i = 0; i < s->num_scroll_regions; i++) {
        if (!s->scroll_regions[i].detected) continue;
        
        int rx1 = s->scroll_regions[i].x1;
        int ry1 = s->scroll_regions[i].y1;
        int rx2 = s->scroll_regions[i].x2;
        int ry2 = s->scroll_regions[i].y2;
        int dx = s->scroll_regions[i].dx;
        int dy = s->scroll_regions[i].dy;
        
        struct scroll_rects r;
        compute_scroll_rects(rx1, ry1, rx2, ry2, dx, dy, &r);
        if (!r.valid) continue;
        
        int copy_w = r.dst_x2 - r.dst_x1;
        int abs_dy = dy < 0 ? -dy : dy;
        
        /* Shift prev_framebuf to match the blit */
        if (dy < 0) {
            for (int y = r.dst_y1; y < r.dst_y2; y++) {
                memmove(&s->prev_framebuf[y * s->width + r.dst_x1],
                        &s->prev_framebuf[(y + abs_dy) * s->width + r.src_x1],
                        copy_w * sizeof(uint32_t));
            }
        } else if (dy > 0) {
            for (int y = r.dst_y2 - 1; y >= r.dst_y1; y--) {
                memmove(&s->prev_framebuf[y * s->width + r.dst_x1],
                        &s->prev_framebuf[(y - abs_dy) * s->width + r.src_x1],
                        copy_w * sizeof(uint32_t));
            }
        } else if (dx != 0) {
            for (int y = r.dst_y1; y < r.dst_y2; y++) {
                memmove(&s->prev_framebuf[y * s->width + r.dst_x1],
                        &s->prev_framebuf[y * s->width + r.src_x1],
                        copy_w * sizeof(uint32_t));
            }
        }
        
        /* Mark exposed strips as dirty */
        if (r.exp_y2 > r.exp_y1) {
            for (int y = r.exp_y1; y < r.exp_y2; y++) {
                for (int x = rx1; x < rx2; x++) {
                    s->prev_framebuf[y * s->width + x] = 0xDEADBEEF;
                }
            }
        }
        if (r.exp_x2 > r.exp_x1) {
            for (int y = ry1; y < ry2; y++) {
                for (int x = r.exp_x1; x < r.exp_x2; x++) {
                    s->prev_framebuf[y * s->width + x] = 0xDEADBEEF;
                }
            }
        }
        
        scrolled_count++;
    }
    
    return scrolled_count;
}

int write_scroll_commands(struct server *s, uint8_t *batch, size_t max_size) {
    struct draw_state *draw = &s->draw;
    int off = 0;
    
    for (int i = 0; i < s->num_scroll_regions; i++) {
        if (!s->scroll_regions[i].detected) continue;
        
        int rx1 = s->scroll_regions[i].x1;
        int ry1 = s->scroll_regions[i].y1;
        int rx2 = s->scroll_regions[i].x2;
        int ry2 = s->scroll_regions[i].y2;
        int dx = s->scroll_regions[i].dx;
        int dy = s->scroll_regions[i].dy;
        
        struct scroll_rects r;
        compute_scroll_rects(rx1, ry1, rx2, ry2, dx, dy, &r);
        if (!r.valid) continue;
        
        /* Validate rectangles */
        if (r.src_y2 <= r.src_y1 || r.dst_y2 <= r.dst_y1) continue;
        if (r.src_x2 <= r.src_x1 || r.dst_x2 <= r.dst_x1) continue;
        
        if (off + 45 > (int)max_size) {
            wlr_log(WLR_ERROR, "Scroll batch overflow");
            break;
        }
        
        off += cmd_copy(batch + off, draw->image_id, draw->image_id,
                       draw->opaque_id,
                       r.dst_x1, r.dst_y1, r.dst_x2, r.dst_y2,
                       r.src_x1, r.src_y1);
        
        wlr_log(WLR_DEBUG, "Scroll %d: dy=%d dx=%d", i, dy, dx);
    }
    
    return off;
}

const struct scroll_timing *scroll_get_timing(void) {
    return &timing;
}

void scroll_cleanup(void) {
    pool_destroy(&scroll_pool);
    phase_correlate_cleanup();
}
