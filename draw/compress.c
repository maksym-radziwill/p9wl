/*
 * compress.c - Tile compression for Plan 9 draw protocol
 *
 * LZ77-style row matching compression with special cases for
 * solid colors and alpha-delta encoding.
 *
 * Note: With TILE_SIZE-aligned dimensions, all tiles should be full-size.
 * Partial tile handling is kept as a safety net.
 */

#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include "compress.h"

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

/* Thread pool state */
static pthread_t *workers;
static int num_workers;
static struct tile_work *work_queue;
static int work_count;
static int work_next;
static pthread_mutex_t work_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t work_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t done_cond = PTHREAD_COND_INITIALIZER;
static int workers_done;
static int pool_running;

static int lz77_compress_row(uint8_t *dst, int dst_max, uint8_t *raw, int pos, int row_end, int raw_start) {
    int out = 0;
    uint8_t lit[128];
    int nlit = 0;
    
    while (pos < row_end) {
        int best = 0, boff = 0;
        int maxoff = pos - raw_start;
        if (maxoff > 1024) maxoff = 1024;
        
        /* Limit match length to not cross row boundary */
        int maxlen = row_end - pos;
        if (maxlen > 34) maxlen = 34;
        
        for (int off = 1; off <= maxoff; off++) {
            int len = 0;
            while (len < maxlen && raw[pos - off + len] == raw[pos + len]) len++;
            if (len > best) { best = len; boff = off; if (best == maxlen) break; }
        }
        
        if (best >= 3) {
            if (nlit > 0) {
                if (out + 1 + nlit > dst_max) return -1;
                dst[out++] = 0x80 | (nlit - 1);
                memcpy(dst + out, lit, nlit);
                out += nlit;
                nlit = 0;
            }
            if (out + 2 > dst_max) return -1;
            dst[out++] = ((best - 3) << 2) | ((boff - 1) >> 8);
            dst[out++] = (boff - 1) & 0xFF;
            pos += best;
        } else {
            lit[nlit++] = raw[pos++];
            if (nlit == 128 || pos == row_end) {
                if (out + 1 + nlit > dst_max) return -1;
                dst[out++] = 0x80 | (nlit - 1);
                memcpy(dst + out, lit, nlit);
                out += nlit;
                nlit = 0;
            }
        }
    }
    
    return out;
}

static int lz77_compress(uint8_t *dst, int dst_max, uint8_t *raw, int raw_size, int bytes_per_row) {
    int out = 0;
    int h = raw_size / bytes_per_row;
    
    for (int row = 0; row < h; row++) {
        int row_start = row * bytes_per_row;
        int row_end = row_start + bytes_per_row;
        int n = lz77_compress_row(dst + out, dst_max - out, raw, row_start, row_end, 0);
        if (n < 0) return 0;
        out += n;
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
        /* All zeros - super efficient encoding */
        int out = 0;
        
        dst[out++] = 0x83;  /* 4 literal bytes */
        dst[out++] = 0; dst[out++] = 0; dst[out++] = 0; dst[out++] = 0;
        
        dst[out++] = (31 << 2) | 0;
        dst[out++] = 3;
        dst[out++] = (23 << 2) | 0;
        dst[out++] = 3;
        
        for (int row = 1; row < h; row++) {
            dst[out++] = (31 << 2) | 0;
            dst[out++] = 63;
            dst[out++] = (27 << 2) | 0;
            dst[out++] = 63;
        }
        return out;
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
    
    int out = 0;
    
    if (is_solid) {
        dst[out++] = 0x83;
        memcpy(dst + out, raw, 4);
        out += 4;
        
        dst[out++] = (31 << 2) | 0;
        dst[out++] = 3;
        dst[out++] = (23 << 2) | 0;
        dst[out++] = 3;
        
        for (int row = 1; row < h; row++) {
            dst[out++] = (31 << 2) | 0;
            dst[out++] = 63;
            dst[out++] = (27 << 2) | 0;
            dst[out++] = 63;
        }
    } else {
        out = lz77_compress(dst, dst_max, raw, raw_size, bytes_per_row);
        if (out == 0) return 0;
    }
    
    /* Only use if we saved at least 25% */
    if (out >= raw_size * 3 / 4) return 0;
    return out;
}

int compress_tile_direct(uint8_t *dst, int dst_max, 
                         uint32_t *pixels, int stride, 
                         int x1, int y1, int w, int h) {
    /* Validate dimensions - allow partial tiles but not invalid ones */
    if (w <= 0 || h <= 0 || w > TILE_SIZE || h > TILE_SIZE) return 0;
    
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
    /* Validate dimensions - allow partial tiles but not invalid ones */
    if (w <= 0 || h <= 0 || w > TILE_SIZE || h > TILE_SIZE) return 0;
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
    /* Validate dimensions - allow partial tiles but not invalid ones */
    if (w <= 0 || h <= 0 || w > TILE_SIZE || h > TILE_SIZE) return 0;
    
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

/* Worker thread function */
static void *compress_worker(void *arg) {
    (void)arg;
    
    while (1) {
        pthread_mutex_lock(&work_lock);
        
        while (work_next >= work_count && pool_running) {
            pthread_cond_wait(&work_cond, &work_lock);
        }
        
        if (!pool_running) {
            pthread_mutex_unlock(&work_lock);
            break;
        }
        
        int idx = work_next++;
        pthread_mutex_unlock(&work_lock);
        
        struct tile_work *w = &work_queue[idx];
        int result = compress_tile_adaptive(
            w->result->data, sizeof(w->result->data),
            w->pixels, w->stride,
            w->prev_pixels, w->prev_stride,
            w->x1, w->y1, w->w, w->h
        );
        
        if (result > 0) {
            w->result->size = result;
            w->result->is_delta = 1;
        } else if (result < 0) {
            w->result->size = -result;
            w->result->is_delta = 0;
        } else {
            w->result->size = 0;
            w->result->is_delta = 0;
        }
        
        pthread_mutex_lock(&work_lock);
        workers_done++;
        if (workers_done == work_count) {
            pthread_cond_signal(&done_cond);
        }
        pthread_mutex_unlock(&work_lock);
    }
    
    return NULL;
}

/* Initialize compression thread pool */
int compress_pool_init(int nthreads) {
    if (workers) return 0;
    
    num_workers = nthreads;
    workers = malloc(nthreads * sizeof(pthread_t));
    if (!workers) return -1;
    
    pool_running = 1;
    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&workers[i], NULL, compress_worker, NULL) != 0) {
            pool_running = 0;
            for (int j = 0; j < i; j++) {
                pthread_join(workers[j], NULL);
            }
            free(workers);
            workers = NULL;
            return -1;
        }
    }
    
    return 0;
}

/* Shutdown compression thread pool */
void compress_pool_shutdown(void) {
    if (!workers) return;
    
    pthread_mutex_lock(&work_lock);
    pool_running = 0;
    pthread_cond_broadcast(&work_cond);
    pthread_mutex_unlock(&work_lock);
    
    for (int i = 0; i < num_workers; i++) {
        pthread_join(workers[i], NULL);
    }
    
    free(workers);
    workers = NULL;
}

/* Compress multiple tiles in parallel */
int compress_tiles_parallel(
    struct tile_work *tiles, 
    struct tile_result *results,
    int count
) {
    if (!workers || count == 0) return -1;
    
    /* Link results to work items */
    for (int i = 0; i < count; i++) {
        tiles[i].result = &results[i];
    }
    
    pthread_mutex_lock(&work_lock);
    work_queue = tiles;
    work_count = count;
    work_next = 0;
    workers_done = 0;
    pthread_cond_broadcast(&work_cond);
    
    while (workers_done < count) {
        pthread_cond_wait(&done_cond, &work_lock);
    }
    pthread_mutex_unlock(&work_lock);
    
    return 0;
}
