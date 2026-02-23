/*
 * clipboard.h - Wayland clipboard <-> Plan 9 /dev/snarf integration
 *
 * Provides bidirectional clipboard synchronization:
 *   - Wayland client copies (Ctrl+C) -> writes to /dev/snarf
 *   - Wayland client pastes (Ctrl+V) -> reads from /dev/snarf
 *   - Plan 9 snarf changes          -> detected via qid.vers polling
 *
 * Design:
 *
 *   Snarf is treated as the single source of truth for
 *   clipboard contents.
 *
 *   Supported MIME types for text:
 *     - text/plain;charset=utf-8
 *     - text/plain
 *     - UTF8_STRING, STRING, TEXT (X11 compatibility)
 *
 *   Maximum clipboard size: 1MB (SNARF_MAX_SIZE)
 *
 * Wayland -> Snarf (Copy):
 *
 *   When a Wayland client sets the selection (copies):
 *     1. on_wayland_copy() handler is called
 *     2. Client becomes selection owner (protocol requirement)
 *     3. Async read via Wayland event loop fd
 *     4. Data written to /dev/snarf via p9_write_file()
 *     5. Snarf poll version updated to avoid false change detection
 *     6. Compositor reclaims selection ownership
 *     7. Future pastes (even Wayland-to-Wayland) go through snarf
 *
 * Snarf -> Wayland (Paste):
 *
 *   The compositor registers as selection owner, so paste requests
 *   come to us:
 *     1. Client requests paste via wlr_data_source_send()
 *     2. snarf_to_wayland_send() spawns detached thread
 *     3. Thread reads from /dev/snarf (blocking OK in thread)
 *     4. Thread writes to client fd and closes it
 *
 *   This async approach prevents blocking the compositor on 9P I/O.
 *
 * Snarf Version Polling:
 *
 *   Rio's /dev/snarf exposes a version counter via qid.vers that
 *   increments on each write. A dedicated thread polls this with
 *   Tstat every 500ms to detect Plan 9-side clipboard changes
 *   (e.g., user copies text in a rio window). When a change is
 *   detected, the thread signals the main event loop via a pipe:
 *
 *     1. Poll thread: Tstat returns new qid.vers
 *     2. Poll thread writes to pipe
 *     3. Event loop wakes, calls snarf_to_wayland_register()
 *     4. wlr_seat_set_selection() emits selection event to clients
 *     5. Clients invalidate cached clipboard data
 *     6. Next paste triggers fresh read from /dev/snarf
 *
 *   The blocking Tstat RPC runs entirely in the poll thread, so the
 *   Wayland event loop is never stalled by 9P I/O.
 *
 * Primary Selection:
 *
 *   Primary selection (highlight-to-copy, middle-click paste) is NOT
 *   synced to snarf. This is intentional - primary selection changes
 *   on every text highlight, which would overwrite the clipboard
 *   unexpectedly. Only explicit Ctrl+C copies go to snarf.
 *
 * Usage:
 *
 *   Initialize during server setup (after seat and p9_snarf are ready):
 *
 *     if (clipboard_init(server) < 0) {
 *         // handle error
 *     }
 *
 *   Clean up during shutdown:
 *
 *     clipboard_cleanup(server);
 */

#ifndef P9WL_CLIPBOARD_H
#define P9WL_CLIPBOARD_H

struct server;

/* ============== Initialization ============== */

/*
 * Initialize clipboard handling.
 *
 * Sets up:
 *   - Listener for Wayland copy events (request_set_selection)
 *   - Listener for primary selection (not synced to snarf)
 *   - Initial registration as selection owner
 *   - Snarf version polling thread (500ms interval)
 *
 * The snarf poll walks to /dev/snarf once (without opening) and
 * keeps the fid for periodic Tstat calls in a background thread.
 * If the initial stat fails, polling is disabled but the clipboard
 * still works for Wayland->Plan9 copies and the first Plan9->Wayland
 * paste.
 *
 * s: server instance (must have seat and p9_snarf initialized)
 *
 * Returns 0 on success, -1 on failure.
 */
int clipboard_init(struct server *s);

/*
 * Clean up clipboard resources.
 *
 * Stops the snarf polling thread, closes the notification pipe,
 * clunks the stat fid, and removes Wayland event listeners. Any
 * in-flight async paste threads will complete independently.
 *
 * s: server instance
 */
void clipboard_cleanup(struct server *s);

#endif /* P9WL_CLIPBOARD_H */
