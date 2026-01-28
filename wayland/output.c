/*
 * output.c - Output and input device handlers
 *
 * Handles output frame rendering, resize handling,
 * and input device attachment.
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
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

/* Handle resize to new physical dimensions */
void handle_resize(struct server *s, int new_w, int new_h, int new_minx, int new_miny) {
    wlr_log(WLR_INFO, "handle_resize: %dx%d -> %dx%d at (%d,%d)", 
            s->draw.width, s->draw.height, new_w, new_h, new_minx, new_miny);
    
    float scale = s->scale;
    if (scale <= 0.0f) scale = 1.0f;
    
    /* Fractional Wayland mode: -W with scale > 1
     * For scale x where k-1 < x <= k (k = ceil(x)):
     *   - Wayland output scale = k
     *   - Buffer = (k/x) * rio_window
     *   - Scene = buffer/k = rio_window/x
     *   - 'a' command downscales by k/x
     */
    int fractional_wl_mode = (s->wl_scaling && scale > 1.001f);
    int k = (int)ceilf(scale);  /* Integer output scale */
    
    int wl_phys_w, wl_phys_h;  /* Wayland buffer dimensions */
    int scene_w, scene_h;      /* Scene/logical dimensions */
    
    if (fractional_wl_mode) {
        /* Fractional Wayland mode: buffer = (k/scale) * physical */
        float buffer_factor = (float)k / scale;
        wl_phys_w = (int)(new_w * buffer_factor + 0.5f);
        wl_phys_h = (int)(new_h * buffer_factor + 0.5f);
        
        /* Align to tile size */
        wl_phys_w = (wl_phys_w / TILE_SIZE) * TILE_SIZE;
        wl_phys_h = (wl_phys_h / TILE_SIZE) * TILE_SIZE;
        if (wl_phys_w < TILE_SIZE * 4) wl_phys_w = TILE_SIZE * 4;
        if (wl_phys_h < TILE_SIZE * 4) wl_phys_h = TILE_SIZE * 4;
        
        /* Scene dims = wl_phys / k = rio / scale */
        scene_w = wl_phys_w / k;
        scene_h = wl_phys_h / k;
        
        wlr_log(WLR_INFO, "Resize (fractional Wayland k=%d): rio %dx%d, wl_buf %dx%d, scene %dx%d",
                k, new_w, new_h, wl_phys_w, wl_phys_h, scene_w, scene_h);
    } else if (s->wl_scaling || scale <= 1.001f) {
        /* Wayland scaling mode (no -S) or no scaling needed (scale <= 1) */
        wl_phys_w = (new_w / TILE_SIZE) * TILE_SIZE;
        wl_phys_h = (new_h / TILE_SIZE) * TILE_SIZE;
        if (wl_phys_w < TILE_SIZE * 4) wl_phys_w = TILE_SIZE * 4;
        if (wl_phys_h < TILE_SIZE * 4) wl_phys_h = TILE_SIZE * 4;
        
        scene_w = wl_phys_w;
        scene_h = wl_phys_h;
        
        wlr_log(WLR_INFO, "Resize (Wayland scaling): physical %dx%d, scene %dx%d",
                new_w, new_h, scene_w, scene_h);
    } else {
        /* 9front scaling mode: compositor at logical resolution */
        wl_phys_w = (int)(new_w / scale);
        wl_phys_h = (int)(new_h / scale);
        
        /* Align logical dimensions to tile size */
        wl_phys_w = (wl_phys_w / TILE_SIZE) * TILE_SIZE;
        wl_phys_h = (wl_phys_h / TILE_SIZE) * TILE_SIZE;
        if (wl_phys_w < TILE_SIZE * 4) wl_phys_w = TILE_SIZE * 4;
        if (wl_phys_h < TILE_SIZE * 4) wl_phys_h = TILE_SIZE * 4;
        
        scene_w = wl_phys_w;
        scene_h = wl_phys_h;
        
        wlr_log(WLR_INFO, "Resize (9front scaling): physical %dx%d, scale %.2f, logical %dx%d",
                new_w, new_h, scale, wl_phys_w, wl_phys_h);
    }
    
    /* Allocate compositor buffers at Wayland physical resolution */
    size_t fb_size = wl_phys_w * wl_phys_h * sizeof(uint32_t);
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
        return;
    }
    
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
    
    /* s->width, s->height = Wayland buffer dimensions */
    s->width = wl_phys_w;
    s->height = wl_phys_h;
    s->tiles_x = (wl_phys_w + TILE_SIZE - 1) / TILE_SIZE;
    s->tiles_y = (wl_phys_h + TILE_SIZE - 1) / TILE_SIZE;
    
    if (fractional_wl_mode) {
        /* Fractional Wayland mode */
        draw->logical_width = wl_phys_w;  /* Source image size */
        draw->logical_height = wl_phys_h;
        draw->width = new_w;              /* Destination (rio window) */
        draw->height = new_h;
        draw->scale = scale / (float)k;   /* Matrix = 128/(scale/k) = 128k/scale */
        draw->input_scale = scale;        /* Mouse conversion */
        draw->scene_width = scene_w;
        draw->scene_height = scene_h;
    } else if (s->wl_scaling || scale <= 1.001f) {
        /* Regular Wayland scaling mode: no 9front scaling */
        draw->width = wl_phys_w;
        draw->height = wl_phys_h;
        draw->logical_width = wl_phys_w;
        draw->logical_height = wl_phys_h;
        draw->scale = 1.0f;
        draw->input_scale = 1.0f;
        draw->scene_width = scene_w;
        draw->scene_height = scene_h;
    } else {
        /* 9front scaling mode: draw->width/height = effective physical */
        int eff_phys_w = (int)(wl_phys_w * scale);
        int eff_phys_h = (int)(wl_phys_h * scale);
        draw->width = eff_phys_w;
        draw->height = eff_phys_h;
        draw->logical_width = wl_phys_w;
        draw->logical_height = wl_phys_h;
        draw->scale = scale;
        draw->input_scale = scale;
        draw->scene_width = scene_w;
        draw->scene_height = scene_h;
    }
    draw->win_minx = new_minx;
    draw->win_miny = new_miny;
    pthread_mutex_unlock(&s->send_lock);
    
    free(old_framebuf);
    free(old_prev_framebuf);
    free(old_send_buf0);
    free(old_send_buf1);
    
    /* Reallocate Plan 9 images at source (Wayland buffer) resolution */
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
    PUT32(bcmd + off, wl_phys_w); off += 4;
    PUT32(bcmd + off, wl_phys_h); off += 4;
    PUT32(bcmd + off, 0); off += 4;
    PUT32(bcmd + off, 0); off += 4;
    PUT32(bcmd + off, wl_phys_w); off += 4;
    PUT32(bcmd + off, wl_phys_h); off += 4;
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
    PUT32(bcmd + off, wl_phys_w); off += 4;
    PUT32(bcmd + off, wl_phys_h); off += 4;
    PUT32(bcmd + off, 0); off += 4;
    PUT32(bcmd + off, 0); off += 4;
    PUT32(bcmd + off, wl_phys_w); off += 4;
    PUT32(bcmd + off, wl_phys_h); off += 4;
    PUT32(bcmd + off, 0x00000000); off += 4;
    p9_write(p9, draw->drawdata_fid, 0, bcmd, off);
    
    /* Resize wlroots output */
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_custom_mode(&state, wl_phys_w, wl_phys_h, 0);
    if (fractional_wl_mode) {
        wlr_output_state_set_scale(&state, 2.0f);
    } else if (s->wl_scaling && scale > 1.001f) {
        wlr_output_state_set_scale(&state, scale);
    }
    wlr_output_commit_state(s->output, &state);
    wlr_output_state_finish(&state);
    
    /* Send configure to all toplevels */
    struct toplevel *tl;
    wl_list_for_each(tl, &s->toplevels, link) {
        if (tl->xdg && tl->xdg->base && tl->xdg->base->initialized) {
            wlr_xdg_toplevel_set_size(tl->xdg, scene_w, scene_h);
        }
    }
    
    /* Resize background (scene uses logical coordinates) */
    if (s->background) {
        wlr_scene_rect_set_size(s->background, scene_w, scene_h);
    }
    
    draw->xor_enabled = 0;
    s->force_full_frame = 1;
    
    wlr_log(WLR_INFO, "Resize complete: rio %dx%d, wl_buf %dx%d, scene %dx%d at (%d,%d)", 
            new_w, new_h, wl_phys_w, wl_phys_h, scene_w, scene_h, new_minx, new_miny);
}

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
        if (new_w == s->draw.width && new_h == s->draw.height) {
            struct draw_state *draw = &s->draw;
            draw->win_minx = new_minx;
            draw->win_miny = new_miny;
            s->force_full_frame = 1;
            wlr_log(WLR_DEBUG, "Position update only: (%d,%d), forcing full frame", new_minx, new_miny);
        } else {
            handle_resize(s, new_w, new_h, new_minx, new_miny);
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
    
    float scale = s->scale;
    if (scale <= 0.0f) scale = 1.0f;
    
    int phys_w = s->width;
    int phys_h = s->height;
    
    /*
     * Three scaling modes:
     * 
     * 1. 9front scaling (default, s->wl_scaling == 0):
     *    - Compositor renders at LOGICAL resolution (physical / scale)
     *    - 9front 'a' command scales to physical window
     *    - Lower bandwidth, quality depends on 9front bilinear
     *
     * 2. Wayland scaling (s->wl_scaling == 1, or -W flag):
     *    - Compositor renders at PHYSICAL resolution
     *    - wlroots uses output scale to report logical size to clients
     *    - Higher bandwidth, may look sharper
     *
     * 3. Fractional Wayland scaling (-W with scale > 1):
     *    - For scale x where k-1 < x <= k (k = ceil(x)):
     *    - Wayland output scale = k (integer)
     *    - Compositor renders at (k/x) * physical resolution
     *    - Scene/logical = physical / x (what apps see)
     *    - 9front 'a' command downscales by k/x to fit window
     *    - Good balance of quality and bandwidth for any fractional scale
     */
    
    /* Fractional Wayland mode: -W with scale > 1 */
    int fractional_wl_mode = (s->wl_scaling && scale > 1.001f);
    int k = (int)ceilf(scale);  /* Integer output scale (ceiling of user scale) */
    
    if (fractional_wl_mode) {
        /* Fractional Wayland scaling mode */
        wlr_log(WLR_INFO, "Using fractional Wayland scaling (scale=%.2f, output_scale=%d)", scale, k);
        
        /* Wayland buffer dimensions = (k/scale) * physical */
        float buffer_factor = (float)k / scale;
        int wl_phys_w = (int)(phys_w * buffer_factor + 0.5f);
        int wl_phys_h = (int)(phys_h * buffer_factor + 0.5f);
        
        /* Align to tile size */
        wl_phys_w = (wl_phys_w / TILE_SIZE) * TILE_SIZE;
        wl_phys_h = (wl_phys_h / TILE_SIZE) * TILE_SIZE;
        if (wl_phys_w < TILE_SIZE * 4) wl_phys_w = TILE_SIZE * 4;
        if (wl_phys_h < TILE_SIZE * 4) wl_phys_h = TILE_SIZE * 4;
        
        /* Scene/logical dims = wl_phys / k = phys / scale (what apps see) */
        int scene_w = wl_phys_w / k;
        int scene_h = wl_phys_h / k;
        
        struct wlr_output_state state;
        wlr_output_state_init(&state);
        wlr_output_state_set_enabled(&state, true);
        wlr_output_state_set_custom_mode(&state, wl_phys_w, wl_phys_h, 60000);
        wlr_output_state_set_scale(&state, (float)k);
        wlr_output_commit_state(out, &state);
        wlr_output_state_finish(&state);
        
        wlr_output_layout_add_auto(s->output_layout, out);
        s->output = out;
        s->scene_output = wlr_scene_output_create(s->scene, out);
        
        s->output_frame.notify = output_frame;
        wl_signal_add(&out->events.frame, &s->output_frame);
        s->output_destroy.notify = output_destroy;
        wl_signal_add(&out->events.destroy, &s->output_destroy);
        
        /* Reallocate buffers at Wayland physical resolution */
        size_t fb_size = wl_phys_w * wl_phys_h * sizeof(uint32_t);
        uint32_t *new_framebuf = calloc(1, fb_size);
        uint32_t *new_prev_framebuf = calloc(1, fb_size);
        uint32_t *new_send_buf0 = calloc(1, fb_size);
        uint32_t *new_send_buf1 = calloc(1, fb_size);
        
        if (new_framebuf && new_prev_framebuf && new_send_buf0 && new_send_buf1) {
            pthread_mutex_lock(&s->send_lock);
            free(s->framebuf);
            free(s->prev_framebuf);
            free(s->send_buf[0]);
            free(s->send_buf[1]);
            s->framebuf = new_framebuf;
            s->prev_framebuf = new_prev_framebuf;
            s->send_buf[0] = new_send_buf0;
            s->send_buf[1] = new_send_buf1;
            s->pending_buf = -1;
            s->active_buf = -1;
            pthread_mutex_unlock(&s->send_lock);
            
            wlr_log(WLR_INFO, "Reallocated buffers: %dx%d rio -> %dx%d Wayland",
                    phys_w, phys_h, wl_phys_w, wl_phys_h);
            
            /* Reallocate 9front source image at Wayland physical resolution */
            struct draw_state *draw = &s->draw;
            struct p9conn *p9 = draw->p9;
            
            /* Free old images */
            uint8_t freecmd[5];
            freecmd[0] = 'f';
            PUT32(freecmd + 1, draw->image_id);
            p9_write(p9, draw->drawdata_fid, 0, freecmd, 5);
            PUT32(freecmd + 1, draw->delta_id);
            p9_write(p9, draw->drawdata_fid, 0, freecmd, 5);
            
            /* Allocate new source image at Wayland physical resolution */
            uint8_t bcmd[64];
            int off = 0;
            bcmd[off++] = 'b';
            PUT32(bcmd + off, draw->image_id); off += 4;
            PUT32(bcmd + off, 0); off += 4;
            bcmd[off++] = 0;
            PUT32(bcmd + off, 0x68081828); off += 4;  /* XRGB32 */
            bcmd[off++] = 0;
            PUT32(bcmd + off, 0); off += 4;
            PUT32(bcmd + off, 0); off += 4;
            PUT32(bcmd + off, wl_phys_w); off += 4;
            PUT32(bcmd + off, wl_phys_h); off += 4;
            PUT32(bcmd + off, 0); off += 4;
            PUT32(bcmd + off, 0); off += 4;
            PUT32(bcmd + off, wl_phys_w); off += 4;
            PUT32(bcmd + off, wl_phys_h); off += 4;
            PUT32(bcmd + off, 0x00000000); off += 4;
            p9_write(p9, draw->drawdata_fid, 0, bcmd, off);
            
            /* Allocate delta image */
            off = 0;
            bcmd[off++] = 'b';
            PUT32(bcmd + off, draw->delta_id); off += 4;
            PUT32(bcmd + off, 0); off += 4;
            bcmd[off++] = 0;
            PUT32(bcmd + off, 0x48081828); off += 4;  /* ARGB32 */
            bcmd[off++] = 0;
            PUT32(bcmd + off, 0); off += 4;
            PUT32(bcmd + off, 0); off += 4;
            PUT32(bcmd + off, wl_phys_w); off += 4;
            PUT32(bcmd + off, wl_phys_h); off += 4;
            PUT32(bcmd + off, 0); off += 4;
            PUT32(bcmd + off, 0); off += 4;
            PUT32(bcmd + off, wl_phys_w); off += 4;
            PUT32(bcmd + off, wl_phys_h); off += 4;
            PUT32(bcmd + off, 0x00000000); off += 4;
            p9_write(p9, draw->drawdata_fid, 0, bcmd, off);
            
            wlr_log(WLR_INFO, "Reallocated 9front images at %dx%d (Wayland physical)", 
                    wl_phys_w, wl_phys_h);
        } else {
            wlr_log(WLR_ERROR, "Failed to reallocate buffers for fractional Wayland mode");
            free(new_framebuf);
            free(new_prev_framebuf);
            free(new_send_buf0);
            free(new_send_buf1);
        }
        
        /* s->width/height = Wayland physical (compositor buffer size) */
        s->width = wl_phys_w;
        s->height = wl_phys_h;
        s->tiles_x = (wl_phys_w + TILE_SIZE - 1) / TILE_SIZE;
        s->tiles_y = (wl_phys_h + TILE_SIZE - 1) / TILE_SIZE;
        
        /* draw->logical_width/height = source image size (Wayland physical) */
        s->draw.logical_width = wl_phys_w;
        s->draw.logical_height = wl_phys_h;
        
        /* draw->width/height = destination size (rio window physical) */
        s->draw.width = phys_w;
        s->draw.height = phys_h;
        
        /* draw->scale = scale/k so that matrix = 128/(scale/k) = 128k/scale
         * This downscales from Wayland physical to rio physical */
        s->draw.scale = scale / (float)k;
        
        /* input_scale = user's scale for mouse coordinate conversion
         * Mouse physical (rio) -> cursor position (scene logical = rio/scale) */
        s->draw.input_scale = scale;
        
        /* Scene dimensions = what Wayland apps see */
        s->draw.scene_width = scene_w;
        s->draw.scene_height = scene_h;
        
        /* Resize background to scene dimensions */
        if (s->background) {
            wlr_scene_rect_set_size(s->background, scene_w, scene_h);
        }
        
        wlr_log(WLR_INFO, "Fractional Wayland mode ready: rio %dx%d, wl_buf %dx%d, scene %dx%d, "
                "output_scale=%d, draw_scale=%.3f, input_scale=%.2f",
                phys_w, phys_h, wl_phys_w, wl_phys_h, scene_w, scene_h,
                k, s->draw.scale, s->draw.input_scale);
        
        /* Trigger first frame */
        s->force_full_frame = 1;
        s->frame_dirty = 1;
        return;
    }
    
    if (s->wl_scaling || scale <= 1.001f) {
        /* Wayland scaling mode (no -S) or no scaling needed (scale <= 1) */
        wlr_log(WLR_INFO, "Using Wayland-side scaling (scale=%.2f)", scale);
        
        struct wlr_output_state state;
        wlr_output_state_init(&state);
        wlr_output_state_set_enabled(&state, true);
        wlr_output_state_set_custom_mode(&state, phys_w, phys_h, 60000);
        /* No output scale needed - fractional scaling handles scale > 1 */
        wlr_output_commit_state(out, &state);
        wlr_output_state_finish(&state);
        
        wlr_output_layout_add_auto(s->output_layout, out);
        s->output = out;
        s->scene_output = wlr_scene_output_create(s->scene, out);
        
        s->output_frame.notify = output_frame;
        wl_signal_add(&out->events.frame, &s->output_frame);
        s->output_destroy.notify = output_destroy;
        wl_signal_add(&out->events.destroy, &s->output_destroy);
        
        /* No 9front scaling needed */
        s->draw.logical_width = phys_w;
        s->draw.logical_height = phys_h;
        s->draw.scale = 1.0f;
        s->draw.input_scale = 1.0f;
        s->draw.scene_width = phys_w;
        s->draw.scene_height = phys_h;
        
        wlr_log(WLR_INFO, "Output ready: %dx%d", phys_w, phys_h);
        
        /* Now that everything is set up correctly, trigger first frame */
        s->force_full_frame = 1;
        s->frame_dirty = 1;
        return;
    }
    
    /* 9front scaling mode */
    wlr_log(WLR_INFO, "Using 9front-side scaling (scale=%.2f)", scale);
    
    int logical_w = (int)(phys_w / scale);
    int logical_h = (int)(phys_h / scale);
    
    /* Align to tile size */
    logical_w = (logical_w / TILE_SIZE) * TILE_SIZE;
    logical_h = (logical_h / TILE_SIZE) * TILE_SIZE;
    if (logical_w < TILE_SIZE * 4) logical_w = TILE_SIZE * 4;
    if (logical_h < TILE_SIZE * 4) logical_h = TILE_SIZE * 4;
    
    /* Set wlroots output to LOGICAL dimensions.
     * Do NOT use wlr_output_state_set_scale() - we handle scaling in 9front.
     */
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    wlr_output_state_set_custom_mode(&state, logical_w, logical_h, 60000);
    wlr_output_commit_state(out, &state);
    wlr_output_state_finish(&state);
    
    wlr_output_layout_add_auto(s->output_layout, out);
    s->output = out;
    s->scene_output = wlr_scene_output_create(s->scene, out);
    
    s->output_frame.notify = output_frame;
    wl_signal_add(&out->events.frame, &s->output_frame);
    s->output_destroy.notify = output_destroy;
    wl_signal_add(&out->events.destroy, &s->output_destroy);
    
    /* Reallocate buffers at logical resolution */
    size_t fb_size = logical_w * logical_h * sizeof(uint32_t);
    uint32_t *new_framebuf = calloc(1, fb_size);
    uint32_t *new_prev_framebuf = calloc(1, fb_size);
    uint32_t *new_send_buf0 = calloc(1, fb_size);
    uint32_t *new_send_buf1 = calloc(1, fb_size);
    
    if (new_framebuf && new_prev_framebuf && new_send_buf0 && new_send_buf1) {
        pthread_mutex_lock(&s->send_lock);
        free(s->framebuf);
        free(s->prev_framebuf);
        free(s->send_buf[0]);
        free(s->send_buf[1]);
        s->framebuf = new_framebuf;
        s->prev_framebuf = new_prev_framebuf;
        s->send_buf[0] = new_send_buf0;
        s->send_buf[1] = new_send_buf1;
        s->pending_buf = -1;
        s->active_buf = -1;
        pthread_mutex_unlock(&s->send_lock);
        
        wlr_log(WLR_INFO, "Reallocated buffers: %dx%d -> %dx%d for logical resolution",
                phys_w, phys_h, logical_w, logical_h);
        
        /* Reallocate 9front source image at logical resolution */
        struct draw_state *draw = &s->draw;
        struct p9conn *p9 = draw->p9;
        
        /* Free old images */
        uint8_t freecmd[5];
        freecmd[0] = 'f';
        PUT32(freecmd + 1, draw->image_id);
        p9_write(p9, draw->drawdata_fid, 0, freecmd, 5);
        PUT32(freecmd + 1, draw->delta_id);
        p9_write(p9, draw->drawdata_fid, 0, freecmd, 5);
        
        /* Allocate new image at logical resolution */
        uint8_t bcmd[64];
        int off = 0;
        bcmd[off++] = 'b';
        PUT32(bcmd + off, draw->image_id); off += 4;
        PUT32(bcmd + off, 0); off += 4;
        bcmd[off++] = 0;
        PUT32(bcmd + off, 0x68081828); off += 4;  /* XRGB32 */
        bcmd[off++] = 0;
        PUT32(bcmd + off, 0); off += 4;
        PUT32(bcmd + off, 0); off += 4;
        PUT32(bcmd + off, logical_w); off += 4;
        PUT32(bcmd + off, logical_h); off += 4;
        PUT32(bcmd + off, 0); off += 4;
        PUT32(bcmd + off, 0); off += 4;
        PUT32(bcmd + off, logical_w); off += 4;
        PUT32(bcmd + off, logical_h); off += 4;
        PUT32(bcmd + off, 0x00000000); off += 4;
        p9_write(p9, draw->drawdata_fid, 0, bcmd, off);
        
        /* Allocate delta image at logical resolution */
        off = 0;
        bcmd[off++] = 'b';
        PUT32(bcmd + off, draw->delta_id); off += 4;
        PUT32(bcmd + off, 0); off += 4;
        bcmd[off++] = 0;
        PUT32(bcmd + off, 0x48081828); off += 4;  /* ARGB32 */
        bcmd[off++] = 0;
        PUT32(bcmd + off, 0); off += 4;
        PUT32(bcmd + off, 0); off += 4;
        PUT32(bcmd + off, logical_w); off += 4;
        PUT32(bcmd + off, logical_h); off += 4;
        PUT32(bcmd + off, 0); off += 4;
        PUT32(bcmd + off, 0); off += 4;
        PUT32(bcmd + off, logical_w); off += 4;
        PUT32(bcmd + off, logical_h); off += 4;
        PUT32(bcmd + off, 0x00000000); off += 4;
        p9_write(p9, draw->drawdata_fid, 0, bcmd, off);
        
        wlr_log(WLR_INFO, "Reallocated 9front images at %dx%d logical", 
                logical_w, logical_h);
    } else {
        wlr_log(WLR_ERROR, "Failed to reallocate buffers for logical resolution");
        free(new_framebuf);
        free(new_prev_framebuf);
        free(new_send_buf0);
        free(new_send_buf1);
    }
    
    /* Update s->width/height to logical (compositor operates at logical resolution) */
    s->width = logical_w;
    s->height = logical_h;
    s->tiles_x = (logical_w + TILE_SIZE - 1) / TILE_SIZE;
    s->tiles_y = (logical_h + TILE_SIZE - 1) / TILE_SIZE;
    
    /* Update draw state.
     * draw->width/height must be the "effective physical" = logical * scale.
     * This ensures the 'a' command's dest rect R exactly matches the scaled source.
     * The gap between effective physical and actual window is filled by borders.
     */
    int eff_phys_w = (int)(logical_w * scale);
    int eff_phys_h = (int)(logical_h * scale);
    s->draw.width = eff_phys_w;
    s->draw.height = eff_phys_h;
    s->draw.logical_width = logical_w;
    s->draw.logical_height = logical_h;
    s->draw.scale = scale;
    s->draw.input_scale = scale;  /* Mouse coord conversion uses same scale */
    s->draw.scene_width = logical_w;   /* Scene = logical in 9front mode */
    s->draw.scene_height = logical_h;
    
    /* Resize background to logical dimensions */
    if (s->background) {
        wlr_scene_rect_set_size(s->background, logical_w, logical_h);
    }
    
    wlr_log(WLR_INFO, "Output ready: %dx%d actual, %dx%d effective physical, scale=%.2f, %dx%d logical (9front scales)",
            phys_w, phys_h, eff_phys_w, eff_phys_h, scale, logical_w, logical_h);
    
    /* Now that everything is set up correctly, trigger first frame */
    s->force_full_frame = 1;
    s->frame_dirty = 1;
}

void new_input(struct wl_listener *l, void *d) {
    struct server *s = wl_container_of(l, s, new_input);
    struct wlr_input_device *dev = d;
    if (dev->type == WLR_INPUT_DEVICE_POINTER)
        wlr_cursor_attach_input_device(s->cursor, dev);
}
