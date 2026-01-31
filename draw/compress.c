/*
 * compress.c - Tile compression for Plan 9 draw protocol
 *
 * LZ77-style row matching compression with special cases for
 * solid colors and alpha-delta encoding.
 *
 * Uses generic thread_pool for parallel compression.
 *
 * Refactored:
 * - Extracted encode_solid_tile() for solid/zero tile encoding
 * - Added tile_dims_valid() inline helper for consistent validation
 */

#include <string.h>
#include <stdlib.h>
#include "compress.h"
#include "thread_pool.h"

/* Hash table for fast match finding */
#define HASH_BITS 10
#define HASH_SIZE (1 << HASH_BITS)
#define HASH_MASK (HASH_SIZE - 1)

static inline uint32_t hash3(const uint8_t *p) {
    return ((p[0] << 5) ^ (p[1] << 2) ^ p[2]) & HASH_MASK;
}

/* Validate tile dimensions */
static inline int tile_dims_valid(int w, int h) {
    return w > 0 && h > 0 && w <= TILE_SIZE && h <= TILE_SIZE;
}

/*
 * Encode a solid-color tile (including all-zeros).
 * This encoding uses a single pixel plus row-repeat references.
 * Returns bytes written to dst.
 */
static int encode_solid_tile(uint8_t *dst, const uint8_t *pixel, int h) {
    int out = 0;
    
    /* 4 literal bytes (the pixel value) */
    dst[out++] = 0x83;
    memcpy(dst + out, pixel, 4);
    out += 4;
    
    /* First row: two back-references to fill from the single pixel */
    dst[out++] = (31 << 2) | 0;  /* len=34, offset=4 */
    dst[out++] = 3;
    dst[out++] = (23 << 2) | 0;  /* len=26, offset=4 */
    dst[out++] = 3;
    
    /* Remaining rows: back-reference to previous row */
    for (int row = 1; row < h; row++) {
        dst[out++] = (31 << 2) | 0;  /* len=34, offset=64 */
        dst[out++] = 63;
        dst[out++] = (27 << 2) | 0;  /* len=30, offset=64 */
        dst[out++] = 63;
    }
    
    return out;
}

/*
 * Fast LZ77 compression for a tile.
 * Uses hash table for O(1) match finding instead of linear search.
 */
static int lz77_compress_fast(uint8_t *dst, int dst_max, uint8_t *raw, int raw_size, int bytes_per_row) {
    int out = 0;
    int h = raw_size / bytes_per_row;
    uint8_t lit[128];
    int nlit = 0;
    
    /* Hash table: maps hash -> most recent position */
    uint16_t htab[HASH_SIZE];
    memset(htab, 0, sizeof(htab));
    
    for (int row = 0; row < h; row++) {
        int row_start = row * bytes_per_row;
        int row_end = row_start + bytes_per_row;
        int pos = row_start;
        
        /* Fast path: check if row equals previous row */
        if (row > 0 && memcmp(raw + row_start, raw + row_start - bytes_per_row, bytes_per_row) == 0) {
            /* Flush literals */
            if (nlit > 0) {
                if (out + 1 + nlit > dst_max) return 0;
                dst[out++] = 0x80 | (nlit - 1);
                memcpy(dst + out, lit, nlit);
                out += nlit;
                nlit = 0;
            }
            /* Emit back-refs for entire row */
            int remaining = bytes_per_row;
            int off_code = bytes_per_row - 1;
            while (remaining > 0) {
                int len = remaining > 34 ? 34 : remaining;
                if (out + 2 > dst_max) return 0;
                dst[out++] = ((len - 3) << 2) | ((off_code >> 8) & 0x03);
                dst[out++] = off_code & 0xFF;
                remaining -= len;
            }
            continue;
        }
        
        while (pos < row_end) {
            int best = 0, boff = 0;
            int maxlen = row_end - pos;
            if (maxlen > 34) maxlen = 34;
            
            if (maxlen >= 3 && pos >= 3) {
                uint32_t hv = hash3(raw + pos);
                int candidate = htab[hv];
                htab[hv] = pos;
                
                /* Check hash candidate */
                if (candidate > 0 && pos - candidate <= 256) {
                    int off = pos - candidate;
                    int len = 0;
                    while (len < maxlen && raw[pos - off + len] == raw[pos + len]) len++;
                    if (len >= 3) { best = len; boff = off; }
                }
                
                /* Check previous row offset */
                if (best < maxlen && row > 0) {
                    int off = bytes_per_row;
                    if (pos - off >= 0) {
                        int len = 0;
                        while (len < maxlen && raw[pos - off + len] == raw[pos + len]) len++;
                        if (len > best) { best = len; boff = off; }
                    }
                }
            }
            
            if (best >= 3) {
                if (nlit > 0) {
                    if (out + 1 + nlit > dst_max) return 0;
                    dst[out++] = 0x80 | (nlit - 1);
                    memcpy(dst + out, lit, nlit);
                    out += nlit;
                    nlit = 0;
                }
                if (out + 2 > dst_max) return 0;
                dst[out++] = ((best - 3) << 2) | ((boff - 1) >> 8);
                dst[out++] = (boff - 1) & 0xFF;
                pos += best;
            } else {
                lit[nlit++] = raw[pos++];
                if (nlit == 128 || pos == row_end) {
                    if (out + 1 + nlit > dst_max) return 0;
                    dst[out++] = 0x80 | (nlit - 1);
                    memcpy(dst + out, lit, nlit);
                    out += nlit;
                    nlit = 0;
                }
            }
        }
    }
    
    return out;
}

int compress_tile_data(uint8_t *dst, int dst_max, 
                       uint8_t *raw, int bytes_per_row, int h) {
    int raw_size = h * bytes_per_row;
    
    /* Check if all zeros */
    int all_zero = 1;
    for (int i = 0; i < raw_size; i += 4) {
        if (*(uint32_t *)(raw + i) != 0) {
            all_zero = 0;
            break;
        }
    }
    
    if (all_zero) {
        static const uint8_t zero_pixel[4] = {0, 0, 0, 0};
        return encode_solid_tile(dst, zero_pixel, h);
    }
    
    /* Check if solid color */
    uint32_t first_pixel;
    memcpy(&first_pixel, raw, 4);
    int is_solid = 1;
    for (int i = 4; i < raw_size; i += 4) {
        uint32_t pixel;
        memcpy(&pixel, raw + i, 4);
        if (pixel != first_pixel) {
            is_solid = 0;
            break;
        }
    }
    
    int out;
    
    if (is_solid) {
        out = encode_solid_tile(dst, raw, h);
    } else {
        out = lz77_compress_fast(dst, dst_max, raw, raw_size, bytes_per_row);
        if (out == 0) return 0;
    }
    
    /* Only use if we saved at least 25% */
    if (out >= raw_size * 3 / 4) return 0;
    return out;
}

int compress_tile_direct(uint8_t *dst, int dst_max, 
                         uint32_t *pixels, int stride, 
                         int x1, int y1, int w, int h) {
    if (!tile_dims_valid(w, h)) return 0;
    
    int bytes_per_row = w * 4;
    
    /* Copy tile to contiguous buffer */
    uint8_t raw[TILE_SIZE * TILE_SIZE * 4];
    for (int row = 0; row < h; row++) {
        memcpy(raw + row * bytes_per_row, 
               &pixels[(y1 + row) * stride + x1], bytes_per_row);
    }
    
    return compress_tile_data(dst, dst_max, raw, bytes_per_row, h);
}

int compress_tile_alpha_delta(uint8_t *dst, int dst_max,
                              uint32_t *pixels, int stride,
                              uint32_t *prev_pixels, int prev_stride,
                              int x1, int y1, int w, int h) {
    if (!tile_dims_valid(w, h)) return 0;
    if (!prev_pixels) return 0;
    
    int bytes_per_row = w * 4;
    uint8_t delta[TILE_SIZE * TILE_SIZE * 4];
    int changed = 0;
    
    for (int row = 0; row < h; row++) {
        uint32_t *curr = &pixels[(y1 + row) * stride + x1];
        uint32_t *prev = &prev_pixels[(y1 + row) * prev_stride + x1];
        uint32_t *out = (uint32_t *)(delta + row * bytes_per_row);
        
        for (int col = 0; col < w; col++) {
            uint32_t c = curr[col] & 0x00FFFFFF;
            uint32_t p = prev[col] & 0x00FFFFFF;
            
            if (c != p) {
                out[col] = 0xFF000000 | c;
                changed++;
            } else {
                out[col] = 0x00000000;
            }
        }
    }
    
    if (changed == 0) return 0;
    if (changed > (w * h * 3 / 4)) return 0;
    
    return compress_tile_data(dst, dst_max, delta, bytes_per_row, h);
}

int compress_tile_adaptive(uint8_t *dst, int dst_max,
                           uint32_t *pixels, int stride,
                           uint32_t *prev_pixels, int prev_stride,
                           int x1, int y1, int w, int h) {
    if (!tile_dims_valid(w, h)) return 0;
    
    uint8_t temp[1200];
    
    /* Try direct compression first */
    int direct_size = compress_tile_direct(dst, dst_max, pixels, stride, x1, y1, w, h);
    
    /* If no prev_pixels, can only do direct */
    if (!prev_pixels) {
        return direct_size > 0 ? -direct_size : 0;
    }
    
    /* Try alpha-delta compression */
    int delta_size = compress_tile_alpha_delta(temp, sizeof(temp),
                                               pixels, stride,
                                               prev_pixels, prev_stride,
                                               x1, y1, w, h);
    
    /* Compare: delta wins if delta_size + overhead < direct_size */
    if (delta_size > 0) {
        int delta_total = delta_size + ALPHA_DELTA_OVERHEAD;
        
        if (direct_size == 0 || delta_total < direct_size) {
            memcpy(dst, temp, delta_size);
            return delta_size;  /* positive = delta */
        }
    }
    
    return direct_size > 0 ? -direct_size : 0;
}

/* ============== Thread Pool Integration ============== */

static struct thread_pool compress_pool = {0};

/* Work context passed to worker function */
struct compress_work_ctx {
    struct tile_work *tiles;
    struct tile_result *results;
};

/* Worker function called by generic thread pool */
static void compress_tile_worker(void *user_data, int idx) {
    struct compress_work_ctx *ctx = user_data;
    struct tile_work *w = &ctx->tiles[idx];
    struct tile_result *r = &ctx->results[idx];
    
    int result = compress_tile_adaptive(
        r->data, sizeof(r->data),
        w->pixels, w->stride,
        w->prev_pixels, w->prev_stride,
        w->x1, w->y1, w->w, w->h
    );
    
    if (result > 0) {
        r->size = result;
        r->is_delta = 1;
    } else if (result < 0) {
        r->size = -result;
        r->is_delta = 0;
    } else {
        r->size = 0;
        r->is_delta = 0;
    }
}

/* Initialize compression thread pool */
int compress_pool_init(int nthreads) {
    return pool_create(&compress_pool, nthreads);
}

/* Shutdown compression thread pool */
void compress_pool_shutdown(void) {
    pool_destroy(&compress_pool);
}

/* Compress multiple tiles in parallel */
int compress_tiles_parallel(
    struct tile_work *tiles, 
    struct tile_result *results,
    int count
) {
    if (!compress_pool.initialized || count == 0) return -1;
    
    struct compress_work_ctx ctx = {
        .tiles = tiles,
        .results = results
    };
    
    pool_process(&compress_pool, compress_tile_worker, &ctx, count);
    return 0;
}
