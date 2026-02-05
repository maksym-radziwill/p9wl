/*
 * draw.c - /dev/draw initialization and rio window management.
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
#define RIO_BORDER 4

struct geom {
	int width, height;       /* aligned */
	int minx, miny;          /* centered content origin */
	int rminx, rminy;        /* actual window rect */
	int rmaxx, rmaxy;
};

/* Round down to tile boundary, clamp to [MIN_ALIGNED_DIM, actual]. */
static int align_dim(int actual) {
	int a = TILE_ALIGN_DOWN(actual);
	if (a < MIN_ALIGNED_DIM) a = MIN_ALIGNED_DIM;
	if (a > actual) a = actual;
	return a;
}

/* Parse ctl buffer into aligned/centered geometry. */
static int parse_ctl(const uint8_t *buf, int n, struct geom *g) {
	if (n < 12 * 12) return -1;

	g->rminx = atoi((char*)buf + 4*12);
	g->rminy = atoi((char*)buf + 5*12);
	g->rmaxx = atoi((char*)buf + 6*12);
	g->rmaxy = atoi((char*)buf + 7*12);

	int aw = g->rmaxx - g->rminx;
	int ah = g->rmaxy - g->rminy;
	g->width = align_dim(aw);
	g->height = align_dim(ah);

	if (g->width <= 0 || g->height <= 0 || g->width > 4096 || g->height > 4096)
		return -1;

	int ex = aw - g->width; if (ex < 0) ex = 0;
	int ey = ah - g->height; if (ey < 0) ey = 0;
	g->minx = g->rminx + ex / 2;
	g->miny = g->rminy + ey / 2;
	return 0;
}

/*
 * Resize rio window to aligned dims + border via /dev/wctl.
 * The 2*RIO_BORDER padding keeps the yellow border visible;
 * centering in parse_ctl naturally offsets content inward.
 */
static int resize_wctl(struct p9conn *p9, int minx, int miny, int w, int h) {
	uint32_t fid = p9->next_fid++;
	const char *wnames[1] = { "wctl" };

	if (p9_walk(p9, p9->root_fid, fid, 1, wnames) < 0) return -1;
	if (p9_open(p9, fid, OWRITE, NULL) < 0) return -1;

	char cmd[128];
	int len = snprintf(cmd, sizeof(cmd), "resize -r %d %d %d %d",
	                   minx, miny, minx + w + 2*RIO_BORDER, miny + h + 2*RIO_BORDER);

	wlr_log(WLR_INFO, "wctl resize: %dx%d+%d border at (%d,%d)", w, h, RIO_BORDER, minx, miny);
	if (p9_write(p9, fid, 0, (uint8_t*)cmd, len) < 0) return -1;
	return 0;
}

/*
 * Read ctl geometry; resize to aligned+border if needed, then re-read.
 */
static int read_and_resize(struct draw_state *draw, struct geom *g) {
	struct p9conn *p9 = draw->p9;
	uint8_t ctlbuf[256];
	uint8_t cmd[128];
	int n, off;

	n = p9_read(p9, draw->drawctl_fid, 0, sizeof(ctlbuf) - 1, ctlbuf);
	if (parse_ctl(ctlbuf, n, g) < 0) return -1;

	int aw = g->rmaxx - g->rminx;
	int ah = g->rmaxy - g->rminy;
	if (aw != g->width + 2*RIO_BORDER || ah != g->height + 2*RIO_BORDER) {
		if (resize_wctl(p9, g->rminx, g->rminy, g->width, g->height) == 0) {
			off = flush_cmd(cmd);
			p9_write(p9, draw->drawdata_fid, 0, cmd, off);
			n = p9_read(p9, draw->drawctl_fid, 0, sizeof(ctlbuf) - 1, ctlbuf);
			parse_ctl(ctlbuf, n, g);
		}
	}
	return 0;
}

static void store_geom(struct draw_state *draw, const struct geom *g) {
	draw->width = g->width;
	draw->height = g->height;
	draw->win_minx = g->minx;
	draw->win_miny = g->miny;
	draw->actual_minx = g->rminx;
	draw->actual_miny = g->rminy;
	draw->actual_maxx = g->rmaxx;
	draw->actual_maxy = g->rmaxy;
}

static int read_winname(struct draw_state *draw, char *buf, int bufsz) {
	int n = p9_read(draw->p9, draw->winname_fid, 0, bufsz - 1, (uint8_t*)buf);
	if (n <= 0) return -1;
	buf[n] = '\0';
	if (buf[n-1] == '\n') buf[n-1] = '\0';
	return 0;
}

int relookup_window(struct server *s) {
	struct draw_state *draw = &s->draw;
	struct p9conn *p9 = draw->p9;
	uint8_t cmd[128];
	int off;

	if (draw->winname_fid) {
		char newname[64] = {0};
		if (read_winname(draw, newname, sizeof(newname)) < 0) {
			wlr_log(WLR_ERROR, "relookup: can't read /dev/winname");
			return -1;
		}
		if (strcmp(newname, draw->winname) != 0)
			wlr_log(WLR_INFO, "winname: '%s' -> '%s'", draw->winname, newname);
		snprintf(draw->winname, sizeof(draw->winname), "%s", newname);
	}

	if (!draw->winname[0]) {
		wlr_log(WLR_ERROR, "relookup: no window name");
		return -1;
	}

	/* Free old, re-lookup by name */
	off = free_image_cmd(cmd, draw->screen_id);
	p9_write(p9, draw->drawdata_fid, 0, cmd, off);
	off = flush_cmd(cmd);
	p9_write(p9, draw->drawdata_fid, 0, cmd, off);

	off = name_cmd(cmd, draw->screen_id, draw->winname, strlen(draw->winname));
	if (p9_write(p9, draw->drawdata_fid, 0, cmd, off) < 0) {
		wlr_log(WLR_ERROR, "relookup: 'n' command failed");
		return -1;
	}
	off = flush_cmd(cmd);
	p9_write(p9, draw->drawdata_fid, 0, cmd, off);

	struct geom g;
	if (read_and_resize(draw, &g) < 0) {
		wlr_log(WLR_ERROR, "relookup: invalid geometry");
		return -1;
	}

	int old_w = draw->width, old_h = draw->height;
	store_geom(draw, &g);

	if (g.width != old_w || g.height != old_h) {
		wlr_log(WLR_INFO, "relookup: resize %dx%d -> %dx%d",
		        old_w, old_h, g.width, g.height);
		draw->xor_enabled = 0;
		s->pending_width = g.width;
		s->pending_height = g.height;
		s->pending_minx = g.minx;
		s->pending_miny = g.miny;
		snprintf(s->pending_winname, sizeof(s->pending_winname), "%s", draw->winname);
		s->resize_pending = 1;
	} else {
		s->force_full_frame = 1;
		s->frame_dirty = 1;
	}
	return 0;
}

void delete_rio_window(struct p9conn *p9) {
	uint32_t fid = p9->next_fid++;
	const char *wnames[1] = { "wctl" };

	if (p9_walk(p9, p9->root_fid, fid, 1, wnames) < 0) return;
	if (p9_open(p9, fid, OWRITE, NULL) < 0) return;
	p9_write(p9, fid, 0, (uint8_t*)"delete", 6);
}

int init_draw(struct server *s) {
	struct p9conn *p9 = &s->p9_draw;
	struct draw_state *draw = &s->draw;
	const char *wnames[3];
	uint8_t cmd[64];
	int off;

	draw->p9 = p9;
	draw->draw_fid = p9->next_fid++;
	draw->drawnew_fid = p9->next_fid++;
	draw->drawdata_fid = p9->next_fid++;
	draw->win_minx = 0;
	draw->win_miny = 0;

	/* Open /dev/draw/new */
	wnames[0] = "draw";
	if (p9_walk(p9, p9->root_fid, draw->draw_fid, 1, wnames) < 0) return -1;
	wnames[0] = "new";
	if (p9_walk(p9, draw->draw_fid, draw->drawnew_fid, 1, wnames) < 0) return -1;
	if (p9_open(p9, draw->drawnew_fid, ORDWR, NULL) < 0) return -1;

	uint8_t buf[256];
	int n = p9_read(p9, draw->drawnew_fid, 0, sizeof(buf) - 1, buf);
	if (n < 12*12) {
		wlr_log(WLR_ERROR, "draw/new: short read (%d bytes)", n);
		return -1;
	}
	buf[n] = '\0';

	draw->client_id = atoi((char*)buf);
	wlr_log(WLR_INFO, "draw client %d", draw->client_id);

	/* Initial geometry from draw/new */
	draw->width = align_dim(atoi((char*)buf + 6*12) - atoi((char*)buf + 4*12));
	draw->height = align_dim(atoi((char*)buf + 7*12) - atoi((char*)buf + 5*12));

	/* Open data and ctl */
	char idstr[32];
	snprintf(idstr, sizeof(idstr), "%d", draw->client_id);

	wnames[0] = idstr;
	wnames[1] = "data";
	if (p9_walk(p9, draw->draw_fid, draw->drawdata_fid, 2, wnames) < 0) return -1;
	uint32_t iounit = 0;
	if (p9_open(p9, draw->drawdata_fid, ORDWR, &iounit) < 0) return -1;
	draw->iounit = iounit ? iounit : (p9->msize - 24);

	draw->drawctl_fid = p9->next_fid++;
	wnames[1] = "ctl";
	if (p9_walk(p9, draw->draw_fid, draw->drawctl_fid, 2, wnames) < 0) return -1;
	if (p9_open(p9, draw->drawctl_fid, OREAD, NULL) < 0) return -1;

	/* Try /dev/winname */
	draw->winname_fid = p9->next_fid++;
	wnames[0] = "winname";
	if (p9_walk(p9, p9->root_fid, draw->winname_fid, 1, wnames) >= 0 &&
	    p9_open(p9, draw->winname_fid, OREAD, NULL) >= 0) {
		char winname[64] = {0};
		if (read_winname(draw, winname, sizeof(winname)) == 0) {
			snprintf(draw->winname, sizeof(draw->winname), "%s", winname);
			wlr_log(WLR_INFO, "winname: '%s'", draw->winname);
		}
	}

	/* Look up window by name */
	uint32_t screen_image_id = 0;
	if (draw->winname[0]) {
		screen_image_id = 10;
		off = name_cmd(cmd, screen_image_id, draw->winname, strlen(draw->winname));
		if (p9_write(p9, draw->drawdata_fid, 0, cmd, off) < 0) {
			wlr_log(WLR_ERROR, "window lookup '%s' failed", draw->winname);
			screen_image_id = 0;
		} else {
			draw->winimage_id = screen_image_id;
			off = flush_cmd(cmd);
			p9_write(p9, draw->drawdata_fid, 0, cmd, off);

			struct geom g;
			if (read_and_resize(draw, &g) == 0) {
				store_geom(draw, &g);
				wlr_log(WLR_INFO, "window (%d,%d)-(%d,%d) -> %dx%d",
				        g.rminx, g.rminy, g.rmaxx, g.rmaxy, g.width, g.height);
			}
		}
	}
	draw->screen_id = screen_image_id;

	/* Allocate images */
	draw->image_id = 1;
	off = alloc_image_cmd(cmd, draw->image_id, CHAN_XRGB32, 0,
	                      0, 0, draw->width, draw->height, 0xFF000000);
	if (p9_write(p9, draw->drawdata_fid, 0, cmd, off) < 0) return -1;

	draw->opaque_id = 2;
	off = alloc_image_cmd(cmd, draw->opaque_id, CHAN_GREY1, 1,
	                      0, 0, 1, 1, 0xFFFFFFFF);
	if (p9_write(p9, draw->drawdata_fid, 0, cmd, off) < 0) return -1;

	draw->delta_id = 5;
	off = alloc_image_cmd(cmd, draw->delta_id, CHAN_ARGB32, 0,
	                      0, 0, draw->width, draw->height, 0x00000000);
	if (p9_write(p9, draw->drawdata_fid, 0, cmd, off) < 0) return -1;

	draw->xor_enabled = 0;
	return 0;
}
