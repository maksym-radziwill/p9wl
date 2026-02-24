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

/*
 * FRACTIONAL SCALING SUPPORT
 *
 * When DRAW_SCALE > 1.0, the compositor runs at logical resolution
 * and 9front scales up to physical resolution using the 'a' command.
 *
 * Example: DRAW_SCALE = 1.5
 *   Physical window: 1552 x 880
 *   Logical source:  1035 x 587 (physical / 1.5)
 *   Bandwidth savings: sends 44% as many pixels (1/1.5²)
 *
 * Set to 1.0 for no scaling (1:1 pixel mapping).
 */
#define DRAW_SCALE 1.0f

/* Round up to nearest multiple of TILE_SIZE so every tile is a full 16×16.
 * The buffer is slightly larger than the visible window; the invisible
 * padding pixels are never displayed because Plan 9's draw device clips
 * the copy-to-screen to the screen image's rectangle. */
#define TILE_ALIGN_UP(x) (((x) + TILE_SIZE - 1) / TILE_SIZE * TILE_SIZE)

/* Minimum usable dimension (at least a few tiles) */
#define MIN_ALIGNED_DIM (TILE_SIZE * 4)

/* Minimum border (pixels) to preserve on each side of the rio window.
 * We inset by RIO_BORDER from the window edges so our content never
 * overwrites rio's border decoration.  The copy-to-screen uses
 * visible dimensions, leaving equal borders on all four sides. */
#define RIO_BORDER 4

/* ============== Window Management ============== */

/* Re-lookup window after "unknown id" error.
 * CRITICAL: When Plan 9 resizes/moves a window, it creates a NEW window
 * with a NEW name (e.g., window.4.14 -> window.4.15). We must re-read
 * /dev/winname to get the current name, then re-lookup with that name.
 *
 * During rapid resizes the name can change multiple times in quick
 * succession.  We retry the read-name → free → lookup sequence with
 * increasing backoff so the name has time to stabilise.
 */
int relookup_window(struct server *s) {
    struct draw_state *draw = &s->draw;
    struct p9conn *p9 = draw->p9;
    uint8_t vcmd[1] = { 'v' };
    
    /* Retry loop: re-read winname, free old ref, 'n' lookup.
     * Back off between attempts to let the rio server settle. */
    static const int backoff_ms[] = { 0, 10, 25, 50, 100 };
    int max_retries = (int)(sizeof(backoff_ms) / sizeof(backoff_ms[0]));
    
    for (int attempt = 0; attempt < max_retries; attempt++) {
        if (attempt > 0) {
            struct timespec ts = {
                .tv_sec  = 0,
                .tv_nsec = backoff_ms[attempt] * 1000000L
            };
            nanosleep(&ts, NULL);
        }
        
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
                wlr_log(WLR_ERROR, "relookup_window: failed to re-read /dev/winname (attempt %d/%d)",
                        attempt + 1, max_retries);
                continue;  /* retry */
            }
        }
        
        if (!draw->winname[0]) {
            wlr_log(WLR_ERROR, "relookup_window: no window name");
            return -1;
        }
        
        wlr_log(WLR_INFO, "Re-looking up window '%s' (screen_id=%d, attempt %d)",
                draw->winname, draw->screen_id, attempt + 1);
        
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
            wlr_log(WLR_ERROR, "relookup_window: 'n' command failed for '%s' (attempt %d/%d)",
                    draw->winname, attempt + 1, max_retries);
            continue;  /* retry with backoff */
        }
        p9_write(p9, draw->drawdata_fid, 0, vcmd, 1);
        
        /* Success — read ctl to get current geometry */
        goto lookup_ok;
    }
    
    wlr_log(WLR_ERROR, "relookup_window: all %d attempts failed", max_retries);
    return -1;
    
lookup_ok:;
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
    
    /* Inset by RIO_BORDER to preserve rio's border, then pad up */
    int vis_w = actual_width - 2 * RIO_BORDER;
    int vis_h = actual_height - 2 * RIO_BORDER;
    if (vis_w < MIN_ALIGNED_DIM) vis_w = MIN_ALIGNED_DIM;
    if (vis_h < MIN_ALIGNED_DIM) vis_h = MIN_ALIGNED_DIM;
    int new_width = TILE_ALIGN_UP(vis_w);
    int new_height = TILE_ALIGN_UP(vis_h);
    
    if (new_width <= 0 || new_height <= 0 || new_width > 4096 || new_height > 4096) {
        wlr_log(WLR_ERROR, "relookup_window: invalid dimensions %dx%d", new_width, new_height);
        return -1;
    }
    
    /* Content origin: inset from window edge */
    draw->win_minx = rminx + RIO_BORDER;
    draw->win_miny = rminy + RIO_BORDER;
    draw->visible_width = vis_w;
    draw->visible_height = vis_h;
    
    if (new_width != vis_w || new_height != vis_h) {
        wlr_log(WLR_INFO, "relookup_window: visible %dx%d, padded %dx%d, border=%d",
                vis_w, vis_h, new_width, new_height, RIO_BORDER);
    }
    
    /* Check if dimensions changed */
    if (new_width != draw->width || new_height != draw->height) {
        wlr_log(WLR_INFO, "relookup_window: size changed from %dx%d to %dx%d",
                draw->width, draw->height, new_width, new_height);
        
        /* Store new dimensions for main thread to handle */
        s->pending_width = new_width;
        s->pending_height = new_height;
        s->pending_visible_width = vis_w;
        s->pending_visible_height = vis_h;
        s->pending_minx = draw->win_minx;
        s->pending_miny = draw->win_miny;
        snprintf(s->pending_winname, sizeof(s->pending_winname), "%s", draw->winname);
        s->resize_pending = 1;
        
        wlr_log(WLR_INFO, "relookup_window: resize pending %dx%d -> %dx%d, main thread will handle",
                draw->width, draw->height, new_width, new_height);
    } else {
        /* Just position change, update now and force full redraw */
        wlr_log(WLR_INFO, "Window position updated: '%s' at (%d,%d) %dx%d",
                draw->winname, draw->win_minx, draw->win_miny, draw->width, draw->height);
        s->force_full_frame = 1;
        s->frame_dirty = 1;
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
    
    /* Inset by RIO_BORDER on each side to preserve rio's window border,
     * then pad up to tile-aligned dimensions for the internal buffer. */
    draw->visible_width = actual_width - 2 * RIO_BORDER;
    draw->visible_height = actual_height - 2 * RIO_BORDER;
    if (draw->visible_width < MIN_ALIGNED_DIM) draw->visible_width = MIN_ALIGNED_DIM;
    if (draw->visible_height < MIN_ALIGNED_DIM) draw->visible_height = MIN_ALIGNED_DIM;
    draw->width = TILE_ALIGN_UP(draw->visible_width);
    draw->height = TILE_ALIGN_UP(draw->visible_height);
    draw->win_minx = rminx + RIO_BORDER;
    draw->win_miny = rminy + RIO_BORDER;
    
    wlr_log(WLR_INFO, "Screen: (%d,%d)-(%d,%d) = %dx%d -> visible %dx%d, padded %dx%d",
            rminx, rminy, rmaxx, rmaxy, actual_width, actual_height,
            draw->visible_width, draw->visible_height,
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
                
                /* Inset by RIO_BORDER to preserve rio's window border */
                draw->visible_width = actual_width - 2 * RIO_BORDER;
                draw->visible_height = actual_height - 2 * RIO_BORDER;
                if (draw->visible_width < MIN_ALIGNED_DIM) draw->visible_width = MIN_ALIGNED_DIM;
                if (draw->visible_height < MIN_ALIGNED_DIM) draw->visible_height = MIN_ALIGNED_DIM;
                draw->width = TILE_ALIGN_UP(draw->visible_width);
                draw->height = TILE_ALIGN_UP(draw->visible_height);
                
                /* Content origin: inset from window edge */
                draw->win_minx = rminx + RIO_BORDER;
                draw->win_miny = rminy + RIO_BORDER;
                
                int pad_w = draw->width - draw->visible_width;
                int pad_h = draw->height - draw->visible_height;
                
                wlr_log(WLR_INFO, "Window: visible %dx%d, padded %dx%d (pad R=%d B=%d), border=%d",
                        draw->visible_width, draw->visible_height,
                        draw->width, draw->height,
                        pad_w, pad_h, RIO_BORDER);
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
    
    /* Compute logical dimensions for the source image.
     * Physical dimensions: draw->visible_width x draw->visible_height
     * Logical dimensions: physical / DRAW_SCALE, then padded to tile alignment
     * The 'a' command will scale from logical to physical.
     */
    int logical_width = (int)(draw->visible_width / DRAW_SCALE + 0.5f);
    int logical_height = (int)(draw->visible_height / DRAW_SCALE + 0.5f);
    
    /* Pad logical dimensions to tile alignment */
    logical_width = TILE_ALIGN_UP(logical_width);
    logical_height = TILE_ALIGN_UP(logical_height);
    if (logical_width < MIN_ALIGNED_DIM) logical_width = MIN_ALIGNED_DIM;
    if (logical_height < MIN_ALIGNED_DIM) logical_height = MIN_ALIGNED_DIM;
    
    if (DRAW_SCALE != 1.0f) {
        wlr_log(WLR_INFO, "Fractional scaling: physical %dx%d -> logical %dx%d (scale=%.2f)",
                draw->width, draw->height, logical_width, logical_height, DRAW_SCALE);
    }
    
    /* Store logical dimensions for use by send.c */
    draw->logical_width = logical_width;
    draw->logical_height = logical_height;
    draw->scale = DRAW_SCALE;
    
    /* 'b' command: allocate image at LOGICAL resolution */
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
    PUT32(bcmd + off, logical_width); off += 4;   /* width at logical resolution */
    PUT32(bcmd + off, logical_height); off += 4;  /* height at logical resolution */
    PUT32(bcmd + off, 0); off += 4;
    PUT32(bcmd + off, 0); off += 4;
    PUT32(bcmd + off, logical_width); off += 4;
    PUT32(bcmd + off, logical_height); off += 4;
    PUT32(bcmd + off, 0xFF000000); off += 4;      /* color = black */
    
    wlr_log(WLR_DEBUG, "Allocating image: 'b' cmd size=%d", off);
    
    int written = p9_write(p9, draw->drawdata_fid, 0, bcmd, off);
    if (written < 0) {
        wlr_log(WLR_ERROR, "Failed to allocate framebuffer image");
        return -1;
    }
    wlr_log(WLR_INFO, "Allocated framebuffer image %d (%dx%d logical, %.2fx scale) format=XRGB32", 
            draw->image_id, logical_width, logical_height, DRAW_SCALE);
    
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
