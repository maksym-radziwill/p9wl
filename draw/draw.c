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
#include "../p9/p9.h"
#include "send.h"  /* For TILE_SIZE */

/* Round down to nearest multiple of TILE_SIZE to avoid partial edge tiles */
#define TILE_ALIGN_DOWN(x) (((x) / TILE_SIZE) * TILE_SIZE)

/* Minimum usable dimension (at least a few tiles) */
#define MIN_ALIGNED_DIM (TILE_SIZE * 4)

/* ============== Window Management ============== */

/*
 * Calculate centered window position within actual window bounds.
 * This ensures equal borders on all sides when dimensions are aligned to TILE_SIZE.
 *
 * Parameters:
 *   actual_min - Actual window minimum coordinate (e.g., rminx from wctl)
 *   actual_max - Actual window maximum coordinate (e.g., rmaxx from wctl)
 *   aligned_dim - TILE_SIZE-aligned dimension we're using
 *   out_min - Output: adjusted minimum coordinate (content starts here)
 *   out_actual_dim - Output: actual window dimension
 */
static void center_in_window(int actual_min, int actual_max, int aligned_dim,
                             int *out_min, int *out_actual_dim) {
    int actual_dim = actual_max - actual_min;
    *out_actual_dim = actual_dim;
    
    /* Calculate excess pixels that won't be used */
    int excess = actual_dim - aligned_dim;
    if (excess < 0) excess = 0;
    
    /* Split excess evenly between both sides */
    int offset = excess / 2;
    
    /* Adjust min coordinate to center the content */
    *out_min = actual_min + offset;
}

/* Re-lookup window after "unknown id" error.
 * CRITICAL: When Plan 9 resizes/moves a window, it creates a NEW window
 * with a NEW name (e.g., window.4.14 -> window.4.15). We must re-read
 * /dev/winname to get the current name, then re-lookup with that name.
 */
int relookup_window(struct server *s) {
    struct draw_state *draw = &s->draw;
    struct p9conn *p9 = draw->p9;
    uint8_t vcmd[1] = { 'v' };
    
    /* Re-read /dev/winname to get the CURRENT window name */
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
            draw->winname[sizeof(draw->winname) - 1] = '\0';
        } else {
            wlr_log(WLR_ERROR, "relookup_window: failed to re-read /dev/winname");
            return -1;
        }
    }
    
    if (!draw->winname[0]) {
        wlr_log(WLR_ERROR, "relookup_window: no window name");
        return -1;
    }
    
    wlr_log(WLR_INFO, "Re-looking up window '%s' (screen_id=%d)", draw->winname, draw->screen_id);
    
    /* Free the old window reference */
    uint8_t freecmd[5];
    freecmd[0] = 'f';
    PUT32(freecmd + 1, draw->screen_id);
    p9_write(p9, draw->drawdata_fid, 0, freecmd, 5);
    p9_write(p9, draw->drawdata_fid, 0, vcmd, 1);
    
    /* Re-lookup with 'n' command using the (possibly new) name */
    int wnamelen = strlen(draw->winname);
    uint8_t ncmd[128];
    int noff = 0;
    
    ncmd[noff++] = 'n';
    PUT32(ncmd + noff, draw->screen_id); noff += 4;
    ncmd[noff++] = (uint8_t)wnamelen;
    memcpy(ncmd + noff, draw->winname, wnamelen); noff += wnamelen;
    
    if (p9_write(p9, draw->drawdata_fid, 0, ncmd, noff) < 0) {
        wlr_log(WLR_ERROR, "relookup_window: 'n' command failed");
        return -1;
    }
    p9_write(p9, draw->drawdata_fid, 0, vcmd, 1);
    
    /* Re-read ctl to get current geometry */
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
    
    /* Get actual window dimensions */
    int actual_width = rmaxx - rminx;
    int actual_height = rmaxy - rminy;
    
    /* Align dimensions down to TILE_SIZE to eliminate partial edge tiles */
    int new_width = TILE_ALIGN_DOWN(actual_width);
    int new_height = TILE_ALIGN_DOWN(actual_height);
    
    /* Ensure minimum usable size */
    if (new_width < MIN_ALIGNED_DIM) new_width = MIN_ALIGNED_DIM;
    if (new_height < MIN_ALIGNED_DIM) new_height = MIN_ALIGNED_DIM;
    
    /* Cap at actual window size */
    if (new_width > actual_width) new_width = actual_width;
    if (new_height > actual_height) new_height = actual_height;
    
    if (new_width <= 0 || new_height <= 0 || new_width > 4096 || new_height > 4096) {
        wlr_log(WLR_ERROR, "relookup_window: invalid dimensions %dx%d", new_width, new_height);
        return -1;
    }
    
    /* Center content in window for equal borders */
    int centered_minx, centered_miny;
    int actual_w, actual_h;
    center_in_window(rminx, rmaxx, new_width, &centered_minx, &actual_w);
    center_in_window(rminy, rmaxy, new_height, &centered_miny, &actual_h);
    
    if (new_width != actual_width || new_height != actual_height) {
        int left_border = centered_minx - rminx;
        int top_border = centered_miny - rminy;
        int right_border = (rmaxx - (centered_minx + new_width));
        int bottom_border = (rmaxy - (centered_miny + new_height));
        wlr_log(WLR_INFO, "relookup_window: aligned %dx%d -> %dx%d, borders: L=%d R=%d T=%d B=%d",
                actual_width, actual_height, new_width, new_height,
                left_border, right_border, top_border, bottom_border);
    }
    
    draw->win_minx = centered_minx;
    draw->win_miny = centered_miny;
    
    /* Store actual window bounds for border drawing */
    draw->actual_minx = rminx;
    draw->actual_miny = rminy;
    draw->actual_maxx = rmaxx;
    draw->actual_maxy = rmaxy;
    
    /* Check if dimensions changed */
    if (new_width != draw->width || new_height != draw->height) {
        wlr_log(WLR_INFO, "relookup_window: size changed from %dx%d to %dx%d",
                draw->width, draw->height, new_width, new_height);
        
        /* Store new dimensions for main thread to handle */
        s->pending_width = new_width;
        s->pending_height = new_height;
        s->pending_minx = centered_minx;
        s->pending_miny = centered_miny;
        snprintf(s->pending_winname, sizeof(s->pending_winname), "%s", draw->winname);
        s->resize_pending = 1;
        
        wlr_log(WLR_INFO, "relookup_window: resize pending %dx%d -> %dx%d, main thread will handle",
                draw->width, draw->height, new_width, new_height);
    } else {
        /* Just position change, update now */
        wlr_log(WLR_INFO, "Window position updated: '%s' at (%d,%d) %dx%d",
                draw->winname, draw->win_minx, draw->win_miny, draw->width, draw->height);
    }
    
    return 0;
}

/* Delete rio window (close to clean up) */
void delete_rio_window(struct p9conn *p9) {
    uint32_t wctl_fid = p9->next_fid++;
    const char *wnames[1] = { "wctl" };
    
    if (p9_walk(p9, p9->root_fid, wctl_fid, 1, wnames) < 0) {
        wlr_log(WLR_ERROR, "Failed to walk to /dev/wctl for delete");
        return;
    }
    
    if (p9_open(p9, wctl_fid, OWRITE, NULL) < 0) {
        wlr_log(WLR_ERROR, "Failed to open /dev/wctl for delete");
        return;
    }
    
    const char *cmd = "delete";
    wlr_log(WLR_INFO, "Deleting rio window");
    p9_write(p9, wctl_fid, 0, (uint8_t*)cmd, strlen(cmd));
}

/* ============== Draw Initialization ============== */

int init_draw(struct server *s) {
    struct p9conn *p9 = &s->p9_draw;
    struct draw_state *draw = &s->draw;
    const char *wnames[3];
    char init_str[256];
    int rminx, rminy, rmaxx, rmaxy;
    int actual_width, actual_height;
    
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
    
    /* Open /dev/draw/new */
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
    wlr_log(WLR_DEBUG, "draw/new (%d bytes): %.144s", n, (char*)buf);
    
    /* Parse client ID from first 12-char field */
    draw->client_id = atoi((char*)buf);
    wlr_log(WLR_INFO, "Draw client ID: %d", draw->client_id);
    
    /* Parse initial dimensions from fields 4-7 (R rectangle: minx, miny, maxx, maxy) */
    rminx = atoi((char*)buf + 4*12);
    rminy = atoi((char*)buf + 5*12);
    rmaxx = atoi((char*)buf + 6*12);
    rmaxy = atoi((char*)buf + 7*12);
    actual_width = rmaxx - rminx;
    actual_height = rmaxy - rminy;
    
    if (actual_width <= 0 || actual_height <= 0) {
        wlr_log(WLR_ERROR, "Invalid screen dimensions: %dx%d", actual_width, actual_height);
        return -1;
    }
    
    /* Align dimensions to TILE_SIZE (initial values, may be overwritten by window lookup) */
    draw->width = TILE_ALIGN_DOWN(actual_width);
    draw->height = TILE_ALIGN_DOWN(actual_height);
    if (draw->width < MIN_ALIGNED_DIM) draw->width = MIN_ALIGNED_DIM;
    if (draw->height < MIN_ALIGNED_DIM) draw->height = MIN_ALIGNED_DIM;
    
    wlr_log(WLR_INFO, "Screen: (%d,%d)-(%d,%d) = %dx%d -> aligned %dx%d",
            rminx, rminy, rmaxx, rmaxy, actual_width, actual_height,
            draw->width, draw->height);
    
    /* Walk to /dev/draw/<clientid>/data */
    snprintf(init_str, sizeof(init_str), "%d", draw->client_id);
    wnames[0] = init_str;
    wnames[1] = "data";
    if (p9_walk(p9, draw->draw_fid, draw->drawdata_fid, 2, wnames) < 0) {
        wlr_log(WLR_ERROR, "Failed to walk to /dev/draw/%d/data", draw->client_id);
        return -1;
    }
    
    uint32_t iounit;
    if (p9_open(p9, draw->drawdata_fid, ORDWR, &iounit) < 0) {
        wlr_log(WLR_ERROR, "Failed to open /dev/draw/%d/data", draw->client_id);
        return -1;
    }
    draw->iounit = iounit;
    wlr_log(WLR_INFO, "Draw data fd opened (iounit=%u)", iounit);
    
    /* Walk to /dev/draw/<clientid>/ctl */
    draw->drawctl_fid = p9->next_fid++;
    wnames[0] = init_str;
    wnames[1] = "ctl";
    if (p9_walk(p9, draw->draw_fid, draw->drawctl_fid, 2, wnames) < 0) {
        wlr_log(WLR_ERROR, "Failed to walk to /dev/draw/%d/ctl", draw->client_id);
        return -1;
    }
    
    if (p9_open(p9, draw->drawctl_fid, OREAD, NULL) < 0) {
        wlr_log(WLR_ERROR, "Failed to open /dev/draw/%d/ctl", draw->client_id);
        return -1;
    }
    
    /* Read the window name from /dev/winname */
    char winname[64] = {0};
    draw->winname_fid = p9->next_fid++;
    wnames[0] = "winname";
    if (p9_walk(p9, p9->root_fid, draw->winname_fid, 1, wnames) == 0) {
        if (p9_open(p9, draw->winname_fid, OREAD, NULL) == 0) {
            n = p9_read(p9, draw->winname_fid, 0, sizeof(winname) - 1, (uint8_t*)winname);
            if (n > 0) {
                winname[n] = '\0';
                if (n > 0 && winname[n-1] == '\n') winname[n-1] = '\0';
                wlr_log(WLR_INFO, "Window name: '%s'", winname);
            }
        }
    }
    
    /* Look up the window by name using 'n' command */
    int screen_image_id = 1000;
    if (winname[0]) {
        int wnamelen = strlen(winname);
        uint8_t ncmd[128];
        int noff = 0;
        
        ncmd[noff++] = 'n';
        PUT32(ncmd + noff, screen_image_id); noff += 4;
        ncmd[noff++] = (uint8_t)wnamelen;
        memcpy(ncmd + noff, winname, wnamelen); noff += wnamelen;
        
        wlr_log(WLR_DEBUG, "Looking up window: 'n' id=%d namelen=%d name='%s'", 
                screen_image_id, wnamelen, winname);
        
        int written = p9_write(p9, draw->drawdata_fid, 0, ncmd, noff);
        if (written < 0) {
            wlr_log(WLR_ERROR, "Failed to lookup window '%s' - falling back to image 0", winname);
            screen_image_id = 0;
        } else {
            wlr_log(WLR_INFO, "Sent 'n' command for window '%s' as image %d", winname, screen_image_id);
            
            /* Store window name and ID */
            strncpy(draw->winname, winname, sizeof(draw->winname) - 1);
            draw->winname[sizeof(draw->winname) - 1] = '\0';
            draw->winimage_id = screen_image_id;
            
            /* Flush with 'v' before reading ctl */
            uint8_t vcmd[1] = { 'v' };
            p9_write(p9, draw->drawdata_fid, 0, vcmd, 1);
            
            /* Read ctl to get the window's rectangle */
            uint8_t ctlbuf[256];
            n = p9_read(p9, draw->drawctl_fid, 0, sizeof(ctlbuf) - 1, ctlbuf);
            wlr_log(WLR_DEBUG, "ctl read returned %d bytes", n);
            if (n >= 12*12) {
                ctlbuf[n] = '\0';
                wlr_log(WLR_INFO, "Window ctl (%d bytes): %.144s", n, (char*)ctlbuf);
                
                /* Parse fixed-width 12-char fields */
                rminx = atoi((char*)ctlbuf + 4*12);
                rminy = atoi((char*)ctlbuf + 5*12);
                rmaxx = atoi((char*)ctlbuf + 6*12);
                rmaxy = atoi((char*)ctlbuf + 7*12);
                
                wlr_log(WLR_INFO, "Window rect: (%d,%d)-(%d,%d)", 
                        rminx, rminy, rmaxx, rmaxy);
                
                actual_width = rmaxx - rminx;
                actual_height = rmaxy - rminy;
                
                /* Align dimensions to TILE_SIZE */
                draw->width = TILE_ALIGN_DOWN(actual_width);
                draw->height = TILE_ALIGN_DOWN(actual_height);
                if (draw->width < MIN_ALIGNED_DIM) draw->width = MIN_ALIGNED_DIM;
                if (draw->height < MIN_ALIGNED_DIM) draw->height = MIN_ALIGNED_DIM;
                
                /* Center content in window for equal borders */
                int centered_minx, centered_miny;
                int actual_w, actual_h;
                center_in_window(rminx, rmaxx, draw->width, &centered_minx, &actual_w);
                center_in_window(rminy, rmaxy, draw->height, &centered_miny, &actual_h);
                
                draw->win_minx = centered_minx;
                draw->win_miny = centered_miny;
                
                /* Store actual window bounds for border drawing */
                draw->actual_minx = rminx;
                draw->actual_miny = rminy;
                draw->actual_maxx = rmaxx;
                draw->actual_maxy = rmaxy;
                
                int left_border = centered_minx - rminx;
                int top_border = centered_miny - rminy;
                int right_border = (rmaxx - (centered_minx + draw->width));
                int bottom_border = (rmaxy - (centered_miny + draw->height));
                
                wlr_log(WLR_INFO, "Aligned dimensions: %dx%d -> %dx%d, borders: L=%d R=%d T=%d B=%d",
                        actual_width, actual_height,
                        draw->width, draw->height,
                        left_border, right_border, top_border, bottom_border);
            } else {
                wlr_log(WLR_ERROR, "ctl read too short: %d bytes (need %d)", n, 12*12);
                if (n > 0) {
                    ctlbuf[n] = '\0';
                    wlr_log(WLR_DEBUG, "ctl content: %s", (char*)ctlbuf);
                }
            }
        }
    } else {
        wlr_log(WLR_ERROR, "No window name available, using image 0");
        screen_image_id = 0;
    }
    
    /* Store the screen image ID for drawing */
    draw->screen_id = screen_image_id;
    
    /* Allocate memory image for framebuffer */
    draw->image_id = 1;
    
    /* 'b' command: allocate image */
    uint8_t bcmd[1 + 4 + 4 + 1 + 4 + 1 + 16 + 16 + 4];
    int off = 0;
    
    bcmd[off++] = 'b';
    PUT32(bcmd + off, draw->image_id); off += 4;
    PUT32(bcmd + off, 0); off += 4;               /* screenid = 0 */
    bcmd[off++] = 0;                              /* refresh = 0 */
    PUT32(bcmd + off, 0x68081828); off += 4;      /* chan = XRGB32 */
    bcmd[off++] = 0;                              /* repl = 0 */
    PUT32(bcmd + off, 0); off += 4;
    PUT32(bcmd + off, 0); off += 4;
    PUT32(bcmd + off, draw->width); off += 4;
    PUT32(bcmd + off, draw->height); off += 4;
    PUT32(bcmd + off, 0); off += 4;
    PUT32(bcmd + off, 0); off += 4;
    PUT32(bcmd + off, draw->width); off += 4;
    PUT32(bcmd + off, draw->height); off += 4;
    PUT32(bcmd + off, 0xFF000000); off += 4;      /* color = black */
    
    wlr_log(WLR_DEBUG, "Allocating image: 'b' cmd size=%d", off);
    
    int written = p9_write(p9, draw->drawdata_fid, 0, bcmd, off);
    if (written < 0) {
        wlr_log(WLR_ERROR, "Failed to allocate framebuffer image");
        return -1;
    }
    wlr_log(WLR_INFO, "Allocated framebuffer image %d (%dx%d) format=XRGB32", 
            draw->image_id, draw->width, draw->height);
    
    /* Allocate opaque mask: 1x1 white image with repl=1 */
    draw->opaque_id = 2;
    off = 0;
    bcmd[off++] = 'b';
    PUT32(bcmd + off, draw->opaque_id); off += 4;
    PUT32(bcmd + off, 0); off += 4;
    bcmd[off++] = 0;
    PUT32(bcmd + off, 0x00000031); off += 4;  /* chan = k1 (GREY1) */
    bcmd[off++] = 1;                           /* repl = 1 */
    PUT32(bcmd + off, 0); off += 4;
    PUT32(bcmd + off, 0); off += 4;
    PUT32(bcmd + off, 1); off += 4;
    PUT32(bcmd + off, 1); off += 4;
    PUT32(bcmd + off, (uint32_t)-0x3FFFFFFF); off += 4;
    PUT32(bcmd + off, (uint32_t)-0x3FFFFFFF); off += 4;
    PUT32(bcmd + off, 0x3FFFFFFF); off += 4;
    PUT32(bcmd + off, 0x3FFFFFFF); off += 4;
    PUT32(bcmd + off, 0xFFFFFFFF); off += 4;  /* color = white */
    
    written = p9_write(p9, draw->drawdata_fid, 0, bcmd, off);
    if (written < 0) {
        wlr_log(WLR_ERROR, "Failed to allocate opaque mask");
        return -1;
    }
    wlr_log(WLR_INFO, "Allocated opaque mask image %d", draw->opaque_id);
    
    /* Allocate border color: 1x1 pale bluish gray with repl=1 */
    draw->border_id = 4;
    off = 0;
    bcmd[off++] = 'b';
    PUT32(bcmd + off, draw->border_id); off += 4;
    PUT32(bcmd + off, 0); off += 4;
    bcmd[off++] = 0;
    PUT32(bcmd + off, 0x48081828); off += 4;  /* chan = XRGB32 */
    bcmd[off++] = 1;                           /* repl = 1 */
    PUT32(bcmd + off, 0); off += 4;
    PUT32(bcmd + off, 0); off += 4;
    PUT32(bcmd + off, 1); off += 4;
    PUT32(bcmd + off, 1); off += 4;
    PUT32(bcmd + off, (uint32_t)-0x3FFFFFFF); off += 4;
    PUT32(bcmd + off, (uint32_t)-0x3FFFFFFF); off += 4;
    PUT32(bcmd + off, 0x3FFFFFFF); off += 4;
    PUT32(bcmd + off, 0x3FFFFFFF); off += 4;
    PUT32(bcmd + off, 0x9EEEEE); off += 4;  /* color = pale bluish gray */
    
    written = p9_write(p9, draw->drawdata_fid, 0, bcmd, off);
    if (written < 0) {
        wlr_log(WLR_ERROR, "Failed to allocate border color image");
        return -1;
    }
    wlr_log(WLR_INFO, "Allocated border color image %d", draw->border_id);
    
    /* Allocate delta image with ARGB32 for sparse updates */
    draw->delta_id = 5;
    off = 0;
    bcmd[off++] = 'b';
    PUT32(bcmd + off, draw->delta_id); off += 4;
    PUT32(bcmd + off, 0); off += 4;
    bcmd[off++] = 0;
    PUT32(bcmd + off, 0x48081828); off += 4;  /* chan = a8r8g8b8 */
    bcmd[off++] = 0;
    PUT32(bcmd + off, 0); off += 4;
    PUT32(bcmd + off, 0); off += 4;
    PUT32(bcmd + off, draw->width); off += 4;
    PUT32(bcmd + off, draw->height); off += 4;
    PUT32(bcmd + off, 0); off += 4;
    PUT32(bcmd + off, 0); off += 4;
    PUT32(bcmd + off, draw->width); off += 4;
    PUT32(bcmd + off, draw->height); off += 4;
    PUT32(bcmd + off, 0x00000000); off += 4;  /* color = transparent black */
    
    written = p9_write(p9, draw->drawdata_fid, 0, bcmd, off);
    if (written < 0) {
        wlr_log(WLR_ERROR, "Failed to allocate delta image");
        return -1;
    }
    wlr_log(WLR_INFO, "Allocated delta image %d (%dx%d) ARGB32 for alpha-delta compression", 
            draw->delta_id, draw->width, draw->height);
    
    draw->xor_enabled = 0;  /* Will be enabled after first successful full frame */
    
    return 0;
}
