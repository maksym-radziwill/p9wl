/*
 * send.c - Frame sending and send thread
 *
 * Handles queuing frames, the send thread main loop,
 * tile compression selection, and pipelined writes.
 *
 * Uses iounit from 9P Ropen to limit batch sizes for atomic writes.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>

#include <wlr/util/log.h>

#include "send.h"
#include "pipeline.h"
#include "compress.h"
#include "scroll.h"
#include "draw/draw.h"
#include "p9/p9.h"
#include "types.h"

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
 * Fractional scaling causes sub-pixel alignment issues that break
 * FFT-based phase correlation scroll detection.
 */
static int scroll_disabled(struct server *s) {
    double floor;
    /* Disable scroll if scaling is fractional */
    return (modf(s->scale, &floor) != 0.0f); 
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
    
    /* Allocate compression buffer */
    const size_t comp_buf_size = TILE_SIZE * TILE_SIZE * 4 + 256;
    uint8_t *comp_buf = malloc(comp_buf_size);
    
    int pending_writes = 0;
    int write_errors = 0;
    while (s->running) {

        /* Wait for work with timeout */
        while (s->pending_buf < 0 && !s->window_changed && s->running) {
            struct timespec ts;
            ts.tv_sec = 0;         // 0 seconds
            ts.tv_nsec = 100000; // 500,000,000 nanoseconds = 0.5 seconds

            nanosleep(&ts, NULL); // The second argument can be used to store remaining time if interrupted by a signal
    
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
            pipeline_reset();
            do_full = 1;
        }
        
        /* Check if wctl thread detected window change */
        if (s->window_changed) {
            wlr_log(WLR_INFO, "Wctl detected window change - re-looking up window");
            s->window_changed = 0;
            relookup_window(s);
            if (s->resize_pending) {
                continue;
            }
            do_full = 1;
        }
        
        /* Also check for "unknown id" error as fallback */
        if (p9->unknown_id_error) {
            wlr_log(WLR_INFO, "Detected unknown_id_error - re-looking up window");
            p9->unknown_id_error = 0;
            relookup_window(s);
            if (s->resize_pending) {
                continue;
            }
            do_full = 1;
        }
        
        /* Periodic probe */
        uint32_t now = now_ms();
        if (now - last_probe_time > 2000 && !s->resize_pending) {
            last_probe_time = now;
            
            uint8_t probe[46];
            int poff = 0;
            probe[poff++] = 'd';
            PUT32(probe + poff, draw->screen_id); poff += 4;
            PUT32(probe + poff, draw->image_id); poff += 4;
            PUT32(probe + poff, draw->opaque_id); poff += 4;
            PUT32(probe + poff, draw->win_minx); poff += 4;
            PUT32(probe + poff, draw->win_miny); poff += 4;
            PUT32(probe + poff, draw->win_minx + 1); poff += 4;
            PUT32(probe + poff, draw->win_miny + 1); poff += 4;
            PUT32(probe + poff, 0); poff += 4;
            PUT32(probe + poff, 0); poff += 4;
            PUT32(probe + poff, 0); poff += 4;
            PUT32(probe + poff, 0); poff += 4;
            probe[poff++] = 'v';
            p9_write(p9, draw->drawdata_fid, 0, probe, poff);
            
            if (p9->unknown_id_error) {
                wlr_log(WLR_INFO, "Probe detected window change - re-looking up");
                p9->unknown_id_error = 0;
                relookup_window(s);
                if (s->resize_pending) {
                    continue;
                }
                do_full = 1;
            }
        }
        
        if (!got_frame) {
            continue;
        }
        
        if (s->resize_pending) {
            wlr_log(WLR_DEBUG, "Send thread: skipping frame, resize pending");
            continue;
        }
        
        if (s->force_full_frame) {
            do_full = 1;
            s->force_full_frame = 0;
        }
        
        uint64_t t_frame_start = now_us();
        uint64_t t_send_done = 0, t_recv_done = 0;
        
        /* Try to detect scroll (disabled for fractional scaling) */
        int scrolled_regions = 0;
        if (!do_full && !scroll_disabled(s)) {
            detect_scroll(s, send_buf);
            scrolled_regions = send_scroll_commands(s, &pending_writes);    
        }
        
        /* Send tiles */
        size_t off = 0;
        int tile_count = 0;
        int batch_count = 0;
        int comp_tiles = 0, delta_tiles = 0;
        size_t bytes_raw = 0, bytes_sent = 0;
        
        /* Per-batch stats */
        int batch_tiles = 0;
        size_t batch_raw = 0, batch_sent = 0;
        int batch_comp = 0, batch_delta = 0;
        
        int can_delta = draw->xor_enabled && !do_full && s->prev_framebuf;
        
        for (int ty = 0; ty < s->tiles_y; ty++) {
            for (int tx = 0; tx < s->tiles_x; tx++) {
                if (do_full || tile_changed_send(s, send_buf, tx, ty)) {
                    int x1 = tx * TILE_SIZE, y1 = ty * TILE_SIZE;
                    int x2 = x1 + TILE_SIZE, y2 = y1 + TILE_SIZE;
                    if (x2 > s->width) x2 = s->width;
                    if (y2 > s->height) y2 = s->height;
                    int w = x2 - x1, h = y2 - y1;
                    if (w <= 0 || h <= 0) continue;
                    
                    int raw_size = w * h * 4;
                    bytes_raw += raw_size;
                    
                    int comp_result = 0;
                    if (comp_buf) {
                        comp_result = compress_tile_adaptive(comp_buf, comp_buf_size,
                                                             send_buf, s->width,
                                                             can_delta ? s->prev_framebuf : NULL,
                                                             s->width,
                                                             x1, y1, w, h);
                    }
                    
                    int use_delta = (comp_result > 0);
                    int comp_size = use_delta ? comp_result : (comp_result < 0 ? -comp_result : 0);

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
                        pending_writes++;
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
                            memcpy(batch + off, comp_buf, comp_size);
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
                            memcpy(batch + off, comp_buf, comp_size);
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
                        
                        for (int row = 0; row < h; row++) {
                            memcpy(batch + off, &send_buf[(y1 + row) * s->width + x1], w * 4);
                            off += w * 4;
                        }
                        bytes_sent += raw_size;
                        batch_sent += raw_size;
                    }
                    
                    /* Track batch stats */
                    batch_tiles++;
                    batch_raw += raw_size;
                    
                    /* Update prev_framebuf */
                    for (int row = 0; row < h; row++) {
                        memcpy(&s->prev_framebuf[(y1 + row) * s->width + x1],
                               &send_buf[(y1 + row) * s->width + x1], w * 4);
                    }
                    
                    tile_count++;
                }
            }
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
                pending_writes++;
                batch_count++;
                off = 0;
                
            }
            
            /* Copy buffer to window */
            batch[off++] = 'd';
            PUT32(batch + off, draw->screen_id); off += 4;
            PUT32(batch + off, draw->image_id); off += 4;
            PUT32(batch + off, draw->opaque_id); off += 4;
            PUT32(batch + off, draw->win_minx); off += 4;
            PUT32(batch + off, draw->win_miny); off += 4;
            PUT32(batch + off, draw->win_minx + draw->width); off += 4;
            PUT32(batch + off, draw->win_miny + draw->height); off += 4;
            PUT32(batch + off, 0); off += 4;
            PUT32(batch + off, 0); off += 4;
            PUT32(batch + off, 0); off += 4;
            PUT32(batch + off, 0); off += 4;
            
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
            pending_writes++;
            batch_count++;
            
            t_send_done = now_us();
            
            while(pending_writes > 0){
                if (p9_write_recv(p9) < 0) {
                    write_errors++;
                }
                pending_writes--;
            }

            t_recv_done = now_us();
            
            if (write_errors > 0) {
                wlr_log(WLR_ERROR, "Pipelined writes: %d/%d failed", write_errors, batch_count);
                memset(s->prev_framebuf, 0xDE, s->width * s->height * 4);
            }
            
            double send_ms = (t_send_done - t_frame_start) / 1000.0;
            double recv_ms = (t_recv_done - t_send_done) / 1000.0;
            double total_ms = (t_recv_done - t_frame_start) / 1000.0;
            
            if (total_ms > 50 || send_count <= 10) {
                wlr_log(WLR_INFO, "PIPE(d=%d): %d batches, send=%.1fms, drain=%.1fms, total=%.1fms",
                    pipeline_get_depth(), batch_count, send_ms, recv_ms, total_ms);
            }
            
            /* Enable alpha-delta mode after first successful full frame */
            if (!draw->xor_enabled && tile_count > 0 && write_errors == 0) {
                draw->xor_enabled = 1;
                wlr_log(WLR_INFO, "Alpha-delta mode enabled for future frames");
            }
            
            send_count++;
            if (send_count <= 20 || send_count % 100 == 0) {
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
    
    if (comp_buf) free(comp_buf);
    free(batch);
    wlr_log(WLR_INFO, "Send thread exiting");
    return NULL;
}
