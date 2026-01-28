/*
 * send.c - Frame sending and send thread (SHARP SCALING VERSION)
 *
 * Handles queuing frames, the send thread main loop,
 * tile compression selection, and pipelined writes.
 *
 * Uses iounit from 9P Ropen to limit batch sizes for atomic writes.
 * Uses a separate drain thread for async response handling.
 *
 * SHARP SCALING: The 'a' (affine warp) command now DOWNSCALES from
 * high-resolution render buffer to physical window, producing sharp results.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

#include <wlr/util/log.h>

#include "send.h"
#include "compress.h"
#include "scroll.h"
#include "draw/draw.h"
#include "p9/p9.h"
#include "types.h"

/* Drain thread state */
struct drain_ctx {
    struct p9conn *p9;
    atomic_int pending;
    atomic_int errors;
    atomic_int running;
    atomic_int paused;      /* Pause flag for synchronous p9 ops */
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    uint8_t *recv_buf;  /* Separate buffer for drain thread */
};

static struct drain_ctx drain;

/* Read one Rwrite response using drain thread's own buffer */
static int drain_recv_one(void) {
    uint8_t *buf = drain.recv_buf;
    struct p9conn *p9 = drain.p9;
    
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
        wlr_log(WLR_ERROR, "9P drain error: %s", errmsg);
        
        /* Set error flags */
        if (strstr(errmsg, "unknown id") != NULL) p9->unknown_id_error = 1;
        if (strstr(errmsg, "short") != NULL) p9->draw_error = 1;
        return -1;
    }
    
    if (type != Rwrite) {
        wlr_log(WLR_ERROR, "9P drain: unexpected type %d, expected Rwrite", type);
        return -1;
    }
    
    return GET32(buf + 7);
}

/* Drain thread - receives 9P responses in background */
static void *drain_thread_func(void *arg) {
    (void)arg;
    
    wlr_log(WLR_INFO, "Drain thread started");
    
    while (atomic_load(&drain.running)) {
        /* Wait if no pending writes */
        pthread_mutex_lock(&drain.lock);
        while (atomic_load(&drain.pending) == 0 && atomic_load(&drain.running)) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 10000000;  /* 10ms timeout */
            pthread_cond_timedwait(&drain.cond, &drain.lock, &ts);
        }
        pthread_mutex_unlock(&drain.lock);
        
        if (!atomic_load(&drain.running)) break;
        
        /* If paused but still have pending, keep draining until pending = 0 */
        /* Then stop and wait for resume */
        if (atomic_load(&drain.paused) && atomic_load(&drain.pending) == 0) {
            continue;  /* Go back to wait loop */
        }
        
        /* Drain one response using our own buffer */
        if (atomic_load(&drain.pending) > 0) {
            if (drain_recv_one() < 0) {
                atomic_fetch_add(&drain.errors, 1);
            }
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
    
    /* Allocate separate buffer for drain thread */
    drain.recv_buf = malloc(p9->msize);
    if (!drain.recv_buf) {
        wlr_log(WLR_ERROR, "Failed to allocate drain recv buffer");
        return -1;
    }
    
    if (pthread_create(&drain.thread, NULL, drain_thread_func, NULL) != 0) {
        wlr_log(WLR_ERROR, "Failed to create drain thread");
        free(drain.recv_buf);
        return -1;
    }
    return 0;
}

static void drain_stop(void) {
    atomic_store(&drain.running, 0);
    
    pthread_mutex_lock(&drain.lock);
    pthread_cond_signal(&drain.cond);
    pthread_mutex_unlock(&drain.lock);
    
    pthread_join(drain.thread, NULL);
    
    /* Drain any remaining using our buffer */
    while (atomic_load(&drain.pending) > 0) {
        drain_recv_one();
        atomic_fetch_sub(&drain.pending, 1);
    }
    
    if (drain.recv_buf) {
        free(drain.recv_buf);
        drain.recv_buf = NULL;
    }
    
    pthread_mutex_destroy(&drain.lock);
    pthread_cond_destroy(&drain.cond);
}

/*
 * Pause drain thread and drain all pending responses.
 * Must be called before any synchronous p9 operations (relookup_window, etc.)
 * The drain thread will stop reading from the socket.
 */
static void drain_pause(void) {
    atomic_store(&drain.paused, 1);
    
    /* Wake up drain thread in case it's waiting on cond var */
    pthread_mutex_lock(&drain.lock);
    pthread_cond_signal(&drain.cond);
    pthread_mutex_unlock(&drain.lock);
    
    /* Wait for drain thread to finish processing all pending responses */
    while (atomic_load(&drain.pending) > 0) {
            struct timespec req;

	        req.tv_sec = 0;
    	    req.tv_nsec = 1000000L; // Use 'L' for long integer

    	    nanosleep(&req, NULL);
    }
}

/*
 * Resume drain thread after synchronous p9 operations.
 */
static void drain_resume(void) {
    atomic_store(&drain.paused, 0);
    
    /* Wake up drain thread */
    pthread_mutex_lock(&drain.lock);
    pthread_cond_signal(&drain.cond);
    pthread_mutex_unlock(&drain.lock);
}

/*
 * Exported function to pause send system for resize operations.
 * Waits for all pending 9P writes to complete.
 * Call this BEFORE modifying 9P images.
 */
void send_system_pause(void) {
    if (atomic_load(&drain.running)) {
        drain_pause();
    }
}

/*
 * Exported function to resume send system after resize operations.
 * Call this AFTER modifying 9P images.
 */
void send_system_resume(void) {
    if (atomic_load(&drain.running)) {
        drain_resume();
    }
}

static void drain_notify(void) {
    atomic_fetch_add(&drain.pending, 1);
    pthread_mutex_lock(&drain.lock);
    pthread_cond_signal(&drain.cond);
    pthread_mutex_unlock(&drain.lock);
}

/* Wait until pending drops below threshold */
static void drain_throttle(int max_pending) {
    while (atomic_load(&drain.pending) > max_pending) {
        struct timespec ts = {0, 1000000};  /* 1ms */
        nanosleep(&ts, NULL);
    }
}

void send_frame(struct server *s) {
    pthread_mutex_lock(&s->send_lock);
    
    /* Check for resize - don't send if resize is pending */
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
    
    /* Copy framebuf to send buffer */
    memcpy(s->send_buf[buf], s->framebuf, s->width * s->height * 4);
    
    /* Mark as pending and signal send thread */
    s->pending_buf = buf;
    if (s->force_full_frame) {
        s->send_full = 1;
    }
    pthread_cond_signal(&s->send_cond);
    pthread_mutex_unlock(&s->send_lock);
}

int send_timer_callback(void *data) {
    struct server *s = data; 

    /* Only send if we have new content */
    if (!s->frame_dirty) {
        return 0;
    }
    s->frame_dirty = 0;
 
    send_frame(s);
    return 0;
}

int tile_changed_send(struct server *s, uint32_t *send_buf, int tx, int ty) {
    int x1 = tx * TILE_SIZE, y1 = ty * TILE_SIZE;
    int x2 = x1 + TILE_SIZE, y2 = y1 + TILE_SIZE;
    if (x2 > s->width) x2 = s->width;
    if (y2 > s->height) y2 = s->height;
    int w = x2 - x1;
    
    for (int y = y1; y < y2; y++) {
        if (memcmp(&send_buf[y * s->width + x1],
                   &s->prev_framebuf[y * s->width + x1], w * 4) != 0) {
            return 1;
        }
    }
    return 0;
}

/*
 * Check if scroll optimization should be disabled.
 * 
 * With the new sharp scaling architecture, scroll detection works
 * because everything operates at render resolution:
 * - Compositor renders at render resolution
 * - Scroll detection compares render pixels
 * - Scroll command operates on render source image
 * 
 * Only disable scroll for other reasons (e.g., debugging).
 */
static int scroll_disabled(struct server *s) {
    (void)s;
    return 0;  /* Scroll works with sharp scaling now */
}

void *send_thread_func(void *arg) {
    struct server *s = arg;
    struct draw_state *draw = &s->draw;
    struct p9conn *p9 = draw->p9;
    static int send_count = 0;
    uint32_t last_probe_time = 0;
    
    wlr_log(WLR_INFO, "Send thread started");
    
    /* Log if scroll optimization is disabled */
    if (scroll_disabled(s)) {
        wlr_log(WLR_INFO, "Scroll optimization disabled (fractional scale: %.2f)", s->scale);
    }
    
    /*
     * Determine max batch size from iounit.
     * iounit is the maximum atomic write size for the drawdata fd.
     * If not set (0), fall back to msize - 24 per 9P spec.
     */
    size_t max_batch = draw->iounit;
    if (max_batch == 0) {
        max_batch = p9->msize - 24;
    }
    /* Leave some headroom for Twrite header (23 bytes) */
    if (max_batch > 23) {
        max_batch -= 23;
    }
    
    wlr_log(WLR_INFO, "Send thread: using max_batch=%zu (iounit=%u, msize=%u)",
            max_batch, draw->iounit, p9->msize);
    
    uint8_t *batch = malloc(max_batch);
    if (!batch) {
        wlr_log(WLR_ERROR, "Send thread: failed to allocate batch buffer");
        return NULL;
    }
    
    /* Start drain thread */
    if (drain_start(p9) < 0) {
        wlr_log(WLR_ERROR, "Failed to start drain thread");
        free(batch);
        return NULL;
    }
    
    /* Initialize parallel compression pool */
    int nthreads = sysconf(_SC_NPROCESSORS_ONLN) / 2;  
    if (compress_pool_init(nthreads) < 0) {
        wlr_log(WLR_ERROR, "Failed to init compression pool, falling back to single-threaded");
        nthreads = 0;
    } else {
        wlr_log(WLR_INFO, "Compression pool initialized with %d threads", nthreads);
    }
    
    /* Allocate work arrays for parallel compression */
    /* Use max possible size (4096x4096 / 16x16 = 65536 tiles) to handle resizes */
    int max_tiles = (4096 / TILE_SIZE) * (4096 / TILE_SIZE);
    struct tile_work *work = malloc(max_tiles * sizeof(struct tile_work));
    struct tile_result *results = malloc(max_tiles * sizeof(struct tile_result));
    if (!work || !results) {
        wlr_log(WLR_ERROR, "Failed to allocate compression work arrays");
        nthreads = 0;  /* Fall back to single-threaded */
    }
    
    /* Allocate compression buffer for single-threaded fallback */
    const size_t comp_buf_size = TILE_SIZE * TILE_SIZE * 4 + 256;
    uint8_t *comp_buf = malloc(comp_buf_size);
    
    int write_errors = 0;
    while (s->running) {

        /* Wait for work with timeout */
        while (s->pending_buf < 0 && !s->window_changed && s->running) {
            struct timespec ts;
            ts.tv_sec = 0;          
            ts.tv_nsec = 100000;  
            nanosleep(&ts, NULL);  
        }
        
        if (!s->running) {
            break;
        }

        pthread_mutex_lock(&s->send_lock);

        /* Capture which buffer to process */
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
        
        /* Check for draw protocol errors */
        if (p9->draw_error) {
            wlr_log(WLR_INFO, "Recovering from draw error - forcing full refresh");
            p9->draw_error = 0;
            draw->xor_enabled = 0;
            memset(s->prev_framebuf, 0, s->width * s->height * 4);
            do_full = 1;
        }
        
        /* Check for drain errors */
        int drain_errs = atomic_exchange(&drain.errors, 0);
        if (drain_errs > 0) {
            wlr_log(WLR_ERROR, "Drain thread reported %d errors", drain_errs);
            memset(s->prev_framebuf, 0xDE, s->width * s->height * 4);
        }
        
        /* Check if wctl thread detected window change */
        if (s->window_changed) {
            wlr_log(WLR_INFO, "Wctl detected window change - re-looking up window");
            s->window_changed = 0;
            drain_pause();  /* Stop drain before synchronous p9 ops */
            relookup_window(s);
            drain_resume(); /* Restart drain */
            if (s->resize_pending) {
                continue;
            }
            do_full = 1;
        }
        
        /* Also check for "unknown id" error as fallback */
        if (p9->unknown_id_error) {
            wlr_log(WLR_INFO, "Detected unknown_id_error - re-looking up window");
            p9->unknown_id_error = 0;
            drain_pause();  /* Stop drain before synchronous p9 ops */
            relookup_window(s);
            drain_resume(); /* Restart drain */
            if (s->resize_pending) {
                continue;
            }
            do_full = 1;
        }
        
        if (!got_frame) {
            continue;
        }
        
        if (s->resize_pending) {
            wlr_log(WLR_DEBUG, "Send thread: skipping frame, resize pending");
            pthread_mutex_lock(&s->send_lock);
            s->active_buf = -1;
            pthread_mutex_unlock(&s->send_lock);
            continue;
        }
        
        if (s->force_full_frame) {
            do_full = 1;
            s->force_full_frame = 0;
        }
        
        uint64_t t_frame_start = now_us();
        uint64_t t_send_done = 0;
        
        /* Try to detect scroll (disabled for fractional scaling) */
        int scrolled_regions = 0;
        if (!do_full && !scroll_disabled(s)) {
            detect_scroll(s, send_buf);
            /* Apply scroll to prev_framebuf BEFORE tile detection */
            scrolled_regions = apply_scroll_to_prevbuf(s);
        }
        
        /* Send tiles */
        size_t off = 0;
        
        /* Write scroll commands to batch first (batched with tiles) */
        if (scrolled_regions > 0) {
            off = write_scroll_commands(s, batch, max_batch);
            wlr_log(WLR_DEBUG, "Batched %d scroll regions (%d bytes)", scrolled_regions, (int)off);
        }
        
        int tile_count = 0;
        int batch_count = 0;
        int comp_tiles = 0, delta_tiles = 0;
        size_t bytes_raw = 0, bytes_sent = 0;
        
        /* Per-batch stats */
        int batch_tiles = 0;
        size_t batch_raw = 0, batch_sent = 0;
        int batch_comp = 0, batch_delta = 0;
        
        int can_delta = draw->xor_enabled && !do_full && s->prev_framebuf;
        
        /* First pass: collect changed tiles */
        int work_count = 0;
        for (int ty = 0; ty < s->tiles_y; ty++) {
            for (int tx = 0; tx < s->tiles_x; tx++) {
                if (do_full || tile_changed_send(s, send_buf, tx, ty)) {
                    int x1 = tx * TILE_SIZE, y1 = ty * TILE_SIZE;
                    int x2 = x1 + TILE_SIZE, y2 = y1 + TILE_SIZE;
                    if (x2 > s->width) x2 = s->width;
                    if (y2 > s->height) y2 = s->height;
                    int w = x2 - x1, h = y2 - y1;
                    if (w <= 0 || h <= 0) continue;
                    
                    if (work_count >= max_tiles) {
                        wlr_log(WLR_ERROR, "Work array overflow!");
                        goto tiles_collected;
                    }
                    
                    /* Check if tile overlaps scroll-exposed region (marked 0xDEADBEEF).
                     * Server's buffer has garbage there after blit, so delta
                     * encoding would produce wrong pixels. Force raw.
                     * Check all 4 edges since exposed boundary may cut through
                     * tile from any direction (vertical or horizontal scroll). */
                    int use_delta = can_delta;
                    if (use_delta) {
                        int x2m1 = x1 + w - 1;
                        int y2m1 = y1 + h - 1;
                        /* Check top and bottom rows */
                        for (int x = x1; x < x1 + w && use_delta; x++) {
                            if (s->prev_framebuf[y1 * s->width + x] == 0xDEADBEEF ||
                                s->prev_framebuf[y2m1 * s->width + x] == 0xDEADBEEF) {
                                use_delta = 0;
                            }
                        }
                        /* Check left and right columns */
                        for (int y = y1; y <= y2m1 && use_delta; y++) {
                            if (s->prev_framebuf[y * s->width + x1] == 0xDEADBEEF ||
                                s->prev_framebuf[y * s->width + x2m1] == 0xDEADBEEF) {
                                use_delta = 0;
                            }
                        }
                    }
                    
                    work[work_count].pixels = send_buf;
                    work[work_count].stride = s->width;
                    work[work_count].prev_pixels = use_delta ? s->prev_framebuf : NULL;
                    work[work_count].prev_stride = s->width;
                    work[work_count].x1 = x1;
                    work[work_count].y1 = y1;
                    work[work_count].w = w;
                    work[work_count].h = h;
                    work_count++;
                }
            }
        }
        tiles_collected:
        
        /* Compress all tiles in parallel */
        if (work_count > 0 && nthreads > 0) {
            compress_tiles_parallel(work, results, work_count);
        }
        
        /* Throttle if too many writes pending */
        drain_throttle(2);
        
        /* Second pass: build batches from results */
        for (int i = 0; i < work_count; i++) {
            struct tile_work *w = &work[i];
            struct tile_result *r = &results[i];
            int x1 = w->x1, y1 = w->y1;
            int x2 = x1 + w->w, y2 = y1 + w->h;
            int raw_size = w->w * w->h * 4;
            
            /* Single-threaded fallback */
            if (nthreads == 0) {
                int comp_result = compress_tile_adaptive(comp_buf, comp_buf_size,
                                                         w->pixels, w->stride,
                                                         w->prev_pixels, w->prev_stride,
                                                         x1, y1, w->w, w->h);
                if (comp_result > 0) {
                    r->size = comp_result;
                    r->is_delta = 1;
                    memcpy(r->data, comp_buf, comp_result);
                } else if (comp_result < 0) {
                    r->size = -comp_result;
                    r->is_delta = 0;
                    memcpy(r->data, comp_buf, -comp_result);
                } else {
                    r->size = 0;
                    r->is_delta = 0;
                }
            }
            
            bytes_raw += raw_size;
            
            int use_delta = r->is_delta;
            int comp_size = r->size;
            
            size_t tile_size;
            if (comp_size > 0) {
                if (use_delta) {
                    tile_size = (1 + 4 + 16 + comp_size) + ALPHA_DELTA_OVERHEAD;
                } else {
                    tile_size = 1 + 4 + 16 + comp_size;
                }
            } else {
                tile_size = 1 + 4 + 16 + raw_size;
            }
            
            /* Flush current batch if needed */
            if (off + tile_size > max_batch && off > 0) {
                if (p9_write_send(p9, draw->drawdata_fid, 0, batch, off) < 0) {
                    wlr_log(WLR_ERROR, "Batch write send failed");
                    memset(s->prev_framebuf, 0xDE, s->width * s->height * 4);
                }
                drain_notify();
                batch_count++;
                
                /* Log batch stats */
                if (batch_tiles > 0) {
                    int ratio = batch_raw > 0 ? (int)(batch_sent * 100 / batch_raw) : 100;
                    wlr_log(WLR_DEBUG, "Batch %d: %d tiles (%d comp, %d delta) %zu->%zu bytes (%d%%)",
                            batch_count, batch_tiles, batch_comp, batch_delta,
                            batch_raw, batch_sent, ratio);
                }
                batch_tiles = 0;
                batch_raw = 0;
                batch_sent = 0;
                batch_comp = 0;
                batch_delta = 0;
                
                off = 0;
            }
            
            if (comp_size > 0) {
                if (use_delta) {
                    /* Alpha-delta path */
                    batch[off++] = 'Y';
                    PUT32(batch + off, draw->delta_id); off += 4;
                    PUT32(batch + off, x1); off += 4;
                    PUT32(batch + off, y1); off += 4;
                    PUT32(batch + off, x2); off += 4;
                    PUT32(batch + off, y2); off += 4;
                    memcpy(batch + off, r->data, comp_size);
                    off += comp_size;
                    
                    /* Composite delta onto image */
                    batch[off++] = 'd';
                    PUT32(batch + off, draw->image_id); off += 4;
                    PUT32(batch + off, draw->delta_id); off += 4;
                    PUT32(batch + off, draw->delta_id); off += 4;
                    PUT32(batch + off, x1); off += 4;
                    PUT32(batch + off, y1); off += 4;
                    PUT32(batch + off, x2); off += 4;
                    PUT32(batch + off, y2); off += 4;
                    PUT32(batch + off, x1); off += 4;
                    PUT32(batch + off, y1); off += 4;
                    PUT32(batch + off, x1); off += 4;
                    PUT32(batch + off, y1); off += 4;
                    
                    bytes_sent += comp_size + ALPHA_DELTA_OVERHEAD;
                    batch_sent += comp_size + ALPHA_DELTA_OVERHEAD;
                    delta_tiles++;
                    batch_delta++;
                } else {
                    /* Direct path */
                    batch[off++] = 'Y';
                    PUT32(batch + off, draw->image_id); off += 4;
                    PUT32(batch + off, x1); off += 4;
                    PUT32(batch + off, y1); off += 4;
                    PUT32(batch + off, x2); off += 4;
                    PUT32(batch + off, y2); off += 4;
                    memcpy(batch + off, r->data, comp_size);
                    off += comp_size;
                    bytes_sent += comp_size;
                    batch_sent += comp_size;
                }
                comp_tiles++;
                batch_comp++;
            } else {
                /* Uncompressed */
                batch[off++] = 'y';
                PUT32(batch + off, draw->image_id); off += 4;
                PUT32(batch + off, x1); off += 4;
                PUT32(batch + off, y1); off += 4;
                PUT32(batch + off, x2); off += 4;
                PUT32(batch + off, y2); off += 4;
                
                for (int row = 0; row < w->h; row++) {
                    memcpy(batch + off, &send_buf[(y1 + row) * s->width + x1], w->w * 4);
                    off += w->w * 4;
                }
                bytes_sent += raw_size;
                batch_sent += raw_size;
            }
            
            /* Track batch stats */
            batch_tiles++;
            batch_raw += raw_size;
            
            /* Update prev_framebuf */
            for (int row = 0; row < w->h; row++) {
                memcpy(&s->prev_framebuf[(y1 + row) * s->width + x1],
                       &send_buf[(y1 + row) * s->width + x1], w->w * 4);
            }
            
            tile_count++;
        }
        
        /* Send final batch with copy+borders+flush */
        if (tile_count > 0 || scrolled_regions > 0) {
            /* Check if we need to flush before adding footer commands */
            size_t footer_size = 45 + 45*4 + 1;  /* draw + 4 borders + flush */
            if (off + footer_size > max_batch && off > 0) {
                if (p9_write_send(p9, draw->drawdata_fid, 0, batch, off) < 0) {
                    wlr_log(WLR_ERROR, "Pre-footer batch write send failed");
                    memset(s->prev_framebuf, 0xDE, s->width * s->height * 4);
                }
                drain_notify();
                batch_count++;
                off = 0;
                
            }
            
            /* Copy buffer to window using 'a' (affine warp) command with DOWNSCALING.
             *
             * Matrix uses 25.7 fixed-point (1.0 = 0x80 = 128).
             *
             * For 1.5× overall HiDPI scaling:
             * - Logical size: L (what clients see, e.g., 960×672)
             * - Render buffer: R = L × 2 (2× Wayland, e.g., 1920×1344)
             * - Physical window: P = L × 1.5 (e.g., 1440×1008)
             * - 9front downscale: R → P by factor 4/3
             *
             * Matrix = render / physical = 4/3 ≈ 1.333
             * In fixed-point: 4/3 × 128 ≈ 171
             *
             * The 'a' command formula: src_p = M * (dest_p - R.min) + sp0
             * With matrix=4/3:
             * - dest pixel 0 → src pixel 0
             * - dest pixel 3 → src pixel 4
             * - etc.
             */
            
            /* Get scale from draw state (4/3 for 1.5× mode = render/physical) */
            float scale = draw->scale;
            if (scale <= 0.0f) scale = 1.0f;
            
            /* Matrix diagonal in 25.7 fixed-point: scale * 128
             * For 4/3 downscaling: matrix ≈ 171
             */
            int matrix_scale;
            if (scale > 1.001f) {
                matrix_scale = (int)(128.0f * scale + 0.5f);
            } else {
                /* No scaling: identity */
                matrix_scale = 128;
            }
            
            /* smooth=1 enables bilinear interpolation for quality downscale */
            int smooth = (scale > 1.001f) ? 1 : 0;
            
            /* sp0 = (0, 0) to start reading from source origin */
            int sp_x = 0;
            int sp_y = 0;
            
            /* Debug logging for first few frames and periodically */
            static int a_cmd_count = 0;
            a_cmd_count++;
            if (a_cmd_count <= 5 || a_cmd_count % 100 == 0) {
                wlr_log(WLR_INFO, "'a' cmd #%d: 4/3 downscale (1.5× HiDPI), matrix=%d "
                        "src=0,0-%d,%d → dst=(%d,%d)-(%d,%d) smooth=%d",
                        a_cmd_count, matrix_scale, 
                        draw->render_width, draw->render_height,  /* source (render res) */
                        draw->win_minx, draw->win_miny,
                        draw->win_minx + draw->width,             /* dest (physical) */
                        draw->win_miny + draw->height,
                        smooth);
            }
            
            batch[off++] = 'a';
            PUT32(batch + off, draw->screen_id); off += 4;
            
            /* R = dest rect in PHYSICAL window coordinates.
             * This is where the scaled image will be drawn.
             * draw->width/height is now PHYSICAL size (not eff_physical).
             */
            PUT32(batch + off, draw->win_minx); off += 4;
            PUT32(batch + off, draw->win_miny); off += 4;
            PUT32(batch + off, draw->win_minx + draw->width); off += 4;
            PUT32(batch + off, draw->win_miny + draw->height); off += 4;
            
            /* Source image ID (contains render_width × render_height pixels) */
            PUT32(batch + off, draw->image_id); off += 4;
            
            /* sp0 = (0, 0) to start reading from source origin */
            PUT32(batch + off, sp_x); off += 4;
            PUT32(batch + off, sp_y); off += 4;
            
            /* Scaling matrix in 25.7 fixed-point
             * For DOWNSCALING: matrix = scale (> 1.0 = > 128)
             * src_coord = dest_coord × (matrix_scale / 128)
             */
            PUT32(batch + off, matrix_scale); off += 4;  /* m[0][0] = scale */
            PUT32(batch + off, 0); off += 4;              /* m[0][1] = 0 */
            PUT32(batch + off, 0); off += 4;              /* m[0][2] = 0 */
            PUT32(batch + off, 0); off += 4;              /* m[1][0] = 0 */
            PUT32(batch + off, matrix_scale); off += 4;   /* m[1][1] = scale */
            PUT32(batch + off, 0); off += 4;              /* m[1][2] = 0 */
            PUT32(batch + off, 0); off += 4;              /* m[2][0] = 0 */
            PUT32(batch + off, 0); off += 4;              /* m[2][1] = 0 */
            PUT32(batch + off, 0x80); off += 4;           /* m[2][2] = 1.0 */
            batch[off++] = smooth;
            
            /* 
             * Draw borders using actual window bounds.
             * This fills the gap between the TILE_SIZE-aligned content area
             * and the actual window edges, creating equal borders on all sides.
             *
             * Content area: (win_minx, win_miny) to (win_minx + width, win_miny + height)
             * Actual window: (actual_minx, actual_miny) to (actual_maxx, actual_maxy)
             */
            int mx = draw->win_minx;
            int my = draw->win_miny;
            int Mx = mx + draw->width;
            int My = my + draw->height;
            
            /* Use actual window bounds if available, otherwise use a small border */
            int ax = draw->actual_minx;
            int ay = draw->actual_miny;
            int Ax = draw->actual_maxx;
            int Ay = draw->actual_maxy;
            
            /* Fallback: if actual bounds not set, use content bounds with 16px margin */
            if (ax == 0 && ay == 0 && Ax == 0 && Ay == 0) {
                ax = mx - 16;
                ay = my - 16;
                Ax = Mx + 16;
                Ay = My + 16;
            }
            
            /* Top border: from actual top to content top, full actual width */
            if (my > ay) {
                batch[off++] = 'd';
                PUT32(batch + off, draw->screen_id); off += 4;
                PUT32(batch + off, draw->border_id); off += 4;
                PUT32(batch + off, draw->opaque_id); off += 4;
                PUT32(batch + off, ax); off += 4;
                PUT32(batch + off, ay); off += 4;
                PUT32(batch + off, Ax); off += 4;
                PUT32(batch + off, my); off += 4;
                PUT32(batch + off, 0); off += 4;
                PUT32(batch + off, 0); off += 4;
                PUT32(batch + off, 0); off += 4;
                PUT32(batch + off, 0); off += 4;
            }
            
            /* Bottom border: from content bottom to actual bottom, full actual width */
            if (Ay > My) {
                batch[off++] = 'd';
                PUT32(batch + off, draw->screen_id); off += 4;
                PUT32(batch + off, draw->border_id); off += 4;
                PUT32(batch + off, draw->opaque_id); off += 4;
                PUT32(batch + off, ax); off += 4;
                PUT32(batch + off, My); off += 4;
                PUT32(batch + off, Ax); off += 4;
                PUT32(batch + off, Ay); off += 4;
                PUT32(batch + off, 0); off += 4;
                PUT32(batch + off, 0); off += 4;
                PUT32(batch + off, 0); off += 4;
                PUT32(batch + off, 0); off += 4;
            }
            
            /* Left border: from content top to content bottom, actual left to content left */
            if (mx > ax) {
                batch[off++] = 'd';
                PUT32(batch + off, draw->screen_id); off += 4;
                PUT32(batch + off, draw->border_id); off += 4;
                PUT32(batch + off, draw->opaque_id); off += 4;
                PUT32(batch + off, ax); off += 4;
                PUT32(batch + off, my); off += 4;
                PUT32(batch + off, mx); off += 4;
                PUT32(batch + off, My); off += 4;
                PUT32(batch + off, 0); off += 4;
                PUT32(batch + off, 0); off += 4;
                PUT32(batch + off, 0); off += 4;
                PUT32(batch + off, 0); off += 4;
            }
            
            /* Right border: from content top to content bottom, content right to actual right */
            if (Ax > Mx) {
                batch[off++] = 'd';
                PUT32(batch + off, draw->screen_id); off += 4;
                PUT32(batch + off, draw->border_id); off += 4;
                PUT32(batch + off, draw->opaque_id); off += 4;
                PUT32(batch + off, Mx); off += 4;
                PUT32(batch + off, my); off += 4;
                PUT32(batch + off, Ax); off += 4;
                PUT32(batch + off, My); off += 4;
                PUT32(batch + off, 0); off += 4;
                PUT32(batch + off, 0); off += 4;
                PUT32(batch + off, 0); off += 4;
                PUT32(batch + off, 0); off += 4;
            }
            
            /* Flush */
            batch[off++] = 'v';
            
            if (p9_write_send(p9, draw->drawdata_fid, 0, batch, off) < 0) {
                wlr_log(WLR_ERROR, "Final batch write send failed");
                memset(s->prev_framebuf, 0xDE, s->width * s->height * 4);
            }
            drain_notify();
            batch_count++;
            
            t_send_done = now_us();
            
            double send_ms = (t_send_done - t_frame_start) / 1000.0;
            int pending = atomic_load(&drain.pending);
            
            /* Log PIPE stats frequently for debugging */
            if (send_count % 30 == 0) {
                wlr_log(WLR_INFO, "PIPE: %d batches, send=%.1fms, pending=%d",
                    batch_count, send_ms, pending);
            }
            
            /* Enable alpha-delta mode after first successful full frame */
            if (!draw->xor_enabled && tile_count > 0) {
                draw->xor_enabled = 1;
                wlr_log(WLR_INFO, "Alpha-delta mode enabled for future frames");
            }
            
            send_count++;
            if (send_count % 30 == 0) {
                int ratio = bytes_raw > 0 ? (int)(bytes_sent * 100 / bytes_raw) : 100;
                if (scrolled_regions > 0) {
                    wlr_log(WLR_INFO, "Send #%d: %d tiles (%d comp, %d delta) %zu->%zu bytes (%d%%) [%d regions scrolled] [%d batches]", 
                            send_count, tile_count, comp_tiles, delta_tiles,
                            bytes_raw, bytes_sent, ratio, scrolled_regions, batch_count);
                } else {
                    wlr_log(WLR_INFO, "Send #%d: %d tiles (%d comp, %d delta) %zu->%zu bytes (%d%%) [%d batches]", 
                            send_count, tile_count, comp_tiles, delta_tiles,
                            bytes_raw, bytes_sent, ratio, batch_count);
                }
            }
        }
        
        /* Mark send as complete */
        pthread_mutex_lock(&s->send_lock);
        s->active_buf = -1;
        pthread_mutex_unlock(&s->send_lock);
    }
    
    /* Cleanup */
    drain_stop();
    compress_pool_shutdown();
    if (work) free(work);
    if (results) free(results);
    if (comp_buf) free(comp_buf);
    free(batch);
    wlr_log(WLR_INFO, "Send thread exiting");
    return NULL;
}
