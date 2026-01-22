/*
 * clipboard.h - Wayland clipboard <-> Plan 9 /dev/snarf integration
 */

#ifndef P9WL_CLIPBOARD_H
#define P9WL_CLIPBOARD_H

#include <wayland-server-core.h>

struct server;
struct p9conn;

/* Initialize clipboard handling */
int clipboard_init(struct server *s);

/* Clean up clipboard resources */
void clipboard_cleanup(struct server *s);

/* Read from /dev/snarf into buffer. Returns length or -1 on error. */
int snarf_read(struct p9conn *p9, char *buf, size_t bufsize);

/* Write to /dev/snarf. Returns 0 on success, -1 on error. */
int snarf_write(struct p9conn *p9, const char *data, size_t len);

/* Sync snarf contents to Wayland clipboard (call before paste operations) */
void clipboard_set_from_snarf(struct server *s);

#endif /* P9WL_CLIPBOARD_H */
