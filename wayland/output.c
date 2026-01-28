/*
 * output.c - Output and input device handlers (SHARP SCALING VERSION)
 *
 * For 9front scaling mode, renders at HIGH resolution (physical × scale)
 * then DOWNSCALES to the physical window for sharp results.
 *
 * Previous approach (blurry):
 *   Compositor at logical (phys/scale) → upscale → blurry
 *
 * New approach (sharp):
 *   Compositor at render (phys×scale) → downscale → sharp
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

/* Handle resize to new physical dimensions */
void handle_resize(struct server *s, int new_w, int new_h, int new_minx, int new_miny) {
    wlr_log(WLR_INFO, "handle_resize: %dx%d -> %dx%d at (%d,%d)", 
            s->draw.width, s->draw.height, new_w, new_h, new_minx, new_miny);
    
    float scale = s->scale;
    if (scale <= 0.0f) scale = 1.0f;
    
    int render_w, render_h;
    int phys_w, phys_h;
    int logical_w, logical_h;
    
    if (s->wl_scaling || scale <= 1.001f) {
        /* Wayland scaling mode or no scaling: physical resolution */
        phys_w = (new_w / TILE_SIZE) * TILE_SIZE;
        phys_h = (new_h / TILE_SIZE) * TILE_SIZE;
        if (phys_w < TILE_SIZE * 4) phys_w = TILE_SIZE * 4;
        if (phys_h < TILE_SIZE * 4) phys_h = TILE_SIZE * 4;
        
        render_w = phys_w;
        render_h = phys_h;
        logical_w = phys_w;
        logical_h = phys_h;
        
        wlr_log(WLR_INFO, "Resize (Wayland/no scaling): %dx%d", phys_w, phys_h);
    } else {
        /* 9front SHARP 1.5× scaling mode:
         * - Physical: tile-aligned window size
         * - Logical: physical / 1.5 (what clients see)
         * - Render: logical × 2 = physical × (4/3)
         */
        phys_w = (new_w / TILE_SIZE) * TILE_SIZE;
        phys_h = (new_h / TILE_SIZE) * TILE_SIZE;
        if (phys_w < TILE_SIZE * 4) phys_w = TILE_SIZE * 4;
        if (phys_h < TILE_SIZE * 4) phys_h = TILE_SIZE * 4;
        
        /* Logical = physical / 1.5, aligned to even for clean 2× */
        logical_w = (phys_w * 2) / 3;
        logical_h = (phys_h * 2) / 3;
        logical_w = (logical_w / 2) * 2;
        logical_h = (logical_h / 2) * 2;
        
        /* Render = logical × 2 */
        render_w = logical_w * 2;
        render_h = logical_h * 2;
        
        wlr_log(WLR_INFO, "Resize (9front 1.5× SHARP): physical %dx%d, logical %dx%d, render %dx%d",
                phys_w, phys_h, logical_w, logical_h, render_w, render_h);
    }
    
    /* Allocate compositor buffers at RENDER resolution */
    size_t fb_size = render_w * render_h * sizeof(uint32_t);
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
    
    /* s->width, s->height = RENDER resolution (compositor buffers) */
    s->width = render_w;
    s->height = render_h;
    s->tiles_x = (render_w + TILE_SIZE - 1) / TILE_SIZE;
    s->tiles_y = (render_h + TILE_SIZE - 1) / TILE_SIZE;
    
    if (s->wl_scaling || scale <= 1.001f) {
        /* Wayland scaling mode: no 9front scaling */
        draw->width = render_w;
        draw->height = render_h;
        draw->logical_width = render_w;
        draw->logical_height = render_h;
        draw->render_width = render_w;
        draw->render_height = render_h;
        draw->scale = 1.0f;
    } else {
        /* 9front 1.5× SHARP scaling mode:
         * - width/height = physical (dest for 'a' command)
         * - logical = physical / 1.5 (what clients see)
         * - render = logical × 2 (what we render at)
         * - scale = render / physical = 4/3 (9front downscale factor)
         */
        draw->width = phys_w;
        draw->height = phys_h;
        draw->logical_width = logical_w;
        draw->logical_height = logical_h;
        draw->render_width = render_w;
        draw->render_height = render_h;
        draw->scale = (float)render_w / (float)phys_w;  /* 4/3 ≈ 1.333 */
    }
    draw->win_minx = new_minx;
    draw->win_miny = new_miny;
    pthread_mutex_unlock(&s->send_lock);
    
    free(old_framebuf);
    free(old_prev_framebuf);
    free(old_send_buf0);
    free(old_send_buf1);
    
    /* Reallocate Plan 9 images at RENDER resolution */
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
    PUT32(bcmd + off, render_w); off += 4;
    PUT32(bcmd + off, render_h); off += 4;
    PUT32(bcmd + off, 0); off += 4;
    PUT32(bcmd + off, 0); off += 4;
    PUT32(bcmd + off, render_w); off += 4;
    PUT32(bcmd + off, render_h); off += 4;
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
    PUT32(bcmd + off, render_w); off += 4;
    PUT32(bcmd + off, render_h); off += 4;
    PUT32(bcmd + off, 0); off += 4;
    PUT32(bcmd + off, 0); off += 4;
    PUT32(bcmd + off, render_w); off += 4;
    PUT32(bcmd + off, render_h); off += 4;
    PUT32(bcmd + off, 0x00000000); off += 4;
    p9_write(p9, draw->drawdata_fid, 0, bcmd, off);
    
    /* Resize wlroots output to RENDER dimensions */
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_custom_mode(&state, render_w, render_h, 0);
    if (!s->wl_scaling && scale > 1.001f) {
        /* 9front 1.5× sharp scaling: set Wayland output scale=2 */
        wlr_output_state_set_scale(&state, 2.0f);
    } else if (s->wl_scaling && scale > 1.001f) {
        wlr_output_state_set_scale(&state, scale);
    }
    wlr_output_commit_state(s->output, &state);
    wlr_output_state_finish(&state);
    
    /* Scene uses LOGICAL coordinates.
     * With 9front 1.5× scaling: scene = logical = physical / 1.5
     */
    int scene_w = logical_w;
    int scene_h = logical_h;
    
    struct toplevel *tl;
    wl_list_for_each(tl, &s->toplevels, link) {
        if (tl->xdg && tl->xdg->base && tl->xdg->base->initialized) {
            wlr_xdg_toplevel_set_size(tl->xdg, scene_w, scene_h);
        }
    }
    
    if (s->background) {
        wlr_scene_rect_set_size(s->background, scene_w, scene_h);
    }
    
    draw->xor_enabled = 0;
    s->force_full_frame = 1;
    
    wlr_log(WLR_INFO, "Resize complete: %dx%d physical, %dx%d logical, %dx%d render at (%d,%d)", 
            phys_w, phys_h, logical_w, logical_h, render_w, render_h, new_minx, new_miny);
}

static uint32_t now_ms_local(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static uint64_t frame_count = 0;

void output_frame(struct wl_listener *l, void *data) {
    struct server *s = wl_container_of(l, s, output_frame);
    struct wlr_scene_output *so = s->scene_output;
    (void)data;
    
    static int frame_log_count = 0;
    frame_log_count++;
    if (frame_log_count <= 5 || frame_log_count % 60 == 0) {
        wlr_log(WLR_INFO, "output_frame #%d called", frame_log_count);
    }
    
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
    
    uint32_t now = now_ms_local();
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
        wlr_output_state_finish(&ostate);
        return;
    }
    
    struct wlr_buffer *buf = ostate.buffer;
    if (!buf) {
        wlr_output_state_finish(&ostate);
        return;
    }
    
    void *data_ptr;
    uint32_t format;
    size_t stride;
    if (!wlr_buffer_begin_data_ptr_access(buf, WLR_BUFFER_DATA_PTR_ACCESS_READ,
                                           &data_ptr, &format, &stride)) {
        wlr_output_state_finish(&ostate);
        return;
    }
    
    /* Copy rendered frame to our buffer */
    int w = s->width;
    int h = s->height;
    uint32_t *framebuf = s->framebuf;
    
    if (stride == (size_t)(w * 4)) {
        memcpy(framebuf, data_ptr, w * h * 4);
    } else {
        uint8_t *src = data_ptr;
        uint8_t *dst = (uint8_t *)framebuf;
        for (int y = 0; y < h; y++) {
            memcpy(dst, src, w * 4);
            src += stride;
            dst += w * 4;
        }
    }
    
    wlr_buffer_end_data_ptr_access(buf);
    
    if (!wlr_output_commit_state(s->output, &ostate)) {
        wlr_log(WLR_ERROR, "Output commit failed");
    }
    wlr_output_state_finish(&ostate);
    
    /* Send frame to 9front - call directly for immediate delivery */
    s->frame_dirty = 1;
    send_frame(s);
    
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    wlr_scene_output_send_frame_done(so, &ts);
}

static void output_destroy(struct wl_listener *l, void *d) {
    struct server *s = wl_container_of(l, s, output_destroy);
    (void)d;
    wlr_log(WLR_INFO, "Output destroyed");
    wl_list_remove(&s->output_frame.link);
    wl_list_remove(&s->output_destroy.link);
    s->output = NULL;
    s->scene_output = NULL;
}

void new_output(struct wl_listener *l, void *d) {
    struct server *s = wl_container_of(l, s, new_output);
    struct wlr_output *out = d;
    
    /* Initialize output rendering - REQUIRED for cursor and other operations */
    wlr_output_init_render(out, s->allocator, s->renderer);
    
    wlr_log(WLR_INFO, "New output: %s (%s)", out->name, out->description ? out->description : "no desc");
    
    /* Get physical window dimensions from draw state */
    int phys_w = s->draw.actual_maxx - s->draw.actual_minx;
    int phys_h = s->draw.actual_maxy - s->draw.actual_miny;
    
    if (phys_w <= 0 || phys_h <= 0) {
        phys_w = s->draw.width;
        phys_h = s->draw.height;
    }
    
    wlr_log(WLR_INFO, "Physical window: %dx%d", phys_w, phys_h);
    
    float scale = s->scale;
    if (scale <= 0.0f) scale = 1.0f;
    
    /*
     * Two scaling modes:
     *
     * 1. 9front SHARP scaling (s->wl_scaling == 0, default with -S flag):
     *    - Compositor renders at HIGH resolution (physical × scale)
     *    - 9front DOWNSCALES to physical window = SHARP
     *    - Higher bandwidth but sharp results
     *
     * 2. Wayland scaling (s->wl_scaling == 1, or -W flag):
     *    - Compositor renders at PHYSICAL resolution
     *    - wlroots uses output scale to report logical size to clients
     *    - No 9front scaling needed
     */
    
    if (s->wl_scaling || scale <= 1.001f) {
        /* Wayland scaling mode OR no scaling needed */
        wlr_log(WLR_INFO, "Using Wayland-side scaling (scale=%.2f)", scale);
        
        struct wlr_output_state state;
        wlr_output_state_init(&state);
        wlr_output_state_set_enabled(&state, true);
        wlr_output_state_set_custom_mode(&state, phys_w, phys_h, 60000);
        if (scale > 1.001f) {
            wlr_output_state_set_scale(&state, scale);
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
        
        s->draw.width = phys_w;
        s->draw.height = phys_h;
        s->draw.logical_width = phys_w;
        s->draw.logical_height = phys_h;
        s->draw.render_width = phys_w;
        s->draw.render_height = phys_h;
        s->draw.scale = 1.0f;
        
        if (scale > 1.001f) {
            int scene_w = (int)(phys_w / scale);
            int scene_h = (int)(phys_h / scale);
            if (s->background) {
                wlr_scene_rect_set_size(s->background, scene_w, scene_h);
            }
            wlr_log(WLR_INFO, "Output ready: %dx%d physical, Wayland scale=%.2f, scene %dx%d", 
                    phys_w, phys_h, scale, scene_w, scene_h);
        } else {
            wlr_log(WLR_INFO, "Output ready: %dx%d", phys_w, phys_h);
        }
        
        s->force_full_frame = 1;
        s->frame_dirty = 1;
        wlr_output_schedule_frame(out);
        return;
    }
    
    /* 9front SHARP scaling mode for 1.5× overall scale:
     *
     * Goal: clients see physical/1.5 logical pixels, sharp output
     * 
     * Method:
     * - Wayland scale = 2 (integer for clean scroll detection)
     * - Logical = physical / 1.5 (what clients see)
     * - Render = logical × 2 = physical × (4/3)
     * - 9front downscales render → physical by factor 4/3
     *
     * Example for 1440×1008 physical:
     * - Logical: 960×672 (clients see this)
     * - Render: 1920×1344 (what Wayland renders)
     * - 9front: 1920×1344 → 1440×1008 (4/3 downscale)
     */
    wlr_log(WLR_INFO, "Using 9front-side SHARP 1.5× scaling (2× Wayland, 4/3 downscale)");
    
    /* Physical dimensions aligned to tile size */
    int aligned_phys_w = (phys_w / TILE_SIZE) * TILE_SIZE;
    int aligned_phys_h = (phys_h / TILE_SIZE) * TILE_SIZE;
    if (aligned_phys_w < TILE_SIZE * 4) aligned_phys_w = TILE_SIZE * 4;
    if (aligned_phys_h < TILE_SIZE * 4) aligned_phys_h = TILE_SIZE * 4;
    
    /* Logical = physical / 1.5 (what clients see) */
    int logical_w = (aligned_phys_w * 2) / 3;
    int logical_h = (aligned_phys_h * 2) / 3;
    /* Align logical to even numbers for clean 2× */
    logical_w = (logical_w / 2) * 2;
    logical_h = (logical_h / 2) * 2;
    
    /* Render = logical × 2 = physical × 4/3 */
    int render_w = logical_w * 2;
    int render_h = logical_h * 2;
    
    /* Set wlroots output to RENDER dimensions with scale=2.
     * Clients will see logical = render/2 = physical dimensions.
     */
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    wlr_output_state_set_custom_mode(&state, render_w, render_h, 60000);
    wlr_output_state_set_scale(&state, 2.0f);  /* Integer scale for clean scrolling */
    if (!wlr_output_commit_state(out, &state)) {
        wlr_log(WLR_ERROR, "Failed to commit output state for %dx%d mode", render_w, render_h);
        wlr_output_state_finish(&state);
        return;
    }
    wlr_output_state_finish(&state);
    
    wlr_output_layout_add_auto(s->output_layout, out);
    s->output = out;
    s->scene_output = wlr_scene_output_create(s->scene, out);
    if (!s->scene_output) {
        wlr_log(WLR_ERROR, "Failed to create scene output for %dx%d", render_w, render_h);
        return;
    }
    
    s->output_frame.notify = output_frame;
    wl_signal_add(&out->events.frame, &s->output_frame);
    s->output_destroy.notify = output_destroy;
    wl_signal_add(&out->events.destroy, &s->output_destroy);
    
    /* Reallocate buffers at RENDER resolution (high res) */
    size_t fb_size = render_w * render_h * sizeof(uint32_t);
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
        
        wlr_log(WLR_INFO, "Allocated buffers: %dx%d render resolution",
                render_w, render_h);
        
        /* Reallocate 9front source image at RENDER resolution */
        struct draw_state *draw = &s->draw;
        struct p9conn *p9 = draw->p9;
        
        /* Free old images */
        uint8_t freecmd[5];
        freecmd[0] = 'f';
        PUT32(freecmd + 1, draw->image_id);
        p9_write(p9, draw->drawdata_fid, 0, freecmd, 5);
        PUT32(freecmd + 1, draw->delta_id);
        p9_write(p9, draw->drawdata_fid, 0, freecmd, 5);
        
        /* Allocate new image at RENDER resolution */
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
        PUT32(bcmd + off, render_w); off += 4;
        PUT32(bcmd + off, render_h); off += 4;
        PUT32(bcmd + off, 0); off += 4;
        PUT32(bcmd + off, 0); off += 4;
        PUT32(bcmd + off, render_w); off += 4;
        PUT32(bcmd + off, render_h); off += 4;
        PUT32(bcmd + off, 0x00000000); off += 4;
        p9_write(p9, draw->drawdata_fid, 0, bcmd, off);
        
        /* Allocate delta image at RENDER resolution */
        off = 0;
        bcmd[off++] = 'b';
        PUT32(bcmd + off, draw->delta_id); off += 4;
        PUT32(bcmd + off, 0); off += 4;
        bcmd[off++] = 0;
        PUT32(bcmd + off, 0x48081828); off += 4;  /* ARGB32 */
        bcmd[off++] = 0;
        PUT32(bcmd + off, 0); off += 4;
        PUT32(bcmd + off, 0); off += 4;
        PUT32(bcmd + off, render_w); off += 4;
        PUT32(bcmd + off, render_h); off += 4;
        PUT32(bcmd + off, 0); off += 4;
        PUT32(bcmd + off, 0); off += 4;
        PUT32(bcmd + off, render_w); off += 4;
        PUT32(bcmd + off, render_h); off += 4;
        PUT32(bcmd + off, 0x00000000); off += 4;
        p9_write(p9, draw->drawdata_fid, 0, bcmd, off);
        
        wlr_log(WLR_INFO, "Reallocated 9front images at %dx%d render res", 
                render_w, render_h);
    } else {
        wlr_log(WLR_ERROR, "Failed to reallocate buffers for render resolution");
        free(new_framebuf);
        free(new_prev_framebuf);
        free(new_send_buf0);
        free(new_send_buf1);
    }
    
    /* Update s->width/height to RENDER resolution (compositor operates at high res) */
    s->width = render_w;
    s->height = render_h;
    s->tiles_x = (render_w + TILE_SIZE - 1) / TILE_SIZE;
    s->tiles_y = (render_h + TILE_SIZE - 1) / TILE_SIZE;
    
    /* Update draw state for SHARP 1.5× scaling:
     * - width/height = PHYSICAL (what we downscale TO, for 'a' command dest rect)
     * - render_width/height = render res (physical × 4/3)
     * - logical_width/height = physical / 1.5 (what clients see)
     * - scale = 4/3 = 1.333... (9front downscale factor: render/physical)
     */
    s->draw.width = aligned_phys_w;
    s->draw.height = aligned_phys_h;
    s->draw.logical_width = logical_w;
    s->draw.logical_height = logical_h;
    s->draw.render_width = render_w;
    s->draw.render_height = render_h;
    s->draw.scale = (float)render_w / (float)aligned_phys_w;  /* 4/3 ≈ 1.333 */
    
    /* Background at LOGICAL dimensions (scene uses logical coords with output scale=2) */
    if (s->background) {
        wlr_scene_rect_set_size(s->background, logical_w, logical_h);
    }
    
    wlr_log(WLR_INFO, "Output ready: %dx%d physical, %dx%d render, %dx%d logical, 9front scale=%.3f (1.5× overall)",
            aligned_phys_w, aligned_phys_h, render_w, render_h, logical_w, logical_h, s->draw.scale);
    
    /* Trigger first frame - headless backend needs explicit scheduling */
    s->force_full_frame = 1;
    s->frame_dirty = 1;
    wlr_output_schedule_frame(out);
}

void new_input(struct wl_listener *l, void *d) {
    struct server *s = wl_container_of(l, s, new_input);
    struct wlr_input_device *dev = d;
    if (dev->type == WLR_INPUT_DEVICE_POINTER)
        wlr_cursor_attach_input_device(s->cursor, dev);
}
