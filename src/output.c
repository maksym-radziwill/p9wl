/*
 * output.c - Output creation, frame rendering, and resize handling
 *
 * Creates the headless wlroots output sized to match the window,
 * runs the frame loop that renders the scene graph into a framebuffer
 * for the send thread, and handles dynamic window resizes.
 *
 * See output.h for the frame loop description and damage tracking design.
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include "types.h"

#ifndef CHAN_XRGB32
#define CHAN_XRGB32 1
#endif
#ifndef CHAN_ARGB32
#define CHAN_ARGB32 2
#endif

static void output_destroy(struct wl_listener *listener, void *data) {
    struct server *s = wl_container_of(listener, s, output_destroy);
    (void)data;
    wl_list_remove(&s->output_frame.link);
    wl_list_remove(&s->output_destroy.link);
}

/*
 * Reallocate draw images after resize.
 */
static void reallocate_draw_images(struct draw_state *draw, int new_w, int new_h) {
    (void)draw;
    (void)new_w;
    (void)new_h;
}

static void dump_framebuffer_ppm(const char *filename, uint32_t *pixels, int width, int height) {
    FILE *f = fopen(filename, "wb");
    if (!f) {
        wlr_log(WLR_ERROR, "Failed to open %s for framebuffer dump: %s", filename, strerror(errno));
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", width, height);
    for (int i = 0; i < width * height; i++) {
        uint32_t p = pixels[i];
        uint8_t b = (p >> 0) & 0xFF;
        uint8_t g = (p >> 8) & 0xFF;
        uint8_t r = (p >> 16) & 0xFF;
        fputc(r, f);
        fputc(g, f);
        fputc(b, f);
    }
    fclose(f);
}

static void output_frame(struct wl_listener *listener, void *data) {
    struct server *s = wl_container_of(listener, s, output_frame);
    struct wlr_scene_output *so = s->scene_output;
    static int frame_count = 0;
    (void)data;
    
    /* Check if window was resized - read atomically under lock */
    pthread_mutex_lock(&s->send_lock);
    int resize_pending = s->resize_pending;
    int new_w = s->pending_width;
    int new_h = s->pending_height;
    int new_vis_w = s->pending_visible_width;
    int new_vis_h = s->pending_visible_height;
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
            /* Visible dimensions may change even without padding change */
            s->visible_width = new_vis_w;
            s->visible_height = new_vis_h;
            draw->visible_width = new_vis_w;
            draw->visible_height = new_vis_h;
            wlr_log(WLR_DEBUG, "Position update only: (%d,%d)", new_minx, new_miny);
        } else {
            wlr_log(WLR_INFO, "Main thread handling resize: %dx%d -> %dx%d (visible %dx%d)",
                    s->width, s->height, new_w, new_h, new_vis_w, new_vis_h);
            
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
                s->visible_width = new_vis_w;
                s->visible_height = new_vis_h;
                /* Padded dimensions are always exact multiples of TILE_SIZE */
                s->tiles_x = new_w / TILE_SIZE;
                s->tiles_y = new_h / TILE_SIZE;
                
                /* Reallocate dirty tile bitmaps */
                uint8_t *old_dirty0 = s->dirty_tiles[0];
                uint8_t *old_dirty1 = s->dirty_tiles[1];
                int ntiles = s->tiles_x * s->tiles_y;
                s->dirty_tiles[0] = ntiles > 0 ? calloc(1, ntiles) : NULL;
                s->dirty_tiles[1] = ntiles > 0 ? calloc(1, ntiles) : NULL;
                s->dirty_valid[0] = 0;
                s->dirty_valid[1] = 0;
                
                draw->width = new_w;
                draw->height = new_h;
                draw->visible_width = new_vis_w;
                draw->visible_height = new_vis_h;
                draw->win_minx = new_minx;
                draw->win_miny = new_miny;
                pthread_mutex_unlock(&s->send_lock);
                
                free(old_framebuf);
                free(old_prev_framebuf);
                free(old_send_buf0);
                free(old_send_buf1);
                free(old_dirty0);
                free(old_dirty1);
                
                /* Reallocate staging buffer (output thread only, no lock) */
                free(s->dirty_staging);
                s->dirty_staging = ntiles > 0 ? calloc(1, ntiles) : NULL;
                s->dirty_staging_valid = 0;
                
                /* Reallocate draw state images */
                reallocate_draw_images(draw, new_w, new_h);
                
                /* Resize wlroots output to VISIBLE dimensions.
                 * The wlroots buffer will be visible_width × visible_height;
                 * the padded framebuf is larger (stride = width). */
                struct wlr_output_state state;
                wlr_output_state_init(&state);
                wlr_output_state_set_custom_mode(&state, new_vis_w, new_vis_h, 0);
                if (s->scale > 1.0f) {
                    wlr_output_state_set_scale(&state, s->scale);
                }
                wlr_output_commit_state(s->output, &state);
                wlr_output_state_finish(&state);
                
                /* Compute logical dimensions from VISIBLE size for Wayland clients */
                int logical_w = (int)(new_vis_w / s->scale + 0.5f);
                int logical_h = (int)(new_vis_h / s->scale + 0.5f);
                
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
                s->scene_dirty = 1;
                
                wlr_log(WLR_INFO, "Resize complete: %dx%d visible (%dx%d padded), %dx%d logical at (%d,%d)",
                        new_vis_w, new_vis_h, new_w, new_h,
                        logical_w, logical_h, new_minx, new_miny);
            }
        }
    }
    
    uint32_t now = now_ms();
#if FRAME_INTERVAL_MS > 0
    if (now - s->last_frame_ms < FRAME_INTERVAL_MS) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        wlr_scene_output_send_frame_done(so, &ts);
        wlr_output_schedule_frame(s->output);
        return;
    }
#endif
    s->last_frame_ms = now;
    frame_count++;
    
    /*
     * Skip rendering entirely when the scene hasn't changed.
     * scene_dirty is set by toplevel_commit/subsurface_commit when a
     * client submits new content.  force_full_frame is set by resize
     * handling (above) and error recovery.  On an idle screen this
     * avoids wlr_scene_output_build_state() (which renders into a
     * buffer) and the subsequent memcpy into framebuf — the dominant
     * source of idle CPU usage.
     */
    if (!s->scene_dirty && !s->force_full_frame) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        wlr_scene_output_send_frame_done(so, &ts);
        wlr_output_schedule_frame(s->output);
        return;
    }
    s->scene_dirty = 0;
    
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
        wlr_output_schedule_frame(s->output);
        return;
    }
    
    s->dirty_staging_valid = 0;  /* Reset; set below if damage extracted */
    int has_dirty = 0;           /* Set if any damage rects found */
    
    struct wlr_buffer *buffer = ostate.buffer;
    if (buffer) {
        void *data_ptr;
        uint32_t format;
        size_t stride;
        
        if (wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ,
                                              &data_ptr, &format, &stride)) {
            int w = s->width;
            int h = s->height;
            uint32_t *fb = s->framebuf;
            int valid_fb = (fb && w > 0 && h > 0 && w <= MAX_SCREEN_DIM && h <= MAX_SCREEN_DIM);
            
            /*
             * Extract damage BEFORE copying pixels.  This lets us skip
             * the buffer copy entirely on idle frames (nrects == 0) and
             * copy only damaged rows on active frames.
             */
            int nrects = 0;
            pixman_box32_t *rects = pixman_region32_rectangles(
                &ostate.damage, &nrects);
            
            /* Build dirty tile bitmap */
            if (!s->dirty_staging && s->tiles_x > 0 && s->tiles_y > 0)
                s->dirty_staging = calloc(1, s->tiles_x * s->tiles_y);
            if (s->dirty_staging && s->tiles_x > 0 && s->tiles_y > 0) {
                int ntiles = s->tiles_x * s->tiles_y;
                memset(s->dirty_staging, 0, ntiles);
                for (int r = 0; r < nrects; r++) {
                    int tx0 = rects[r].x1 / TILE_SIZE;
                    int ty0 = rects[r].y1 / TILE_SIZE;
                    int tx1 = (rects[r].x2 + TILE_SIZE - 1) / TILE_SIZE;
                    int ty1 = (rects[r].y2 + TILE_SIZE - 1) / TILE_SIZE;
                    if (tx0 < 0) tx0 = 0;
                    if (ty0 < 0) ty0 = 0;
                    if (tx1 > s->tiles_x) tx1 = s->tiles_x;
                    if (ty1 > s->tiles_y) ty1 = s->tiles_y;
                    for (int ty = ty0; ty < ty1; ty++)
                        for (int tx = tx0; tx < tx1; tx++)
                            s->dirty_staging[ty * s->tiles_x + tx] = 1;
                }
                s->dirty_staging_valid = 1;
                has_dirty = (nrects > 0);
            }
            
            if (valid_fb) {
                int buf_w = buffer->width;
                int buf_h = buffer->height;
                int vis_w = s->visible_width;
                int vis_h = s->visible_height;
                int copy_w = (buf_w < vis_w) ? buf_w : vis_w;
                int copy_h = (buf_h < vis_h) ? buf_h : vis_h;
                
                pthread_mutex_lock(&s->send_lock);
                for (int y = 0; y < copy_h; y++) {
                    memcpy(&fb[y * w],
                           (uint8_t*)data_ptr + y * stride,
                           copy_w * 4);
                }
                pthread_mutex_unlock(&s->send_lock);
            }
            
            wlr_buffer_end_data_ptr_access(buffer);

            /* Dump every 10th rendered frame buffer to a PPM file (retaining the last 100) */
            if (frame_count % 10 == 0 && valid_fb) {
                char filename[128];
                snprintf(filename, sizeof(filename), "/app/frame_%d.ppm", frame_count);
                pthread_mutex_lock(&s->send_lock);
                dump_framebuffer_ppm(filename, s->framebuf, s->width, s->height);
                pthread_mutex_unlock(&s->send_lock);
                wlr_log(WLR_INFO, "Dumped frame %d to %s", frame_count, filename);

                /* Remove the oldest frame dump that exceeds the 100-file rolling window */
                if (frame_count >= 1010) {
                    char old_filename[128];
                    snprintf(old_filename, sizeof(old_filename), "/app/frame_%d.ppm", frame_count - 1000);
                    if (unlink(old_filename) == 0) {
                        wlr_log(WLR_DEBUG, "Pruned old frame dump: %s", old_filename);
                    }
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
    wlr_output_schedule_frame(s->output);
    
    if (s->force_full_frame || has_dirty || !s->dirty_staging_valid) {
        send_frame(s);
    }
}

void new_output(struct wl_listener *l, void *d) {
    struct server *s = wl_container_of(l, s, new_output);
    struct wlr_output *out = d;
    
    wlr_output_init_render(out, s->allocator, s->renderer);
    
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    wlr_output_state_set_custom_mode(&state, s->visible_width, s->visible_height, 60000);
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
    
    s->force_full_frame = 1;
    s->scene_dirty = 1;
    wlr_output_schedule_frame(out);

    if (s->scale > 1.0f) {
        int logical_w = (int)(s->visible_width / s->scale + 0.5f);
        int logical_h = (int)(s->visible_height / s->scale + 0.5f);
        wlr_log(WLR_INFO, "Output ready: %dx%d visible (%dx%d padded), scale=%.2f, %dx%d logical",
                s->visible_width, s->visible_height, s->width, s->height,
                s->scale, logical_w, logical_h);
    } else {
        wlr_log(WLR_INFO, "Output ready: %dx%d visible (%dx%d padded)",
                s->visible_width, s->visible_height, s->width, s->height);
    }
}

void new_input(struct wl_listener *l, void *d) {
    struct server *s = wl_container_of(l, s, new_input);
    struct wlr_input_device *dev = d;
    if (dev->type == WLR_INPUT_DEVICE_POINTER)
        wlr_cursor_attach_input_device(s->cursor, dev);
}