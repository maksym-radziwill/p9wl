/*
 * send.c - Frame sending and send thread (refactored)
 *
 * Handles queuing frames, the send thread main loop,
 * tile compression selection, and pipelined writes.
 *
 * Changes from original:
 * - Uses cmd_* helpers from draw_helpers.h
 * - Consolidated border drawing into loop
 * - Extracted drain_wake() helper
 * - Simplified tile bounds with tile_bounds()
 * - Replaced full-frame memcpy in send_frame() with pointer swap
 * - Added damage-based dirty tile tracking to skip unchanged tiles
 *   without pixel scanning (falls back to tile_changed on scroll/errors)
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

#include <wlr/util/log.h>

#include "send.h"
#include "compress.h"
#include "scroll.h"
#include "draw/draw.h"
#include "draw_helpers.h"
#include "p9/p9.h"
#include "types.h"

/* ============== Drain Thread ============== */

struct drain_ctx {
    struct p9conn *p9;
    atomic_int pending;
    atomic_int errors;
    atomic_int running;
    atomic_int paused;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    uint8_t *recv_buf;
};

static struct drain_ctx drain;

/* Wake drain thread */
static inline void drain_wake(void) {
    pthread_mutex_lock(&drain.lock);
    pthread_cond_signal(&drain.cond);
    pthread_mutex_unlock(&drain.lock);
}

/* Read one Rwrite response */
static int drain_recv_one(void) {
    uint8_t *buf = drain.recv_buf;
    struct p9conn *p9 = drain.p9;
    
    if (p9_read_full(p9, buf, 4) != 4) return -1;
    uint32_t rxlen = GET32(buf);
    if (rxlen < 7 || rxlen > p9->msize) return -1;
    if (p9_read_full(p9, buf + 4, rxlen - 4) != (int)(rxlen - 4)) return -1;
    
    int type = buf[4];
    if (type == Rerror) {
        uint16_t elen = GET16(buf + 7);
        char errmsg[256];
        int copylen = (elen < 255) ? elen : 255;
        memcpy(errmsg, buf + 9, copylen);
        errmsg[copylen] = '\0';
        wlr_log(WLR_ERROR, "9P drain error: %s", errmsg);
        if (strstr(errmsg, "unknown id")) p9->unknown_id_error = 1;
        if (strstr(errmsg, "short")) p9->draw_error = 1;
        return -1;
    }
    
    return (type == Rwrite) ? (int)GET32(buf + 7) : -1;
}

static void *drain_thread_func(void *arg) {
    (void)arg;
    wlr_log(WLR_INFO, "Drain thread started");
    
    while (atomic_load(&drain.running)) {
        pthread_mutex_lock(&drain.lock);
        while (atomic_load(&drain.pending) == 0 && atomic_load(&drain.running)) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 10000000;  /* 10ms timeout */
            pthread_cond_timedwait(&drain.cond, &drain.lock, &ts);
        }
        pthread_mutex_unlock(&drain.lock);
        
        if (!atomic_load(&drain.running)) break;
        if (atomic_load(&drain.paused) && atomic_load(&drain.pending) == 0) continue;
        
        if (atomic_load(&drain.pending) > 0) {
            if (drain_recv_one() < 0) atomic_fetch_add(&drain.errors, 1);
            atomic_fetch_sub(&drain.pending, 1);
        }
    }
    
    wlr_log(WLR_INFO, "Drain thread exiting");
    return NULL;
}

static int drain_start(struct p9conn *p9) {
    atomic_store(&drain.pending, 0);
    atomic_store(&drain.errors, 0);
    atomic_store(&drain.running, 1);
    atomic_store(&drain.paused, 0);
    drain.p9 = p9;
    pthread_mutex_init(&drain.lock, NULL);
    pthread_cond_init(&drain.cond, NULL);
    
    drain.recv_buf = malloc(p9->msize);
    if (!drain.recv_buf) return -1;
    
    if (pthread_create(&drain.thread, NULL, drain_thread_func, NULL) != 0) {
        free(drain.recv_buf);
        return -1;
    }
    return 0;
}

static void drain_stop(void) {
    atomic_store(&drain.running, 0);
    drain_wake();
    pthread_join(drain.thread, NULL);
    
    while (atomic_load(&drain.pending) > 0) {
        drain_recv_one();
        atomic_fetch_sub(&drain.pending, 1);
    }
    
    free(drain.recv_buf);
    drain.recv_buf = NULL;
    pthread_mutex_destroy(&drain.lock);
    pthread_cond_destroy(&drain.cond);
}

static void drain_pause(void) {
    atomic_store(&drain.paused, 1);
    drain_wake();
    while (atomic_load(&drain.pending) > 0) {
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, NULL);
    }
}

static void drain_resume(void) {
    atomic_store(&drain.paused, 0);
    drain_wake();
}

static void drain_notify(void) {
    atomic_fetch_add(&drain.pending, 1);
    drain_wake();
}

static void drain_throttle(int max_pending) {
    while (atomic_load(&drain.pending) > max_pending) {
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, NULL);
    }
}

/* ============== Frame Sending ============== */

void send_frame(struct server *s) {
    pthread_mutex_lock(&s->send_lock);
    
    if (s->resize_pending) {
        pthread_mutex_unlock(&s->send_lock);
        return;
    }

    /* Find a free buffer */
    int buf = -1;
    for (int i = 0; i < 2; i++) {
        if (i != s->active_buf && i != s->pending_buf) {
            buf = i;
            break;
        }
    }
    
    if (buf < 0) {
        pthread_mutex_unlock(&s->send_lock);
        return;
    }
    
    /*
     * Swap the framebuffer pointer with the free send buffer instead of
     * copying.  After the swap the send thread owns what was framebuf
     * (the just-rendered frame) and the compositor gets a recycled
     * buffer to render the next frame into.  All three buffers
     * (framebuf, send_buf[0], send_buf[1]) must be allocated with the
     * same size and alignment for this to be safe.
     */
    uint32_t *tmp     = s->send_buf[buf];
    s->send_buf[buf]  = s->framebuf;
    s->framebuf       = tmp;

    /* Copy dirty tile bitmap from staging area if available */
    if (s->dirty_staging_valid) {
        int ntiles = s->tiles_x * s->tiles_y;
        if (!s->dirty_tiles[buf] && ntiles > 0)
            s->dirty_tiles[buf] = calloc(1, ntiles);
        if (s->dirty_tiles[buf] && ntiles > 0) {
            memcpy(s->dirty_tiles[buf], s->dirty_staging, ntiles);
            s->dirty_valid[buf] = 1;
        } else {
            s->dirty_valid[buf] = 0;
        }
        s->dirty_staging_valid = 0;
    } else {
        s->dirty_valid[buf] = 0;
    }

    s->pending_buf = buf;
    if (s->force_full_frame) s->send_full = 1;
    pthread_cond_signal(&s->send_cond);
    pthread_mutex_unlock(&s->send_lock);
}

int send_timer_callback(void *data) {
    struct server *s = data;
    if (!s->frame_dirty) return 0;
    s->frame_dirty = 0;
    send_frame(s);
    return 0;
}

static int scroll_disabled(struct server *s) {
    double floor_val;
    return (modf(s->scale, &floor_val) != 0.0);
}

/*
 * Write border fill commands around the content area.
 * Returns bytes written to batch.
 */
static int write_borders(uint8_t *batch, struct draw_state *draw) {
    int off = 0;
    
    int mx = draw->win_minx, my = draw->win_miny;
    int Mx = mx + draw->width, My = my + draw->height;
    int ax = draw->actual_minx, ay = draw->actual_miny;
    int Ax = draw->actual_maxx, Ay = draw->actual_maxy;
    
    /* Fallback if actual bounds not set */
    if (ax == 0 && ay == 0 && Ax == 0 && Ay == 0) {
        ax = mx - 16; ay = my - 16;
        Ax = Mx + 16; Ay = My + 16;
    }
    
    /* Border rectangles: {x1, y1, x2, y2} */
    struct { int x1, y1, x2, y2; } borders[] = {
        {ax, ay, Ax, my},   /* Top */
        {ax, My, Ax, Ay},   /* Bottom */
        {ax, my, mx, My},   /* Left */
        {Mx, my, Ax, My},   /* Right */
    };
    
    for (int i = 0; i < 4; i++) {
        if (borders[i].x2 > borders[i].x1 && borders[i].y2 > borders[i].y1) {
            off += cmd_fill(batch + off, draw->screen_id, draw->border_id,
                           draw->opaque_id, borders[i].x1, borders[i].y1,
                           borders[i].x2, borders[i].y2);
        }
    }
    
    return off;
}

void *send_thread_func(void *arg) {
    struct server *s = arg;
    struct draw_state *draw = &s->draw;
    struct p9conn *p9 = draw->p9;
    static int send_count = 0;
    
    wlr_log(WLR_INFO, "Send thread started");
    
    if (scroll_disabled(s)) {
        wlr_log(WLR_INFO, "Scroll optimization disabled (fractional scale: %.2f)", s->scale);
    }
    
    /* Determine max batch size from iounit */
    size_t max_batch = draw->iounit ? draw->iounit : (p9->msize - 24);
    if (max_batch > 23) max_batch -= 23;
    
    wlr_log(WLR_INFO, "Send thread: max_batch=%zu", max_batch);
    
    uint8_t *batch = malloc(max_batch);
    if (!batch) return NULL;
    
    if (drain_start(p9) < 0) {
        free(batch);
        return NULL;
    }
    
    /* Initialize parallel compression */
    int nthreads = sysconf(_SC_NPROCESSORS_ONLN) / 2;
    if (compress_pool_init(nthreads) < 0) nthreads = 0;
    
    int max_tiles = (4096 / TILE_SIZE) * (4096 / TILE_SIZE);
    struct tile_work *work = malloc(max_tiles * sizeof(*work));
    struct tile_result *results = malloc(max_tiles * sizeof(*results));
    if (!work || !results) nthreads = 0;
    
    const size_t comp_buf_size = TILE_SIZE * TILE_SIZE * 4 + 256;
    uint8_t *comp_buf = malloc(comp_buf_size);
    
    while (s->running) {
        /* Wait for work */
        while (s->pending_buf < 0 && !s->window_changed && s->running) {
            struct timespec ts = {0, 100000};
            nanosleep(&ts, NULL);
        }
        if (!s->running) break;

        pthread_mutex_lock(&s->send_lock);
        int current_buf = s->pending_buf;
        int got_frame = (current_buf >= 0);
        if (got_frame) {
            s->active_buf = current_buf;
            s->pending_buf = -1;
        }
        int do_full = s->send_full;
        s->send_full = 0;
        pthread_mutex_unlock(&s->send_lock);
        
        uint32_t *send_buf = got_frame ? s->send_buf[current_buf] : NULL;
        
        /* Handle errors and window changes */
        if (p9->draw_error) {
            p9->draw_error = 0;
            draw->xor_enabled = 0;
            memset(s->prev_framebuf, 0, s->width * s->height * 4);
            do_full = 1;
        }
        
        int drain_errs = atomic_exchange(&drain.errors, 0);
        if (drain_errs > 0) {
            memset(s->prev_framebuf, 0xDE, s->width * s->height * 4);
            do_full = 1;
        }
        
        if (s->window_changed) {
            s->window_changed = 0;
            drain_pause();
            relookup_window(s);
            drain_resume();
            if (s->resize_pending) continue;
            do_full = 1;
        }
        
        if (p9->unknown_id_error) {
            p9->unknown_id_error = 0;
            drain_pause();
            relookup_window(s);
            drain_resume();
            if (s->resize_pending) continue;
            do_full = 1;
        }
        
        if (!got_frame) continue;
        if (s->resize_pending) continue;
        if (s->force_full_frame) {
            do_full = 1;
            s->force_full_frame = 0;
        }
        
        /* Detect and apply scroll */
        int scrolled_regions = 0;
        if (!do_full && !scroll_disabled(s)) {
            detect_scroll(s, send_buf);
            scrolled_regions = apply_scroll_to_prevbuf(s);
        }
        
        /* Build batch */
        size_t off = 0;
        if (scrolled_regions > 0) {
            off = write_scroll_commands(s, batch, max_batch);
        }
        
        int tile_count = 0, batch_count = 0;
        int comp_tiles = 0, delta_tiles = 0;
        size_t bytes_raw = 0, bytes_sent = 0;
        int can_delta = draw->xor_enabled && !do_full && s->prev_framebuf;
        
        /*
         * Use damage-based dirty map when available.  This lets us skip
         * tile_changed() entirely for clean tiles — avoiding two
         * full-frame memory reads per frame.  Falls back to pixel
         * comparison when scroll happened (prev_framebuf was modified)
         * or when no damage info is available.
         */
        uint8_t *dirty_map = NULL;
        if (!do_full && scrolled_regions == 0 &&
            s->dirty_valid[current_buf] && s->dirty_tiles[current_buf]) {
            dirty_map = s->dirty_tiles[current_buf];
        }
        
        /* Collect changed tiles */
        int work_count = 0;
        for (int ty = 0; ty < s->tiles_y; ty++) {
            for (int tx = 0; tx < s->tiles_x; tx++) {
                int x1, y1, w, h;
                tile_bounds(tx, ty, s->width, s->height, &x1, &y1, &w, &h);
                if (w <= 0 || h <= 0) continue;
                
                int changed;
                if (dirty_map) {
                    /* Fast path: use compositor damage bitmap */
                    changed = dirty_map[ty * s->tiles_x + tx];
                } else {
                    /* Fallback: pixel-by-pixel comparison */
                    changed = do_full || tile_changed(send_buf, s->prev_framebuf,
                                                       s->width, x1, y1, w, h);
                }
                if (!changed) continue;
                
                if (work_count >= max_tiles) break;
                
                /* Check for scroll-exposed region (marked 0xDEADBEEF) */
                int use_delta = can_delta;
                if (use_delta) {
                    int x2m1 = x1 + w - 1, y2m1 = y1 + h - 1;
                    for (int x = x1; x < x1 + w && use_delta; x++) {
                        if (s->prev_framebuf[y1 * s->width + x] == 0xDEADBEEF ||
                            s->prev_framebuf[y2m1 * s->width + x] == 0xDEADBEEF)
                            use_delta = 0;
                    }
                    for (int y = y1; y <= y2m1 && use_delta; y++) {
                        if (s->prev_framebuf[y * s->width + x1] == 0xDEADBEEF ||
                            s->prev_framebuf[y * s->width + x2m1] == 0xDEADBEEF)
                            use_delta = 0;
                    }
                }
                
                work[work_count] = (struct tile_work){
                    .pixels = send_buf, .stride = s->width,
                    .prev_pixels = use_delta ? s->prev_framebuf : NULL,
                    .prev_stride = s->width,
                    .x1 = x1, .y1 = y1, .w = w, .h = h
                };
                work_count++;
            }
        }
        
        /* Compress tiles in parallel */
        if (work_count > 0 && nthreads > 0) {
            compress_tiles_parallel(work, results, work_count);
        }
        
        drain_throttle(2);
        
        /* Build and send batches */
        for (int i = 0; i < work_count; i++) {
            struct tile_work *tw = &work[i];
            struct tile_result *r = &results[i];
            int x1 = tw->x1, y1 = tw->y1;
            int x2 = x1 + tw->w, y2 = y1 + tw->h;
            int raw_size = tw->w * tw->h * 4;
            
            /* Single-threaded fallback */
            if (nthreads == 0) {
                int res = compress_tile_adaptive(comp_buf, comp_buf_size,
                                                  tw->pixels, tw->stride,
                                                  tw->prev_pixels, tw->prev_stride,
                                                  x1, y1, tw->w, tw->h);
                r->is_delta = (res > 0);
                r->size = (res > 0) ? res : (res < 0) ? -res : 0;
                if (r->size > 0) memcpy(r->data, comp_buf, r->size);
            }
            
            bytes_raw += raw_size;
            
            size_t tile_size = (r->size > 0)
                ? (21 + r->size + (r->is_delta ? ALPHA_DELTA_OVERHEAD : 0))
                : (21 + raw_size);
            
            /* Flush if batch full */
            if (off + tile_size > max_batch && off > 0) {
                if (p9_write_send(p9, draw->drawdata_fid, 0, batch, off) < 0) {
                    memset(s->prev_framebuf, 0xDE, s->width * s->height * 4);
                    s->send_full = 1;
                }
                drain_notify();
                batch_count++;
                off = 0;
            }
            
            /* Write tile command */
            if (r->size > 0) {
                uint32_t img_id = r->is_delta ? draw->delta_id : draw->image_id;
                off += cmd_load_hdr(batch + off, img_id, x1, y1, x2, y2);
                memcpy(batch + off, r->data, r->size);
                off += r->size;
                
                if (r->is_delta) {
                    /* Composite delta onto image */
                    off += cmd_draw(batch + off, draw->image_id, draw->delta_id,
                                   draw->delta_id, x1, y1, x2, y2, x1, y1, x1, y1);
                    bytes_sent += r->size + ALPHA_DELTA_OVERHEAD;
                    delta_tiles++;
                } else {
                    bytes_sent += r->size;
                    comp_tiles++;
                }
            } else {
                /* Uncompressed */
                off += cmd_loadraw_hdr(batch + off, draw->image_id, x1, y1, x2, y2);
                for (int row = 0; row < tw->h; row++) {
                    memcpy(batch + off, &send_buf[(y1 + row) * s->width + x1], tw->w * 4);
                    off += tw->w * 4;
                }
                bytes_sent += raw_size;
            }
            
            /* Update prev_framebuf */
            for (int row = 0; row < tw->h; row++) {
                memcpy(&s->prev_framebuf[(y1 + row) * s->width + x1],
                       &send_buf[(y1 + row) * s->width + x1], tw->w * 4);
            }
            tile_count++;
        }
        
        /* Final batch with copy + borders + flush */
        if (tile_count > 0 || scrolled_regions > 0) {
            size_t footer_size = 45 + 45 * 4 + 1;
            if (off + footer_size > max_batch && off > 0) {
                if (p9_write_send(p9, draw->drawdata_fid, 0, batch, off) < 0) {
                    memset(s->prev_framebuf, 0xDE, s->width * s->height * 4);
                    s->send_full = 1;
                }
                drain_notify();
                batch_count++;
                off = 0;
            }
            
            /* Copy buffer to window */
            off += cmd_copy(batch + off, draw->screen_id, draw->image_id,
                           draw->opaque_id,
                           draw->win_minx, draw->win_miny,
                           draw->win_minx + draw->width,
                           draw->win_miny + draw->height,
                           0, 0);
            
            /* Draw borders */
            off += write_borders(batch + off, draw);
            
            /* Flush */
            off += cmd_flush(batch + off);
            
            if (p9_write_send(p9, draw->drawdata_fid, 0, batch, off) < 0) {
                memset(s->prev_framebuf, 0xDE, s->width * s->height * 4);
                s->send_full = 1;
            }
            drain_notify();
            batch_count++;
            
            if (!draw->xor_enabled && tile_count > 0) {
                draw->xor_enabled = 1;
                wlr_log(WLR_INFO, "Alpha-delta mode enabled");
            }
            
            send_count++;
            if (send_count % 30 == 0) {
                int ratio = bytes_raw > 0 ? (int)(bytes_sent * 100 / bytes_raw) : 100;
                wlr_log(WLR_INFO, "Send #%d: %d tiles (%d comp, %d delta) %zu->%zu (%d%%) [%d batches]",
                        send_count, tile_count, comp_tiles, delta_tiles,
                        bytes_raw, bytes_sent, ratio, batch_count);
            }
        }
        
        pthread_mutex_lock(&s->send_lock);
        s->active_buf = -1;
        pthread_mutex_unlock(&s->send_lock);
    }
    
    drain_stop();
    compress_pool_shutdown();
    free(work);
    free(results);
    free(comp_buf);
    free(batch);
    wlr_log(WLR_INFO, "Send thread exiting");
    return NULL;
}
