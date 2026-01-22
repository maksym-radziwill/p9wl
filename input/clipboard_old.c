/*
 * clipboard.c - Wayland clipboard <-> Plan 9 /dev/snarf integration
 *
 * Syncs the Wayland clipboard with Plan 9's /dev/snarf:
 * - When a Wayland client copies, write to /dev/snarf
 * - When a Wayland client pastes, read from /dev/snarf
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

/* Read from /dev/snarf */
int snarf_read(struct p9conn *p9, char *buf, size_t bufsize) {
    uint32_t snarf_fid;
    const char *wnames[] = { "snarf" };
    int total = 0;
    
    /* Walk to /dev/snarf */
    snarf_fid = p9->next_fid++;
    if (p9_walk(p9, p9->root_fid, snarf_fid, 1, wnames) < 0) {
        wlr_log(WLR_ERROR, "snarf_read: failed to walk to /dev/snarf");
        return -1;
    }
    
    /* Open for reading */
    if (p9_open(p9, snarf_fid, OREAD, NULL) < 0) {
        wlr_log(WLR_ERROR, "snarf_read: failed to open /dev/snarf");
        p9_clunk(p9, snarf_fid);
        return -1;
    }
    
    /* Read contents */
    uint64_t offset = 0;
    while ((size_t)total < bufsize - 1) {
        int n = p9_read(p9, snarf_fid, offset, bufsize - 1 - total, 
                        (uint8_t*)buf + total);
        if (n < 0) {
            wlr_log(WLR_ERROR, "snarf_read: read error");
            p9_clunk(p9, snarf_fid);
            return -1;
        }
        if (n == 0) break;  /* EOF */
        total += n;
        offset += n;
    }
    
    buf[total] = '\0';
    p9_clunk(p9, snarf_fid);
    
    wlr_log(WLR_DEBUG, "snarf_read: got %d bytes", total);
    return total;
}

/* Write to /dev/snarf */
int snarf_write(struct p9conn *p9, const char *data, size_t len) {
    uint32_t snarf_fid;
    const char *wnames[] = { "snarf" };
    
    /* Walk to /dev/snarf */
    snarf_fid = p9->next_fid++;
    if (p9_walk(p9, p9->root_fid, snarf_fid, 1, wnames) < 0) {
        wlr_log(WLR_ERROR, "snarf_write: failed to walk to /dev/snarf");
        return -1;
    }
    
    /* Open for writing */
    if (p9_open(p9, snarf_fid, OWRITE, NULL) < 0) {
        wlr_log(WLR_ERROR, "snarf_write: failed to open /dev/snarf");
        p9_clunk(p9, snarf_fid);
        return -1;
    }
    
    /* Write contents - snarf replaces content on write at offset 0 */
    uint64_t offset = 0;
    size_t remaining = len;
    while (remaining > 0) {
        size_t chunk = remaining > 8192 ? 8192 : remaining;
        int n = p9_write(p9, snarf_fid, offset, (uint8_t*)data + offset, chunk);
        if (n < 0) {
            wlr_log(WLR_ERROR, "snarf_write: write error");
            p9_clunk(p9, snarf_fid);
            return -1;
        }
        offset += n;
        remaining -= n;
    }
    
    p9_clunk(p9, snarf_fid);
    
    wlr_log(WLR_DEBUG, "snarf_write: wrote %zu bytes", len);
    return 0;
}

/* Data source for providing snarf contents to Wayland clients */
struct snarf_data_source {
    struct wlr_data_source base;
    struct server *server;
    char *data;
    size_t len;
};

static void snarf_source_destroy(struct wlr_data_source *source) {
    struct snarf_data_source *snarf = wl_container_of(source, snarf, base);
    free(snarf->data);
    free(snarf);
}

static void snarf_source_send(struct wlr_data_source *source,
                              const char *mime_type, int fd) {
    struct snarf_data_source *snarf = wl_container_of(source, snarf, base);
    
    if (strcmp(mime_type, "text/plain") == 0 ||
        strcmp(mime_type, "text/plain;charset=utf-8") == 0 ||
        strcmp(mime_type, "UTF8_STRING") == 0 ||
        strcmp(mime_type, "TEXT") == 0 ||
        strcmp(mime_type, "STRING") == 0) {
        if (snarf->data && snarf->len > 0) {
            ssize_t ret = write(fd, snarf->data, snarf->len);
            (void)ret;  /* Ignore - best effort */
        }
    }
    close(fd);
}

static const struct wlr_data_source_impl snarf_source_impl = {
    .send = snarf_source_send,
    .destroy = snarf_source_destroy,
};

/* Async clipboard read state */
struct clipboard_read {
    struct server *server;
    struct wl_event_source *event_source;
    int fd;
    char *buf;
    size_t len;
    size_t capacity;
};

static int clipboard_read_handler(int fd, uint32_t mask, void *data) {
    struct clipboard_read *cr = data;
    
    if (mask & WL_EVENT_READABLE) {
        char tmp[4096];
        ssize_t n = read(fd, tmp, sizeof(tmp));
        if (n > 0) {
            if (cr->len + n < cr->capacity) {
                memcpy(cr->buf + cr->len, tmp, n);
                cr->len += n;
            }
            return 0;  /* Keep listening */
        }
        /* n == 0 (EOF) or error - fall through to cleanup */
    }
    
    /* Done reading or error - write to snarf and cleanup */
    if (cr->len > 0) {
        cr->buf[cr->len] = '\0';
        int ret = snarf_write(&cr->server->p9_snarf, cr->buf, cr->len);
        if (ret < 0) {
            wlr_log(WLR_ERROR, "clipboard: snarf_write failed");
        } else {
            wlr_log(WLR_INFO, "clipboard: copied %zu bytes to snarf", cr->len);
        }
    } else {
        wlr_log(WLR_DEBUG, "clipboard: no data received from source");
    }
    
    wl_event_source_remove(cr->event_source);
    close(cr->fd);
    free(cr->buf);
    free(cr);
    return 0;
}

/* Called when seat selection changes (client copied something) */
static void handle_request_set_selection(struct wl_listener *listener, void *data) {
    struct server *s = wl_container_of(listener, s, request_set_selection);
    struct wlr_seat_request_set_selection_event *event = data;
    
    wlr_log(WLR_DEBUG, "clipboard: request_set_selection called, source=%p", (void*)event->source);
    
    wlr_seat_set_selection(s->seat, event->source, event->serial);
    
    if (!event->source) {
        return;
    }
    
    /* Check if this is a text mime type */
    const char *mime = NULL;
    char **mime_types = event->source->mime_types.data;
    size_t n_types = event->source->mime_types.size / sizeof(char*);
    
    for (size_t i = 0; i < n_types; i++) {
        if (strcmp(mime_types[i], "text/plain") == 0 ||
            strcmp(mime_types[i], "text/plain;charset=utf-8") == 0 ||
            strcmp(mime_types[i], "UTF8_STRING") == 0) {
            mime = mime_types[i];
            break;
        }
    }
    
    if (!mime) {
        return;  /* Not text, skip */
    }
    
    wlr_log(WLR_DEBUG, "clipboard: using mime type: %s", mime);
    
    /* Create pipe for reading data */
    int fds[2];
    if (pipe(fds) < 0) {
        wlr_log(WLR_ERROR, "clipboard: pipe failed");
        return;
    }
    
    /* Set read end non-blocking */
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    
    /* Set up async read state */
    struct clipboard_read *cr = calloc(1, sizeof(*cr));
    if (!cr) {
        close(fds[0]);
        close(fds[1]);
        return;
    }
    
    cr->server = s;
    cr->fd = fds[0];
    cr->capacity = SNARF_MAX_SIZE;
    cr->buf = malloc(cr->capacity);
    if (!cr->buf) {
        free(cr);
        close(fds[0]);
        close(fds[1]);
        return;
    }
    cr->len = 0;
    
    /* Add to event loop */
    cr->event_source = wl_event_loop_add_fd(
        wl_display_get_event_loop(s->display),
        fds[0], WL_EVENT_READABLE,
        clipboard_read_handler, cr);
    
    if (!cr->event_source) {
        free(cr->buf);
        free(cr);
        close(fds[0]);
        close(fds[1]);
        return;
    }
    
    /* Request data from source - this closes fds[1] */
    wlr_data_source_send(event->source, mime, fds[1]);
}

/* Set snarf contents as Wayland selection (for pasting into Wayland clients) */
void clipboard_set_from_snarf(struct server *s) {
    char *buf = malloc(SNARF_MAX_SIZE);
    if (!buf) return;
    
    int len = snarf_read(&s->p9_snarf, buf, SNARF_MAX_SIZE);
    
    if (len <= 0) {
        free(buf);
        return;
    }
    
    /* Create data source */
    struct snarf_data_source *source = calloc(1, sizeof(*source));
    if (!source) {
        free(buf);
        return;
    }
    
    source->server = s;
    source->data = buf;
    source->len = len;
    
    wlr_data_source_init(&source->base, &snarf_source_impl);
    
    /* Set mime types */
    wl_array_init(&source->base.mime_types);
    
    const char *types[] = {
        "text/plain",
        "text/plain;charset=utf-8",
        "UTF8_STRING",
        "STRING",
        "TEXT",
    };
    
    for (size_t i = 0; i < sizeof(types)/sizeof(types[0]); i++) {
        char **p = wl_array_add(&source->base.mime_types, sizeof(char*));
        if (p) {
            *p = strdup(types[i]);
        }
    }
    
    wlr_seat_set_selection(s->seat, &source->base, wl_display_next_serial(s->display));
    wlr_log(WLR_INFO, "clipboard: set selection from snarf (%d bytes)", len);
}

/* Primary selection handlers (middle-click paste) */
static void handle_request_set_primary_selection(struct wl_listener *listener, void *data) {
    struct server *s = wl_container_of(listener, s, request_set_primary_selection);
    struct wlr_seat_request_set_primary_selection_event *event = data;
    
    wlr_seat_set_primary_selection(s->seat, event->source, event->serial);
    
    /* Primary selection uses wlr_primary_selection_source, which has different API */
    /* For now, just sync the regular selection to snarf */
    /* TODO: Implement proper primary selection sync if needed */
}

int clipboard_init(struct server *s) {
    /* Listen for selection change requests */
    s->request_set_selection.notify = handle_request_set_selection;
    wl_signal_add(&s->seat->events.request_set_selection, &s->request_set_selection);
    
    s->request_set_primary_selection.notify = handle_request_set_primary_selection;
    wl_signal_add(&s->seat->events.request_set_primary_selection, 
                  &s->request_set_primary_selection);
    
    /* Initial sync from snarf */
    clipboard_set_from_snarf(s);
    
    wlr_log(WLR_INFO, "clipboard: initialized snarf integration");
    return 0;
}

void clipboard_cleanup(struct server *s) {
    wl_list_remove(&s->request_set_selection.link);
    wl_list_remove(&s->request_set_primary_selection.link);
}
