/*
 * compress.h - Tile compression for Plan 9 draw protocol
 */

#ifndef P9WL_COMPRESS_H
#define P9WL_COMPRESS_H

#include <stdint.h>
#include "../types.h"

/* Overhead for alpha-delta compositing (extra 'd' command) */
#define ALPHA_DELTA_OVERHEAD 45

/*
 * Compress tile data using LZ77-style row matching.
 * Returns compressed size, or 0 if compression didn't save enough.
 */
int compress_tile_data(uint8_t *dst, int dst_max, 
                       uint8_t *raw, int bytes_per_row, int h);

/*
 * Compress a tile from a framebuffer (direct, no delta).
 * Copies tile to contiguous buffer then compresses.
 * Returns compressed size, or 0 if failed.
 */
int compress_tile_direct(uint8_t *dst, int dst_max, 
                         uint32_t *pixels, int stride, 
                         int x1, int y1, int w, int h);

/*
 * Compress a tile using alpha-delta encoding.
 * Changed pixels get alpha=0xFF, unchanged get alpha=0x00.
 * Returns compressed size, or 0 if no changes or too many changes.
 */
int compress_tile_alpha_delta(uint8_t *dst, int dst_max,
                              uint32_t *pixels, int stride,
                              uint32_t *prev_pixels, int prev_stride,
                              int x1, int y1, int w, int h);

/*
 * Adaptive compression: tries both direct and alpha-delta.
 * Returns:
 *   > 0: alpha-delta size (use delta path)
 *   < 0: negated direct size (use direct path)  
 *   = 0: compression failed, use uncompressed
 */
int compress_tile_adaptive(uint8_t *dst, int dst_max,
                           uint32_t *pixels, int stride,
                           uint32_t *prev_pixels, int prev_stride,
                           int x1, int y1, int w, int h);

#endif /* P9WL_COMPRESS_H */
