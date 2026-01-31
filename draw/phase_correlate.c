/*
 * phase_correlate.c - FFT-based motion detection using phase correlation
 *
 * Detects translation (scrolling) between two image regions.
 *
 * Uses single-precision FFTW for performance.
 * Thread-local storage for parallel execution (assumes persistent worker threads).
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
    fftwf_complex *fft_out1;
    fftwf_complex *fft_out2;
    fftwf_complex *fft_cross;
    fftwf_plan plan_fwd1;
    fftwf_plan plan_fwd2;
    fftwf_plan plan_inv;
    int initialized;
};

static __thread struct fft_thread_resources tls = {0};
static pthread_mutex_t fftw_plan_lock = PTHREAD_MUTEX_INITIALIZER;

static struct fft_thread_resources *get_thread_resources(void) {
    if (tls.initialized) return &tls;
    
    init_hann_lut();
    
    tls.fft_in1 = fftwf_alloc_real(FFT_SIZE * FFT_SIZE);
    tls.fft_in2 = fftwf_alloc_real(FFT_SIZE * FFT_SIZE);
    tls.fft_corr = fftwf_alloc_real(FFT_SIZE * FFT_SIZE);
    tls.fft_out1 = fftwf_alloc_complex((FFT_SIZE/2 + 1) * FFT_SIZE);
    tls.fft_out2 = fftwf_alloc_complex((FFT_SIZE/2 + 1) * FFT_SIZE);
    tls.fft_cross = fftwf_alloc_complex((FFT_SIZE/2 + 1) * FFT_SIZE);
    
    if (!tls.fft_in1 || !tls.fft_in2 || !tls.fft_corr ||
        !tls.fft_out1 || !tls.fft_out2 || !tls.fft_cross) {
        return NULL;
    }
    
    pthread_mutex_lock(&fftw_plan_lock);
    tls.plan_fwd1 = fftwf_plan_dft_r2c_2d(FFT_SIZE, FFT_SIZE, tls.fft_in1, tls.fft_out1, FFTW_MEASURE);
    tls.plan_fwd2 = fftwf_plan_dft_r2c_2d(FFT_SIZE, FFT_SIZE, tls.fft_in2, tls.fft_out2, FFTW_MEASURE);
    tls.plan_inv = fftwf_plan_dft_c2r_2d(FFT_SIZE, FFT_SIZE, tls.fft_cross, tls.fft_corr, FFTW_MEASURE);
    pthread_mutex_unlock(&fftw_plan_lock);
    
    if (!tls.plan_fwd1 || !tls.plan_fwd2 || !tls.plan_inv) {
        return NULL;
    }
    
    tls.initialized = 1;
    return &tls;
}

static inline float pixel_to_gray(uint32_t pixel) {
    uint8_t r = (pixel >> 16) & 0xFF;
    uint8_t g = (pixel >> 8) & 0xFF;
    uint8_t b = pixel & 0xFF;
    return 0.299f * r + 0.587f * g + 0.114f * b;
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
    
    int rw = rx2 - rx1;
    int rh = ry2 - ry1;
    
    if (rw < 16 || rh < 16) return result;
    
    struct fft_thread_resources *res = get_thread_resources();
    if (!res) return result;
    
    /* Extract regions with Hann windowing */
    extract_region_windowed(curr_buf, buf_width, rx1, ry1, rx2, ry2, res->fft_in1);
    extract_region_windowed(prev_buf, buf_width, rx1, ry1, rx2, ry2, res->fft_in2);
    
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
    /* TLS resources are not explicitly freed - OS reclaims at exit.
     * This cleans up FFTW global state. */
    fftwf_cleanup();
}
