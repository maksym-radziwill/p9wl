/*
 * clipboard.h - Wayland clipboard <-> Plan 9 /dev/snarf integration
 *
 * Provides bidirectional clipboard synchronization:
 *   - Wayland client copies (Ctrl+C) → writes to /dev/snarf
 *   - Wayland client pastes (Ctrl+V) → reads from /dev/snarf
 *
 * Design: All 9P I/O is performed asynchronously to avoid blocking
 * the Wayland event loop. Snarf is treated as the single source of
 * truth for clipboard contents.
 *
 * Note: Primary selection (highlight-to-copy) is NOT synced to snarf
 * to avoid overwriting the clipboard on every text selection.
 */

#ifndef P9WL_CLIPBOARD_H
#define P9WL_CLIPBOARD_H

struct server;

/*
 * Initialize clipboard handling.
 *
 * Sets up listeners for Wayland selection events and registers
 * as the initial selection owner so pastes read from /dev/snarf.
 *
 * @param s  Server instance (must have seat and p9_snarf initialized)
 * @return   0 on success, -1 on failure
 */
int clipboard_init(struct server *s);

/*
 * Clean up clipboard resources.
 *
 * Removes Wayland event listeners. Should be called during
 * server shutdown.
 *
 * @param s  Server instance
 */
void clipboard_cleanup(struct server *s);

#endif /* P9WL_CLIPBOARD_H */
