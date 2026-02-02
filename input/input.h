/*
 * input.h - Input handling for Plan 9 to Wayland bridge
 *
 * This module handles:
 *   - Keyboard input: Reading from /dev/cons, translating Plan 9 runes
 *     to Linux keycodes, and injecting into Wayland
 *   - Mouse input: Reading from /dev/mouse and forwarding to Wayland
 *   - Window control: Monitoring /dev/wctl for geometry changes
 *   - Input queue: Thread-safe queue for passing events to main loop
 *
 * Key Translation:
 *
 *   Plan 9 sends UTF-8 runes (characters) while Wayland/Linux expects
 *   keycodes (physical keys). This module provides both static and
 *   dynamic (via /dev/kbmap) translation tables.
 *
 * Usage:
 *
 *   Initialize the input queue before starting threads:
 *
 *     input_queue_init(&server->input_queue);
 *
 *   Start input threads:
 *
 *     pthread_create(&server->mouse_thread, NULL, mouse_thread_func, server);
 *     pthread_create(&server->kbd_thread, NULL, kbd_thread_func, server);
 *     pthread_create(&server->wctl_thread, NULL, wctl_thread_func, server);
 *
 *   Key lookup with dynamic kbmap fallback:
 *
 *     const struct key_map *km = keymap_lookup_dynamic(&server->kbmap, rune);
 *     if (km) {
 *         // Use km->keycode, km->shift, km->ctrl
 *     }
 */

#ifndef P9WL_INPUT_H
#define P9WL_INPUT_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <wlr/types/wlr_keyboard.h>

#include "../types.h"       /* For struct server, input_queue, input_event */
#include "plan9_keys.h"     /* Canonical Plan 9 key definitions */

/* Forward declaration */
struct kbmap;

/* ============== Key Mapping ============== */

/*
 * Key mapping entry: maps Plan 9 rune to Linux keycode.
 *
 * When translating a rune to a key event:
 *   1. Send shift press if shift=1
 *   2. Send ctrl press if ctrl=1
 *   3. Send keycode press/release
 *   4. Send modifier releases
 */
struct key_map {
    int rune;           /* Plan 9 rune value (Unicode codepoint or Kxxx) */
    int keycode;        /* Linux keycode (KEY_* from input-event-codes.h) */
    int shift;          /* 1 if shift modifier required, 0 otherwise */
    int ctrl;           /* 1 if ctrl modifier required, 0 otherwise */
};

/*
 * Static keymap table (US QWERTY layout fallback).
 * Null-terminated array defined in input.c.
 */
extern const struct key_map keymap[];

/* ============== Keymap Lookup Functions ============== */

/*
 * Look up a key mapping by rune using the static table.
 *
 * This is the fallback when dynamic kbmap is unavailable.
 * Prefer keymap_lookup_dynamic().
 *
 * rune: Plan 9 rune value to look up
 *
 * Returns pointer to static key_map entry, or NULL if not found.
 * Logs error for unmapped runes >= 0x80 or special keys.
 */
const struct key_map *keymap_lookup(uint32_t rune);

/*
 * Look up a key mapping using dynamic kbmap with static fallback.
 *
 * Checks the dynamic kbmap first (loaded from /dev/kbmap), then
 * falls back to the static keymap table if not found or if kbmap
 * is not loaded.
 *
 * km:   pointer to dynamic kbmap (may be NULL)
 * rune: Plan 9 rune value to look up
 *
 * Returns pointer to key_map entry, or NULL if not found.
 *
 * Note: For dynamic lookups, returns pointer to static internal
 * storage that is overwritten on each call. Copy if needed.
 */
const struct key_map *keymap_lookup_dynamic(struct kbmap *km, uint32_t rune);

/*
 * Get wlroots modifier mask for a modifier rune.
 *
 * Used to track which modifiers are currently held down.
 *
 * rune: Plan 9 modifier rune (Kshift, Kctl, Kalt, etc.)
 *
 * Returns WLR_MODIFIER_* mask, or 0 if not a modifier.
 */
uint32_t keymapmod(int rune);

/* ============== UTF-8 Decoding ============== */

/*
 * Decode a single UTF-8 rune from a byte buffer.
 *
 * p:    pointer to start of UTF-8 sequence
 * end:  pointer past end of buffer (for bounds checking)
 * rune: output - decoded Unicode codepoint
 *
 * Returns number of bytes consumed (1-4), or 0 on error/truncation.
 */
int utf8_decode(const unsigned char *p, const unsigned char *end, int *rune);

/* ============== Input Queue ============== */

/*
 * Initialize an input queue.
 *
 * Initializes the mutex. Must be called before any push/pop.
 * Initializes pipe read by wl_event_loop_add_fd  
 * q: queue to initialize
 */
void input_queue_init(struct input_queue *q);

/*
 * Push an event onto the input queue.
 *
 * Thread-safe. Events are silently dropped if queue is full.
 *
 * q:  queue to push to
 * ev: event to copy into queue
 */
void input_queue_push(struct input_queue *q, struct input_event *ev);

/*
 * Pop an event from the input queue.
 *
 * Thread-safe. Non-blocking - returns immediately if empty.
 *
 * q:  queue to pop from
 * ev: output - event copied from queue
 *
 * Returns 1 if event was popped, 0 if queue was empty.
 */
int input_queue_pop(struct input_queue *q, struct input_event *ev);

/* ============== Input Threads ============== */

/*
 * Mouse input thread function.
 *
 * Reads from /dev/mouse in a loop, parses Plan 9 mouse format,
 * and pushes INPUT_MOUSE events to the queue.
 *
 * arg: pointer to struct server
 *
 * Returns NULL (runs until server->running becomes false).
 */
void *mouse_thread_func(void *arg);

/*
 * Keyboard input thread function.
 *
 * Reads from /dev/cons in a loop, handles 'c', 'k', and 'K' message
 * types, and pushes INPUT_KEY events to the queue.
 *
 * Handles modifier tracking to avoid duplicate events when both
 * 'c' (cooked) and 'k'/'K' (raw) messages arrive.
 *
 * arg: pointer to struct server
 *
 * Returns NULL (runs until server->running becomes false).
 */
void *kbd_thread_func(void *arg);

/*
 * Window control monitoring thread function.
 *
 * Polls /dev/wctl for geometry changes. When the window is resized
 * or moved, sets server->window_changed flag and signals the send
 * condition variable.
 *
 * Also detects hidden->visible transitions to trigger full redraws.
 *
 * arg: pointer to struct server
 *
 * Returns NULL (runs until server->running becomes false).
 */
void *wctl_thread_func(void *arg);

/*
 * Note: Time utility functions now_ms() and now_us() are defined
 * as static inline in types.h to avoid multiple definition issues.
 */

#endif /* P9WL_INPUT_H */
