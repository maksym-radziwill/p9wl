/*
 * wl_input.h - Wayland input event processing
 *
 * This module bridges Plan 9 input events to Wayland clients. Input is
 * read from Plan 9 devices (/dev/mouse, /dev/kbd) by background threads,
 * queued via the input_queue, and processed here on the main Wayland
 * event loop thread.
 *
 * Architecture:
 *
 *   Plan 9 Input Threads          Main Thread
 *   ----------------------        -----------
 *   mouse_thread reads            handle_input_events()
 *   /dev/mouse, pushes to  --->   pops events from queue,
 *   input_queue                   calls handle_mouse()
 *
 *   kbd_thread reads               handle_key() processes
 *   /dev/kbd, pushes to    --->   keyboard events
 *   input_queue
 *
 * Mouse Handling:
 *
 *   handle_mouse() translates Plan 9 mouse events (absolute position,
 *   button bitmask) to Wayland pointer events:
 *
 *     - Cursor position updates via wlr_cursor_warp_absolute()
 *       (coordinates clamped to visible window area, not padded buffer)
 *     - Pointer focus changes via focus_handle_click()
 *     - Button press/release via wlr_seat_pointer_notify_button()
 *     - Scroll wheel via wlr_seat_pointer_notify_axis()
 *
 *   The button mapping uses a table-driven approach:
 *     Bit 0 (1)  -> BTN_LEFT
 *     Bit 1 (2)  -> BTN_MIDDLE
 *     Bit 2 (4)  -> BTN_RIGHT
 *     Bits 3-6   -> Scroll directions
 *
 *   Scroll source:
 *     Plan 9's /dev/mouse delivers scroll as discrete button bits
 *     regardless of the physical device. All scroll events are
 *     reported as SOURCE_WHEEL with value120 discrete steps.
 *
 * Keyboard Handling:
 *
 *   handle_key() translates Plan 9 runes (Unicode codepoints or special
 *   key codes) to Wayland key events:
 *
 *     - Escape key can dismiss grabbed popups
 *     - Modifier keys update the focus manager's modifier state
 *     - Regular keys are looked up in the keymap
 *     - Key events sent via wlr_seat_keyboard_notify_key()
 *
 * Usage:
 *
 *   Register handle_input_events as a pipe event source:
 *
 *     server->input_event = wl_event_loop_add_fd(loop,
 *         server->input_queue.pipe_fd[0],
 *         WL_EVENT_READABLE,
 *         handle_input_events,
 *         server);
 */

#ifndef P9WL_WL_INPUT_H
#define P9WL_WL_INPUT_H

#include <stdint.h>
#include "../types.h"

/* ============== Keyboard Handling ============== */

/*
 * Process a keyboard event from Plan 9.
 *
 * The rune parameter is either a Unicode codepoint (for printable
 * characters) or a Plan 9 special key code (Kxxx constants).
 *
 * For modifier keys, updates the focus manager's modifier state.
 * For regular keys, looks up the keycode in the keymap
 * and sends the appropriate Wayland key event.
 *
 * If pressed is true and rune is Escape (0x1B), attempts to dismiss
 * the topmost grabbed popup before processing as a regular key.
 *
 * s:       server instance
 * rune:    Plan 9 rune (Unicode codepoint or Kxxx constant)
 * pressed: 1 for key press, 0 for key release
 */
void handle_key(struct server *s, uint32_t rune, int pressed);

/* ============== Mouse Handling ============== */

/*
 * Process a mouse event from Plan 9.
 *
 * Coordinates (mx, my) are in Plan 9 screen coordinates. They are
 * translated to window-local coordinates using draw.win_minx/miny.
 *
 * The buttons parameter is a bitmask:
 *   Bit 0: Left button
 *   Bit 1: Middle button
 *   Bit 2: Right button
 *   Bit 3: Scroll up
 *   Bit 4: Scroll down
 *   Bit 5: Scroll left
 *   Bit 6: Scroll right
 *
 * Handles focus changes (click-to-focus), button events, scroll events,
 * and pointer motion. Popup dismissal occurs when clicking outside the
 * popup stack.
 *
 * s:       server instance
 * mx:      X coordinate in Plan 9 screen coordinates
 * my:      Y coordinate in Plan 9 screen coordinates
 * buttons: button bitmask
 */
void handle_mouse(struct server *s, int mx, int my, int buttons);

/* ============== Event Loop Integration ============== */

/*
 * Main loop callback for input events.
 *
 * Called by the Wayland event loop when the input queue pipe is readable.
 * Drains the pipe and processes all queued input events by calling
 * handle_mouse() or handle_key() as appropriate.
 *
 * fd:   file descriptor (input_queue.pipe_fd[0])
 * mask: event mask (WL_EVENT_READABLE)
 * data: pointer to struct server
 *
 * Returns 0 to continue receiving events.
 */
int handle_input_events(int fd, uint32_t mask, void *data);

#endif /* P9WL_WL_INPUT_H */
