/*
 * toplevel.h - Toplevel and subsurface handlers
 */

#ifndef P9WL_TOPLEVEL_H
#define P9WL_TOPLEVEL_H

#include "../types.h"

/* Handler for new toplevel creation */
void new_toplevel(struct wl_listener *l, void *d);

#endif /* P9WL_TOPLEVEL_H */
