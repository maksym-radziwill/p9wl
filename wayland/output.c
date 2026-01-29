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

/* ============== Scaling Mode ============== */

enum scaling_mode {
    SCALE_MODE_NONE,           /* No scaling (scale <= 1) */
    SCALE_MODE_WAYLAND,        /* Wayland-side scaling */
    SCALE_MODE_FRACTIONAL_WL,  /* Fractional Wayland (scale > 1, wl_scaling) */
    SCALE_MODE_9FRONT,         /* 9front-side scaling */
};

/* Computed dimensions for a given scaling configuration */
struct output_dims {
    enum scaling_mode mode;
    int k;                /* Integer output scale = ceil(scale) */
    int wl_phys_w;        /* Wayland buffer dimensions */
    int wl_phys_h;
    int scene_w;          /* Scene/logical dimensions (what apps see) */
    int scene_h;
    int dest_w;           /* Destination (rio window) dimensions */
    int dest_h;
    float draw_scale;     /* Scale factor for 9front 'a' command */
    float input_scale;    /* Scale factor for mouse coordinates */
};

/*
 * Compute output dimensions based on scaling mode.
 * 
 * phys_w, phys_h: Physical window size (rio window or initial size)
 * scale: User-requested scale factor
 * wl_scaling: Whether Wayland-side scaling is enabled
 */
static struct output_dims compute_dimensions(int phys_w, int phys_h, 
                                              float scale, int wl_scaling) {
    struct output_dims dims = {0};
    
    if (scale <= 0.0f) scale = 1.0f;
    dims.k = (int)ceilf(scale);
    dims.dest_w = phys_w;
    dims.dest_h = phys_h;
    
    if (wl_scaling && scale > 1.001f) {
        /* Fractional Wayland mode */
        dims.mode = SCALE_MODE_FRACTIONAL_WL;
        
        float buffer_factor = (float)dims.k / scale;
        dims.wl_phys_w = (int)((phys_w * buffer_factor + 0.5f) / dims.k) * dims.k;
        dims.wl_phys_h = (int)((phys_h * buffer_factor + 0.5f) / dims.k) * dims.k;
        
        /* Align to k * TILE_SIZE */
        dims.wl_phys_w = (dims.wl_phys_w / (dims.k * TILE_SIZE)) * dims.k * TILE_SIZE;
        dims.wl_phys_h = (dims.wl_phys_h / (dims.k * TILE_SIZE)) * dims.k * TILE_SIZE;
        if (dims.wl_phys_w < dims.k * TILE_SIZE * 4) 
            dims.wl_phys_w = dims.k * TILE_SIZE * 4;
        if (dims.wl_phys_h < dims.k * TILE_SIZE * 4) 
            dims.wl_phys_h = dims.k * TILE_SIZE * 4;
        
        dims.scene_w = dims.wl_phys_w / dims.k;
        dims.scene_h = dims.wl_phys_h / dims.k;
        dims.draw_scale = scale / (float)dims.k;
        dims.input_scale = scale;
        
    } else if (wl_scaling || scale <= 1.001f) {
        /* Wayland scaling or no scaling */
        dims.mode = (scale <= 1.001f) ? SCALE_MODE_NONE : SCALE_MODE_WAYLAND;
        
        dims.wl_phys_w = (phys_w / TILE_SIZE) * TILE_SIZE;
        dims.wl_phys_h = (phys_h / TILE_SIZE) * TILE_SIZE;
        if (dims.wl_phys_w < TILE_SIZE * 4) dims.wl_phys_w = TILE_SIZE * 4;
        if (dims.wl_phys_h < TILE_SIZE * 4) dims.wl_phys_h = TILE_SIZE * 4;
        
        dims.scene_w = dims.wl_phys_w;
        dims.scene_h = dims.wl_phys_h;
        dims.draw_scale = 1.0f;
        dims.input_scale = 1.0f;
        
    } else {
        /* 9front scaling mode */
        dims.mode = SCALE_MODE_9FRONT;
        
        dims.wl_phys_w = (int)(phys_w / scale);
        dims.wl_phys_h = (int)(phys_h / scale);
        
        /* Align to TILE_SIZE */
        dims.wl_phys_w = (dims.wl_phys_w / TILE_SIZE) * TILE_SIZE;
        dims.wl_phys_h = (dims.wl_phys_h / TILE_SIZE) * TILE_SIZE;
        if (dims.wl_phys_w < TILE_SIZE * 4) dims.wl_phys_w = TILE_SIZE * 4;
        if (dims.wl_phys_h < TILE_SIZE * 4) dims.wl_phys_h = TILE_SIZE * 4;
        
        dims.scene_w = dims.wl_phys_w;
        dims.scene_h = dims.wl_phys_h;
        dims.dest_w = (int)(dims.wl_phys_w * scale);
        dims.dest_h = (int)(dims.wl_phys_h * scale);
        dims.draw_scale = scale;
        dims.input_scale = scale;
    }
    
    return dims;
}

/* ============== Buffer Management ============== */

/*
 * Allocate compositor framebuffers.
 * Returns 0 on success, -1 on failure.
 */
static int allocate_buffers(struct server *s, int width, int height) {
    size_t fb_size = width * height * sizeof(uint32_t);
    
    uint32_t *new_framebuf = calloc(1, fb_size);
    uint32_t *new_prev_framebuf = calloc(1, fb_size);
    uint32_t *new_send_buf0 = calloc(1, fb_size);
    uint32_t *new_send_buf1 = calloc(1, fb_size);
    
    if (!new_framebuf || !new_prev_framebuf || !new_send_buf0 || !new_send_buf1) {
        wlr_log(WLR_ERROR, "Failed to allocate buffers for %dx%d", width, height);
        free(new_framebuf);
        free(new_prev_framebuf);
        free(new_send_buf0);
        free(new_send_buf1);
        return -1;
    }
    
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
    
    wlr_log(WLR_INFO, "Allocated buffers: %dx%d", width, height);
    return 0;
}

/* ============== Plan 9 Image Management ============== */

/*
 * Allocate a Plan 9 draw image.
 * format: 0x68081828 for XRGB32, 0x48081828 for ARGB32
 */
static void p9_alloc_image(struct p9conn *p9, uint32_t fid, uint32_t image_id,
                           int width, int height, uint32_t format) {
    uint8_t cmd[64];
    int off = 0;
    
    cmd[off++] = 'b';
    PUT32(cmd + off, image_id); off += 4;
    PUT32(cmd + off, 0); off += 4;           /* screen id */
    cmd[off++] = 0;                          /* refresh */
    PUT32(cmd + off, format); off += 4;      /* channel format */
    cmd[off++] = 0;                          /* repl */
    PUT32(cmd + off, 0); off += 4;           /* r.min.x */
    PUT32(cmd + off, 0); off += 4;           /* r.min.y */
    PUT32(cmd + off, width); off += 4;       /* r.max.x */
    PUT32(cmd + off, height); off += 4;      /* r.max.y */
    PUT32(cmd + off, 0); off += 4;           /* clipr.min.x */
    PUT32(cmd + off, 0); off += 4;           /* clipr.min.y */
    PUT32(cmd + off, width); off += 4;       /* clipr.max.x */
    PUT32(cmd + off, height); off += 4;      /* clipr.max.y */
    PUT32(cmd + off, 0x00000000); off += 4;  /* value (background) */
    
    p9_write(p9, fid, 0, cmd, off);
}

/*
 * Free a Plan 9 draw image.
 */
static void p9_free_image(struct p9conn *p9, uint32_t fid, uint32_t image_id) {
    uint8_t cmd[5];
    cmd[0] = 'f';
    PUT32(cmd + 1, image_id);
    p9_write(p9, fid, 0, cmd, 5);
}

/*
 * Reallocate Plan 9 images at new dimensions.
 */
static void reallocate_p9_images(struct draw_state *draw, int width, int height) {
    struct p9conn *p9 = draw->p9;
    uint32_t fid = draw->drawdata_fid;
    
    /* Free old images */
    p9_free_image(p9, fid, draw->image_id);
    p9_free_image(p9, fid, draw->delta_id);
    
    /* Allocate new images */
    p9_alloc_image(p9, fid, draw->image_id, width, height, 0x68081828);  /* XRGB32 */
    p9_alloc_image(p9, fid, draw->delta_id, width, height, 0x48081828);  /* ARGB32 */
    
    wlr_log(WLR_INFO, "Reallocated 9front images: %dx%d", width, height);
}

/* ============== Draw State Update ============== */

/*
 * Update draw_state and server dimensions from computed output dims.
 */
static void update_state(struct server *s, const struct output_dims *dims,
                         int win_minx, int win_miny) {
    struct draw_state *draw = &s->draw;
    
    s->width = dims->wl_phys_w;
    s->height = dims->wl_phys_h;
    s->tiles_x = (dims->wl_phys_w + TILE_SIZE - 1) / TILE_SIZE;
    s->tiles_y = (dims->wl_phys_h + TILE_SIZE - 1) / TILE_SIZE;
    
    draw->logical_width = dims->wl_phys_w;
    draw->logical_height = dims->wl_phys_h;
    draw->width = dims->dest_w;
    draw->height = dims->dest_h;
    draw->scale = dims->draw_scale;
    draw->input_scale = dims->input_scale;
    draw->scene_width = dims->scene_w;
    draw->scene_height = dims->scene_h;
    draw->win_minx = win_minx;
    draw->win_miny = win_miny;
}

/* ============== Output State Helpers ============== */

/*
 * Configure wlroots output state for given dimensions.
 */
static void configure_output_state(struct wlr_output_state *state,
                                   const struct output_dims *dims,
                                   int set_enabled) {
    wlr_output_state_init(state);
    
    if (set_enabled) {
        wlr_output_state_set_enabled(state, true);
    }
    
    wlr_output_state_set_custom_mode(state, dims->wl_phys_w, dims->wl_phys_h,
                                     set_enabled ? 60000 : 0);
    
    if (dims->mode == SCALE_MODE_FRACTIONAL_WL) {
        wlr_output_state_set_scale(state, (float)dims->k);
    }
}

/* ============== Resize Handler ============== */

void handle_resize(struct server *s, int new_w, int new_h, int new_minx, int new_miny) {
    wlr_log(WLR_INFO, "handle_resize: %dx%d -> %dx%d at (%d,%d)", 
            s->draw.width, s->draw.height, new_w, new_h, new_minx, new_miny);
    
    struct output_dims dims = compute_dimensions(new_w, new_h, s->scale, s->wl_scaling);
    
    wlr_log(WLR_INFO, "Resize mode=%d: rio %dx%d, wl_buf %dx%d, scene %dx%d",
            dims.mode, new_w, new_h, dims.wl_phys_w, dims.wl_phys_h, 
            dims.scene_w, dims.scene_h);
    
    /* Allocate compositor buffers */
    if (allocate_buffers(s, dims.wl_phys_w, dims.wl_phys_h) < 0) {
        return;
    }
    
    /* Update state */
    pthread_mutex_lock(&s->send_lock);
    update_state(s, &dims, new_minx, new_miny);
    pthread_mutex_unlock(&s->send_lock);
    
    /* Reallocate Plan 9 images */
    reallocate_p9_images(&s->draw, dims.wl_phys_w, dims.wl_phys_h);
    
    /* Configure wlroots output */
    struct wlr_output_state state;
    configure_output_state(&state, &dims, 0);
    wlr_output_commit_state(s->output, &state);
    wlr_output_state_finish(&state);
    
    /* Reconfigure toplevels */
    struct toplevel *tl;
    wl_list_for_each(tl, &s->toplevels, link) {
        if (tl->xdg && tl->xdg->base && tl->xdg->base->initialized) {
            wlr_xdg_toplevel_set_size(tl->xdg, dims.scene_w, dims.scene_h);
        }
    }
    
    /* Resize background */
    if (s->background) {
        wlr_scene_rect_set_size(s->background, dims.scene_w, dims.scene_h);
    }
    
    s->draw.xor_enabled = 0;
    s->force_full_frame = 1;
    
    wlr_log(WLR_INFO, "Resize complete: wl_buf %dx%d, scene %dx%d at (%d,%d)", 
            dims.wl_phys_w, dims.wl_phys_h, dims.scene_w, dims.scene_h, 
            new_minx, new_miny);
}

/* ============== Frame Handler ============== */

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
    
    /* Check for pending resize */
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
            s->draw.win_minx = new_minx;
            s->draw.win_miny = new_miny;
            s->force_full_frame = 1;
            wlr_log(WLR_DEBUG, "Position update only: (%d,%d)", new_minx, new_miny);
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

/* ============== New Output Handler ============== */

void new_output(struct wl_listener *l, void *d) {
    struct server *s = wl_container_of(l, s, new_output);
    struct wlr_output *out = d;
    
    wlr_output_init_render(out, s->allocator, s->renderer);
    
    float scale = s->scale;
    if (scale <= 0.0f) scale = 1.0f;
    
    int phys_w = s->width;
    int phys_h = s->height;
    
    struct output_dims dims = compute_dimensions(phys_w, phys_h, scale, s->wl_scaling);
    
    const char *mode_names[] = {"none", "wayland", "fractional_wl", "9front"};
    wlr_log(WLR_INFO, "new_output: mode=%s scale=%.2f k=%d",
            mode_names[dims.mode], scale, dims.k);
    wlr_log(WLR_INFO, "  rio: %dx%d, wl_buf: %dx%d, scene: %dx%d",
            phys_w, phys_h, dims.wl_phys_w, dims.wl_phys_h, 
            dims.scene_w, dims.scene_h);
    
    /* Configure and commit output */
    struct wlr_output_state state;
    configure_output_state(&state, &dims, 1);
    wlr_output_commit_state(out, &state);
    wlr_output_state_finish(&state);
    
    /* Setup output in layout */
    wlr_output_layout_add_auto(s->output_layout, out);
    s->output = out;
    s->scene_output = wlr_scene_output_create(s->scene, out);
    
    /* Setup listeners */
    s->output_frame.notify = output_frame;
    wl_signal_add(&out->events.frame, &s->output_frame);
    s->output_destroy.notify = output_destroy;
    wl_signal_add(&out->events.destroy, &s->output_destroy);
    
    /* Allocate buffers */
    if (allocate_buffers(s, dims.wl_phys_w, dims.wl_phys_h) < 0) {
        wlr_log(WLR_ERROR, "Failed to allocate buffers for new output");
        return;
    }
    
    /* Reallocate Plan 9 images */
    reallocate_p9_images(&s->draw, dims.wl_phys_w, dims.wl_phys_h);
    
    /* Update state */
    update_state(s, &dims, s->draw.win_minx, s->draw.win_miny);
    
    /* Resize background */
    if (s->background) {
        wlr_scene_rect_set_size(s->background, dims.scene_w, dims.scene_h);
    }
    
    wlr_log(WLR_INFO, "Output ready: wl_buf %dx%d, scene %dx%d, "
            "draw_scale=%.3f, input_scale=%.2f",
            dims.wl_phys_w, dims.wl_phys_h, dims.scene_w, dims.scene_h,
            dims.draw_scale, dims.input_scale);
    
    /* Trigger first frame */
    s->force_full_frame = 1;
    s->frame_dirty = 1;
}

/* ============== Input Handler ============== */

void new_input(struct wl_listener *l, void *d) {
    struct server *s = wl_container_of(l, s, new_input);
    struct wlr_input_device *dev = d;
    if (dev->type == WLR_INPUT_DEVICE_POINTER)
        wlr_cursor_attach_input_device(s->cursor, dev);
}
