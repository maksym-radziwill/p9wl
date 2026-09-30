/*
 * clipboard.c - FreeRDP cliprdr integration (no Plan 9 /dev/snarf backend)
 *
 * Syncs the FreeRDP client clipboard directly:
 * - When the FreeRDP client copies, capture data via cliprdr callback
 * - When requested, push updates back to the FreeRDP client
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <freerdp/log.h>
#include <freerdp/channels/cliprdr.h>
#include "types.h"

#define TAG FREERDP_TAG("p9wl.clipboard")

#define CLIPBOARD_MAX_SIZE (1024 * 1024)  /* 1MB max clipboard */

/* ─────────────────────────────────────────────────────────────────────────────
 * FreeRDP Clipboard Handling
 * ───────────────────────────────────────────────────────────────────────────── */

void rdp_to_local_write(struct server *s, const void *data, size_t size) {
    if (!data || size == 0) return;
    
    if (size >= CLIPBOARD_MAX_SIZE) {
        size = CLIPBOARD_MAX_SIZE - 1;
    }

    char *buf = malloc(size + 1);
    if (!buf) {
        WLog_ERR(TAG, "rdp_to_local: failed to allocate buffer");
        return;
    }
    
    memcpy(buf, data, size);
    buf[size] = '\0';
    
    WLog_INFO(TAG, "rdp_to_local: received %zu bytes from RDP client", size);
    
    /* TODO: Handle local clipboard storage/forwarding if needed */
    
    free(buf);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Public API
 * ───────────────────────────────────────────────────────────────────────────── */

int clipboard_init(struct server *s) {
    (void)s;
    WLog_INFO(TAG, "clipboard: initialized");
    return 0;
}

void clipboard_cleanup(struct server *s) {
    (void)s;
}