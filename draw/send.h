/*
 * send.h - Frame sending and send thread
 *
 * Handles queuing frames, the send thread main loop,
 * tile compression selection, and pipelined writes.
 *
 * The send thread runs independently, receiving frames from the
 * compositor thread via a double-buffered queue. It detects changed
 * tiles, compresses them (optionally using delta encoding), and
 * sends batched draw commands over 9P to the Plan 9 draw device.
 */

#ifndef SEND_H
#define SEND_H

#include <stdint.h>

/* Forward declarations */
struct server;

/* Tile size for change detection and compression */
#ifndef TILE_SIZE
#define TILE_SIZE 16
#endif

/*
 * Queue a frame for sending.
 *
 * Copies the current framebuffer to a send buffer and signals
 * the send thread. Uses double-buffering to avoid blocking
 * the compositor while the previous frame is being sent.
 *
 * Thread-safe: acquires s->send_lock internally.
 *
 * s: server state containing framebuffer and send thread context
 */
void send_frame(struct server *s);

/*
 * Timer callback for throttled frame sending.
 *
 * Called by the Wayland event loop at the configured frame rate.
 * Checks if there are pending changes (s->frame_dirty) and
 * triggers send_frame() if so.
 *
 * data: pointer to struct server
 *
 * Returns 0 (Wayland event loop convention for success).
 */
int send_timer_callback(void *data);

/*
 * Send thread main function.
 *
 * Runs in a dedicated thread, processing queued frames:
 * 1. Waits for a frame to be queued via send_frame()
 * 2. Detects scrolling regions (if enabled)
 * 3. Identifies changed tiles by comparing with prev_framebuf
 * 4. Compresses tiles using LZ77 and/or delta encoding
 * 5. Batches draw commands and sends via 9P
 * 6. Updates prev_framebuf for next frame's comparison
 *
 * arg: pointer to struct server
 *
 * Returns NULL on thread exit.
 */
void *send_thread_func(void *arg);

#endif /* SEND_H */
