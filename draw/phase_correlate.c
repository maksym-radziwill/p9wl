/*
 * phase_correlate.c - FFT-based motion detection using phase correlation
 *
 * Detects translation (scrolling) between two image regions.
 *
 * Phase correlation algorithm:
 * 1. Extract regions from both frames, apply Hann window
 * 2. Downsample by k (integer output scale) - scroll offsets are k-aligned
 * 3. Compute forward FFT of both regions
 * 4. Compute cross-power spectrum: (F1 * conj(F2)) / |F1 * conj(F2)|
 * 5. Compute inverse FFT to get correlation surface
 * 6. Find peak in correlation surface - location indicates translation
 *
 * With integer output scaling k = ceil(scale), all valid scroll offsets
 * are multiples of k. This allows us to downsample by k, reducing FFT
 * size and computation while still detecting all valid scroll motions.
 *
 * Uses single-precision FFTW for performance.
 * Thread-local storage allows multiple threads to process regions in parallel.
 *
 * Compile with: -lfftw3f -lm
 */

#define _POSIX_C_SOURCE 200809L
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <pthread.h>
#include <fftw3.h>
#include <wlr/util/log.h>

#include "phase_correlate.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Maximum threads we track for cleanup */
#define MAX_TRACKED_THREADS 64

/*
 * Maximum FFT size at full resolution. Actual FFT size = FFT_SIZE_MAX / k.
 * k=1: 256×256, k=2: 128×128, k=3: ~85×85, k=4: 64×64
 */
#define FFT_SIZE_MAX 256

/* Precomputed Hann window lookup table (at maximum FFT size) */
static float hann_lut[FFT_SIZE_MAX];
static pthread_once_t hann_once = PTHREAD_ONCE_INIT;

static void init_hann_lut_internal(void) {
    for (int i = 0; i < FFT_SIZE_MAX; i++) {
        hann_lut[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (FFT_SIZE_MAX - 1)));
    }
}

static void init_hann_lut(void) {
    pthread_once(&hann_once, init_hann_lut_internal);
}

/* Thread-local FFT resources - allocated at maximum size */
struct fft_thread_resources {
    float *fft_in1;
    float *fft_in2;
    float *fft_corr;
    fftwf_complex *fft_out1;
    fftwf_complex *fft_out2;
    fftwf_complex *fft_cross;
    /* Plans for different k values (k=1,2,3,4) */
    fftwf_plan plan_fwd1[5];
    fftwf_plan plan_fwd2[5];
    fftwf_plan plan_inv[5];
    int initialized;
};

static __thread struct fft_thread_resources tls_resources = {0};

/* Global tracking for cleanup */
static pthread_mutex_t resources_lock = PTHREAD_MUTEX_INITIALIZER;
static struct fft_thread_resources *tracked_resources[MAX_TRACKED_THREADS];
static int num_tracked = 0;

static pthread_mutex_t fftw_plan_lock = PTHREAD_MUTEX_INITIALIZER;

static void track_resources(struct fft_thread_resources *res) {
    pthread_mutex_lock(&resources_lock);
    if (num_tracked < MAX_TRACKED_THREADS) {
        tracked_resources[num_tracked++] = res;
    }
    pthread_mutex_unlock(&resources_lock);
}

static void free_thread_resources(struct fft_thread_resources *res) {
    if (!res || !res->initialized) return;
    
    for (int k = 1; k <= 4; k++) {
        if (res->plan_fwd1[k]) fftwf_destroy_plan(res->plan_fwd1[k]);
        if (res->plan_fwd2[k]) fftwf_destroy_plan(res->plan_fwd2[k]);
        if (res->plan_inv[k]) fftwf_destroy_plan(res->plan_inv[k]);
    }
    
    if (res->fft_in1) fftwf_free(res->fft_in1);
    if (res->fft_in2) fftwf_free(res->fft_in2);
    if (res->fft_corr) fftwf_free(res->fft_corr);
    if (res->fft_out1) fftwf_free(res->fft_out1);
    if (res->fft_out2) fftwf_free(res->fft_out2);
    if (res->fft_cross) fftwf_free(res->fft_cross);
    
    memset(res, 0, sizeof(*res));
}

int phase_correlate_init(void) {
    struct fft_thread_resources *res = &tls_resources;
    
    if (res->initialized) return 0;
    
    init_hann_lut();
    
    /* Allocate at maximum size */
    res->fft_in1 = fftwf_alloc_real(FFT_SIZE_MAX * FFT_SIZE_MAX);
    res->fft_in2 = fftwf_alloc_real(FFT_SIZE_MAX * FFT_SIZE_MAX);
    res->fft_corr = fftwf_alloc_real(FFT_SIZE_MAX * FFT_SIZE_MAX);
    res->fft_out1 = fftwf_alloc_complex((FFT_SIZE_MAX/2 + 1) * FFT_SIZE_MAX);
    res->fft_out2 = fftwf_alloc_complex((FFT_SIZE_MAX/2 + 1) * FFT_SIZE_MAX);
    res->fft_cross = fftwf_alloc_complex((FFT_SIZE_MAX/2 + 1) * FFT_SIZE_MAX);
    
    if (!res->fft_in1 || !res->fft_in2 || !res->fft_corr ||
        !res->fft_out1 || !res->fft_out2 || !res->fft_cross) {
        wlr_log(WLR_ERROR, "Phase correlate: failed to allocate FFT buffers");
        free_thread_resources(res);
        return -1;
    }
    
    pthread_mutex_lock(&fftw_plan_lock);
    
    /* Create plans for k=1,2,3,4 */
    for (int k = 1; k <= 4; k++) {
        int fft_size = FFT_SIZE_MAX / k;
        if (fft_size < 16) fft_size = 16;  /* Minimum useful FFT size */
        
        res->plan_fwd1[k] = fftwf_plan_dft_r2c_2d(fft_size, fft_size, 
                                                   res->fft_in1, res->fft_out1, FFTW_MEASURE);
        res->plan_fwd2[k] = fftwf_plan_dft_r2c_2d(fft_size, fft_size, 
                                                   res->fft_in2, res->fft_out2, FFTW_MEASURE);
        res->plan_inv[k] = fftwf_plan_dft_c2r_2d(fft_size, fft_size, 
                                                  res->fft_cross, res->fft_corr, FFTW_MEASURE);
        
        if (!res->plan_fwd1[k] || !res->plan_fwd2[k] || !res->plan_inv[k]) {
            wlr_log(WLR_ERROR, "Phase correlate: failed to create FFTW plans for k=%d", k);
            pthread_mutex_unlock(&fftw_plan_lock);
            free_thread_resources(res);
            return -1;
        }
    }
    
    pthread_mutex_unlock(&fftw_plan_lock);
    
    res->initialized = 1;
    track_resources(res);
    
    return 0;
}

/*
 * Extract region with k-based downsampling and Hann windowing.
 * Averages k×k pixel blocks - scroll offsets are guaranteed to be k-aligned.
 * The downsampling also provides natural smoothing.
 */
static void extract_region_windowed(
    uint32_t *buf, int buf_width,
    int rx1, int ry1, int rx2, int ry2,
    float *fft_buf, int k, int fft_size
) {
    int rw = rx2 - rx1;
    int rh = ry2 - ry1;
    
    /* Downsampled dimensions */
    int ds_w = rw / k;
    int ds_h = rh / k;
    
    memset(fft_buf, 0, fft_size * fft_size * sizeof(float));
    
    /* Center the downsampled region in FFT buffer */
    int off_x = (fft_size - ds_w) / 2;
    int off_y = (fft_size - ds_h) / 2;
    if (off_x < 0) off_x = 0;
    if (off_y < 0) off_y = 0;
    
    int copy_w = (ds_w < fft_size) ? ds_w : fft_size;
    int copy_h = (ds_h < fft_size) ? ds_h : fft_size;
    
    /* Scale for Hann window lookup */
    float scale_x = (copy_w > 1) ? (float)(FFT_SIZE_MAX - 1) / (copy_w - 1) : 0;
    float scale_y = (copy_h > 1) ? (float)(FFT_SIZE_MAX - 1) / (copy_h - 1) : 0;
    
    /* Precompute inverse of block size for averaging */
    const float inv_block = 1.0f / (k * k);
    
    for (int dy = 0; dy < copy_h; dy++) {
        int lut_y = (int)(dy * scale_y);
        if (lut_y >= FFT_SIZE_MAX) lut_y = FFT_SIZE_MAX - 1;
        float wy = hann_lut[lut_y];
        float *dst_row = &fft_buf[(dy + off_y) * fft_size + off_x];
        
        /* Source Y range for this downsampled row */
        int src_y0 = ry1 + dy * k;
        
        for (int dx = 0; dx < copy_w; dx++) {
            int lut_x = (int)(dx * scale_x);
            if (lut_x >= FFT_SIZE_MAX) lut_x = FFT_SIZE_MAX - 1;
            float wx = hann_lut[lut_x];
            
            /* Source X for this downsampled column */
            int src_x0 = rx1 + dx * k;
            
            /* Average k×k block (grayscale) */
            float sum = 0;
            for (int by = 0; by < k; by++) {
                uint32_t *src_row = &buf[(src_y0 + by) * buf_width + src_x0];
                for (int bx = 0; bx < k; bx++) {
                    uint32_t px = src_row[bx];
                    /* Fast grayscale: approximate (r+g+b)/3 */
                    sum += ((px >> 16) & 0xFF) + ((px >> 8) & 0xFF) + (px & 0xFF);
                }
            }
            
            /* Apply Hann window to averaged value */
            dst_row[dx] = (sum * inv_block * (1.0f/3.0f)) * wy * wx;
        }
    }
}

static void compute_phase_correlation(
    fftwf_complex *out1,
    fftwf_complex *out2,
    fftwf_complex *cross,
    int n
) {
    for (int i = 0; i < n; i++) {
        float re1 = out1[i][0], im1 = out1[i][1];
        float re2 = out2[i][0], im2 = out2[i][1];
        
        float cross_re = re1 * re2 + im1 * im2;
        float cross_im = im1 * re2 - re1 * im2;
        
        float mag = sqrtf(cross_re * cross_re + cross_im * cross_im);
        
        if (mag > 1e-10f) {
            cross[i][0] = cross_re / mag;
            cross[i][1] = cross_im / mag;
        } else {
            cross[i][0] = 0;
            cross[i][1] = 0;
        }
    }
}

static void find_correlation_peak(
    float *corr,
    int fft_size,
    int *out_dx, int *out_dy,
    int max_shift
) {
    float peak_val = -1e30f;
    int peak_x = 0, peak_y = 0;
    
    for (int dy = -max_shift; dy <= max_shift; dy++) {
        for (int dx = -max_shift; dx <= max_shift; dx++) {
            int cy = (dy + fft_size) % fft_size;
            int cx = (dx + fft_size) % fft_size;
            
            float val = corr[cy * fft_size + cx];
            
            if (val > peak_val) {
                peak_val = val;
                peak_x = dx;
                peak_y = dy;
            }
        }
    }
    
    *out_dx = peak_x;
    *out_dy = peak_y;
}

struct phase_result phase_correlate_detect(
    uint32_t *curr_buf, uint32_t *prev_buf, int buf_width,
    int rx1, int ry1, int rx2, int ry2,
    int max_shift, int k
) {
    struct phase_result result = {0};
    struct fft_thread_resources *res = &tls_resources;
    
    /* Clamp k to valid range */
    if (k < 1) k = 1;
    if (k > 4) k = 4;
    
    int fft_size = FFT_SIZE_MAX / k;
    if (fft_size < 16) fft_size = 16;
    
    int rw = rx2 - rx1;
    int rh = ry2 - ry1;
    
    /* Need at least k×k pixels per FFT cell, and minimum 4 cells */
    if (rw < k * 4 || rh < k * 4) return result;
    if (phase_correlate_init() < 0) return result;
    
    /* Extract regions with k-based downsampling and Hann windowing */
    extract_region_windowed(curr_buf, buf_width, rx1, ry1, rx2, ry2, 
                           res->fft_in1, k, fft_size);
    extract_region_windowed(prev_buf, buf_width, rx1, ry1, rx2, ry2, 
                           res->fft_in2, k, fft_size);
    
    /* Forward FFT using plan for this k value */
    fftwf_execute(res->plan_fwd1[k]);
    fftwf_execute(res->plan_fwd2[k]);
    
    /* Cross-power spectrum */
    int fft_complex_size = (fft_size/2 + 1) * fft_size;
    compute_phase_correlation(res->fft_out1, res->fft_out2, res->fft_cross, fft_complex_size);
    
    /* Inverse FFT to get correlation surface */
    fftwf_execute(res->plan_inv[k]);
    
    /* Convert max_shift to downsampled domain */
    int ds_max_shift = max_shift / k;
    int ds_rw = rw / k;
    int ds_rh = rh / k;
    
    if (ds_max_shift > ds_rw / 2) ds_max_shift = ds_rw / 2;
    if (ds_max_shift > ds_rh / 2) ds_max_shift = ds_rh / 2;
    if (ds_max_shift < 1) ds_max_shift = 1;
    
    /* Find peak in downsampled correlation surface */
    int ds_dx, ds_dy;
    find_correlation_peak(res->fft_corr, fft_size, &ds_dx, &ds_dy, ds_max_shift);
    
    /* Scale result back to full resolution - result is k-aligned */
    result.dx = ds_dx * k;
    result.dy = ds_dy * k;
    result.valid = 1;
    
    return result;
}

void phase_correlate_cleanup(void) {
    pthread_mutex_lock(&resources_lock);
    
    for (int i = 0; i < num_tracked; i++) {
        free_thread_resources(tracked_resources[i]);
        tracked_resources[i] = NULL;
    }
    num_tracked = 0;
    
    pthread_mutex_unlock(&resources_lock);
    
    fftwf_cleanup();
}
