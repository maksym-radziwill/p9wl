/*
 * scroll.c - Scroll detection and 9P scroll commands
 *
 * Detects scrolling regions and sends efficient blit commands.
 * Uses thread pool for parallel region processing.
 * Uses FFT-based phase correlation for motion detection.
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
};

static struct scroll_work current_work;

static inline double get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

/*
 * Process a single scroll region (called from thread pool workers).
 * Uses compression comparison to verify scroll benefit.
 */
static void detect_region_scroll_worker(void *user_data, int reg_idx) {
    struct scroll_work *work = user_data;
    struct server *s = work->s;
    uint32_t *send_buf = work->send_buf;
    uint32_t *prev_buf = s->prev_framebuf;
    int width = s->width;
    
    int rx1 = s->scroll_regions[reg_idx].x1;
    int ry1 = s->scroll_regions[reg_idx].y1;
    int rx2 = s->scroll_regions[reg_idx].x2;
    int ry2 = s->scroll_regions[reg_idx].y2;
    
    s->scroll_regions[reg_idx].detected = 0;
    s->scroll_regions[reg_idx].dx = 0;
    s->scroll_regions[reg_idx].dy = 0;
    
    /* Detect scroll using phase correlation */
    struct phase_result result = phase_correlate_detect(
        send_buf, prev_buf, width,
        rx1, ry1, rx2, ry2,
        MAX_SCROLL_PIXELS
    );
    
    if (result.dx == 0 && result.dy == 0) return;
    
    int dx = result.dx;
    int dy = result.dy;
    
    /* Compare actual compressed sizes */
    int bytes_no_scroll = 0;
    int bytes_with_scroll = 0;
    int tiles_identical_no = 0, tiles_identical_with = 0;
    int tiles_checked = 0;
    uint8_t comp_buf[TILE_SIZE * TILE_SIZE * 4 + 256];
    
    int tx1 = rx1 / TILE_SIZE;
    int ty1 = ry1 / TILE_SIZE;
    int tx2 = (rx2 + TILE_SIZE - 1) / TILE_SIZE;
    int ty2 = (ry2 + TILE_SIZE - 1) / TILE_SIZE;
    
    for (int ty = ty1; ty < ty2; ty++) {
        for (int tx = tx1; tx < tx2; tx++) {
            int x1 = tx * TILE_SIZE;
            int y1 = ty * TILE_SIZE;
            
            /* Skip tiles that extend past frame boundaries */
            if (x1 + TILE_SIZE > s->width || y1 + TILE_SIZE > s->height) continue;
            
            /* Check if tile is identical without scroll */
            int identical_no = 1;
            for (int row = 0; row < TILE_SIZE && identical_no; row++) {
                if (memcmp(&send_buf[(y1 + row) * width + x1],
                           &prev_buf[(y1 + row) * width + x1],
                           TILE_SIZE * sizeof(uint32_t)) != 0) {
                    identical_no = 0;
                }
            }
            
            if (!identical_no) {
                int size_no = compress_tile_adaptive(comp_buf, sizeof(comp_buf),
                                                     send_buf, width,
                                                     prev_buf, width,
                                                     x1, y1, TILE_SIZE, TILE_SIZE);
                if (size_no < 0) size_no = -size_no;
                if (size_no == 0) size_no = TILE_SIZE * TILE_SIZE * 4;  /* compression failed */
                bytes_no_scroll += size_no;
            } else {
                tiles_identical_no++;
            }
            tiles_checked++;
            
            /* Check with scroll */
            int src_x1 = x1 - dx;
            int src_y1 = y1 - dy;
            
            /* Check if tile falls in exposed region after blit.
             * The blit only covers dst=[ry1+abs_dy, ry2] for dy>0,
             * so tiles at [ry1, ry1+abs_dy) are exposed. */
            int in_exposed = 0;
            int abs_dx = dx < 0 ? -dx : dx;
            int abs_dy = dy < 0 ? -dy : dy;
            if (dy > 0 && y1 < ry1 + abs_dy) in_exposed = 1;
            if (dy < 0 && y1 + TILE_SIZE > ry2 - abs_dy) in_exposed = 1;
            if (dx > 0 && x1 < rx1 + abs_dx) in_exposed = 1;
            if (dx < 0 && x1 + TILE_SIZE > rx2 - abs_dx) in_exposed = 1;
            
            if (!in_exposed && src_x1 >= 0 && src_y1 >= 0 && 
                src_x1 + TILE_SIZE <= s->width && src_y1 + TILE_SIZE <= s->height) {
                
                /* Check if tile is identical with scroll */
                int identical_with = 1;
                for (int row = 0; row < TILE_SIZE && identical_with; row++) {
                    if (memcmp(&send_buf[(y1 + row) * width + x1],
                               &prev_buf[(src_y1 + row) * width + src_x1],
                               TILE_SIZE * sizeof(uint32_t)) != 0) {
                        identical_with = 0;
                    }
                }
                
                if (!identical_with) {
                    /* Copy current tile too, since compress uses x1,y1 offsets */
                    uint32_t curr_tile[TILE_SIZE * TILE_SIZE];
                    uint32_t shifted[TILE_SIZE * TILE_SIZE];
                    for (int row = 0; row < TILE_SIZE; row++) {
                        memcpy(&curr_tile[row * TILE_SIZE],
                               &send_buf[(y1 + row) * width + x1],
                               TILE_SIZE * sizeof(uint32_t));
                        memcpy(&shifted[row * TILE_SIZE],
                               &prev_buf[(src_y1 + row) * width + src_x1],
                               TILE_SIZE * sizeof(uint32_t));
                    }
                    int size_with = compress_tile_adaptive(comp_buf, sizeof(comp_buf),
                                                           curr_tile, TILE_SIZE,
                                                           shifted, TILE_SIZE,
                                                           0, 0, TILE_SIZE, TILE_SIZE);
                    if (size_with < 0) size_with = -size_with;
                    if (size_with == 0) size_with = TILE_SIZE * TILE_SIZE * 4;
                    bytes_with_scroll += size_with;
                } else {
                    tiles_identical_with++;
                }
            } else {
                bytes_with_scroll += TILE_SIZE * TILE_SIZE * 4;  /* exposed area */
            }
        }
    }
    
    /* Only scroll if it saves */
    if (bytes_with_scroll > bytes_no_scroll) return;
    
    s->scroll_regions[reg_idx].detected = 1;
    s->scroll_regions[reg_idx].dx = dx;
    s->scroll_regions[reg_idx].dy = dy;
    
    wlr_log(WLR_DEBUG, "Scroll dx=%d dy=%d: %d tiles, identical %d->%d, bytes %d->%d (%.0f%% saved)",
            dx, dy, tiles_checked,
            tiles_identical_no, tiles_identical_with,
            bytes_no_scroll, bytes_with_scroll,
            bytes_no_scroll > 0 ? (1.0 - (double)bytes_with_scroll / bytes_no_scroll) * 100 : 0);
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
    
    /* Divide frame into 2x2 grid (4 quadrants) with tile-aligned boundaries.
     * Leave TILE_SIZE margin at edges for exposed region handling. */
    int margin = TILE_SIZE;
    int cols = 2, rows = 2;
    int cell_w = ((s->width - 2 * margin) / cols / TILE_SIZE) * TILE_SIZE;
    int cell_h = ((s->height - 2 * margin) / rows / TILE_SIZE) * TILE_SIZE;
    
    s->scroll_regions_x = cols;
    s->scroll_regions_y = rows;
    s->num_scroll_regions = 0;
    
    for (int ry = 0; ry < rows; ry++) {
        for (int rx = 0; rx < cols; rx++) {
            int x1 = margin + rx * cell_w;
            int y1 = margin + ry * cell_h;
            int x2 = (rx == cols - 1) ? s->width - margin : x1 + cell_w;
            int y2 = (ry == rows - 1) ? s->height - margin : y1 + cell_h;
            
            /* Skip if region too small */
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
    
    /* Process regions in parallel using thread pool */
    if (s->num_scroll_regions > 0 && scroll_pool.initialized) {
        current_work.s = s;
        current_work.send_buf = send_buf;
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
        wlr_log(WLR_INFO, "Scroll detected in %d/%d regions (%.1fus)",
                detected_count, s->num_scroll_regions, timing.total_us);
    }
}

int send_scroll_commands(struct server *s, int *pending_writes) {
    struct draw_state *draw = &s->draw;
    struct p9conn *p9 = draw->p9;
    int scrolled_count = 0;
    
    for (int i = 0; i < s->num_scroll_regions; i++) {
        if (!s->scroll_regions[i].detected) continue;
        
        int dx = s->scroll_regions[i].dx;
        int dy = s->scroll_regions[i].dy;
        
        /* Align region to tile boundaries */
        int rx1 = (s->scroll_regions[i].x1 / TILE_SIZE) * TILE_SIZE;
        int ry1 = (s->scroll_regions[i].y1 / TILE_SIZE) * TILE_SIZE;
        int rx2 = ((s->scroll_regions[i].x2 + TILE_SIZE - 1) / TILE_SIZE) * TILE_SIZE;
        int ry2 = ((s->scroll_regions[i].y2 + TILE_SIZE - 1) / TILE_SIZE) * TILE_SIZE;
        
        if (rx2 > s->width) rx2 = s->width;
        if (ry2 > s->height) ry2 = s->height;
        
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
        
        /* Send 'd' command (pipelined) */
        uint8_t dcmd[64];
        int off = 0;
        dcmd[off++] = 'd';
        PUT32(dcmd + off, draw->image_id); off += 4;
        PUT32(dcmd + off, draw->image_id); off += 4;
        PUT32(dcmd + off, draw->opaque_id); off += 4;
        PUT32(dcmd + off, dst_x1); off += 4;
        PUT32(dcmd + off, dst_y1); off += 4;
        PUT32(dcmd + off, dst_x2); off += 4;
        PUT32(dcmd + off, dst_y2); off += 4;
        PUT32(dcmd + off, src_x1); off += 4;
        PUT32(dcmd + off, src_y1); off += 4;
        PUT32(dcmd + off, 0); off += 4;
        PUT32(dcmd + off, 0); off += 4;
        
        if (p9_write_send(p9, draw->drawdata_fid, 0, dcmd, off) < 0) {
            wlr_log(WLR_ERROR, "Scroll command send failed");
            /* Mark region dirty for full retransmit */
            for (int y = ry1; y < ry2; y++) {
                for (int x = rx1; x < rx2; x++) {
                    s->prev_framebuf[y * s->width + x] = 0xDEADBEEF;
                }
            }
            continue;
        }
        (*pending_writes)++;
        
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
        
        /* Mark exposed strip as dirty */
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
        wlr_log(WLR_DEBUG, "Scroll region %d: dy=%d dx=%d src=(%d,%d)-(%d,%d) dst=(%d,%d)-(%d,%d)",
                i, dy, dx, src_x1, src_y1, src_x2, src_y2, dst_x1, dst_y1, dst_x2, dst_y2);
    }
    
    if (scrolled_count > 0) {
        wlr_log(WLR_INFO, "Executed %d scroll commands", scrolled_count);
    }
    
    return scrolled_count;
}

const struct scroll_timing *scroll_get_timing(void) {
    return &timing;
}

void scroll_cleanup(void) {
    pool_destroy(&scroll_pool);
    phase_correlate_cleanup();
}
