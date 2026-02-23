/*
 * compress.h - Tile compression for Plan 9 draw protocol
 *
 * Provides LZ77-style compression for tiles sent to the Plan 9 draw
 * device. Supports direct XRGB32 compression and alpha-delta encoding
 * for efficient sparse updates.
 *
 * Architecture Overview:
 *
 *   compress_tile_adaptive() selects between two encoding paths:
 *
 *     Direct Path:
 *       raw pixels → LZ77 compress → output
 *
 *     Alpha-Delta Path:
 *       raw pixels + prev frame → delta buffer → LZ77 compress → output
 *
 *   Both paths feed into the same LZ77 compression stage. The adaptive
 *   function picks whichever produces smaller output (accounting for
 *   the 45-byte ALPHA_DELTA_OVERHEAD of the composite command).
 *
 * Encoding Paths:
 *
 *   Direct (XRGB32):
 *     Extracts tile pixels and compresses them directly with LZ77.
 *     Best for tiles with significant content changes.
 *
 *   Alpha-Delta (ARGB32):
 *     Preprocessing step that creates a sparse delta buffer:
 *       - Unchanged pixels → 0x00000000 (transparent)
 *       - Changed pixels   → 0xFF000000 | RGB (opaque)
 *     The delta buffer is then compressed with LZ77. Sparse updates
 *     produce long zero runs that compress extremely well.
 *     Best for small changes (cursors, blinking carets, UI highlights).
 *     Requires additional 45-byte composite command on the server side.
 *
 * LZ77 Compression (compress_tile_data):
 *
 *   Both encoding paths call compress_tile_data() for final compression.
 *   Uses a hash table for O(1) match finding:
 *     - HASH_SIZE = 1024 entries
 *     - Matches 3+ bytes against hash and previous row
 *     - Back-reference: (len-3) << 2 | (offset-1) >> 8, (offset-1) & 0xFF
 *     - Literal: 0x80 | (count-1), followed by bytes
 *
 *   Internal optimizations:
 *     - Solid color: tiles with a single color (including all-black)
 *       use a compact encoding with one literal pixel plus row-repeat
 *       back-references, skipping hash-table scanning entirely.
 *     - Row matching: when an entire row equals the previous row,
 *       emits pre-computed back-references without per-byte scanning.
 *
 *   Compression is rejected if it doesn't achieve at least 25% reduction.
 *
 * Adaptive Selection (compress_tile_adaptive):
 *
 *   Tries both encoding paths and returns the smaller result.
 *   When comparing, the alpha-delta compressed size has
 *   ALPHA_DELTA_OVERHEAD (45 bytes) added to account for the
 *   composite command needed server-side; the returned size does
 *   NOT include this overhead (it is the raw compressed data size).
 *
 *     - Returns positive size if alpha-delta (including overhead) is smaller
 *     - Returns negative size if direct is smaller
 *     - Returns 0 if neither achieves 25% compression
 *
 *   When prev_pixels is NULL, only the direct path is attempted.
 *
 *   Note: alpha-delta may also return 0 (and thus be unavailable) if
 *   more than 75% of pixels changed, since the delta buffer would be
 *   too dense for effective compression.
 *
 * Parallel Compression:
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
 *
 * Results are written to the corresponding tile_result in the
 * results array passed to compress_tiles_parallel(), indexed by
 * work item position.
 */
struct tile_work {
    uint32_t *pixels;
    int stride;
    uint32_t *prev_pixels;
    int prev_stride;
    int x1, y1, w, h;
};

/* ============== Core Compression Functions ============== */

/*
 * Compress raw tile data using LZ77.
 *
 * This is the shared compression stage used by both encoding paths.
 * Automatically detects and optimizes solid-color tiles.
 *
 * dst:           output buffer for compressed data
 * dst_max:       maximum bytes to write
 * raw:           raw pixel data (row-major, XRGB32 or ARGB32 delta)
 * bytes_per_row: bytes per row in raw buffer
 * h:             number of rows
 *
 * Returns compressed size, or 0 if compression failed or didn't
 * achieve at least 25% reduction.
 */
int compress_tile_data(uint8_t *dst, int dst_max, 
                       uint8_t *raw, int bytes_per_row, int h);

/*
 * Compress a tile using the direct encoding path.
 *
 * Extracts tile pixels from frame buffer and compresses with LZ77.
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
 * Compress a tile using the alpha-delta encoding path.
 *
 * Creates an ARGB32 delta buffer where unchanged pixels are
 * transparent (0x00000000) and changed pixels are opaque with
 * the new color (0xFF000000 | RGB). The delta buffer is then
 * compressed with LZ77.
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
 *   - More than 75% of pixels changed (direct likely better)
 *   - LZ77 compression failed or didn't achieve 25% reduction
 */
int compress_tile_alpha_delta(uint8_t *dst, int dst_max,
                              uint32_t *pixels, int stride,
                              uint32_t *prev_pixels, int prev_stride,
                              int x1, int y1, int w, int h);

/*
 * Adaptively compress a tile using the best encoding path.
 *
 * Tries both direct and alpha-delta paths, returns the smaller.
 * When comparing sizes, ALPHA_DELTA_OVERHEAD is added to the
 * alpha-delta compressed size to account for the composite command
 * required on the server side. The returned value is the raw
 * compressed data size (without overhead).
 *
 * dst:         output buffer
 * dst_max:     maximum bytes to write
 * pixels:      current frame buffer (XRGB32)
 * stride:      current frame stride in pixels
 * prev_pixels: previous frame buffer (NULL for direct-only mode)
 * prev_stride: previous frame stride in pixels
 * x1, y1:      tile top-left corner
 * w, h:        tile dimensions
 *
 * Returns:
 *   > 0: alpha-delta size (use alpha composite to draw);
 *        delta_size + ALPHA_DELTA_OVERHEAD was smaller than direct_size
 *   < 0: negated direct size (use direct draw)
 *   = 0: compression failed or not worthwhile for either path
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
 * Results are written to the corresponding tile_result at the
 * same index as each tile_work item.
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
