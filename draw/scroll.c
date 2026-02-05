/*
 * scroll.c - Scroll detection and 9P scroll commands
 */

#define _POSIX_C_SOURCE 200809L
#include <string.h>
#include <stdlib.h>
#include <wlr/util/log.h>

#include "scroll.h"
#include "send.h"
#include "compress.h"
#include "parallel.h"
#include "phase_correlate.h"
#include "draw/draw_helpers.h"
#include "types.h"
#include "p9/p9.h"
#include "draw/draw.h"

static struct {
    struct server *s;
    uint32_t *send_buf;
} scroll_ctx;

static void extract_tile(uint32_t *dst, const uint32_t *src, int stride,
                         int x, int y, int w, int h) {
    for (int row = 0; row < h; row++)
        memcpy(&dst[row * TILE_SIZE], &src[(y + row) * stride + x], w * 4);
}

static void detect_region_scroll(void *ctx, int idx) {
    (void)ctx;
    struct server *s = scroll_ctx.s;
    uint32_t *send_buf = scroll_ctx.send_buf;
    uint32_t *prev_buf = s->prev_framebuf;
    int width = s->width;

    int rx1 = s->scroll_regions[idx].x1;
    int ry1 = s->scroll_regions[idx].y1;
    int rx2 = s->scroll_regions[idx].x2;
    int ry2 = s->scroll_regions[idx].y2;

    s->scroll_regions[idx].detected = 0;
    s->scroll_regions[idx].dx = 0;
    s->scroll_regions[idx].dy = 0;

    int max_scroll = ((rx2 - rx1) < (ry2 - ry1) ? (rx2 - rx1) : (ry2 - ry1)) / 2;

    struct phase_result result = phase_correlate_detect(
        send_buf, prev_buf, width, rx1, ry1, rx2, ry2, max_scroll);

    int dx = result.dx, dy = result.dy;
    if (dx == 0 && dy == 0)
        return;

    int abs_dx = dx < 0 ? -dx : dx;
    int abs_dy = dy < 0 ? -dy : dy;
    if (abs_dx >= (rx2 - rx1) / 2 || abs_dy >= (ry2 - ry1) / 2)
        return;

    wlr_log(WLR_INFO, "Region %d: FFT detected scroll dx=%d dy=%d", idx, dx, dy);

    struct scroll_rects rects;
    compute_scroll_rects(rx1, ry1, rx2, ry2, dx, dy, &rects);
    if (!rects.valid)
        return;

    /* Compare compression cost with vs without scroll */
    int bytes_no_scroll = 0, bytes_with_scroll = 0;
    uint8_t comp_buf[TILE_SIZE * TILE_SIZE * 4 + 256];

    int tx1 = rx1 / TILE_SIZE, ty1 = ry1 / TILE_SIZE;
    int tx2 = rx2 / TILE_SIZE, ty2 = ry2 / TILE_SIZE;

    for (int ty = ty1; ty < ty2; ty++) {
        for (int tx = tx1; tx < tx2; tx++) {
            int x1, y1, w, h;
            tile_bounds(tx, ty, s->width, s->height, &x1, &y1, &w, &h);
            if (w != TILE_SIZE || h != TILE_SIZE)
                continue;

            /* Cost without scroll */
            if (!tile_changed(send_buf, prev_buf, width, x1, y1, w, h))
                continue; /* identical - no cost either way */

            int size = compress_tile_adaptive(comp_buf, sizeof(comp_buf),
                                              send_buf, width, prev_buf, width,
                                              x1, y1, w, h);
            if (size < 0) size = -size;
            if (size == 0) size = w * h * 4;
            bytes_no_scroll += size;

            /* Cost with scroll */
            int src_x = x1 - dx, src_y = y1 - dy;
            int in_exposed = (dy != 0 && y1 >= rects.exp_y1 && y1 < rects.exp_y2) ||
                             (dx != 0 && x1 >= rects.exp_x1 && x1 < rects.exp_x2);

            if (!in_exposed && src_x >= 0 && src_y >= 0 &&
                src_x + w <= s->width && src_y + h <= s->height) {
                /* Check if tile matches shifted position */
                int match = 1;
                for (int row = 0; row < h && match; row++) {
                    if (memcmp(&send_buf[(y1 + row) * width + x1],
                               &prev_buf[(src_y + row) * width + src_x], w * 4))
                        match = 0;
                }
                if (match)
                    continue; /* identical after scroll - no cost */

                uint32_t curr[TILE_SIZE * TILE_SIZE], shifted[TILE_SIZE * TILE_SIZE];
                extract_tile(curr, send_buf, width, x1, y1, w, h);
                extract_tile(shifted, prev_buf, width, src_x, src_y, w, h);

                size = compress_tile_adaptive(comp_buf, sizeof(comp_buf),
                                              curr, TILE_SIZE, shifted, TILE_SIZE,
                                              0, 0, w, h);
            } else {
                uint32_t curr[TILE_SIZE * TILE_SIZE], prev[TILE_SIZE * TILE_SIZE];
                extract_tile(curr, send_buf, width, x1, y1, w, h);
                extract_tile(prev, prev_buf, width, x1, y1, w, h);

                size = compress_tile_adaptive(comp_buf, sizeof(comp_buf),
                                              curr, TILE_SIZE, prev, TILE_SIZE,
                                              0, 0, w, h);
            }
            if (size < 0) size = -size;
            if (size == 0) size = w * h * 4;
            bytes_with_scroll += size;
        }
    }

    if (bytes_no_scroll == 0 || bytes_with_scroll >= bytes_no_scroll) {
        wlr_log(WLR_INFO, "Region %d: scroll rejected (no benefit)", idx);
        return;
    }

    s->scroll_regions[idx].detected = 1;
    s->scroll_regions[idx].dx = dx;
    s->scroll_regions[idx].dy = dy;

    int saved = bytes_no_scroll - bytes_with_scroll;
    wlr_log(WLR_INFO, "Region %d: scroll accepted, saves %d bytes (%d%%)",
            idx, saved, saved * 100 / bytes_no_scroll);
}

void detect_scroll(struct server *s, uint32_t *send_buf) {
    if (!send_buf || !s->prev_framebuf)
        return;

    int margin = TILE_SIZE;
    int cols = s->width / 256;
    int rows = s->height / 256;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;

    int cell_w = ((s->width - 2 * margin) / cols / TILE_SIZE) * TILE_SIZE;
    int cell_h = ((s->height - 2 * margin) / rows / TILE_SIZE) * TILE_SIZE;
    if (cell_w < TILE_SIZE) cell_w = TILE_SIZE;
    if (cell_h < TILE_SIZE) cell_h = TILE_SIZE;

    s->scroll_regions_x = cols;
    s->scroll_regions_y = rows;
    s->num_scroll_regions = 0;

    int max_x = (s->width / TILE_SIZE) * TILE_SIZE;
    int max_y = (s->height / TILE_SIZE) * TILE_SIZE;

    for (int ry = 0; ry < rows; ry++) {
        for (int rx = 0; rx < cols; rx++) {
            int x1 = (margin + rx * cell_w) / TILE_SIZE * TILE_SIZE;
            int y1 = (margin + ry * cell_h) / TILE_SIZE * TILE_SIZE;
            int x2 = (rx == cols - 1)
                ? ((s->width - margin) / TILE_SIZE) * TILE_SIZE
                : ((x1 + cell_w + TILE_SIZE - 1) / TILE_SIZE) * TILE_SIZE;
            int y2 = (ry == rows - 1)
                ? ((s->height - margin) / TILE_SIZE) * TILE_SIZE
                : ((y1 + cell_h + TILE_SIZE - 1) / TILE_SIZE) * TILE_SIZE;

            if (x2 > max_x) x2 = max_x;
            if (y2 > max_y) y2 = max_y;
            if (x2 - x1 < 64 || y2 - y1 < 64)
                continue;

            int idx = s->num_scroll_regions++;
            s->scroll_regions[idx].x1 = x1;
            s->scroll_regions[idx].y1 = y1;
            s->scroll_regions[idx].x2 = x2;
            s->scroll_regions[idx].y2 = y2;
            s->scroll_regions[idx].detected = 0;
        }
    }

    if (s->num_scroll_regions > 0) {
        scroll_ctx.s = s;
        scroll_ctx.send_buf = send_buf;
        parallel_for(s->num_scroll_regions, detect_region_scroll, NULL);
    }

    int detected = 0;
    for (int i = 0; i < s->num_scroll_regions; i++)
        if (s->scroll_regions[i].detected)
            detected++;

    if (detected > 0)
        wlr_log(WLR_INFO, "Scroll detected in %d/%d regions",
                detected, s->num_scroll_regions);
}

int apply_scroll_to_prevbuf(struct server *s) {
    int count = 0;

    for (int i = 0; i < s->num_scroll_regions; i++) {
        if (!s->scroll_regions[i].detected)
            continue;

        int rx1 = s->scroll_regions[i].x1;
        int ry1 = s->scroll_regions[i].y1;
        int rx2 = s->scroll_regions[i].x2;
        int ry2 = s->scroll_regions[i].y2;
        int dx = s->scroll_regions[i].dx;
        int dy = s->scroll_regions[i].dy;

        struct scroll_rects r;
        compute_scroll_rects(rx1, ry1, rx2, ry2, dx, dy, &r);
        if (!r.valid)
            continue;

        int copy_w = r.dst_x2 - r.dst_x1;
        int abs_dy = dy < 0 ? -dy : dy;

        /* Shift pixels in prev_framebuf */
        if (dy < 0) {
            for (int y = r.dst_y1; y < r.dst_y2; y++)
                memmove(&s->prev_framebuf[y * s->width + r.dst_x1],
                        &s->prev_framebuf[(y + abs_dy) * s->width + r.src_x1],
                        copy_w * sizeof(uint32_t));
        } else if (dy > 0) {
            for (int y = r.dst_y2 - 1; y >= r.dst_y1; y--)
                memmove(&s->prev_framebuf[y * s->width + r.dst_x1],
                        &s->prev_framebuf[(y - abs_dy) * s->width + r.src_x1],
                        copy_w * sizeof(uint32_t));
        } else if (dx != 0) {
            for (int y = r.dst_y1; y < r.dst_y2; y++)
                memmove(&s->prev_framebuf[y * s->width + r.dst_x1],
                        &s->prev_framebuf[y * s->width + r.src_x1],
                        copy_w * sizeof(uint32_t));
        }

        /* Mark exposed regions */
        if (r.exp_y2 > r.exp_y1) {
            for (int y = r.exp_y1; y < r.exp_y2; y++)
                for (int x = rx1; x < rx2; x++)
                    s->prev_framebuf[y * s->width + x] = 0xDEADBEEF;
        }
        if (r.exp_x2 > r.exp_x1) {
            for (int y = ry1; y < ry2; y++)
                for (int x = r.exp_x1; x < r.exp_x2; x++)
                    s->prev_framebuf[y * s->width + x] = 0xDEADBEEF;
        }

        count++;
    }

    return count;
}

int write_scroll_commands(struct server *s, uint8_t *batch, size_t max_size) {
    struct draw_state *draw = &s->draw;
    int off = 0;

    for (int i = 0; i < s->num_scroll_regions; i++) {
        if (!s->scroll_regions[i].detected)
            continue;

        struct scroll_rects r;
        compute_scroll_rects(s->scroll_regions[i].x1, s->scroll_regions[i].y1,
                             s->scroll_regions[i].x2, s->scroll_regions[i].y2,
                             s->scroll_regions[i].dx, s->scroll_regions[i].dy, &r);

        if (!r.valid || r.src_y2 <= r.src_y1 || r.dst_y2 <= r.dst_y1 ||
            r.src_x2 <= r.src_x1 || r.dst_x2 <= r.dst_x1)
            continue;

        if (off + 45 > (int)max_size) {
            wlr_log(WLR_ERROR, "Scroll batch overflow");
            break;
        }

        off += cmd_copy(batch + off, draw->image_id, draw->image_id,
                        draw->opaque_id,
                        r.dst_x1, r.dst_y1, r.dst_x2, r.dst_y2,
                        r.src_x1, r.src_y1);

        wlr_log(WLR_DEBUG, "Scroll %d: dy=%d dx=%d", i,
                s->scroll_regions[i].dy, s->scroll_regions[i].dx);
    }

    return off;
}

void scroll_cleanup(void) {
    parallel_cleanup();
    phase_correlate_cleanup();
}
