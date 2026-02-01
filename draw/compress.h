/*
 * compress.h - Tile compression for Plan 9 draw protocol
 *
 * Provides LZ77-style compression for tiles sent to the Plan 9 draw
 * device. Supports direct XRGB32 compression and alpha-delta encoding
 * for efficient sparse updates.
 *
 * Compression Methods:
 *
 *   Direct (XRGB32):
 *     Compresses tile pixels directly using LZ77 row matching.
 *     Best for tiles with significant content changes.
 *
 *   Alpha-Delta (ARGB32):
 *     Creates an alpha-channel delta between current and previous frame.
 *     Unchanged pixels have alpha=0, changed pixels have alpha=FF.
 *     Best for sparse updates (cursors, small UI changes).
 *     Requires additional 45-byte composite command (ALPHA_DELTA_OVERHEAD).
 *
 *   Solid Color:
 *     Special fast path for tiles with a single color (including black).
 *     Uses one literal pixel plus row-repeat back-references.
 *
 * Adaptive Selection:
 *
 *   compress_tile_adaptive() tries both methods and picks the smaller:
 *     - Returns positive size if delta encoding is smaller
 *     - Returns negative size if direct encoding is smaller
 *     - Returns 0 if neither achieves 25% compression
 *
 * LZ77 Implementation:
 *
 *   Uses hash table for O(1) match finding:
 *     - HASH_SIZE = 1024 entries
 *     - Matches 3+ bytes against hash and previous row
 *     - Back-reference: (len-3) << 2 | (offset-1) >> 8, (offset-1) & 0xFF
 *     - Literal: 0x80 | (count-1), followed by bytes
 *
 *   Fast path: entire row matching previous row uses pre-computed
 *   back-references without per-byte scanning.
 *
 * Thread Pool:
 *
 *   compress_tiles_parallel() uses the parallel_for() infrastructure
 *   to compress multiple tiles concurrently. No explicit pool init
 *   or shutdown is needed - parallel_for handles thread management.
 */

#ifndef COMPRESS_H
#define COMPRESS_H

#include <stdint.h>

#ifndef TILE_SIZE
#define TILE_SIZE 16
#endif

/* Overhead for alpha-delta composite command (Plan 9 'd' command) */
#define ALPHA_DELTA_OVERHEAD 45

/* ============== Data Structures ============== */

/*
 * Per-tile compression result.
 *
 * data:     compressed data buffer (sized for worst case)
 * size:     compressed size in bytes, 0 if compression failed
 * is_delta: 1 if alpha-delta encoding, 0 if direct XRGB32
 */
struct tile_result {
    uint8_t data[TILE_SIZE * TILE_SIZE * 4 + 256];
    int size;
    int is_delta;
};

/*
 * Work item for parallel compression.
 *
 * pixels:      current frame pixel buffer (XRGB32)
 * stride:      current frame stride in pixels
 * prev_pixels: previous frame pixel buffer (XRGB32), may be NULL
 * prev_stride: previous frame stride in pixels
 * x1, y1:      top-left corner of tile in pixel coordinates
 * w, h:        tile dimensions (may be < TILE_SIZE at edges)
 * result:      output - compression result written here
 */
struct tile_work {
    uint32_t *pixels;
    int stride;
    uint32_t *prev_pixels;
    int prev_stride;
    int x1, y1, w, h;
    struct tile_result *result;
};

/* ============== Core Compression Functions ============== */

/*
 * Compress raw tile data using LZ77.
 *
 * dst:           output buffer for compressed data
 * dst_max:       maximum bytes to write
 * raw:           raw pixel data (row-major, XRGB32/ARGB32)
 * bytes_per_row: bytes per row in raw buffer
 * h:             number of rows
 *
 * Returns compressed size, or 0 if compression failed or didn't
 * achieve at least 25% reduction.
 */
int compress_tile_data(uint8_t *dst, int dst_max, 
                       uint8_t *raw, int bytes_per_row, int h);

/*
 * Compress a tile using direct XRGB32 encoding.
 *
 * Extracts tile pixels from frame buffer and compresses.
 *
 * dst:     output buffer
 * dst_max: maximum bytes to write
 * pixels:  frame buffer (XRGB32)
 * stride:  frame buffer stride in pixels
 * x1, y1:  tile top-left corner
 * w, h:    tile dimensions (must be <= TILE_SIZE)
 *
 * Returns compressed size, or 0 if compression failed.
 */
int compress_tile_direct(uint8_t *dst, int dst_max, 
                         uint32_t *pixels, int stride, 
                         int x1, int y1, int w, int h);

/*
 * Compress a tile using alpha-delta encoding.
 *
 * Creates ARGB32 delta: unchanged pixels have alpha=0 (transparent),
 * changed pixels have alpha=FF with new color.
 *
 * dst:         output buffer
 * dst_max:     maximum bytes to write
 * pixels:      current frame buffer (XRGB32)
 * stride:      current frame stride in pixels
 * prev_pixels: previous frame buffer (XRGB32), must not be NULL
 * prev_stride: previous frame stride in pixels
 * x1, y1:      tile top-left corner
 * w, h:        tile dimensions (must be <= TILE_SIZE)
 *
 * Returns compressed size, or 0 if:
 *   - No pixels changed (delta is empty)
 *   - More than 75% of pixels changed (direct is better)
 *   - Compression failed
 */
int compress_tile_alpha_delta(uint8_t *dst, int dst_max,
                              uint32_t *pixels, int stride,
                              uint32_t *prev_pixels, int prev_stride,
                              int x1, int y1, int w, int h);

/*
 * Adaptively compress a tile using best method.
 *
 * Tries both direct and alpha-delta compression, returns the smaller.
 * Accounts for ALPHA_DELTA_OVERHEAD when comparing.
 *
 * dst:         output buffer
 * dst_max:     maximum bytes to write
 * pixels:      current frame buffer (XRGB32)
 * stride:      current frame stride in pixels
 * prev_pixels: previous frame buffer (may be NULL for direct-only)
 * prev_stride: previous frame stride in pixels
 * x1, y1:      tile top-left corner
 * w, h:        tile dimensions
 *
 * Returns:
 *   > 0: delta encoding size (copy dst, use alpha composite)
 *   < 0: negated direct encoding size (copy dst, use direct draw)
 *   = 0: compression failed or not worthwhile
 */
int compress_tile_adaptive(uint8_t *dst, int dst_max,
                           uint32_t *pixels, int stride,
                           uint32_t *prev_pixels, int prev_stride,
                           int x1, int y1, int w, int h);

/* ============== Parallel Compression API ============== */

/*
 * Initialize compression thread pool.
 *
 * Currently a no-op - parallel_for() handles thread management.
 * Provided for API consistency.
 *
 * nthreads: ignored
 *
 * Returns 0 always.
 */
int compress_pool_init(int nthreads);

/*
 * Shutdown compression thread pool.
 *
 * Currently a no-op - call parallel_cleanup() at program exit instead.
 */
void compress_pool_shutdown(void);

/*
 * Compress multiple tiles in parallel.
 *
 * Uses parallel_for() to distribute work across thread pool.
 * Results are written to corresponding tile_result structs.
 *
 * tiles:   array of tile work items
 * results: array of result structs (same length as tiles)
 * count:   number of tiles to compress
 *
 * Returns 0 on success, -1 if count <= 0.
 */
int compress_tiles_parallel(struct tile_work *tiles, 
                            struct tile_result *results, 
                            int count);

#endif /* COMPRESS_H */
