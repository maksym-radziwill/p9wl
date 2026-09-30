/*
 * draw.c - Headless RDP Surface & Frame Management (Alpine/musl variant)
 *
 * Manages surface initialization, layout tracking, and RDP stream handoff
 * for headless Weston/FreeRDP channel pipelines.
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <wlr/util/log.h>
#include "types.h"

#define DRAW_SCALE 1.0f

/* Round up to nearest multiple of TILE_SIZE so every tile is a full 16×16. */
#define TILE_ALIGN_UP(x) (((x) + TILE_SIZE - 1) / TILE_SIZE * TILE_SIZE)

/* Minimum usable dimension (at least a few tiles) */
#define MIN_ALIGNED_DIM (TILE_SIZE * 4)

/* ============== Surface & Window Management ============== */

int relookup_window(struct server *s) {
    struct draw_state *draw = &s->draw;
    
    wlr_log(WLR_INFO, "RDP surface layout check: width=%d, height=%d", draw->width, draw->height);
    
    /* Headless RDP sessions rely on dynamic client sizing and surface updates
     * rather than window manager lookups. */
    s->force_full_frame = 1;
    s->frame_dirty = 1;
    
    return 0;
}

void delete_rio_window(void *p9) {
    (void)p9;
    wlr_log(WLR_INFO, "RDP headless session cleanup invoked");
}

/* ============== Draw Initialization ============== */

int init_draw(struct server *s) {
    struct draw_state *draw = &s->draw;
    
    draw->win_minx = 0;
    draw->win_miny = 0;
    
    /* Default headless surface geometry */
    int actual_width = 1024;
    int actual_height = 768;
    
    draw->visible_width = actual_width;
    draw->visible_height = actual_height;
    if (draw->visible_width < MIN_ALIGNED_DIM) draw->visible_width = MIN_ALIGNED_DIM;
    if (draw->visible_height < MIN_ALIGNED_DIM) draw->visible_height = MIN_ALIGNED_DIM;
    
    draw->width = TILE_ALIGN_UP(draw->visible_width);
    draw->height = TILE_ALIGN_UP(draw->visible_height);
    
    draw->screen_id = 1;
    draw->image_id = 1;
    draw->opaque_id = 2;
    draw->delta_id = 5;
    
    int logical_width = (int)(draw->visible_width / DRAW_SCALE + 0.5f);
    int logical_height = (int)(draw->visible_height / DRAW_SCALE + 0.5f);
    
    logical_width = TILE_ALIGN_UP(logical_width);
    logical_height = TILE_ALIGN_UP(logical_height);
    if (logical_width < MIN_ALIGNED_DIM) logical_width = MIN_ALIGNED_DIM;
    if (logical_height < MIN_ALIGNED_DIM) logical_height = MIN_ALIGNED_DIM;
    
    draw->logical_width = logical_width;
    draw->logical_height = logical_height;
    draw->scale = DRAW_SCALE;
    
    wlr_log(WLR_INFO, "Headless RDP Surface Initialized: %dx%d (logical %dx%d), Format=XRGB32",
            draw->width, draw->height, logical_width, logical_height);
    
    draw->xor_enabled = 0;
    
    return 0;
}