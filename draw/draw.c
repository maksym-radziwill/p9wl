/*
 * draw.c - Plan 9 draw device initialization and window management
 *
 * Manages connection to /dev/draw and window lookup/relookup.
 *
 * Refactored:
 * - Extracted align_dimension() for consistent tile alignment
 * - Extracted compute_centered_window() for centering calculations
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>

#include <wlr/util/log.h>

#include "draw.h"
#include "draw_cmd.h"
#include "../p9/p9.h"
#include "send.h"

#define TILE_ALIGN_DOWN(x) (((x) / TILE_SIZE) * TILE_SIZE)
#define MIN_ALIGNED_DIM (TILE_SIZE * 4)
#define RIO_BORDER 4  /* rio window border width in pixels */

/* ============== Dimension Helpers ============== */

/*
 * Align a dimension to tile boundaries with minimum constraint.
 * Returns aligned dimension that is:
 *   - Tile-aligned (multiple of TILE_SIZE)
 *   - At least MIN_ALIGNED_DIM (unless actual is smaller)
 *   - Not larger than actual
 */
static int align_dimension(int actual) {
    int aligned = TILE_ALIGN_DOWN(actual);
    if (aligned < MIN_ALIGNED_DIM) aligned = MIN_ALIGNED_DIM;
    if (aligned > actual) aligned = actual;
    return aligned;
}

/*
 * Compute centered position within window bounds.
 * Given actual bounds and aligned dimension, returns:
 *   - out_min: starting coordinate (centered)
 *   - out_actual_dim: original dimension for reference
 */
static void center_in_window(int actual_min, int actual_max, int aligned_dim,
                             int *out_min, int *out_actual_dim) {
    int actual_dim = actual_max - actual_min;
    *out_actual_dim = actual_dim;
    int excess = actual_dim - aligned_dim;
    if (excess < 0) excess = 0;
    *out_min = actual_min + excess / 2;
}

/*
 * Parse ctl buffer and compute aligned/centered dimensions.
 * Encapsulates the common pattern of reading geometry from ctl.
 */
static int parse_ctl_geometry(const uint8_t *ctlbuf, int n,
                              int *out_width, int *out_height,
                              int *out_minx, int *out_miny,
                              int *out_actual_minx, int *out_actual_miny,
                              int *out_actual_maxx, int *out_actual_maxy) {
    if (n < 12 * 12) return -1;
    
    int rminx = atoi((char*)ctlbuf + 4*12);
    int rminy = atoi((char*)ctlbuf + 5*12);
    int rmaxx = atoi((char*)ctlbuf + 6*12);
    int rmaxy = atoi((char*)ctlbuf + 7*12);
    
    int actual_width = rmaxx - rminx;
    int actual_height = rmaxy - rminy;
    
    int width = align_dimension(actual_width);
    int height = align_dimension(actual_height);
    
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
        return -1;
    }
    
    int centered_minx, centered_miny, actual_w, actual_h;
    center_in_window(rminx, rmaxx, width, &centered_minx, &actual_w);
    center_in_window(rminy, rmaxy, height, &centered_miny, &actual_h);
    
    *out_width = width;
    *out_height = height;
    *out_minx = centered_minx;
    *out_miny = centered_miny;
    *out_actual_minx = rminx;
    *out_actual_miny = rminy;
    *out_actual_maxx = rmaxx;
    *out_actual_maxy = rmaxy;
    
    return 0;
}

/*
 * Resize the rio window via /dev/wctl to fit aligned dimensions plus border.
 * The window rect is expanded by RIO_BORDER on each side so that rio's
 * border remains visible around the framebuffer content.
 *
 * After resize, parse_ctl_geometry's centering logic naturally places
 * win_minx/win_miny at (rminx + RIO_BORDER, rminy + RIO_BORDER) since
 * 2*RIO_BORDER < TILE_SIZE, so the excess pixels become the border margin.
 *
 * Returns 0 on success, -1 on failure (window may not support resize).
 */
static int resize_to_aligned(struct p9conn *p9, int minx, int miny,
                              int aligned_width, int aligned_height) {
    uint32_t wctl_fid = p9->next_fid++;
    const char *wnames[1] = { "wctl" };

    if (p9_walk(p9, p9->root_fid, wctl_fid, 1, wnames) < 0)
        return -1;
    if (p9_open(p9, wctl_fid, OWRITE, NULL) < 0)
        return -1;

    char cmd[128];
    int maxx = minx + aligned_width + 2 * RIO_BORDER;
    int maxy = miny + aligned_height + 2 * RIO_BORDER;
    int len = snprintf(cmd, sizeof(cmd), "resize -r %d %d %d %d",
                       minx, miny, maxx, maxy);

    wlr_log(WLR_INFO, "Resizing window to aligned rect with border: (%d,%d)-(%d,%d) [content %dx%d + %dpx border]",
            minx, miny, maxx, maxy, aligned_width, aligned_height, RIO_BORDER);

    if (p9_write(p9, wctl_fid, 0, (uint8_t*)cmd, len) < 0) {
        wlr_log(WLR_ERROR, "resize_to_aligned: wctl write failed");
        return -1;
    }

    return 0;
}

/* ============== Window Management ============== */

int relookup_window(struct server *s) {
    struct draw_state *draw = &s->draw;
    struct p9conn *p9 = draw->p9;
    uint8_t cmd[128];
    
    /* Re-read /dev/winname */
    if (draw->winname_fid) {
        char newname[64] = {0};
        int n = p9_read(p9, draw->winname_fid, 0, sizeof(newname) - 1, (uint8_t*)newname);
        if (n > 0) {
            newname[n] = '\0';
            if (n > 0 && newname[n-1] == '\n') newname[n-1] = '\0';
            if (strcmp(newname, draw->winname) != 0) {
                wlr_log(WLR_INFO, "Window name changed: '%s' -> '%s'", draw->winname, newname);
            }
            snprintf(draw->winname, sizeof(draw->winname), "%s", newname);
        } else {
            wlr_log(WLR_ERROR, "relookup_window: failed to re-read /dev/winname");
            return -1;
        }
    }
    
    if (!draw->winname[0]) {
        wlr_log(WLR_ERROR, "relookup_window: no window name");
        return -1;
    }
    
    wlr_log(WLR_INFO, "Re-looking up window '%s'", draw->winname);
    
    /* Free old window reference */
    int off = free_image_cmd(cmd, draw->screen_id);
    p9_write(p9, draw->drawdata_fid, 0, cmd, off);
    off = flush_cmd(cmd);
    p9_write(p9, draw->drawdata_fid, 0, cmd, off);
    
    /* Re-lookup with 'n' command */
    int wnamelen = strlen(draw->winname);
    off = name_cmd(cmd, draw->screen_id, draw->winname, wnamelen);
    if (p9_write(p9, draw->drawdata_fid, 0, cmd, off) < 0) {
        wlr_log(WLR_ERROR, "relookup_window: 'n' command failed");
        return -1;
    }
    off = flush_cmd(cmd);
    p9_write(p9, draw->drawdata_fid, 0, cmd, off);
    
    /* Re-read ctl for geometry */
    uint8_t ctlbuf[256];
    int n = p9_read(p9, draw->drawctl_fid, 0, sizeof(ctlbuf) - 1, ctlbuf);
    
    int new_width, new_height, centered_minx, centered_miny;
    int rminx, rminy, rmaxx, rmaxy;
    
    if (parse_ctl_geometry(ctlbuf, n, &new_width, &new_height,
                           &centered_minx, &centered_miny,
                           &rminx, &rminy, &rmaxx, &rmaxy) < 0) {
        wlr_log(WLR_ERROR, "relookup_window: invalid geometry");
        return -1;
    }
    
    /* Resize window to aligned dimensions if needed */
    {
        int actual_width = rmaxx - rminx;
        int actual_height = rmaxy - rminy;
        int expected_with_border = new_width + 2 * RIO_BORDER;
        int expected_h_border = new_height + 2 * RIO_BORDER;
        /* Only resize if the window isn't already sized for aligned+border */
        if (actual_width != expected_with_border || actual_height != expected_h_border) {
            if (resize_to_aligned(p9, rminx, rminy,
                                  new_width, new_height) == 0) {
                wlr_log(WLR_INFO, "relookup: resized window from %dx%d to %dx%d+border",
                        actual_width, actual_height, new_width, new_height);
                
                /* Re-read ctl for post-resize geometry */
                int off2 = flush_cmd(cmd);
                p9_write(p9, draw->drawdata_fid, 0, cmd, off2);
                
                n = p9_read(p9, draw->drawctl_fid, 0, sizeof(ctlbuf) - 1, ctlbuf);
                parse_ctl_geometry(ctlbuf, n, &new_width, &new_height,
                                   &centered_minx, &centered_miny,
                                   &rminx, &rminy, &rmaxx, &rmaxy);
            }
        }
    }
    
    draw->win_minx = centered_minx;
    draw->win_miny = centered_miny;
    draw->actual_minx = rminx;
    draw->actual_miny = rminy;
    draw->actual_maxx = rmaxx;
    draw->actual_maxy = rmaxy;
    
    if (new_width != draw->width || new_height != draw->height) {
        wlr_log(WLR_INFO, "relookup_window: size changed %dx%d -> %dx%d",
                draw->width, draw->height, new_width, new_height);
        s->pending_width = new_width;
        s->pending_height = new_height;
        s->pending_minx = centered_minx;
        s->pending_miny = centered_miny;
        snprintf(s->pending_winname, sizeof(s->pending_winname), "%s", draw->winname);
        s->resize_pending = 1;
    } else {
        wlr_log(WLR_INFO, "Window position updated: (%d,%d)", draw->win_minx, draw->win_miny);
        s->force_full_frame = 1;
        s->frame_dirty = 1;
    }
    
    return 0;
}

void delete_rio_window(struct p9conn *p9) {
    uint32_t wctl_fid = p9->next_fid++;
    const char *wnames[1] = { "wctl" };
    
    if (p9_walk(p9, p9->root_fid, wctl_fid, 1, wnames) < 0) return;
    if (p9_open(p9, wctl_fid, OWRITE, NULL) < 0) return;
    
    wlr_log(WLR_INFO, "Deleting rio window");
    p9_write(p9, wctl_fid, 0, (uint8_t*)"delete", 6);
}

/* ============== Draw Initialization ============== */

int init_draw(struct server *s) {
    struct p9conn *p9 = &s->p9_draw;
    struct draw_state *draw = &s->draw;
    const char *wnames[3];
    uint8_t cmd[64];
    
    draw->p9 = p9;
    draw->draw_fid = p9->next_fid++;
    draw->drawnew_fid = p9->next_fid++;
    draw->drawdata_fid = p9->next_fid++;
    draw->win_minx = 0;
    draw->win_miny = 0;
    
    /* Walk to /dev/draw */
    wnames[0] = "draw";
    if (p9_walk(p9, p9->root_fid, draw->draw_fid, 1, wnames) < 0) {
        wlr_log(WLR_ERROR, "Failed to walk to /dev/draw");
        return -1;
    }
    
    /* Walk to /dev/draw/new */
    wnames[0] = "new";
    if (p9_walk(p9, draw->draw_fid, draw->drawnew_fid, 1, wnames) < 0) {
        wlr_log(WLR_ERROR, "Failed to walk to /dev/draw/new");
        return -1;
    }
    
    if (p9_open(p9, draw->drawnew_fid, ORDWR, NULL) < 0) {
        wlr_log(WLR_ERROR, "Failed to open /dev/draw/new");
        return -1;
    }
    
    /* Read client ID and screen info */
    uint8_t buf[256];
    int n = p9_read(p9, draw->drawnew_fid, 0, sizeof(buf) - 1, buf);
    if (n < 12*12) {
        wlr_log(WLR_ERROR, "draw/new read too short: got %d bytes", n);
        return -1;
    }
    buf[n] = '\0';
    
    draw->client_id = atoi((char*)buf);
    wlr_log(WLR_INFO, "Draw client ID: %d", draw->client_id);
    
    /* Parse initial geometry from draw/new */
    int rminx = atoi((char*)buf + 4*12);
    int rminy = atoi((char*)buf + 5*12);
    int rmaxx = atoi((char*)buf + 6*12);
    int rmaxy = atoi((char*)buf + 7*12);
    int actual_width = rmaxx - rminx;
    int actual_height = rmaxy - rminy;
    
    draw->width = align_dimension(actual_width);
    draw->height = align_dimension(actual_height);

    wlr_log(WLR_INFO, "Screen dimensions: %dx%d (aligned to %dx%d)",
            actual_width, actual_height, draw->width, draw->height);
    
    /* Walk to data file */
    char datapath[32];
    snprintf(datapath, sizeof(datapath), "%d", draw->client_id);
    wnames[0] = datapath;
    wnames[1] = "data";
    if (p9_walk(p9, draw->draw_fid, draw->drawdata_fid, 2, wnames) < 0) {
        wlr_log(WLR_ERROR, "Failed to walk to /dev/draw/%d/data", draw->client_id);
        return -1;
    }
    
    uint32_t iounit = 0;
    if (p9_open(p9, draw->drawdata_fid, ORDWR, &iounit) < 0) {
        wlr_log(WLR_ERROR, "Failed to open /dev/draw/%d/data", draw->client_id);
        return -1;
    }
    draw->iounit = iounit ? iounit : (p9->msize - 24);
    wlr_log(WLR_INFO, "Opened /dev/draw/%d/data, iounit=%u", draw->client_id, draw->iounit);
    
    /* Walk to ctl file */
    draw->drawctl_fid = p9->next_fid++;
    wnames[0] = datapath;
    wnames[1] = "ctl";
    if (p9_walk(p9, draw->draw_fid, draw->drawctl_fid, 2, wnames) < 0) {
        wlr_log(WLR_ERROR, "Failed to walk to /dev/draw/%d/ctl", draw->client_id);
        return -1;
    }
    if (p9_open(p9, draw->drawctl_fid, OREAD, NULL) < 0) {
        wlr_log(WLR_ERROR, "Failed to open /dev/draw/%d/ctl", draw->client_id);
        return -1;
    }
    
    /* Open /dev/winname for window name tracking */
    draw->winname_fid = p9->next_fid++;
    wnames[0] = "winname";
    if (p9_walk(p9, p9->root_fid, draw->winname_fid, 1, wnames) >= 0) {
        if (p9_open(p9, draw->winname_fid, OREAD, NULL) >= 0) {
            char winname[64] = {0};
            n = p9_read(p9, draw->winname_fid, 0, sizeof(winname) - 1, (uint8_t*)winname);
            if (n > 0) {
                winname[n] = '\0';
                if (n > 0 && winname[n-1] == '\n') winname[n-1] = '\0';
                snprintf(draw->winname, sizeof(draw->winname), "%s", winname);
                wlr_log(WLR_INFO, "Window name: '%s'", draw->winname);
            }
        }
    }
    
    /* Look up window by name */
    uint32_t screen_image_id = 10;
    if (draw->winname[0]) {
        int wnamelen = strlen(draw->winname);
        int off = name_cmd(cmd, screen_image_id, draw->winname, wnamelen);
        
        int written = p9_write(p9, draw->drawdata_fid, 0, cmd, off);
        if (written < 0) {
            wlr_log(WLR_ERROR, "Failed to lookup window '%s'", draw->winname);
            screen_image_id = 0;
        } else {
            wlr_log(WLR_INFO, "Window '%s' as image %d", draw->winname, screen_image_id);
            draw->winimage_id = screen_image_id;
            
            /* Flush and read ctl for window geometry */
            off = flush_cmd(cmd);
            p9_write(p9, draw->drawdata_fid, 0, cmd, off);
            
            uint8_t ctlbuf[256];
            n = p9_read(p9, draw->drawctl_fid, 0, sizeof(ctlbuf) - 1, ctlbuf);
            
            int new_width, new_height, centered_minx, centered_miny;
            int act_minx, act_miny, act_maxx, act_maxy;
            
            if (parse_ctl_geometry(ctlbuf, n, &new_width, &new_height,
                                   &centered_minx, &centered_miny,
                                   &act_minx, &act_miny, &act_maxx, &act_maxy) == 0) {
                int actual_width = act_maxx - act_minx;
                int actual_height = act_maxy - act_miny;
                
                /* Resize window to aligned dimensions if needed */
                int expected_with_border = new_width + 2 * RIO_BORDER;
                int expected_h_border = new_height + 2 * RIO_BORDER;
                if (actual_width != expected_with_border || actual_height != expected_h_border) {
                    if (resize_to_aligned(p9, act_minx, act_miny,
                                          new_width, new_height) == 0) {
                        wlr_log(WLR_INFO, "Resized window from %dx%d to %dx%d",
                                actual_width, actual_height, new_width, new_height);
                        
                        /* Re-read ctl for post-resize geometry */
                        off = flush_cmd(cmd);
                        p9_write(p9, draw->drawdata_fid, 0, cmd, off);
                        
                        n = p9_read(p9, draw->drawctl_fid, 0, sizeof(ctlbuf) - 1, ctlbuf);
                        if (parse_ctl_geometry(ctlbuf, n, &new_width, &new_height,
                                               &centered_minx, &centered_miny,
                                               &act_minx, &act_miny,
                                               &act_maxx, &act_maxy) == 0) {
                            wlr_log(WLR_INFO, "Post-resize rect: (%d,%d)-(%d,%d) -> %dx%d",
                                    act_minx, act_miny, act_maxx, act_maxy,
                                    new_width, new_height);
                        }
                    } else {
                        wlr_log(WLR_INFO, "Window resize not supported, using centered alignment");
                    }
                }
                
                draw->width = new_width;
                draw->height = new_height;
                draw->win_minx = centered_minx;
                draw->win_miny = centered_miny;
                draw->actual_minx = act_minx;
                draw->actual_miny = act_miny;
                draw->actual_maxx = act_maxx;
                draw->actual_maxy = act_maxy;
                
                wlr_log(WLR_INFO, "Window rect: (%d,%d)-(%d,%d) -> %dx%d",
                        act_minx, act_miny, act_maxx, act_maxy, 
                        draw->width, draw->height);
            }
        }
    } else {
        screen_image_id = 0;
    }
    
    draw->screen_id = screen_image_id;
    
    /* Allocate images using helpers */
    int off;
    
    /* Framebuffer image */
    draw->image_id = 1;
    off = alloc_image_cmd(cmd, draw->image_id, CHAN_XRGB32, 0,
                          0, 0, draw->width, draw->height, 0xFF000000);
    if (p9_write(p9, draw->drawdata_fid, 0, cmd, off) < 0) {
        wlr_log(WLR_ERROR, "Failed to allocate framebuffer image");
        return -1;
    }
    wlr_log(WLR_INFO, "Allocated framebuffer image %d (%dx%d)", 
            draw->image_id, draw->width, draw->height);
    
    /* Opaque mask: 1x1 white with repl=1 */
    draw->opaque_id = 2;
    off = alloc_image_cmd(cmd, draw->opaque_id, CHAN_GREY1, 1,
                          0, 0, 1, 1, 0xFFFFFFFF);
    if (p9_write(p9, draw->drawdata_fid, 0, cmd, off) < 0) {
        wlr_log(WLR_ERROR, "Failed to allocate opaque mask");
        return -1;
    }
    wlr_log(WLR_INFO, "Allocated opaque mask image %d", draw->opaque_id);
    
    /* Border color: 1x1 pale gray with repl=1 */
    draw->border_id = 4;
    off = alloc_image_cmd(cmd, draw->border_id, CHAN_ARGB32, 1,
                          0, 0, 1, 1, 0x9EEEEE);
    if (p9_write(p9, draw->drawdata_fid, 0, cmd, off) < 0) {
        wlr_log(WLR_ERROR, "Failed to allocate border color image");
        return -1;
    }
    wlr_log(WLR_INFO, "Allocated border color image %d", draw->border_id);
    
    /* Delta image: ARGB32 for sparse updates */
    draw->delta_id = 5;
    off = alloc_image_cmd(cmd, draw->delta_id, CHAN_ARGB32, 0,
                          0, 0, draw->width, draw->height, 0x00000000);
    if (p9_write(p9, draw->drawdata_fid, 0, cmd, off) < 0) {
        wlr_log(WLR_ERROR, "Failed to allocate delta image");
        return -1;
    }
    wlr_log(WLR_INFO, "Allocated delta image %d (%dx%d)", 
            draw->delta_id, draw->width, draw->height);
    
    draw->xor_enabled = 0;

    return 0;
}
