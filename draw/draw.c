/*
 * draw.c - Plan 9 draw device initialization and window management
 *
 * Manages connection to /dev/draw and window lookup/relookup.
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

/* ============== Window Management ============== */

static void center_in_window(int actual_min, int actual_max, int aligned_dim,
                             int *out_min, int *out_actual_dim) {
    int actual_dim = actual_max - actual_min;
    *out_actual_dim = actual_dim;
    int excess = actual_dim - aligned_dim;
    if (excess < 0) excess = 0;
    *out_min = actual_min + excess / 2;
}

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
            strncpy(draw->winname, newname, sizeof(draw->winname) - 1);
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
    if (n < 12*12) {
        wlr_log(WLR_ERROR, "relookup_window: failed to read ctl");
        return -1;
    }
    
    ctlbuf[n] = '\0';
    int rminx = atoi((char*)ctlbuf + 4*12);
    int rminy = atoi((char*)ctlbuf + 5*12);
    int rmaxx = atoi((char*)ctlbuf + 6*12);
    int rmaxy = atoi((char*)ctlbuf + 7*12);
    
    int actual_width = rmaxx - rminx;
    int actual_height = rmaxy - rminy;
    int new_width = TILE_ALIGN_DOWN(actual_width);
    int new_height = TILE_ALIGN_DOWN(actual_height);
    
    if (new_width < MIN_ALIGNED_DIM) new_width = MIN_ALIGNED_DIM;
    if (new_height < MIN_ALIGNED_DIM) new_height = MIN_ALIGNED_DIM;
    if (new_width > actual_width) new_width = actual_width;
    if (new_height > actual_height) new_height = actual_height;
    
    if (new_width <= 0 || new_height <= 0 || new_width > 4096 || new_height > 4096) {
        wlr_log(WLR_ERROR, "relookup_window: invalid dimensions %dx%d", new_width, new_height);
        return -1;
    }
    
    int centered_minx, centered_miny, actual_w, actual_h;
    center_in_window(rminx, rmaxx, new_width, &centered_minx, &actual_w);
    center_in_window(rminy, rmaxy, new_height, &centered_miny, &actual_h);
    
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
    int rminx, rminy, rmaxx, rmaxy;
    int actual_width, actual_height;
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
    
    rminx = atoi((char*)buf + 4*12);
    rminy = atoi((char*)buf + 5*12);
    rmaxx = atoi((char*)buf + 6*12);
    rmaxy = atoi((char*)buf + 7*12);
    actual_width = rmaxx - rminx;
    actual_height = rmaxy - rminy;
    
    draw->width = TILE_ALIGN_DOWN(actual_width);
    draw->height = TILE_ALIGN_DOWN(actual_height);
    if (draw->width < MIN_ALIGNED_DIM) draw->width = MIN_ALIGNED_DIM;
    if (draw->height < MIN_ALIGNED_DIM) draw->height = MIN_ALIGNED_DIM;
    
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
                strncpy(draw->winname, winname, sizeof(draw->winname) - 1);
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
            if (n >= 12*12) {
                ctlbuf[n] = '\0';
                rminx = atoi((char*)ctlbuf + 4*12);
                rminy = atoi((char*)ctlbuf + 5*12);
                rmaxx = atoi((char*)ctlbuf + 6*12);
                rmaxy = atoi((char*)ctlbuf + 7*12);
                
                actual_width = rmaxx - rminx;
                actual_height = rmaxy - rminy;
                
                draw->width = TILE_ALIGN_DOWN(actual_width);
                draw->height = TILE_ALIGN_DOWN(actual_height);
                if (draw->width < MIN_ALIGNED_DIM) draw->width = MIN_ALIGNED_DIM;
                if (draw->height < MIN_ALIGNED_DIM) draw->height = MIN_ALIGNED_DIM;
                
                int centered_minx, centered_miny, actual_w, actual_h;
                center_in_window(rminx, rmaxx, draw->width, &centered_minx, &actual_w);
                center_in_window(rminy, rmaxy, draw->height, &centered_miny, &actual_h);
                
                draw->win_minx = centered_minx;
                draw->win_miny = centered_miny;
                draw->actual_minx = rminx;
                draw->actual_miny = rminy;
                draw->actual_maxx = rmaxx;
                draw->actual_maxy = rmaxy;
                
                wlr_log(WLR_INFO, "Window rect: (%d,%d)-(%d,%d) -> %dx%d",
                        rminx, rminy, rmaxx, rmaxy, draw->width, draw->height);
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
