/*
 * output.c - Output and input device handlers
 *
 * Handles output frame rendering, resize handling, and input device attachment.
 *
 * ═══════════════════════════════════════════════════════════════════════════════
 * COORDINATE SYSTEM DOCUMENTATION
 * ═══════════════════════════════════════════════════════════════════════════════
 *
 * This compositor bridges Wayland applications to a Plan 9 rio window.
 * There are multiple coordinate systems involved, which can be confusing.
 * This documentation clarifies each one.
 *
 * ┌─────────────────────────────────────────────────────────────────────────────┐
 * │ COORDINATE SPACE     │ SIZE VARIABLES      │ PURPOSE                        │
 * ├─────────────────────────────────────────────────────────────────────────────┤
 * │ Rio Window           │ rio_w, rio_h        │ Physical pixels in rio window  │
 * │                      │ (from wctl rect)    │ where final image appears      │
 * ├─────────────────────────────────────────────────────────────────────────────┤
 * │ Compositor Buffer    │ buffer_w, buffer_h  │ Framebuffer we render into     │
 * │                      │ (s->width/height)   │ and compress tiles from        │
 * ├─────────────────────────────────────────────────────────────────────────────┤
 * │ Wayland Scene        │ scene_w, scene_h    │ Logical size apps see          │
 * │                      │ (toplevel configure)│ (may differ with HiDPI)        │
 * ├─────────────────────────────────────────────────────────────────────────────┤
 * │ Plan 9 Source Image  │ p9src_w, p9src_h    │ Size of image_id we load       │
 * │                      │ (draw->logical_*)   │ pixels into via 'y' command    │
 * ├─────────────────────────────────────────────────────────────────────────────┤
 * │ Plan 9 Destination   │ p9dst_w, p9dst_h    │ Where we blit in rio window    │
 * │                      │ (draw->width/height)│ using 'd' or 'a' command       │
 * └─────────────────────────────────────────────────────────────────────────────┘
 *
 * ═══════════════════════════════════════════════════════════════════════════════
 * SCALING MODES
 * ═══════════════════════════════════════════════════════════════════════════════
 *
 * The compositor supports three scaling modes controlled by -S and -B flags:
 *
 * ┌─────────────────────────────────────────────────────────────────────────────┐
 * │ MODE                 │ FLAGS      │ HOW IT WORKS                            │
 * ├─────────────────────────────────────────────────────────────────────────────┤
 * │ No Scaling           │ (default)  │ All coordinates identical               │
 * │                      │            │ rio = buffer = scene = p9src = p9dst    │
 * │                      │            │ 1:1 pixel mapping, no scaling anywhere  │
 * ├─────────────────────────────────────────────────────────────────────────────┤
 * │ Wayland Fractional   │ -S 1.5     │ Wayland renders at higher resolution    │
 * │ (wl_scaling=1)       │            │ buffer > scene (by factor k=ceil(scale))│
 * │                      │            │ 9front receives full-res, may downsample│
 * │                      │            │ to fit rio window (p9_blit_scale < 1)   │
 * ├─────────────────────────────────────────────────────────────────────────────┤
 * │ 9front Backend       │ -S 2 -B    │ Compositor renders at low resolution    │
 * │ (wl_scaling=0)       │            │ buffer = scene (both small)             │
 * │                      │            │ 9front upscales via 'a' command         │
 * │                      │            │ p9_blit_scale > 1, saves bandwidth      │
 * └─────────────────────────────────────────────────────────────────────────────┘
 *
 * ═══════════════════════════════════════════════════════════════════════════════
 * WORKED EXAMPLES
 * ═══════════════════════════════════════════════════════════════════════════════
 *
 * Example 1: No scaling (default)
 *   rio window: 1920x1080
 *   All sizes: 1920x1080 (aligned to TILE_SIZE: 1920x1072)
 *   p9_blit_scale: 1.0
 *   input_scale: 1.0
 *
 * Example 2: Wayland fractional scaling (-S 1.5)
 *   rio window: 1920x1080
 *   k = ceil(1.5) = 2
 *   buffer: ~2560x1440 (rio * k/scale, aligned)
 *   scene: 1280x720 (buffer / k)
 *   p9src: 2560x1440 (same as buffer)
 *   p9dst: 1920x1080 (rio window)
 *   p9_blit_scale: 0.75 (1.5/2 - slight downsample)
 *   input_scale: 1.5 (rio coords / 1.5 = scene coords)
 *
 * Example 3: 9front backend scaling (-S 2 -B)
 *   rio window: 1920x1080
 *   buffer: 960x540 (rio / scale)
 *   scene: 960x540 (same as buffer)
 *   p9src: 960x540 (same as buffer)
 *   p9dst: 1920x1080 (rio window, upscaled)
 *   p9_blit_scale: 2.0 (upscale factor)
 *   input_scale: 2.0 (rio coords / 2 = scene coords)
 *
 * ═══════════════════════════════════════════════════════════════════════════════
 * MOUSE INPUT COORDINATE TRANSLATION
 * ═══════════════════════════════════════════════════════════════════════════════
 *
 * Mouse events from 9front arrive in rio (physical) coordinates.
 * They must be translated to scene (logical) coordinates for Wayland:
 *
 *   scene_x = (rio_x - win_minx) / input_scale
 *   scene_y = (rio_y - win_miny) / input_scale
 *
 * Where win_minx/win_miny is the top-left corner of our content area
 * within the rio window (accounting for centering due to TILE_SIZE alignment).
 *
 * ═══════════════════════════════════════════════════════════════════════════════
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

/* ═══════════════════════════════════════════════════════════════════════════════
 * DIMENSION CALCULATION
 * ═══════════════════════════════════════════════════════════════════════════════ */

enum scaling_mode {
    SCALE_MODE_NONE,           /* No scaling (scale ≈ 1) */
    SCALE_MODE_FRACTIONAL_WL,  /* Wayland does scaling (scale > 1, default) */
    SCALE_MODE_9FRONT,         /* 9front does scaling (scale > 1, -B flag) */
};

/*
 * Computed dimensions for a given scaling configuration.
 * 
 * This structure centralizes all dimension calculations. Once computed,
 * these values are copied to the appropriate places in server/draw_state.
 *
 * Variable naming convention:
 *   rio_*    = physical rio window coordinates
 *   buffer_* = compositor framebuffer (what we render to)
 *   scene_*  = Wayland logical coordinates (what apps see)
 *   p9src_*  = Plan 9 source image dimensions
 *   p9dst_*  = Plan 9 blit destination dimensions
 */
struct computed_dims {
    enum scaling_mode mode;
    
    /* Integer scale factor for Wayland fractional scaling */
    int k;  /* k = ceil(scale), used as wlr_output scale */
    
    /* Rio window (physical pixels on 9front display) */
    int rio_w;
    int rio_h;
    
    /* Compositor buffer (framebuffer we render into) */
    int buffer_w;
    int buffer_h;
    
    /* Wayland scene (logical coordinates apps see) */
    int scene_w;
    int scene_h;
    
    /* Plan 9 source image (what we load pixels into) */
    int p9src_w;
    int p9src_h;
    
    /* Plan 9 destination (where we blit in rio window) */
    int p9dst_w;
    int p9dst_h;
    
    /* Scale factor for 'a' command: controls src→dst mapping */
    float p9_blit_scale;
    
    /* Scale factor for mouse input: rio coords → scene coords */
    float input_scale;
};

static const char *mode_name(enum scaling_mode mode) {
    switch (mode) {
        case SCALE_MODE_NONE: return "none";
        case SCALE_MODE_FRACTIONAL_WL: return "wayland-fractional";
        case SCALE_MODE_9FRONT: return "9front-backend";
    }
    return "unknown";
}

/*
 * Compute all dimensions based on scaling configuration.
 *
 * Parameters:
 *   rio_w, rio_h: Physical rio window size (from wctl rect)
 *   scale: User-requested scale factor (from -S flag, default 1.0)
 *   wl_scaling: 1=Wayland scaling (default), 0=9front scaling (-B flag)
 *
 * Returns: fully populated computed_dims structure
 */
static struct computed_dims compute_dimensions(int rio_w, int rio_h,
                                                float scale, int wl_scaling) {
    struct computed_dims d = {0};
    
    if (scale <= 0.0f) scale = 1.0f;
    d.k = (int)ceilf(scale);
    
    /* Rio dimensions are always the physical window */
    d.rio_w = rio_w;
    d.rio_h = rio_h;
    
    if (wl_scaling && scale > 1.001f) {
        /*
         * SCALE_MODE_FRACTIONAL_WL: Wayland does the scaling
         *
         * Apps see smaller logical size, compositor renders at larger
         * physical size (k * logical), 9front receives full-res pixels.
         *
         * buffer_factor = k / scale gives us slightly larger than rio
         * so we can downsample to exactly fill the rio window.
         */
        d.mode = SCALE_MODE_FRACTIONAL_WL;
        
        float buffer_factor = (float)d.k / scale;
        d.buffer_w = (int)((rio_w * buffer_factor + 0.5f) / d.k) * d.k;
        d.buffer_h = (int)((rio_h * buffer_factor + 0.5f) / d.k) * d.k;
        
        /* Align to k * TILE_SIZE for clean tile boundaries */
        d.buffer_w = (d.buffer_w / (d.k * TILE_SIZE)) * d.k * TILE_SIZE;
        d.buffer_h = (d.buffer_h / (d.k * TILE_SIZE)) * d.k * TILE_SIZE;
        
        /* Minimum size */
        if (d.buffer_w < d.k * TILE_SIZE * 4) 
            d.buffer_w = d.k * TILE_SIZE * 4;
        if (d.buffer_h < d.k * TILE_SIZE * 4) 
            d.buffer_h = d.k * TILE_SIZE * 4;
        
        d.scene_w = d.buffer_w / d.k;
        d.scene_h = d.buffer_h / d.k;
        
        d.p9src_w = d.buffer_w;
        d.p9src_h = d.buffer_h;
        d.p9dst_w = d.rio_w;
        d.p9dst_h = d.rio_h;
        
        d.p9_blit_scale = scale / (float)d.k;
        d.input_scale = scale;
        
    } else if (wl_scaling || scale <= 1.001f) {
        /*
         * SCALE_MODE_NONE: No scaling (1:1 pixel mapping)
         *
         * All coordinate systems are identical.
         */
        d.mode = SCALE_MODE_NONE;
        
        /* Align to TILE_SIZE */
        d.buffer_w = (rio_w / TILE_SIZE) * TILE_SIZE;
        d.buffer_h = (rio_h / TILE_SIZE) * TILE_SIZE;
        if (d.buffer_w < TILE_SIZE * 4) d.buffer_w = TILE_SIZE * 4;
        if (d.buffer_h < TILE_SIZE * 4) d.buffer_h = TILE_SIZE * 4;
        
        d.scene_w = d.buffer_w;
        d.scene_h = d.buffer_h;
        
        d.p9src_w = d.buffer_w;
        d.p9src_h = d.buffer_h;
        d.p9dst_w = d.buffer_w;
        d.p9dst_h = d.buffer_h;
        
        d.p9_blit_scale = 1.0f;
        d.input_scale = 1.0f;
        
    } else {
        /*
         * SCALE_MODE_9FRONT: 9front does the scaling
         *
         * Compositor renders at lower resolution (rio / scale),
         * 9front upscales via the 'a' command to fill rio window.
         * This saves bandwidth at the cost of 9front CPU.
         */
        d.mode = SCALE_MODE_9FRONT;
        
        d.buffer_w = (int)(rio_w / scale);
        d.buffer_h = (int)(rio_h / scale);
        
        /* Align to TILE_SIZE */
        d.buffer_w = (d.buffer_w / TILE_SIZE) * TILE_SIZE;
        d.buffer_h = (d.buffer_h / TILE_SIZE) * TILE_SIZE;
        if (d.buffer_w < TILE_SIZE * 4) d.buffer_w = TILE_SIZE * 4;
        if (d.buffer_h < TILE_SIZE * 4) d.buffer_h = TILE_SIZE * 4;
        
        d.scene_w = d.buffer_w;
        d.scene_h = d.buffer_h;
        
        d.p9src_w = d.buffer_w;
        d.p9src_h = d.buffer_h;
        d.p9dst_w = (int)(d.buffer_w * scale);
        d.p9dst_h = (int)(d.buffer_h * scale);
        
        d.p9_blit_scale = scale;
        d.input_scale = scale;
    }
    
    return d;
}

static void log_dimensions(const struct computed_dims *d, const char *context) {
    wlr_log(WLR_INFO, "%s: mode=%s k=%d", context, mode_name(d->mode), d->k);
    wlr_log(WLR_INFO, "  rio:    %4d x %4d (physical rio window)", 
            d->rio_w, d->rio_h);
    wlr_log(WLR_INFO, "  buffer: %4d x %4d (compositor framebuffer)", 
            d->buffer_w, d->buffer_h);
    wlr_log(WLR_INFO, "  scene:  %4d x %4d (Wayland logical)", 
            d->scene_w, d->scene_h);
    wlr_log(WLR_INFO, "  p9src:  %4d x %4d (Plan 9 source image)", 
            d->p9src_w, d->p9src_h);
    wlr_log(WLR_INFO, "  p9dst:  %4d x %4d (Plan 9 blit dest)", 
            d->p9dst_w, d->p9dst_h);
    wlr_log(WLR_INFO, "  p9_blit_scale=%.3f input_scale=%.3f", 
            d->p9_blit_scale, d->input_scale);
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * STATE APPLICATION
 * ═══════════════════════════════════════════════════════════════════════════════
 *
 * Map computed dimensions to existing struct fields.
 *
 * Unfortunately the existing codebase uses different names for these concepts.
 * This function documents the mapping:
 *
 *   s->width, s->height         ← buffer_w, buffer_h (compositor framebuffer)
 *   s->tiles_x, s->tiles_y      ← derived from buffer dimensions
 *   draw->width, draw->height   ← p9dst_w, p9dst_h (Plan 9 blit destination)
 *   draw->logical_width/height  ← p9src_w, p9src_h (Plan 9 source image)
 *   draw->scene_width/height    ← scene_w, scene_h (Wayland logical)
 *   draw->scale                 ← p9_blit_scale
 *   draw->input_scale           ← input_scale
 */
static void apply_dimensions(struct server *s, const struct computed_dims *d,
                              int win_minx, int win_miny) {
    struct draw_state *draw = &s->draw;
    
    /* Compositor uses buffer dimensions */
    s->width = d->buffer_w;
    s->height = d->buffer_h;
    s->tiles_x = (d->buffer_w + TILE_SIZE - 1) / TILE_SIZE;
    s->tiles_y = (d->buffer_h + TILE_SIZE - 1) / TILE_SIZE;
    
    /* Draw state - Plan 9 coordinates */
    draw->logical_width = d->p9src_w;   /* Source image size */
    draw->logical_height = d->p9src_h;
    draw->width = d->p9dst_w;           /* Blit destination size */
    draw->height = d->p9dst_h;
    draw->scale = d->p9_blit_scale;     /* For 'a' command matrix */
    
    /* Draw state - Wayland coordinates */
    draw->scene_width = d->scene_w;
    draw->scene_height = d->scene_h;
    draw->input_scale = d->input_scale;
    
    /* Window position (for mouse coordinate translation) */
    draw->win_minx = win_minx;
    draw->win_miny = win_miny;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * BUFFER MANAGEMENT
 * ═══════════════════════════════════════════════════════════════════════════════ */

/*
 * Allocate compositor framebuffers.
 * Returns 0 on success, -1 on failure.
 */
static int allocate_buffers(struct server *s, int width, int height) {
    size_t fb_size = (size_t)width * height * sizeof(uint32_t);
    
    uint32_t *new_framebuf = calloc(1, fb_size);
    uint32_t *new_prev_framebuf = calloc(1, fb_size);
    uint32_t *new_send_buf0 = calloc(1, fb_size);
    uint32_t *new_send_buf1 = calloc(1, fb_size);
    
    if (!new_framebuf || !new_prev_framebuf || !new_send_buf0 || !new_send_buf1) {
        wlr_log(WLR_ERROR, "Failed to allocate buffers for %dx%d (%zu bytes)",
                width, height, fb_size);
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
    
    wlr_log(WLR_DEBUG, "Allocated buffers: %dx%d (%zu bytes each)", 
            width, height, fb_size);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * PLAN 9 IMAGE MANAGEMENT
 * ═══════════════════════════════════════════════════════════════════════════════ */

/* Plan 9 channel format constants */
#define P9_CHAN_XRGB32 0x68081828  /* x8r8g8b8 - opaque RGB */
#define P9_CHAN_ARGB32 0x48081828  /* a8r8g8b8 - alpha + RGB (for delta) */

/*
 * Allocate a Plan 9 draw image.
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
    p9_alloc_image(p9, fid, draw->image_id, width, height, P9_CHAN_XRGB32);
    p9_alloc_image(p9, fid, draw->delta_id, width, height, P9_CHAN_ARGB32);
    
    wlr_log(WLR_INFO, "Reallocated Plan 9 images: %dx%d", width, height);
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * OUTPUT CONFIGURATION
 * ═══════════════════════════════════════════════════════════════════════════════ */

/*
 * Configure wlroots output state for given dimensions.
 */
static void configure_output_state(struct wlr_output_state *state,
                                   const struct computed_dims *d,
                                   int set_enabled) {
    wlr_output_state_init(state);
    
    if (set_enabled) {
        wlr_output_state_set_enabled(state, true);
    }
    
    wlr_output_state_set_custom_mode(state, d->buffer_w, d->buffer_h,
                                     set_enabled ? 60000 : 0);
    
    if (d->mode == SCALE_MODE_FRACTIONAL_WL) {
        wlr_output_state_set_scale(state, (float)d->k);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * RESIZE HANDLER
 * ═══════════════════════════════════════════════════════════════════════════════ */

void handle_resize(struct server *s, int new_rio_w, int new_rio_h, 
                   int new_minx, int new_miny) {
    wlr_log(WLR_INFO, "handle_resize: rio %dx%d -> %dx%d at (%d,%d)", 
            s->draw.width, s->draw.height, new_rio_w, new_rio_h, 
            new_minx, new_miny);
    
    struct computed_dims d = compute_dimensions(new_rio_w, new_rio_h, 
                                                 s->scale, s->wl_scaling);
    log_dimensions(&d, "Resize");
    
    /* Allocate compositor buffers */
    if (allocate_buffers(s, d.buffer_w, d.buffer_h) < 0) {
        return;
    }
    
    /* Update state */
    pthread_mutex_lock(&s->send_lock);
    apply_dimensions(s, &d, new_minx, new_miny);
    pthread_mutex_unlock(&s->send_lock);
    
    /* Reallocate Plan 9 images */
    reallocate_p9_images(&s->draw, d.p9src_w, d.p9src_h);
    
    /* Configure wlroots output */
    struct wlr_output_state state;
    configure_output_state(&state, &d, 0);
    wlr_output_commit_state(s->output, &state);
    wlr_output_state_finish(&state);
    
    /* Reconfigure toplevels to new scene size */
    struct toplevel *tl;
    wl_list_for_each(tl, &s->toplevels, link) {
        if (tl->xdg && tl->xdg->base && tl->xdg->base->initialized) {
            wlr_xdg_toplevel_set_size(tl->xdg, d.scene_w, d.scene_h);
        }
    }
    
    /* Resize background */
    if (s->background) {
        wlr_scene_rect_set_size(s->background, d.scene_w, d.scene_h);
    }
    
    s->draw.xor_enabled = 0;
    s->force_full_frame = 1;
    
    wlr_log(WLR_INFO, "Resize complete");
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * FRAME HANDLER
 * ═══════════════════════════════════════════════════════════════════════════════ */

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
            /* Position change only */
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
                    wlr_log(WLR_DEBUG, "Frame %d: first=0x%08x mid=0x%08x (changed=%d)", 
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

/* ═══════════════════════════════════════════════════════════════════════════════
 * NEW OUTPUT HANDLER
 * ═══════════════════════════════════════════════════════════════════════════════ */

void new_output(struct wl_listener *l, void *d) {
    struct server *s = wl_container_of(l, s, new_output);
    struct wlr_output *out = d;
    
    wlr_output_init_render(out, s->allocator, s->renderer);
    
    float scale = s->scale;
    if (scale <= 0.0f) scale = 1.0f;
    
    /* Initial rio dimensions come from draw init */
    int rio_w = s->width;
    int rio_h = s->height;
    
    struct computed_dims dims = compute_dimensions(rio_w, rio_h, scale, s->wl_scaling);
    log_dimensions(&dims, "new_output");
    
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
    if (allocate_buffers(s, dims.buffer_w, dims.buffer_h) < 0) {
        wlr_log(WLR_ERROR, "Failed to allocate buffers for new output");
        return;
    }
    
    /* Reallocate Plan 9 images */
    reallocate_p9_images(&s->draw, dims.p9src_w, dims.p9src_h);
    
    /* Update state */
    apply_dimensions(s, &dims, s->draw.win_minx, s->draw.win_miny);
    
    /* Resize background */
    if (s->background) {
        wlr_scene_rect_set_size(s->background, dims.scene_w, dims.scene_h);
    }
    
    wlr_log(WLR_INFO, "Output ready");
    
    /* Trigger first frame */
    s->force_full_frame = 1;
    s->frame_dirty = 1;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * INPUT HANDLER
 * ═══════════════════════════════════════════════════════════════════════════════ */

void new_input(struct wl_listener *l, void *d) {
    struct server *s = wl_container_of(l, s, new_input);
    struct wlr_input_device *dev = d;
    if (dev->type == WLR_INPUT_DEVICE_POINTER)
        wlr_cursor_attach_input_device(s->cursor, dev);
}
