/*
 * wl_input.h - Wayland input event handling
 */

#ifndef P9WL_WL_INPUT_H
#define P9WL_WL_INPUT_H

#include <stdint.h>
#include "../types.h"

/* Handle keyboard input from Plan 9 */
void handle_key(struct server *s, uint32_t rune, int pressed);

/* Handle mouse input from Plan 9 */
void handle_mouse(struct server *s, int mx, int my, int buttons);

/* Main loop callback for input events from input thread */
int handle_input_events(int fd, uint32_t mask, void *data);

#endif /* P9WL_WL_INPUT_H */
