/*
 * client.h - Client handling, decoration, and cleanup
 */

#ifndef P9WL_CLIENT_H
#define P9WL_CLIENT_H

#include <stdbool.h>
#include "../types.h"

/* Handler for new XDG decoration */
void handle_new_decoration(struct wl_listener *listener, void *data);

/* Clean up server resources */
void server_cleanup(struct server *s);

#endif /* P9WL_CLIENT_H */
