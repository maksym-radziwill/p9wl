/*
 * phase_correlate.h - FFT-based scroll detection
 *
 * Detects translation (scrolling) between two image regions using
 * phase correlation in the frequency domain. This allows the compositor
 * to send efficient 'd' (copy) commands instead of retransmitting pixels.
 *
 * Algorithm:
 *
 *   Phase correlation detects translation by:
 *     1. Extract corresponding regions from current and previous frame
 *     2. Apply Hann window to reduce edge effects
 *     3. Compute 2D FFT of both regions
 *     4. Compute normalized cross-power spectrum: F1 * conj(F2) / |F1 * conj(F2)|
 *     5. Inverse FFT to get correlation surface
 *     6. Find peak location - offset from center gives (dx, dy)
 *
 * Implementation:
 *
 *   Uses single-precision FFTW (fftwf) for performance.
 *   Thread-local storage for FFT buffers with automatic cleanup.
 *   Hann window is precomputed once via pthread_once().
 *
 *   FFTW plans are created with FFTW_MEASURE for optimal performance.
 *   Plan creation is serialized via mutex (FFTW requirement).
 *
 * Thread Safety:
 *
 *   Each thread gets its own FFT resources via pthread_key_t:
 *     - fft_in1, fft_in2: input buffers (FFT_SIZE x FFT_SIZE floats)
 *     - fft_out1, fft_out2: FFT output (complex)
 *     - fft_cross: cross-power spectrum (complex)
 *     - fft_corr: correlation surface (real)
 *     - Forward and inverse FFT plans
 *
 *   Resources are automatically freed when threads exit via
 *   pthread_key destructor.
 *
 * Accuracy vs Speed:
 *
 *   FFT_SIZE determines accuracy and speed:
 *     - Larger = more accurate but slower
 *     - 256 is a good balance for scroll detection
 *     - Must be power of 2
 */

#ifndef PHASE_CORRELATE_H
#define PHASE_CORRELATE_H

#include <stdint.h>

/* FFT size - must be power of 2, larger = more accurate but slower */
#define FFT_SIZE 256

/*
 * Maximum scroll distance to detect (pixels).
 *
 * NOTE: This uses MAX_SCROLL_REGIONS which should be defined in types.h.
 * Typical value is FFT_SIZE/2 = 128 pixels.
 */
#define MAX_SCROLL_PIXELS (MAX_SCROLL_REGIONS / 2)

/* ============== Data Structures ============== */

/*
 * Result of phase correlation.
 *
 * dx:    detected horizontal shift (positive = content moved right)
 * dy:    detected vertical shift (positive = content moved down)
 * valid: non-zero if detection succeeded, 0 if region too small
 */
struct phase_result {
    int dx;
    int dy;
    int valid;
};

/* ============== API Functions ============== */

/*
 * Initialize thread-local FFT resources.
 *
 * Called automatically on first use by phase_correlate_detect().
 * Allocates FFTW buffers and creates FFT plans.
 *
 * Resources are thread-local and automatically freed when the
 * calling thread exits.
 *
 * Returns 0 on success, -1 on allocation failure.
 */
int phase_correlate_init(void);

/*
 * Detect scroll between current and previous frame regions.
 *
 * Computes phase correlation between the same region in two frames
 * to detect translation (scrolling).
 *
 * curr_buf:  current frame pixel buffer (XRGB32)
 * prev_buf:  previous frame pixel buffer (XRGB32)
 * buf_width: width of both buffers in pixels (stride)
 * rx1, ry1:  top-left corner of region to analyze
 * rx2, ry2:  bottom-right corner of region (exclusive)
 * max_shift: maximum scroll distance to search (pixels)
 *
 * Region must be at least 16x16 pixels. If smaller, returns
 * result with valid=0.
 *
 * The detected shift is relative to previous frame:
 *   - dx > 0: content moved right (scroll left occurred)
 *   - dy > 0: content moved down (scroll up occurred)
 *
 * Returns phase_result with detected shift and validity flag.
 */
struct phase_result phase_correlate_detect(
    uint32_t *curr_buf, uint32_t *prev_buf, int buf_width,
    int rx1, int ry1, int rx2, int ry2,
    int max_shift
);

/*
 * Free all FFT resources.
 *
 * Calls fftwf_cleanup() to release FFTW global state.
 * Thread-local resources are freed automatically when threads exit.
 *
 * Call at program shutdown for clean resource release.
 */
void phase_correlate_cleanup(void);

#endif /* PHASE_CORRELATE_H */
