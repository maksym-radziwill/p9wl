/*
 * phase_correlate.c - FFT-based motion detection using phase correlation
 *
 * Detects translation (scrolling) between two image regions.
 *
 * Phase correlation algorithm:
 * 1. Extract regions from both frames, apply Hann window
 * 2. Apply smoothing filter (average ±4 neighbors) to sharpen FFT peak
 * 3. Compute forward FFT of both regions
 * 4. Compute cross-power spectrum: (F1 * conj(F2)) / |F1 * conj(F2)|
 * 5. Compute inverse FFT to get correlation surface
 * 6. Find peak in correlation surface - location indicates translation
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

/* Smoothing radius: average over [-SMOOTH_RADIUS, +SMOOTH_RADIUS] */
#define SMOOTH_RADIUS 8

/* Maximum threads we track for cleanup */
#define MAX_TRACKED_THREADS 64

/* Precomputed Hann window lookup table */
static float hann_lut[FFT_SIZE];
static pthread_once_t hann_once = PTHREAD_ONCE_INIT;

static void init_hann_lut_internal(void) {
    for (int i = 0; i < FFT_SIZE; i++) {
        hann_lut[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (FFT_SIZE - 1)));
    }
}

static void init_hann_lut(void) {
    pthread_once(&hann_once, init_hann_lut_internal);
}

/* Thread-local FFT resources */
struct fft_thread_resources {
    float *fft_in1;
    float *fft_in2;
    float *fft_corr;
    float *smooth_tmp;  /* Temporary buffer for smoothing */
    fftwf_complex *fft_out1;
    fftwf_complex *fft_out2;
    fftwf_complex *fft_cross;
    fftwf_plan plan_fwd1;
    fftwf_plan plan_fwd2;
    fftwf_plan plan_inv;
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
    
    if (res->plan_fwd1) fftwf_destroy_plan(res->plan_fwd1);
    if (res->plan_fwd2) fftwf_destroy_plan(res->plan_fwd2);
    if (res->plan_inv) fftwf_destroy_plan(res->plan_inv);
    
    if (res->fft_in1) fftwf_free(res->fft_in1);
    if (res->fft_in2) fftwf_free(res->fft_in2);
    if (res->fft_corr) fftwf_free(res->fft_corr);
    if (res->smooth_tmp) fftwf_free(res->smooth_tmp);
    if (res->fft_out1) fftwf_free(res->fft_out1);
    if (res->fft_out2) fftwf_free(res->fft_out2);
    if (res->fft_cross) fftwf_free(res->fft_cross);
    
    memset(res, 0, sizeof(*res));
}

int phase_correlate_init(void) {
    struct fft_thread_resources *res = &tls_resources;
    
    if (res->initialized) return 0;
    
    init_hann_lut();
    
    res->fft_in1 = fftwf_alloc_real(FFT_SIZE * FFT_SIZE);
    res->fft_in2 = fftwf_alloc_real(FFT_SIZE * FFT_SIZE);
    res->fft_corr = fftwf_alloc_real(FFT_SIZE * FFT_SIZE);
    res->smooth_tmp = fftwf_alloc_real(FFT_SIZE * FFT_SIZE);
    res->fft_out1 = fftwf_alloc_complex((FFT_SIZE/2 + 1) * FFT_SIZE);
    res->fft_out2 = fftwf_alloc_complex((FFT_SIZE/2 + 1) * FFT_SIZE);
    res->fft_cross = fftwf_alloc_complex((FFT_SIZE/2 + 1) * FFT_SIZE);
    
    if (!res->fft_in1 || !res->fft_in2 || !res->fft_corr || !res->smooth_tmp ||
        !res->fft_out1 || !res->fft_out2 || !res->fft_cross) {
        wlr_log(WLR_ERROR, "Phase correlate: failed to allocate FFT buffers");
        free_thread_resources(res);
        return -1;
    }
    
    pthread_mutex_lock(&fftw_plan_lock);
    res->plan_fwd1 = fftwf_plan_dft_r2c_2d(FFT_SIZE, FFT_SIZE, res->fft_in1, res->fft_out1, FFTW_MEASURE);
    res->plan_fwd2 = fftwf_plan_dft_r2c_2d(FFT_SIZE, FFT_SIZE, res->fft_in2, res->fft_out2, FFTW_MEASURE);
    res->plan_inv = fftwf_plan_dft_c2r_2d(FFT_SIZE, FFT_SIZE, res->fft_cross, res->fft_corr, FFTW_MEASURE);
    pthread_mutex_unlock(&fftw_plan_lock);
    
    if (!res->plan_fwd1 || !res->plan_fwd2 || !res->plan_inv) {
        wlr_log(WLR_ERROR, "Phase correlate: failed to create FFTW plans");
        free_thread_resources(res);
        return -1;
    }
    
    res->initialized = 1;
    track_resources(res);
    
    return 0;
}

static inline float pixel_to_gray(uint32_t pixel) {
    uint8_t r = (pixel >> 16) & 0xFF;
    uint8_t g = (pixel >> 8) & 0xFF;
    uint8_t b = pixel & 0xFF;
    return 0.299f * r + 0.587f * g + 0.114f * b;
}

/*
 * Apply 2D box filter smoothing using sliding window (O(n²) instead of O(n²×r)).
 * Each pixel becomes the average of its neighborhood within radius SMOOTH_RADIUS.
 * Uses separable filtering (horizontal then vertical) with running sums.
 * buf: input/output buffer
 * tmp: scratch buffer (must be FFT_SIZE * FFT_SIZE)
 */
static void smooth_buffer(float *buf, float *tmp) {
    const int r = SMOOTH_RADIUS;
    const int w = FFT_SIZE;
    
    /* Horizontal pass: buf -> tmp (sliding window) */
    for (int y = 0; y < w; y++) {
        float *src_row = &buf[y * w];
        float *dst_row = &tmp[y * w];
        
        /* Initialize sum for x=0 */
        float sum = 0;
        int count = 0;
        for (int dx = 0; dx <= r && dx < w; dx++) {
            sum += src_row[dx];
            count++;
        }
        dst_row[0] = sum / count;
        
        /* Slide window across row */
        for (int x = 1; x < w; x++) {
            /* Remove left element leaving window */
            int left = x - r - 1;
            if (left >= 0) {
                sum -= src_row[left];
                count--;
            }
            /* Add right element entering window */
            int right = x + r;
            if (right < w) {
                sum += src_row[right];
                count++;
            }
            dst_row[x] = sum / count;
        }
    }
    
    /* Vertical pass: tmp -> buf (sliding window) */
    for (int x = 0; x < w; x++) {
        /* Initialize sum for y=0 */
        float sum = 0;
        int count = 0;
        for (int dy = 0; dy <= r && dy < w; dy++) {
            sum += tmp[dy * w + x];
            count++;
        }
        buf[x] = sum / count;
        
        /* Slide window down column */
        for (int y = 1; y < w; y++) {
            /* Remove top element leaving window */
            int top = y - r - 1;
            if (top >= 0) {
                sum -= tmp[top * w + x];
                count--;
            }
            /* Add bottom element entering window */
            int bot = y + r;
            if (bot < w) {
                sum += tmp[bot * w + x];
                count++;
            }
            buf[y * w + x] = sum / count;
        }
    }
}

static void extract_region_windowed(
    uint32_t *buf, int buf_width,
    int rx1, int ry1, int rx2, int ry2,
    float *fft_buf
) {
    int rw = rx2 - rx1;
    int rh = ry2 - ry1;
    
    memset(fft_buf, 0, FFT_SIZE * FFT_SIZE * sizeof(float));
    
    int off_x = (FFT_SIZE - rw) / 2;
    int off_y = (FFT_SIZE - rh) / 2;
    if (off_x < 0) off_x = 0;
    if (off_y < 0) off_y = 0;
    
    int copy_w = (rw < FFT_SIZE) ? rw : FFT_SIZE;
    int copy_h = (rh < FFT_SIZE) ? rh : FFT_SIZE;
    
    float scale_x = (float)(FFT_SIZE - 1) / (copy_w - 1);
    float scale_y = (float)(FFT_SIZE - 1) / (copy_h - 1);
    
    for (int y = 0; y < copy_h; y++) {
        int lut_y = (int)(y * scale_y);
        float wy = hann_lut[lut_y];
        float *row = &fft_buf[(y + off_y) * FFT_SIZE + off_x];
        uint32_t *src_row = &buf[(ry1 + y) * buf_width + rx1];
        
        for (int x = 0; x < copy_w; x++) {
            int lut_x = (int)(x * scale_x);
            row[x] = pixel_to_gray(src_row[x]) * wy * hann_lut[lut_x];
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
    int *out_dx, int *out_dy,
    int max_shift
) {
    float peak_val = -1e30f;
    int peak_x = 0, peak_y = 0;
    
    for (int dy = -max_shift; dy <= max_shift; dy++) {
        for (int dx = -max_shift; dx <= max_shift; dx++) {
            int cy = (dy + FFT_SIZE) % FFT_SIZE;
            int cx = (dx + FFT_SIZE) % FFT_SIZE;
            
            float val = corr[cy * FFT_SIZE + cx];
            
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
    int max_shift
) {
    struct phase_result result = {0};
    struct fft_thread_resources *res = &tls_resources;
    
    int rw = rx2 - rx1;
    int rh = ry2 - ry1;
    
    if (rw < 16 || rh < 16) return result;
    if (phase_correlate_init() < 0) return result;
    
    /* Extract regions with Hann windowing */
    extract_region_windowed(curr_buf, buf_width, rx1, ry1, rx2, ry2, res->fft_in1);
    extract_region_windowed(prev_buf, buf_width, rx1, ry1, rx2, ry2, res->fft_in2);
    
    /* Smooth both buffers to sharpen FFT correlation peak */
    smooth_buffer(res->fft_in1, res->smooth_tmp);
    smooth_buffer(res->fft_in2, res->smooth_tmp);
    
    /* Forward FFT */
    fftwf_execute(res->plan_fwd1);
    fftwf_execute(res->plan_fwd2);
    
    /* Cross-power spectrum */
    int fft_complex_size = (FFT_SIZE/2 + 1) * FFT_SIZE;
    compute_phase_correlation(res->fft_out1, res->fft_out2, res->fft_cross, fft_complex_size);
    
    /* Inverse FFT to get correlation surface */
    fftwf_execute(res->plan_inv);
    
    /* Limit search range */
    if (max_shift > rw / 2) max_shift = rw / 2;
    if (max_shift > rh / 2) max_shift = rh / 2;
    if (max_shift < 1) max_shift = 1;
    
    /* Find peak */
    find_correlation_peak(res->fft_corr, &result.dx, &result.dy, max_shift);
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
