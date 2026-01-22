/*
 * output.h - Output and input device handlers
 */

#ifndef P9WL_OUTPUT_H
#define P9WL_OUTPUT_H

#include "../types.h"

/* Handler for new output creation */
void new_output(struct wl_listener *l, void *d);

/* Handler for new input device */
void new_input(struct wl_listener *l, void *d);

#endif /* P9WL_OUTPUT_H */
