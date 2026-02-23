/*
 * phase_correlate.h - FFT-based scroll detection
 *
 * Detects translation (scrolling) between two image regions using
 * phase correlation in the frequency domain. This allows the compositor
 * to send efficient 'd' (copy) commands instead of retransmitting pixels.
 *
 * Algorithm Overview:
 *
 *   Phase correlation detects translation by finding the peak in the
 *   cross-power spectrum of two images:
 *
 *     1. Extract corresponding regions from current and previous frame
 *     2. Apply Hann window to reduce edge effects (spectral leakage)
 *     3. Compute 2D FFT of both regions â†' F1, F2
 *     4. Compute normalized cross-power spectrum:
 *          C = (F1 * conj(F2)) / |F1 * conj(F2)|
 *     5. Inverse FFT of C â†' correlation surface
 *     6. Find peak location â†' offset from center gives (dx, dy)
 *
 *   The normalization step (dividing by magnitude) makes the algorithm
 *   robust to brightness and contrast differences between frames.
 *
 * Hann Windowing:
 *
 *   Before FFT, pixels are multiplied by a Hann (raised cosine) window:
 *
 *     w(n) = 0.5 * (1 - cos(2Ï€n / (N-1)))
 *
 *   This smoothly tapers values to zero at the edges, preventing the
 *   discontinuity at image boundaries from creating spectral artifacts.
 *   The Hann lookup table is computed once and reused.
 *
 * Implementation Details:
 *
 *   Uses single-precision FFTW (fftwf) for performance:
 *     - FFT_SIZE x FFT_SIZE real-to-complex forward transforms
 *     - Complex-to-real inverse transform for correlation
 *     - FFTW_MEASURE flag for optimal FFT planning
 *
 *   Thread-local storage for FFT buffers:
 *     - Each thread gets its own set of buffers and plans
 *     - Avoids contention when multiple workers call concurrently
 *     - Automatic cleanup when threads exit (pthread_key destructor)
 *
 *   FFTW plan creation is serialized:
 *     - fftw_plan_* functions are not thread-safe
 *     - Uses fftw_plan_lock mutex during plan creation
 *     - Plans are reused after creation (no per-call overhead)
 *
 * Thread-Local Resources:
 *
 *   Each thread allocates via get_thread_resources():
 *
 *     fft_in1, fft_in2:   Input buffers (FFT_SIZE² floats each)
 *     fft_out1, fft_out2: FFT output (complex, (FFT_SIZE/2+1)*FFT_SIZE each)
 *     fft_cross:          Cross-power spectrum (complex)
 *     fft_corr:           Correlation surface (FFT_SIZE² floats)
 *     plan_fwd1, plan_fwd2: Forward FFT plans
 *     plan_inv:           Inverse FFT plan
 *
 *   Total per-thread allocation: ~1.5 MB for FFT_SIZE=256
 *
 * Accuracy vs Speed:
 *
 *   FFT_SIZE determines the accuracy/speed tradeoff:
 *
 *     Size  | Memory/thread | Accuracy | Speed
 *     ------+---------------+----------+-------
 *      64   |    ~80 KB     |   Low    | Fast
 *     128   |   ~320 KB     |  Medium  | Medium
 *     256   |   ~1.5 MB     |   High   | Slower
 *     512   |   ~5.2 MB     |  V.High  | Slow
 *
 *   256 is the default, providing good accuracy for detecting scrolls
 *   of 1-128 pixels while remaining fast enough for real-time use.
 *
 * Limitations:
 *
 *   - Only detects pure translation (no rotation, scaling, or shear)
 *   - Region must be at least 16x16 pixels
 *   - Maximum detectable shift is limited by max_shift parameter
 *   - Requires pixel-accurate alignment (fails with fractional scaling)
 *   - Content must have sufficient texture for correlation
 */

#ifndef PHASE_CORRELATE_H
#define PHASE_CORRELATE_H

#include <stdint.h>

/* ============== Constants ============== */

/*
 * FFT size for phase correlation.
 *
 * Must be a power of 2. Larger sizes give more accuracy but are slower
 * and use more memory. 256 is a good balance for scroll detection.
 *
 * Memory usage per thread: ~24 * FFT_SIZE² bytes
 */
#define FFT_SIZE 256

/*
 * Maximum detectable scroll distance.
 *
 * The correlation peak can only be found within [-max, +max] of center.
 * Typically FFT_SIZE/2 = 128 pixels, but can be limited further by
 * the max_shift parameter to phase_correlate_detect().
 */
#define MAX_SCROLL_DETECT (FFT_SIZE / 2)

/* ============== Data Structures ============== */

/*
 * Result of phase correlation detection.
 *
 * dx:    detected horizontal shift in pixels
 *        Positive = content moved right (user scrolled left)
 *        Negative = content moved left (user scrolled right)
 *
 * dy:    detected vertical shift in pixels
 *        Positive = content moved down (user scrolled up)
 *        Negative = content moved up (user scrolled down)
 *
 * valid: non-zero if detection succeeded
 *        0 if region was too small or resources unavailable
 *
 * Note: The shift describes how content moved, not the scroll direction.
 * To scroll content back, apply the inverse: copy from (x-dx, y-dy).
 */
struct phase_result {
    int dx;
    int dy;
    int valid;
};

/* ============== API Functions ============== */

/*
 * Detect scroll between current and previous frame regions.
 *
 * Computes phase correlation between the same rectangular region in
 * two frame buffers to detect translation (scrolling).
 *
 * curr_buf:  current frame pixel buffer (XRGB32 format)
 * prev_buf:  previous frame pixel buffer (XRGB32 format)
 * buf_width: width of both buffers in pixels (stride in pixels)
 * rx1, ry1:  top-left corner of region to analyze
 * rx2, ry2:  bottom-right corner of region (exclusive)
 * max_shift: maximum scroll distance to search (pixels)
 *
 * The region (rx1,ry1)-(rx2,ry2) must:
 *   - Be at least 16x16 pixels
 *   - Be within both buffer bounds
 *   - Have the same coordinates in both buffers
 *
 * max_shift limits the search range:
 *   - Smaller values = faster but may miss large scrolls
 *   - Larger values = slower but catches larger scrolls
 *   - Clamped to region_size/2 and FFT_SIZE/2 internally
 *
 * Thread-local FFT resources are allocated on first call per thread.
 * Subsequent calls reuse existing resources.
 *
 * Returns phase_result:
 *   - valid=1, dx/dy set: scroll detected
 *   - valid=0: region too small or allocation failed
 *   - dx=0, dy=0 with valid=1: no scroll detected (static content)
 */
struct phase_result phase_correlate_detect(
    uint32_t *curr_buf, uint32_t *prev_buf, int buf_width,
    int rx1, int ry1, int rx2, int ry2,
    int max_shift
);

/*
 * Free global FFT resources.
 *
 * Calls fftwf_cleanup() to release FFTW's global state (wisdom,
 * configuration data). Thread-local buffers and plans are freed
 * automatically when threads exit via pthread_key destructor.
 *
 * Call at program shutdown for clean resource release.
 * Safe to call multiple times (fftwf_cleanup is idempotent).
 *
 * Note: After this call, any remaining threads with FFT resources
 * will still clean up properly when they exit.
 */
void phase_correlate_cleanup(void);

#endif /* PHASE_CORRELATE_H */
