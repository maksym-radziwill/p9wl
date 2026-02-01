/*
 * output.h - Output and input device management
 *
 * This module handles Wayland output (display) creation and the frame
 * rendering loop, as well as input device attachment.
 *
 * Output Handling:
 *
 *   new_output() is called when wlroots creates a new output. For p9wl,
 *   there is typically one headless output sized to match the Plan 9
 *   window. The output is configured with:
 *
 *     - Custom mode matching the Plan 9 window dimensions
 *     - Optional HiDPI scale factor
 *     - Scene graph output for rendering
 *
 * Frame Loop:
 *
 *   The output_frame handler (internal) runs the compositor's render loop:
 *
 *     1. Check for pending resize from wctl thread
 *     2. Reallocate framebuffers and Plan 9 images if resized
 *     3. Build scene output state via wlroots
 *     4. Copy rendered pixels to the framebuffer
 *     5. Trigger send_frame() to transmit to Plan 9
 *
 * Input Device Handling:
 *
 *   new_input() attaches pointer devices to the cursor. Keyboard input
 *   comes from Plan 9 via the input threads, not from Wayland input
 *   devices, so keyboard devices are ignored.
 *
 * Usage:
 *
 *   Wire up handlers during server initialization:
 *
 *     server->new_output.notify = new_output;
 *     wl_signal_add(&backend->events.new_output, &server->new_output);
 *
 *     server->new_input.notify = new_input;
 *     wl_signal_add(&backend->events.new_input, &server->new_input);
 */

#ifndef P9WL_OUTPUT_H
#define P9WL_OUTPUT_H

#include "../types.h"

/*
 * Handle new output creation.
 *
 * Initializes rendering for the output, sets the custom mode to match
 * the Plan 9 window size, adds it to the output layout, and sets up
 * frame and destroy listeners.
 *
 * For HiDPI displays (scale > 1.0), configures the output scale and
 * logs both physical and logical dimensions.
 */
void new_output(struct wl_listener *l, void *d);

/*
 * Handle new input device.
 *
 * For pointer devices, attaches them to the server's cursor so that
 * cursor motion events are properly aggregated.
 *
 * Keyboard and other device types are currently ignored since input
 * comes from Plan 9 rather than local devices.
 */
void new_input(struct wl_listener *l, void *d);

#endif /* P9WL_OUTPUT_H */
