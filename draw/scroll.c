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

/* Result of computing scroll cost for a given dx/dy */
struct scroll_cost_result {
    int bytes_with_scroll;
    int tiles_identical_with;
};

/*
 * Quick count of identical tiles for a scroll offset (no compression).
 * Used for fast pre-filtering before expensive full compression.
 */
static int quick_count_identical(
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
 * Compute compression cost for a given scroll offset (dx, dy).
 * Returns the total bytes needed if this scroll is applied.
 */
static struct scroll_cost_result compute_scroll_cost(
    struct server *s,
    uint32_t *send_buf,
    uint32_t *prev_buf,
    int rx1, int ry1, int rx2, int ry2,
    int dx, int dy)
{
    struct scroll_cost_result result = {0, 0};
    int width = s->width;
    int abs_dx = dx < 0 ? -dx : dx;
    int abs_dy = dy < 0 ? -dy : dy;
    uint8_t comp_buf[TILE_SIZE * TILE_SIZE * 4 + 256];
    
    int tx1 = rx1 / TILE_SIZE;
    int ty1 = ry1 / TILE_SIZE;
    int tx2 = rx2 / TILE_SIZE;
    int ty2 = ry2 / TILE_SIZE;
    
    /* Compute src/dst exactly as send_scroll_commands does */
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
    
    /* Compute tile-aligned exposed boundaries EXACTLY as send_scroll_commands does */
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
            
            /* Skip tiles that extend past frame boundaries */
            if (x1 + TILE_SIZE > s->width || y1 + TILE_SIZE > s->height) continue;
            
            /* Check with scroll */
            int src_x1 = x1 - dx;
            int src_y1 = y1 - dy;
            
            /* Check if tile falls in exposed region after blit */
            int in_exposed = 0;
            if (dy != 0 && y1 >= exposed_y1 && y1 < exposed_y2) in_exposed = 1;
            if (dx != 0 && x1 >= exposed_x1 && x1 < exposed_x2) in_exposed = 1;
            
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
                    result.bytes_with_scroll += size_with;
                } else {
                    result.tiles_identical_with++;
                }
            } else {
                /* Exposed area - compress against original prev_buf for fair delta estimate */
                uint32_t curr_tile[TILE_SIZE * TILE_SIZE];
                uint32_t prev_tile[TILE_SIZE * TILE_SIZE];
                for (int row = 0; row < TILE_SIZE; row++) {
                    memcpy(&curr_tile[row * TILE_SIZE],
                           &send_buf[(y1 + row) * width + x1],
                           TILE_SIZE * sizeof(uint32_t));
                    memcpy(&prev_tile[row * TILE_SIZE],
                           &prev_buf[(y1 + row) * width + x1],
                           TILE_SIZE * sizeof(uint32_t));
                }
                int size_exp = compress_tile_adaptive(comp_buf, sizeof(comp_buf),
                                                       curr_tile, TILE_SIZE,
                                                       prev_tile, TILE_SIZE,
                                                       0, 0, TILE_SIZE, TILE_SIZE);
                if (size_exp < 0) size_exp = -size_exp;
                if (size_exp == 0) size_exp = TILE_SIZE * TILE_SIZE * 4;
                result.bytes_with_scroll += size_exp;
            }
        }
    }
    
    return result;
}

/*
 * Process a single scroll region (called from thread pool workers).
 * Uses compression comparison to verify scroll benefit.
 * Tests detected scroll and ±1 pixel variations to find optimal offset.
 */
static void detect_region_scroll_worker(void *user_data, int reg_idx) {
    struct scroll_work *work = user_data;
    struct server *s = work->s;
    uint32_t *send_buf = work->send_buf;
    uint32_t *prev_buf = s->prev_framebuf;
    int width = s->width;
    
    /* Region bounds are already tile-aligned from detect_scroll */
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
    
    /* Detect scroll using phase correlation */
    struct phase_result result = phase_correlate_detect(
        send_buf, prev_buf, width,
        rx1, ry1, rx2, ry2,
        max_scroll
    );
    
    if (result.dx == 0 && result.dy == 0) return;
    
    int base_dx = result.dx;
    int base_dy = result.dy;
    
    /* Reject scroll at boundaries - likely aliasing artifact */
    int abs_dx = base_dx < 0 ? -base_dx : base_dx;
    int abs_dy = base_dy < 0 ? -base_dy : base_dy;
    if (abs_dx >= max_scroll_x || abs_dy >= max_scroll_y) {
        return;
    }
    
    wlr_log(WLR_INFO, "Region %d: FFT detected scroll dx=%d dy=%d", reg_idx, base_dx, base_dy);
    
    /* Compute bytes without scroll (baseline) */
    int bytes_no_scroll = 0;
    int tiles_identical_no = 0;
    uint8_t comp_buf[TILE_SIZE * TILE_SIZE * 4 + 256];
    
    int tx1 = rx1 / TILE_SIZE;
    int ty1 = ry1 / TILE_SIZE;
    int tx2 = rx2 / TILE_SIZE;
    int ty2 = ry2 / TILE_SIZE;
    
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
        }
    }
    
    /* If nothing changed, scroll can't help */
    if (bytes_no_scroll == 0) {
        return;
    }
    
    /* Smart refinement: only test variations in the relevant direction(s).
     * With 4× downsampled FFT, results are quantized to 4 pixels.
     * If dx=0, only test dy variations (and vice versa).
     * Two-pass approach: quick identical count, then full compression only for best. */
    
    /* Build list of candidates to test */
    struct { int dx, dy; } candidates[16];
    int num_candidates = 0;
    
    /* Always include base FFT result */
    if (base_dx != 0 || base_dy != 0) {
        candidates[num_candidates].dx = base_dx;
        candidates[num_candidates].dy = base_dy;
        num_candidates++;
    }
    
    /* Add dy variations if dy != 0 */
    if (base_dy != 0) {
        static const int dy_offsets[] = {-3, -2, -1, +1, +2, +3};
        for (int i = 0; i < 6 && num_candidates < 15; i++) {
            int test_dy = base_dy + dy_offsets[i];
            int test_abs_dy = test_dy < 0 ? -test_dy : test_dy;
            if (test_abs_dy < max_scroll_y) {
                candidates[num_candidates].dx = base_dx;
                candidates[num_candidates].dy = test_dy;
                num_candidates++;
            }
        }
    }
    
    /* Add dx variations if dx != 0 */
    if (base_dx != 0) {
        static const int dx_offsets[] = {-3, -2, -1, +1, +2, +3};
        for (int i = 0; i < 6 && num_candidates < 15; i++) {
            int test_dx = base_dx + dx_offsets[i];
            int test_abs_dx = test_dx < 0 ? -test_dx : test_dx;
            if (test_abs_dx < max_scroll_x) {
                candidates[num_candidates].dx = test_dx;
                candidates[num_candidates].dy = base_dy;
                num_candidates++;
            }
        }
    }
    
    /* Pass 1: Quick identical tile count for all candidates */
    int best_quick_idx = -1;
    int best_quick_identical = 0;
    
    for (int c = 0; c < num_candidates; c++) {
        int identical = quick_count_identical(
            s, send_buf, prev_buf, rx1, ry1, rx2, ry2,
            candidates[c].dx, candidates[c].dy);
        
        if (identical > best_quick_identical) {
            best_quick_identical = identical;
            best_quick_idx = c;
        }
    }
    
    /* If no candidate has more identical tiles than baseline, reject */
    if (best_quick_idx < 0 || best_quick_identical <= tiles_identical_no) {
        wlr_log(WLR_DEBUG, "Region %d: REJECTED dx=%d dy=%d - quick check shows no improvement (%d vs %d identical)",
                reg_idx, base_dx, base_dy, best_quick_identical, tiles_identical_no);
        return;
    }
    
    /* Pass 2: Full compression only for the best candidate */
    int best_dx = candidates[best_quick_idx].dx;
    int best_dy = candidates[best_quick_idx].dy;
    
    struct scroll_cost_result cost = compute_scroll_cost(
        s, send_buf, prev_buf, rx1, ry1, rx2, ry2, best_dx, best_dy);
    
    wlr_log(WLR_DEBUG, "Region %d: best candidate dx=%d dy=%d -> %d bytes (identical=%d)",
            reg_idx, best_dx, best_dy, cost.bytes_with_scroll, cost.tiles_identical_with);
    
    int best_bytes = cost.bytes_with_scroll;
    int best_tiles_identical = cost.tiles_identical_with;
    
    /* Check if any scroll variant was better than no scroll */
    if (best_dx == 0 && best_dy == 0) {
        wlr_log(WLR_INFO, "Region %d: REJECTED dx=%d dy=%d and variants - no scroll saves bytes",
                reg_idx, base_dx, base_dy);
        return;
    }
    
    /* Verify the best scroll actually saves bytes vs no scroll */
    if (best_bytes >= bytes_no_scroll) {
        int extra = best_bytes - bytes_no_scroll;
        wlr_log(WLR_INFO, "Region %d: REJECTED best dx=%d dy=%d - scroll costs %d more bytes (%d vs %d)",
                reg_idx, best_dx, best_dy, extra, best_bytes, bytes_no_scroll);
        return;
    }
    
    s->scroll_regions[reg_idx].detected = 1;
    s->scroll_regions[reg_idx].dx = best_dx;
    s->scroll_regions[reg_idx].dy = best_dy;
    
    int saved = bytes_no_scroll - best_bytes;
    int pct = bytes_no_scroll > 0 ? (saved * 100 / bytes_no_scroll) : 0;
    
    if (best_dx != base_dx || best_dy != base_dy) {
        wlr_log(WLR_INFO, "Region %d: ACCEPTED dx=%d dy=%d (adjusted from %d,%d) - saves %d bytes (%d%%), %d vs %d bytes, identical %d->%d tiles",
                reg_idx, best_dx, best_dy, base_dx, base_dy, saved, pct, best_bytes, bytes_no_scroll,
                tiles_identical_no, best_tiles_identical);
    } else {
        wlr_log(WLR_INFO, "Region %d: ACCEPTED dx=%d dy=%d - saves %d bytes (%d%%), %d vs %d bytes, identical %d->%d tiles",
                reg_idx, best_dx, best_dy, saved, pct, best_bytes, bytes_no_scroll,
                tiles_identical_no, best_tiles_identical);
    }
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
    
    /* Divide frame into 16x4 grid (64 regions) with tile-aligned boundaries.
     * Leave TILE_SIZE margin at edges for exposed region handling. */
    int margin = TILE_SIZE;
    int dim = 256; 
    int cols = s->width / dim > 0 ? s->width / dim : 1, rows = s->height / dim > 0 ? s->height / dim : 1;
    cols = 4; rows = 4; 
    int cell_w = ((s->width - 2 * margin) / cols / TILE_SIZE) * TILE_SIZE;
    int cell_h = ((s->height - 2 * margin) / rows / TILE_SIZE) * TILE_SIZE;
    
    /* Ensure minimum cell size */
    if (cell_w < TILE_SIZE) cell_w = TILE_SIZE;
    if (cell_h < TILE_SIZE) cell_h = TILE_SIZE;
    
    s->scroll_regions_x = cols;
    s->scroll_regions_y = rows;
    s->num_scroll_regions = 0;
    
    /* Maximum valid tile-aligned coordinates */
    int max_x = (s->width / TILE_SIZE) * TILE_SIZE;
    int max_y = (s->height / TILE_SIZE) * TILE_SIZE;
    
    for (int ry = 0; ry < rows; ry++) {
        for (int rx = 0; rx < cols; rx++) {
            int x1 = margin + rx * cell_w;
            int y1 = margin + ry * cell_h;
            int x2 = (rx == cols - 1) ? 
                ((s->width - margin) / TILE_SIZE) * TILE_SIZE : x1 + cell_w;
            int y2 = (ry == rows - 1) ? 
                ((s->height - margin) / TILE_SIZE) * TILE_SIZE : y1 + cell_h;
            
            /* Tile-align all boundaries */
            x1 = (x1 / TILE_SIZE) * TILE_SIZE;
            y1 = (y1 / TILE_SIZE) * TILE_SIZE;
            x2 = ((x2 + TILE_SIZE - 1) / TILE_SIZE) * TILE_SIZE;
            y2 = ((y2 + TILE_SIZE - 1) / TILE_SIZE) * TILE_SIZE;
            
            /* Clamp to frame bounds (tile-aligned) */
            if (x2 > max_x) x2 = max_x;
            if (y2 > max_y) y2 = max_y;
            
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
