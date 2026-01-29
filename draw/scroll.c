/*
 * scroll.c - Scroll detection and 9P scroll commands
 *
 * Detects scrolling regions and sends efficient blit commands.
 * Uses thread pool for parallel region processing.
 * Uses FFT-based phase correlation for motion detection.
 *
 * With integer output scaling (k = ceil(scale)), scroll offsets are
 * guaranteed to be multiples of k. The FFT with k-based downsampling
 * directly returns the correct k-aligned offset - no refinement needed.
 */

#define _POSIX_C_SOURCE 200809L
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>
#include <wlr/util/log.h>

#include "scroll.h"
#include "send.h"
#include "compress.h"
#include "thread_pool.h"
#include "phase_correlate.h"
#include "types.h"
#include "p9/p9.h"
#include "draw/draw.h"

#define REGION_STRIDE SCROLL_REGION_SIZE

/* Thread pool for parallel region processing */
static struct thread_pool scroll_pool = {0};

/* Timing statistics */
static struct scroll_timing timing;

/* Work context for parallel processing */
struct scroll_work {
    struct server *s;
    uint32_t *send_buf;
    int k;  /* Integer output scale = ceil(scale) */
};

static struct scroll_work current_work;

static inline double get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

/*
 * Count identical tiles for a scroll offset.
 * Used to quickly verify if scroll provides benefit.
 */
static int count_identical_tiles(
    struct server *s,
    uint32_t *send_buf,
    uint32_t *prev_buf,
    int rx1, int ry1, int rx2, int ry2,
    int dx, int dy)
{
    int width = s->width;
    int abs_dx = dx < 0 ? -dx : dx;
    int abs_dy = dy < 0 ? -dy : dy;
    int identical = 0;
    
    int tx1 = rx1 / TILE_SIZE;
    int ty1 = ry1 / TILE_SIZE;
    int tx2 = rx2 / TILE_SIZE;
    int ty2 = ry2 / TILE_SIZE;
    
    /* Compute dst bounds */
    int dst_x1 = rx1, dst_x2 = rx2;
    int dst_y1, dst_y2;
    
    if (dy < 0) {
        dst_y1 = ry1;
        dst_y2 = ry2 - abs_dy;
    } else if (dy > 0) {
        dst_y1 = ry1 + abs_dy;
        dst_y2 = ry2;
    } else {
        dst_y1 = ry1;
        dst_y2 = ry2;
    }
    
    if (dx < 0) {
        dst_x1 = rx1;
        dst_x2 = rx2 - abs_dx;
    } else if (dx > 0) {
        dst_x1 = rx1 + abs_dx;
        dst_x2 = rx2;
    }
    
    /* Compute exposed boundaries */
    int exposed_x1 = 0, exposed_x2 = 0;
    int exposed_y1 = 0, exposed_y2 = 0;
    
    if (dy < 0) {
        exposed_y1 = (dst_y2 / TILE_SIZE) * TILE_SIZE;
        exposed_y2 = ry2;
    } else if (dy > 0) {
        exposed_y1 = ry1;
        exposed_y2 = ((dst_y1 + TILE_SIZE - 1) / TILE_SIZE) * TILE_SIZE;
    }
    
    if (dx < 0) {
        exposed_x1 = (dst_x2 / TILE_SIZE) * TILE_SIZE;
        exposed_x2 = rx2;
    } else if (dx > 0) {
        exposed_x1 = rx1;
        exposed_x2 = ((dst_x1 + TILE_SIZE - 1) / TILE_SIZE) * TILE_SIZE;
    }
    
    for (int ty = ty1; ty < ty2; ty++) {
        for (int tx = tx1; tx < tx2; tx++) {
            int x1 = tx * TILE_SIZE;
            int y1 = ty * TILE_SIZE;
            
            if (x1 + TILE_SIZE > s->width || y1 + TILE_SIZE > s->height) continue;
            
            int src_x1 = x1 - dx;
            int src_y1 = y1 - dy;
            
            /* Check if in exposed region */
            int in_exposed = 0;
            if (dy != 0 && y1 >= exposed_y1 && y1 < exposed_y2) in_exposed = 1;
            if (dx != 0 && x1 >= exposed_x1 && x1 < exposed_x2) in_exposed = 1;
            
            if (!in_exposed && src_x1 >= 0 && src_y1 >= 0 && 
                src_x1 + TILE_SIZE <= s->width && src_y1 + TILE_SIZE <= s->height) {
                
                int is_identical = 1;
                for (int row = 0; row < TILE_SIZE && is_identical; row++) {
                    if (memcmp(&send_buf[(y1 + row) * width + x1],
                               &prev_buf[(src_y1 + row) * width + src_x1],
                               TILE_SIZE * sizeof(uint32_t)) != 0) {
                        is_identical = 0;
                    }
                }
                if (is_identical) identical++;
            }
        }
    }
    
    return identical;
}

/*
 * Process a single scroll region (called from thread pool workers).
 *
 * With integer output scaling k = ceil(scale), scroll offsets are
 * multiples of k. The FFT with k-based downsampling directly returns
 * the correct k-aligned offset - just validate it saves bytes.
 */
static void detect_region_scroll_worker(void *user_data, int reg_idx) {
    struct scroll_work *work = user_data;
    struct server *s = work->s;
    uint32_t *send_buf = work->send_buf;
    uint32_t *prev_buf = s->prev_framebuf;
    int width = s->width;
    int k = work->k;
    
    /* Region bounds are already k*TILE_SIZE-aligned from detect_scroll */
    int rx1 = s->scroll_regions[reg_idx].x1;
    int ry1 = s->scroll_regions[reg_idx].y1;
    int rx2 = s->scroll_regions[reg_idx].x2;
    int ry2 = s->scroll_regions[reg_idx].y2;
    
    s->scroll_regions[reg_idx].detected = 0;
    s->scroll_regions[reg_idx].dx = 0;
    s->scroll_regions[reg_idx].dy = 0;
    
    /* Compute max scroll based on region dimensions (FFT limit is half the window) */
    int max_scroll_x = (rx2 - rx1) / 2;
    int max_scroll_y = (ry2 - ry1) / 2;
    int max_scroll = max_scroll_x < max_scroll_y ? max_scroll_x : max_scroll_y;
    
    /* Detect scroll using phase correlation with k-based downsampling */
    struct phase_result result = phase_correlate_detect(
        send_buf, prev_buf, width,
        rx1, ry1, rx2, ry2,
        max_scroll, k
    );
    
    if (result.dx == 0 && result.dy == 0) return;
    
    int dx = result.dx;
    int dy = result.dy;
    
    /* Reject scroll at boundaries - likely aliasing artifact */
    int abs_dx = dx < 0 ? -dx : dx;
    int abs_dy = dy < 0 ? -dy : dy;
    if (abs_dx >= max_scroll_x || abs_dy >= max_scroll_y) {
        return;
    }
    
    wlr_log(WLR_INFO, "Region %d: FFT detected scroll dx=%d dy=%d (k=%d)", 
            reg_idx, dx, dy, k);
    
    /* Count identical tiles without scroll (baseline) */
    int tiles_identical_no = 0;
    
    int tx1 = rx1 / TILE_SIZE;
    int ty1 = ry1 / TILE_SIZE;
    int tx2 = rx2 / TILE_SIZE;
    int ty2 = ry2 / TILE_SIZE;
    
    for (int ty = ty1; ty < ty2; ty++) {
        for (int tx = tx1; tx < tx2; tx++) {
            int x1 = tx * TILE_SIZE;
            int y1 = ty * TILE_SIZE;
            
            if (x1 + TILE_SIZE > s->width || y1 + TILE_SIZE > s->height) continue;
            
            int identical = 1;
            for (int row = 0; row < TILE_SIZE && identical; row++) {
                if (memcmp(&send_buf[(y1 + row) * width + x1],
                           &prev_buf[(y1 + row) * width + x1],
                           TILE_SIZE * sizeof(uint32_t)) != 0) {
                    identical = 0;
                }
            }
            if (identical) tiles_identical_no++;
        }
    }
    
    /* Count identical tiles with scroll */
    int tiles_identical_with = count_identical_tiles(
        s, send_buf, prev_buf, rx1, ry1, rx2, ry2, dx, dy);
    
    /* Reject if scroll doesn't improve tile matching */
    if (tiles_identical_with <= tiles_identical_no) {
        wlr_log(WLR_DEBUG, "Region %d: REJECTED dx=%d dy=%d - no improvement (%d vs %d identical tiles)",
                reg_idx, dx, dy, tiles_identical_with, tiles_identical_no);
        return;
    }
    
    s->scroll_regions[reg_idx].detected = 1;
    s->scroll_regions[reg_idx].dx = dx;
    s->scroll_regions[reg_idx].dy = dy;
    
    wlr_log(WLR_INFO, "Region %d: ACCEPTED dx=%d dy=%d - identical tiles %d->%d",
            reg_idx, dx, dy, tiles_identical_no, tiles_identical_with);
}

void scroll_init(void) {
    if (!scroll_pool.initialized) {
        pool_create(&scroll_pool, 0);  /* Auto-detect thread count */
    }
}

void detect_scroll(struct server *s, uint32_t *send_buf) {
    if (!send_buf || !s->prev_framebuf) return;
    
    /* Initialize resources on first call */
    scroll_init();
    
    /* Reset timing stats */
    double t_start = get_time_us();
    memset(&timing, 0, sizeof(timing));
    
    /* Compute k = ceil(scale) for integer output scaling alignment */
    float scale = s->scale;
    if (scale < 1.0f) scale = 1.0f;
    int k = (int)ceilf(scale);
    if (k < 1) k = 1;
    if (k > 4) k = 4;  /* Clamp to reasonable range */
    
    /* Alignment unit: regions aligned to k * TILE_SIZE ensures scroll
     * offsets (which are multiples of k) align with tile boundaries */
    int align = k * TILE_SIZE;
    
    /* Divide frame into grid with k*TILE_SIZE-aligned boundaries.
     * Leave margin at edges for exposed region handling. */
    int margin = align;
    int cols = 4, rows = 4;
    int cell_w = ((s->width - 2 * margin) / cols / align) * align;
    int cell_h = ((s->height - 2 * margin) / rows / align) * align;
    
    /* Ensure minimum cell size */
    if (cell_w < align) cell_w = align;
    if (cell_h < align) cell_h = align;
    
    s->scroll_regions_x = cols;
    s->scroll_regions_y = rows;
    s->num_scroll_regions = 0;
    
    /* Maximum valid aligned coordinates */
    int max_x = (s->width / align) * align;
    int max_y = (s->height / align) * align;
    
    for (int ry = 0; ry < rows; ry++) {
        for (int rx = 0; rx < cols; rx++) {
            int x1 = margin + rx * cell_w;
            int y1 = margin + ry * cell_h;
            int x2 = (rx == cols - 1) ? 
                ((s->width - margin) / align) * align : x1 + cell_w;
            int y2 = (ry == rows - 1) ? 
                ((s->height - margin) / align) * align : y1 + cell_h;
            
            /* Align all boundaries to k*TILE_SIZE */
            x1 = (x1 / align) * align;
            y1 = (y1 / align) * align;
            x2 = ((x2 + align - 1) / align) * align;
            y2 = ((y2 + align - 1) / align) * align;
            
            /* Clamp to frame bounds (aligned) */
            if (x2 > max_x) x2 = max_x;
            if (y2 > max_y) y2 = max_y;
            
            /* Skip if region too small */
            if (x2 - x1 < align * 2 || y2 - y1 < align * 2) continue;
            
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
    
    /* Process regions in parallel using thread pool */
    if (s->num_scroll_regions > 0 && scroll_pool.initialized) {
        current_work.s = s;
        current_work.send_buf = send_buf;
        current_work.k = k;
        pool_process(&scroll_pool, detect_region_scroll_worker, 
                     &current_work, s->num_scroll_regions);
    }
    
    /* Count and log detected scrolls */
    int detected_count = 0;
    for (int i = 0; i < s->num_scroll_regions; i++) {
        if (s->scroll_regions[i].detected) {
            detected_count++;
            timing.regions_detected++;
            wlr_log(WLR_DEBUG, "Region %d (%d,%d)-(%d,%d): scroll dx=%d dy=%d",
                    i, s->scroll_regions[i].x1, s->scroll_regions[i].y1,
                    s->scroll_regions[i].x2, s->scroll_regions[i].y2,
                    s->scroll_regions[i].dx, s->scroll_regions[i].dy);
        }
    }
    
    timing.total_us = get_time_us() - t_start;
    timing.regions_processed = s->num_scroll_regions;
    
    if (detected_count > 0) {
        wlr_log(WLR_INFO, "Scroll detected in %d/%d regions (%.1fus, k=%d)",
                detected_count, s->num_scroll_regions, timing.total_us, k);
    }
}

/*
 * Apply scroll to prev_framebuf only (shift pixels + mark exposed dirty).
 * Must be called BEFORE tile change detection so tiles compare against
 * the post-scroll state.
 */
int apply_scroll_to_prevbuf(struct server *s) {
    int scrolled_count = 0;
    
    for (int i = 0; i < s->num_scroll_regions; i++) {
        if (!s->scroll_regions[i].detected) continue;
        
        int dx = s->scroll_regions[i].dx;
        int dy = s->scroll_regions[i].dy;
        
        int rx1 = s->scroll_regions[i].x1;
        int ry1 = s->scroll_regions[i].y1;
        int rx2 = s->scroll_regions[i].x2;
        int ry2 = s->scroll_regions[i].y2;
        
        int rw = rx2 - rx1;
        int rh = ry2 - ry1;
        if (rw < 16 || rh < 16) continue;
        
        int abs_dy = dy < 0 ? -dy : dy;
        int abs_dx = dx < 0 ? -dx : dx;
        if (abs_dy >= rh || abs_dx >= rw) continue;
        
        /* Compute src/dst rectangles */
        int src_x1 = rx1;
        int dst_x1 = rx1, dst_x2 = rx2;
        int dst_y1, dst_y2;
        
        if (dy < 0) {
            dst_y1 = ry1;
            dst_y2 = ry2 - abs_dy;
        } else if (dy > 0) {
            dst_y1 = ry1 + abs_dy;
            dst_y2 = ry2;
        } else {
            dst_y1 = ry1;
            dst_y2 = ry2;
        }
        
        if (dx < 0) {
            src_x1 = rx1 + abs_dx;
            dst_x1 = rx1;
            dst_x2 = rx2 - abs_dx;
        } else if (dx > 0) {
            src_x1 = rx1;
            dst_x1 = rx1 + abs_dx;
            dst_x2 = rx2;
        }
        
        /* Shift prev_framebuf to match the blit */
        int copy_w = dst_x2 - dst_x1;
        
        if (dy < 0) {
            for (int y = dst_y1; y < dst_y2; y++) {
                int sy = y + abs_dy;
                memmove(&s->prev_framebuf[y * s->width + dst_x1],
                        &s->prev_framebuf[sy * s->width + src_x1],
                        copy_w * sizeof(uint32_t));
            }
        } else if (dy > 0) {
            for (int y = dst_y2 - 1; y >= dst_y1; y--) {
                int sy = y - abs_dy;
                memmove(&s->prev_framebuf[y * s->width + dst_x1],
                        &s->prev_framebuf[sy * s->width + src_x1],
                        copy_w * sizeof(uint32_t));
            }
        } else if (dx != 0) {
            for (int y = dst_y1; y < dst_y2; y++) {
                memmove(&s->prev_framebuf[y * s->width + dst_x1],
                        &s->prev_framebuf[y * s->width + src_x1],
                        copy_w * sizeof(uint32_t));
            }
        }
        
        /* Mark exposed strips as dirty (tile-aligned) */
        if (dy < 0) {
            int exposed_y1 = (dst_y2 / TILE_SIZE) * TILE_SIZE;
            for (int y = exposed_y1; y < ry2; y++) {
                for (int x = rx1; x < rx2; x++) {
                    s->prev_framebuf[y * s->width + x] = 0xDEADBEEF;
                }
            }
        } else if (dy > 0) {
            int exposed_y2 = ((dst_y1 + TILE_SIZE - 1) / TILE_SIZE) * TILE_SIZE;
            for (int y = ry1; y < exposed_y2; y++) {
                for (int x = rx1; x < rx2; x++) {
                    s->prev_framebuf[y * s->width + x] = 0xDEADBEEF;
                }
            }
        }
        
        if (dx < 0) {
            int exposed_x1 = (dst_x2 / TILE_SIZE) * TILE_SIZE;
            for (int y = ry1; y < ry2; y++) {
                for (int x = exposed_x1; x < rx2; x++) {
                    s->prev_framebuf[y * s->width + x] = 0xDEADBEEF;
                }
            }
        } else if (dx > 0) {
            int exposed_x2 = ((dst_x1 + TILE_SIZE - 1) / TILE_SIZE) * TILE_SIZE;
            for (int y = ry1; y < ry2; y++) {
                for (int x = rx1; x < exposed_x2; x++) {
                    s->prev_framebuf[y * s->width + x] = 0xDEADBEEF;
                }
            }
        }
        
        scrolled_count++;
    }
    
    return scrolled_count;
}

/*
 * Write scroll 'd' commands to batch buffer. Returns bytes written.
 * Does NOT update prev_framebuf - call apply_scroll_to_prevbuf first.
 */
int write_scroll_commands(struct server *s, uint8_t *batch, size_t max_size) {
    struct draw_state *draw = &s->draw;
    int off = 0;
    
    for (int i = 0; i < s->num_scroll_regions; i++) {
        if (!s->scroll_regions[i].detected) continue;
        
        int dx = s->scroll_regions[i].dx;
        int dy = s->scroll_regions[i].dy;
        
        int rx1 = s->scroll_regions[i].x1;
        int ry1 = s->scroll_regions[i].y1;
        int rx2 = s->scroll_regions[i].x2;
        int ry2 = s->scroll_regions[i].y2;
        
        int rw = rx2 - rx1;
        int rh = ry2 - ry1;
        if (rw < 16 || rh < 16) continue;
        
        int abs_dy = dy < 0 ? -dy : dy;
        int abs_dx = dx < 0 ? -dx : dx;
        if (abs_dy >= rh || abs_dx >= rw) continue;
        
        /* Compute src/dst rectangles */
        int src_x1 = rx1, src_x2 = rx2;
        int dst_x1 = rx1, dst_x2 = rx2;
        int src_y1, src_y2, dst_y1, dst_y2;
        
        if (dy < 0) {
            src_y1 = ry1 + abs_dy;
            src_y2 = ry2;
            dst_y1 = ry1;
            dst_y2 = ry2 - abs_dy;
        } else if (dy > 0) {
            src_y1 = ry1;
            src_y2 = ry2 - abs_dy;
            dst_y1 = ry1 + abs_dy;
            dst_y2 = ry2;
        } else {
            src_y1 = dst_y1 = ry1;
            src_y2 = dst_y2 = ry2;
        }
        
        if (dx < 0) {
            src_x1 = rx1 + abs_dx;
            src_x2 = rx2;
            dst_x1 = rx1;
            dst_x2 = rx2 - abs_dx;
        } else if (dx > 0) {
            src_x1 = rx1;
            src_x2 = rx2 - abs_dx;
            dst_x1 = rx1 + abs_dx;
            dst_x2 = rx2;
        }
        
        /* Validation */
        if (src_y1 < ry1 || src_y2 > ry2 || dst_y1 < ry1 || dst_y2 > ry2) continue;
        if (src_x1 < rx1 || src_x2 > rx2 || dst_x1 < rx1 || dst_x2 > rx2) continue;
        if (src_y2 <= src_y1 || dst_y2 <= dst_y1) continue;
        if (src_x2 <= src_x1 || dst_x2 <= dst_x1) continue;
        
        /* Check space */
        if (off + 45 > (int)max_size) {
            wlr_log(WLR_ERROR, "Scroll batch overflow");
            break;
        }
        
        /* Write 'd' command to batch */
        batch[off++] = 'd';
        PUT32(batch + off, draw->image_id); off += 4;
        PUT32(batch + off, draw->image_id); off += 4;
        PUT32(batch + off, draw->opaque_id); off += 4;
        PUT32(batch + off, dst_x1); off += 4;
        PUT32(batch + off, dst_y1); off += 4;
        PUT32(batch + off, dst_x2); off += 4;
        PUT32(batch + off, dst_y2); off += 4;
        PUT32(batch + off, src_x1); off += 4;
        PUT32(batch + off, src_y1); off += 4;
        PUT32(batch + off, 0); off += 4;
        PUT32(batch + off, 0); off += 4;
        
        wlr_log(WLR_DEBUG, "Scroll region %d: dy=%d dx=%d src=(%d,%d)-(%d,%d) dst=(%d,%d)-(%d,%d)",
                i, dy, dx, src_x1, src_y1, src_x2, src_y2, dst_x1, dst_y1, dst_x2, dst_y2);
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
