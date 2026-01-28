/*
 * clipboard.c - Wayland clipboard <-> Plan 9 /dev/snarf integration
 *
 * Syncs the Wayland clipboard with Plan 9's /dev/snarf:
 * - When a Wayland client copies, write to /dev/snarf
 * - When a Wayland client pastes, read from /dev/snarf (lazily, on demand)
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/types.h>

#include <wayland-server-core.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/util/log.h>

#include "clipboard.h"
#include "../types.h"
#include "../p9/p9.h"

#define SNARF_MAX_SIZE (1024 * 1024)  /* 1MB max clipboard */
#define SNARF_CHUNK_SIZE 8192         /* Read chunk size for async transfer */

/* Forward declaration */
static void snarf_to_wayland_register(struct server *s);

/* ─────────────────────────────────────────────────────────────────────────────
 * Mime type handling
 * ───────────────────────────────────────────────────────────────────────────── */

static const char *text_mime_types[] = {
    "text/plain;charset=utf-8",
    "text/plain",
    "UTF8_STRING",
    "STRING",
    "TEXT",
};

#define NUM_TEXT_MIME_TYPES (sizeof(text_mime_types) / sizeof(text_mime_types[0]))

/* Check if mime type is a text type we handle */
static bool is_text_mime_type(const char *mime) {
    if (!mime) return false;
    
    /* Handle text/plain with any charset variant */
    if (strncmp(mime, "text/plain", 10) == 0) {
        return true;
    }
    
    /* Check exact matches for X11-style types */
    for (size_t i = 2; i < NUM_TEXT_MIME_TYPES; i++) {
        if (strcmp(mime, text_mime_types[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Find first text mime type in a wl_array of mime type strings */
static const char *find_text_mime_type(struct wl_array *mime_types) {
    char **types = mime_types->data;
    size_t n_types = mime_types->size / sizeof(char*);
    
    for (size_t i = 0; i < n_types; i++) {
        if (is_text_mime_type(types[i])) {
            return types[i];
        }
    }
    return NULL;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Wayland → Snarf (client copies text)
 *
 * When a Wayland client copies, we capture the data and write it to snarf.
 * 
 * Ownership dance:
 *   1. Client copies  → Wayland makes client the "selection owner"
 *   2. We read data   → async via pipe, then write to snarf
 *   3. We reclaim     → register ourselves as owner again
 *   4. Future pastes  → all go through snarf, even Wayland-to-Wayland
 *
 * This keeps snarf as the single source of truth for the clipboard.
 * ───────────────────────────────────────────────────────────────────────────── */

/* State for async transfer from Wayland client to snarf */
struct wayland_to_snarf_transfer {
    struct server *server;
    struct wl_event_source *event_source;
    int fd;
    char *buf;
    size_t len;
    size_t capacity;
};

static int wayland_to_snarf_read_handler(int fd, uint32_t mask, void *data) {
    struct wayland_to_snarf_transfer *transfer = data;
    
    if (mask & WL_EVENT_READABLE) {
        char tmp[SNARF_CHUNK_SIZE];
        ssize_t n = read(fd, tmp, sizeof(tmp));
        if (n > 0) {
            if (transfer->len + n < transfer->capacity) {
                memcpy(transfer->buf + transfer->len, tmp, n);
                transfer->len += n;
            }
            return 0;  /* Keep listening */
        }
        /* n == 0 (EOF) or error - fall through to cleanup */
    }
    
    /* Step 2: Write captured data to snarf */
    if (transfer->len > 0) {
        transfer->buf[transfer->len] = '\0';
        int ret = p9_write_file(&transfer->server->p9_snarf, "snarf", 
                                transfer->buf, transfer->len);
        if (ret < 0) {
            wlr_log(WLR_ERROR, "wayland_to_snarf: write failed");
        } else {
            wlr_log(WLR_INFO, "wayland_to_snarf: copied %zu bytes", transfer->len);
        }
    } else {
        wlr_log(WLR_DEBUG, "wayland_to_snarf: no data received from source");
    }
    
    struct server *s = transfer->server;
    
    wl_event_source_remove(transfer->event_source);
    close(transfer->fd);
    free(transfer->buf);
    free(transfer);
    
    /* Step 3: Reclaim ownership so future pastes go through snarf */
    snarf_to_wayland_register(s);
    
    return 0;
}

/* Signal handler: Wayland client requests to set selection (copy) */
static void on_wayland_copy(struct wl_listener *listener, void *data) {
    struct server *s = wl_container_of(listener, s, wayland_to_snarf);
    struct wlr_seat_request_set_selection_event *event = data;
    
    wlr_log(WLR_DEBUG, "on_wayland_copy: source=%p", (void*)event->source);
    
    /* Step 1: Let client become selection owner (Wayland protocol requirement) */
    wlr_seat_set_selection(s->seat, event->source, event->serial);
    
    if (!event->source) {
        return;
    }
    
    /* Find a text mime type we can use */
    const char *mime = find_text_mime_type(&event->source->mime_types);
    if (!mime) {
        return;  /* Not text, skip */
    }
    
    wlr_log(WLR_DEBUG, "on_wayland_copy: using mime type: %s", mime);
    
    /* Create pipe for reading data */
    int fds[2];
    if (pipe(fds) < 0) {
        wlr_log(WLR_ERROR, "on_wayland_copy: pipe failed: %s", strerror(errno));
        return;
    }
    
    /* Set read end non-blocking */
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    
    /* Set up async transfer state */
    struct wayland_to_snarf_transfer *transfer = calloc(1, sizeof(*transfer));
    if (!transfer) {
        wlr_log(WLR_ERROR, "on_wayland_copy: failed to allocate transfer state");
        close(fds[0]);
        close(fds[1]);
        return;
    }
    
    transfer->server = s;
    transfer->fd = fds[0];
    transfer->capacity = SNARF_MAX_SIZE;
    transfer->buf = malloc(transfer->capacity);
    if (!transfer->buf) {
        wlr_log(WLR_ERROR, "on_wayland_copy: failed to allocate transfer buffer");
        free(transfer);
        close(fds[0]);
        close(fds[1]);
        return;
    }
    transfer->len = 0;
    
    /* Add to event loop */
    transfer->event_source = wl_event_loop_add_fd(
        wl_display_get_event_loop(s->display),
        fds[0], WL_EVENT_READABLE,
        wayland_to_snarf_read_handler, transfer);
    
    if (!transfer->event_source) {
        wlr_log(WLR_ERROR, "on_wayland_copy: failed to add fd to event loop");
        free(transfer->buf);
        free(transfer);
        close(fds[0]);
        close(fds[1]);
        return;
    }
    
    /* Request data from source - this closes fds[1] */
    wlr_data_source_send(event->source, mime, fds[1]);
}

/* 
 * Signal handler: primary selection (highlight to copy, middle-click to paste)
 *
 * We intentionally do NOT sync primary selection to snarf. Primary selection
 * changes frequently (every text highlight), and syncing would overwrite the
 * snarf buffer unexpectedly. Only explicit Ctrl+C copies go to snarf.
 *
 * This still allows middle-click paste to work between Wayland clients.
 */
static void on_wayland_primary_copy(struct wl_listener *listener, void *data) {
    struct server *s = wl_container_of(listener, s, wayland_to_snarf_primary);
    struct wlr_seat_request_set_primary_selection_event *event = data;
    
    wlr_seat_set_primary_selection(s->seat, event->source, event->serial);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Snarf → Wayland (client requests paste)
 *
 * We register as the Wayland "selection owner" so paste requests come to us.
 *
 * Lazy read:
 *   1. Client pastes  → Wayland calls our send callback
 *   2. We read snarf  → fresh read from /dev/snarf at that moment
 *   3. We write data  → to the client's fd, then close it
 *
 * By reading lazily (not caching), pastes always get current snarf contents,
 * even if Plan 9 modified snarf since our last copy.
 * ───────────────────────────────────────────────────────────────────────────── */

/* 
 * Data source that provides snarf contents to Wayland clients.
 * Reads fresh from snarf on each paste request (lazy evaluation).
 */
struct snarf_to_wayland_source {
    struct wlr_data_source base;
    struct server *server;
};

static void snarf_to_wayland_destroy(struct wlr_data_source *source) {
    struct snarf_to_wayland_source *sts = wl_container_of(source, sts, base);
    /* Note: wlroots frees mime_types strings before calling this */
    free(sts);
}

static void snarf_to_wayland_send(struct wlr_data_source *source,
                                  const char *mime_type, int fd) {
    struct snarf_to_wayland_source *sts = wl_container_of(source, sts, base);
    
    wlr_log(WLR_INFO, "snarf_to_wayland_send: mime='%s' fd=%d", mime_type, fd);
    
    if (is_text_mime_type(mime_type)) {
        char *buf = malloc(SNARF_MAX_SIZE);
        if (buf) {
            int len = p9_read_file(&sts->server->p9_snarf, "snarf", buf, SNARF_MAX_SIZE);
            wlr_log(WLR_INFO, "snarf_to_wayland_send: p9_read_file returned %d", len);
            if (len > 0) {
                ssize_t ret = write(fd, buf, len);
                wlr_log(WLR_INFO, "snarf_to_wayland_send: wrote %zd/%d bytes", ret, len);
            }
            free(buf);
        } else {
            wlr_log(WLR_ERROR, "snarf_to_wayland_send: failed to allocate buffer");
        }
    } else {
        wlr_log(WLR_INFO, "snarf_to_wayland_send: unsupported mime '%s'", mime_type);
    }
    close(fd);
}

static const struct wlr_data_source_impl snarf_to_wayland_impl = {
    .send = snarf_to_wayland_send,
    .destroy = snarf_to_wayland_destroy,
};

/* Register ourselves as selection owner. Pastes will then read fresh from snarf. */
static void snarf_to_wayland_register(struct server *s) {
    struct snarf_to_wayland_source *source = calloc(1, sizeof(*source));
    if (!source) {
        wlr_log(WLR_ERROR, "snarf_to_wayland_register: failed to allocate source");
        return;
    }
    
    source->server = s;
    
    wlr_data_source_init(&source->base, &snarf_to_wayland_impl);
    wl_array_init(&source->base.mime_types);
    
    for (size_t i = 0; i < NUM_TEXT_MIME_TYPES; i++) {
        char **p = wl_array_add(&source->base.mime_types, sizeof(char*));
        if (p) {
            *p = strdup(text_mime_types[i]);
            if (!*p) {
                wlr_log(WLR_ERROR, "snarf_to_wayland_register: strdup failed");
            }
        }
    }
    
    wlr_seat_set_selection(s->seat, &source->base, wl_display_next_serial(s->display));
    wlr_log(WLR_INFO, "snarf_to_wayland_register: registered as selection owner");
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Public API
 * ───────────────────────────────────────────────────────────────────────────── */

int clipboard_init(struct server *s) {
    /* Wayland → Snarf: client copies text */
    s->wayland_to_snarf.notify = on_wayland_copy;
    wl_signal_add(&s->seat->events.request_set_selection, &s->wayland_to_snarf);
    
    /* Wayland → Snarf: primary selection (middle-click) */
    s->wayland_to_snarf_primary.notify = on_wayland_primary_copy;
    wl_signal_add(&s->seat->events.request_set_primary_selection, 
                  &s->wayland_to_snarf_primary);
    
    /* Snarf → Wayland: register as selection owner (reads lazily on paste) */
    snarf_to_wayland_register(s);
    
    wlr_log(WLR_INFO, "clipboard: initialized");
    return 0;
}

void clipboard_cleanup(struct server *s) {
    wl_list_remove(&s->wayland_to_snarf.link);
    wl_list_remove(&s->wayland_to_snarf_primary.link);
}
