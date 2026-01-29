/*
 * output.c - Output and input device handlers
 *
 * Handles output frame rendering, resize handling,
 * and input device attachment.
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "output.h"
#include "../draw/send.h"
#include "../p9/p9.h"

static void output_destroy(struct wl_listener *listener, void *data) {
    struct server *s = wl_container_of(listener, s, output_destroy);
    (void)data;
    wl_list_remove(&s->output_frame.link);
    wl_list_remove(&s->output_destroy.link);
}

static void output_frame(struct wl_listener *listener, void *data) {
    struct server *s = wl_container_of(listener, s, output_frame);
    struct wlr_scene_output *so = s->scene_output;
    static int frame_count = 0;
    static uint32_t last_first_pixel = 0;
    (void)data;
    
    /* Check if window was resized - read atomically under lock */
    pthread_mutex_lock(&s->send_lock);
    int resize_pending = s->resize_pending;
    int new_w = s->pending_width;
    int new_h = s->pending_height;
    int new_minx = s->pending_minx;
    int new_miny = s->pending_miny;
    if (resize_pending) {
        s->resize_pending = 0;
    }
    pthread_mutex_unlock(&s->send_lock);
    
    if (resize_pending) {
        if (new_w == s->width && new_h == s->height) {
            struct draw_state *draw = &s->draw;
            draw->win_minx = new_minx;
            draw->win_miny = new_miny;
            wlr_log(WLR_DEBUG, "Position update only: (%d,%d)", new_minx, new_miny);
        } else {
            wlr_log(WLR_INFO, "Main thread handling resize: %dx%d -> %dx%d", 
                    s->width, s->height, new_w, new_h);
            
            size_t fb_size = new_w * new_h * sizeof(uint32_t);
            uint32_t *new_framebuf = calloc(1, fb_size);
            uint32_t *new_prev_framebuf = calloc(1, fb_size);
            uint32_t *new_send_buf0 = calloc(1, fb_size);
            uint32_t *new_send_buf1 = calloc(1, fb_size);
            
            if (!new_framebuf || !new_prev_framebuf || !new_send_buf0 || !new_send_buf1) {
                wlr_log(WLR_ERROR, "Resize failed: could not allocate buffers");
                free(new_framebuf);
                free(new_prev_framebuf);
                free(new_send_buf0);
                free(new_send_buf1);
            } else {
                uint32_t *old_framebuf = s->framebuf;
                uint32_t *old_prev_framebuf = s->prev_framebuf;
                uint32_t *old_send_buf0, *old_send_buf1;
                struct draw_state *draw = &s->draw;
                
                pthread_mutex_lock(&s->send_lock);
                old_send_buf0 = s->send_buf[0];
                old_send_buf1 = s->send_buf[1];
                
                s->framebuf = new_framebuf;
                s->prev_framebuf = new_prev_framebuf;
                s->send_buf[0] = new_send_buf0;
                s->send_buf[1] = new_send_buf1;
                s->pending_buf = -1;
                s->active_buf = -1;
                
                s->width = new_w;
                s->height = new_h;
                s->tiles_x = (new_w + TILE_SIZE - 1) / TILE_SIZE;
                s->tiles_y = (new_h + TILE_SIZE - 1) / TILE_SIZE;
                draw->width = new_w;
                draw->height = new_h;
                draw->win_minx = new_minx;
                draw->win_miny = new_miny;
                pthread_mutex_unlock(&s->send_lock);
                
                free(old_framebuf);
                free(old_prev_framebuf);
                free(old_send_buf0);
                free(old_send_buf1);
                
                /* Reallocate Plan 9 images */
                struct p9conn *p9 = draw->p9;
                uint8_t freecmd[5];
                freecmd[0] = 'f';
                PUT32(freecmd + 1, draw->image_id);
                p9_write(p9, draw->drawdata_fid, 0, freecmd, 5);
                PUT32(freecmd + 1, draw->delta_id);
                p9_write(p9, draw->drawdata_fid, 0, freecmd, 5);
                
                uint8_t bcmd[64];
                int off = 0;
                bcmd[off++] = 'b';
                PUT32(bcmd + off, draw->image_id); off += 4;
                PUT32(bcmd + off, 0); off += 4;
                bcmd[off++] = 0;
                PUT32(bcmd + off, 0x68081828); off += 4;
                bcmd[off++] = 0;
                PUT32(bcmd + off, 0); off += 4;
                PUT32(bcmd + off, 0); off += 4;
                PUT32(bcmd + off, new_w); off += 4;
                PUT32(bcmd + off, new_h); off += 4;
                PUT32(bcmd + off, 0); off += 4;
                PUT32(bcmd + off, 0); off += 4;
                PUT32(bcmd + off, new_w); off += 4;
                PUT32(bcmd + off, new_h); off += 4;
                PUT32(bcmd + off, 0x00000000); off += 4;
                p9_write(p9, draw->drawdata_fid, 0, bcmd, off);
                
                off = 0;
                bcmd[off++] = 'b';
                PUT32(bcmd + off, draw->delta_id); off += 4;
                PUT32(bcmd + off, 0); off += 4;
                bcmd[off++] = 0;
                PUT32(bcmd + off, 0x48081828); off += 4;
                bcmd[off++] = 0;
                PUT32(bcmd + off, 0); off += 4;
                PUT32(bcmd + off, 0); off += 4;
                PUT32(bcmd + off, new_w); off += 4;
                PUT32(bcmd + off, new_h); off += 4;
                PUT32(bcmd + off, 0); off += 4;
                PUT32(bcmd + off, 0); off += 4;
                PUT32(bcmd + off, new_w); off += 4;
                PUT32(bcmd + off, new_h); off += 4;
                PUT32(bcmd + off, 0x00000000); off += 4;
                p9_write(p9, draw->drawdata_fid, 0, bcmd, off);
                
                /* Resize wlroots output */
                struct wlr_output_state state;
                wlr_output_state_init(&state);
                wlr_output_state_set_custom_mode(&state, new_w, new_h, 0);
                if (s->scale > 1.0f) {
                    wlr_output_state_set_scale(&state, s->scale);
                }
                wlr_output_commit_state(s->output, &state);
                wlr_output_state_finish(&state);
                
                /* Compute logical dimensions for Wayland clients */
                int logical_w = (int)(new_w / s->scale + 0.5f);
                int logical_h = (int)(new_h / s->scale + 0.5f);
                
                /* Send configure to all toplevels (using logical dimensions) */
                struct toplevel *tl;
                wl_list_for_each(tl, &s->toplevels, link) {
                    if (tl->xdg && tl->xdg->base && tl->xdg->base->initialized) {
                        wlr_xdg_toplevel_set_size(tl->xdg, logical_w, logical_h);
                    }
                }
                
                /* Resize background (scene uses logical coordinates) */
                if (s->background) {
                    wlr_scene_rect_set_size(s->background, logical_w, logical_h);
                }
                
                draw->xor_enabled = 0;
                s->force_full_frame = 1;
                
                wlr_log(WLR_INFO, "Resize complete: %dx%d physical, %dx%d logical at (%d,%d)", 
                        new_w, new_h, logical_w, logical_h, new_minx, new_miny);
            }
        }
    }
    
    uint32_t now = now_ms();
#if FRAME_INTERVAL_MS > 0
    if (now - s->last_frame_ms < FRAME_INTERVAL_MS) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        wlr_scene_output_send_frame_done(so, &ts);
        return;
    }
#endif
    s->last_frame_ms = now;
    frame_count++;
    
    struct wlr_output_state ostate;
    wlr_output_state_init(&ostate);
    struct wlr_scene_output_state_options opts = {0};
    
    if (!wlr_scene_output_build_state(so, &ostate, &opts)) {
        if (frame_count <= 10 || frame_count % 60 == 0) {
            wlr_log(WLR_DEBUG, "Frame %d: build_state failed", frame_count);
        }
        wlr_output_state_finish(&ostate);
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        wlr_scene_output_send_frame_done(so, &ts);
        return;
    }
    
    struct wlr_buffer *buffer = ostate.buffer;
    if (buffer) {
        void *data_ptr;
        uint32_t format;
        size_t stride;
        
        if (wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ,
                                              &data_ptr, &format, &stride)) {
            pthread_mutex_lock(&s->send_lock);
            
            int w = s->width;
            int h = s->height;
            uint32_t *fb = s->framebuf;
            uint32_t first_pix = 0, mid_pix = 0;
            int valid_fb = 0;
            
            if (fb && w > 0 && h > 0 && w <= 4096 && h <= 4096) {
                valid_fb = 1;
                int buf_w = buffer->width;
                int buf_h = buffer->height;
                int copy_w = (buf_w < w) ? buf_w : w;
                int copy_h = (buf_h < h) ? buf_h : h;
                
                for (int y = 0; y < copy_h; y++) {
                    memcpy(&fb[y * w],
                           (uint8_t*)data_ptr + y * stride,
                           copy_w * 4);
                }
                
                first_pix = fb[0];
                mid_pix = fb[(h/2) * w + w/2];
            }
            
            pthread_mutex_unlock(&s->send_lock);
            wlr_buffer_end_data_ptr_access(buffer);
            
            if (valid_fb) {
                if (first_pix != last_first_pixel || frame_count <= 10 || frame_count % 60 == 0) {
                    wlr_log(WLR_INFO, "Frame %d: first=0x%08x mid=0x%08x (changed=%d)", 
                            frame_count, first_pix, mid_pix, first_pix != last_first_pixel);
                    last_first_pixel = first_pix;
                }
            }
        } else {
            if (frame_count <= 10 || frame_count % 60 == 0)
                wlr_log(WLR_ERROR, "Frame %d: buffer access failed", frame_count);
        }
    } else {
        if (frame_count <= 10 || frame_count % 60 == 0)
            wlr_log(WLR_DEBUG, "Frame %d: no buffer in state", frame_count);
    }
    
    wlr_output_commit_state(s->output, &ostate);
    wlr_output_state_finish(&ostate);
    
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    wlr_scene_output_send_frame_done(so, &ts);
    
    send_frame(s);
}

void new_output(struct wl_listener *l, void *d) {
    struct server *s = wl_container_of(l, s, new_output);
    struct wlr_output *out = d;
    
    wlr_output_init_render(out, s->allocator, s->renderer);
    
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    wlr_output_state_set_custom_mode(&state, s->width, s->height, 60000);
    if (s->scale > 1.0f) {
        wlr_output_state_set_scale(&state, s->scale);
    }
    wlr_output_commit_state(out, &state);
    wlr_output_state_finish(&state);
    
    wlr_output_layout_add_auto(s->output_layout, out);
    s->output = out;
    s->scene_output = wlr_scene_output_create(s->scene, out);
    
    s->output_frame.notify = output_frame;
    wl_signal_add(&out->events.frame, &s->output_frame);
    s->output_destroy.notify = output_destroy;
    wl_signal_add(&out->events.destroy, &s->output_destroy);
    
    if (s->scale > 1.0f) {
        int logical_w = (int)(s->width / s->scale + 0.5f);
        int logical_h = (int)(s->height / s->scale + 0.5f);
        wlr_log(WLR_INFO, "Output ready: %dx%d physical, scale=%.2f, %dx%d logical",
                s->width, s->height, s->scale, logical_w, logical_h);
    } else {
        wlr_log(WLR_INFO, "Output ready: %dx%d", s->width, s->height);
    }
}

void new_input(struct wl_listener *l, void *d) {
    struct server *s = wl_container_of(l, s, new_input);
    struct wlr_input_device *dev = d;
    if (dev->type == WLR_INPUT_DEVICE_POINTER)
        wlr_cursor_attach_input_device(s->cursor, dev);
}
