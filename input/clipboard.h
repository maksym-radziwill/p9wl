/*
 * clipboard.h - Wayland clipboard <-> Plan 9 /dev/snarf integration
 */

#ifndef P9WL_CLIPBOARD_H
#define P9WL_CLIPBOARD_H

struct server;

/* Initialize clipboard handling */
int clipboard_init(struct server *s);

/* Clean up clipboard resources */
void clipboard_cleanup(struct server *s);

#endif /* P9WL_CLIPBOARD_H */
