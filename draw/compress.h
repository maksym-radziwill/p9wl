/*
 * compress.h - Tile compression for Plan 9 draw protocol
 */

#ifndef COMPRESS_H
#define COMPRESS_H

#include <stdint.h>

#ifndef TILE_SIZE
#define TILE_SIZE 16
#endif

/* Overhead for alpha-delta composite command */
#define ALPHA_DELTA_OVERHEAD 45

/* Per-tile compression result */
struct tile_result {
    uint8_t data[TILE_SIZE * TILE_SIZE * 4 + 256];
    int size;       /* compressed size, 0 if failed */
    int is_delta;   /* 1 if delta compression, 0 if direct */
};

/* Work item for parallel compression */
struct tile_work {
    uint32_t *pixels;
    int stride;
    uint32_t *prev_pixels;
    int prev_stride;
    int x1, y1, w, h;
    struct tile_result *result;
};

/* Core compression functions */
int compress_tile_data(uint8_t *dst, int dst_max, 
                       uint8_t *raw, int bytes_per_row, int h);

int compress_tile_direct(uint8_t *dst, int dst_max, 
                         uint32_t *pixels, int stride, 
                         int x1, int y1, int w, int h);

int compress_tile_alpha_delta(uint8_t *dst, int dst_max,
                              uint32_t *pixels, int stride,
                              uint32_t *prev_pixels, int prev_stride,
                              int x1, int y1, int w, int h);

int compress_tile_adaptive(uint8_t *dst, int dst_max,
                           uint32_t *pixels, int stride,
                           uint32_t *prev_pixels, int prev_stride,
                           int x1, int y1, int w, int h);

/* Thread pool API */
int compress_pool_init(int nthreads);
void compress_pool_shutdown(void);
int compress_tiles_parallel(struct tile_work *tiles, struct tile_result *results, int count);

#endif /* COMPRESS_H */
