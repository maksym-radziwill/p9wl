/*
 * send.h - Frame sending and send thread
 *
 * Handles queuing frames, the send thread main loop,
 * tile compression selection, and pipelined writes.
 */

#ifndef SEND_H
#define SEND_H

#include <stdint.h>

/* Forward declarations */
struct server;

/* Tile size for change detection and compression */
#define TILE_SIZE 16

/* Alpha-delta overhead (bytes) for compressed delta tiles */
#define ALPHA_DELTA_OVERHEAD 50

/*
 * Queue a frame for sending.
 * Copies the framebuffer and signals the send thread.
 */
void send_frame(struct server *s);

/*
 * Timer callback for throttled frame sending.
 * Returns 0 (Wayland event loop convention).
 */
int send_timer_callback(void *data);

/*
 * Check if a tile has changed since last send.
 * Returns non-zero if tile at (tx,ty) differs from prev_framebuf.
 */
int tile_changed_send(struct server *s, uint32_t *send_buf, int tx, int ty);

/*
 * Send thread main function.
 * Processes queued frames, compresses tiles, sends via 9P.
 */
void *send_thread_func(void *arg);

/*
 * Get current timestamp in milliseconds.
 */
uint32_t now_ms(void);

/*
 * Get current timestamp in microseconds.
 */
uint64_t now_us(void);

#endif /* SEND_H */
