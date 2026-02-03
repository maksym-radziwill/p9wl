/*
 * clipboard.c - Wayland clipboard <-> Plan 9 /dev/snarf bridge
 *
 * Copy: read pipe from Wayland client (async, event loop), write to /dev/snarf,
 *       then reclaim selection ownership so all future pastes go through snarf.
 * Paste: read /dev/snarf in detached thread (blocking 9P), write to client fd.
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>

#include <wayland-server-core.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/util/log.h>

#include "clipboard.h"
#include "../types.h"
#include "../p9/p9.h"

#define SNARF_MAX (1024 * 1024)

static void reclaim_selection(struct server *s);

static const char *text_mimes[] = {
	"text/plain",
	"text/plain;charset=utf-8",
	"UTF8_STRING",
	"STRING",
	"TEXT",
};
#define NMIME (sizeof(text_mimes) / sizeof(text_mimes[0]))

static bool is_text(const char *mime) {
	for (size_t i = 0; i < NMIME; i++)
		if (strcmp(mime, text_mimes[i]) == 0)
			return true;
	return false;
}

static const char *find_text(struct wl_array *types) {
	const char **t;
	wl_array_for_each(t, types)
		if (is_text(*t))
			return *t;
	return NULL;
}

/*
 * Wayland → Snarf
 *
 * Client pipe delivers data asynchronously, so we read via the event loop.
 * On EOF: write to snarf, reclaim selection ownership.
 */

struct copy_state {
	struct server *server;
	struct wl_event_source *ev;
	int fd;
	char *buf;
	size_t len;
};

static int copy_readable(int fd, uint32_t mask, void *data) {
	struct copy_state *st = data;

	if (mask & WL_EVENT_READABLE) {
		char tmp[8192];
		ssize_t n = read(fd, tmp, sizeof(tmp));
		if (n > 0) {
			if (st->len + (size_t)n < SNARF_MAX) {
				memcpy(st->buf + st->len, tmp, n);
				st->len += n;
			}
			return 0;
		}
	}

	if (st->len > 0) {
		st->buf[st->len] = '\0';
		p9_write_file(&st->server->p9_snarf, "snarf", st->buf, st->len);
	}

	struct server *s = st->server;
	wl_event_source_remove(st->ev);
	close(st->fd);
	free(st->buf);
	free(st);
	reclaim_selection(s);
	return 0;
}

static void on_copy(struct wl_listener *listener, void *data) {
	struct server *s = wl_container_of(listener, s, wayland_to_snarf);
	struct wlr_seat_request_set_selection_event *ev = data;
	int fds[2];

	wlr_seat_set_selection(s->seat, ev->source, ev->serial);
	if (!ev->source)
		return;
	const char *mime = find_text(&ev->source->mime_types);
	if (!mime)
		return;
	if (pipe(fds) < 0)
		return;
	fcntl(fds[0], F_SETFL, O_NONBLOCK);

	struct copy_state *st = calloc(1, sizeof(*st));
	if (!st)
		goto fail;
	st->buf = malloc(SNARF_MAX);
	if (!st->buf)
		goto fail;
	st->server = s;
	st->fd = fds[0];
	st->ev = wl_event_loop_add_fd(wl_display_get_event_loop(s->display),
		fds[0], WL_EVENT_READABLE, copy_readable, st);
	if (!st->ev)
		goto fail;

	wlr_data_source_send(ev->source, mime, fds[1]);
	return;
fail:
	if (st) free(st->buf);
	free(st);
	close(fds[0]);
	close(fds[1]);
}

/* Primary selection: pass through, don't sync to snarf. */
static void on_primary(struct wl_listener *listener, void *data) {
	struct server *s = wl_container_of(listener, s, wayland_to_snarf_primary);
	struct wlr_seat_request_set_primary_selection_event *ev = data;
	wlr_seat_set_primary_selection(s->seat, ev->source, ev->serial);
}

/*
 * Snarf → Wayland
 *
 * We hold selection ownership. On paste, a detached thread does the
 * blocking 9P read and writes the result to the client fd.
 */

struct snarf_source {
	struct wlr_data_source base;
	struct server *server;
};

struct paste_args {
	struct p9conn *p9;
	int fd;
};

static void *paste_thread(void *arg) {
	struct paste_args *a = arg;
	char *buf = malloc(SNARF_MAX);
	int len = buf ? p9_read_file(a->p9, "snarf", buf, SNARF_MAX) : 0;

	for (ssize_t off = 0; off < len; ) {
		ssize_t n = write(a->fd, buf + off, len - off);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		off += n;
	}

	free(buf);
	close(a->fd);
	free(a);
	return NULL;
}

static void snarf_send(struct wlr_data_source *source, const char *mime, int fd) {
	struct snarf_source *src = wl_container_of(source, src, base);

	if (!is_text(mime)) {
		close(fd);
		return;
	}

	struct paste_args *a = malloc(sizeof(*a));
	if (!a) {
		close(fd);
		return;
	}
	a->p9 = &src->server->p9_snarf;
	a->fd = fd;

	pthread_t th;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&th, &attr, paste_thread, a) != 0) {
		close(fd);
		free(a);
	}
	pthread_attr_destroy(&attr);
}

static void snarf_destroy(struct wlr_data_source *source) {
	free(wl_container_of(source, (struct snarf_source *)0, base));
}

static const struct wlr_data_source_impl snarf_impl = {
	.send = snarf_send,
	.destroy = snarf_destroy,
};

static void reclaim_selection(struct server *s) {
	struct snarf_source *src = calloc(1, sizeof(*src));
	if (!src)
		return;
	src->server = s;

	wlr_data_source_init(&src->base, &snarf_impl);
	wl_array_init(&src->base.mime_types);
	for (size_t i = 0; i < NMIME; i++) {
		char **p = wl_array_add(&src->base.mime_types, sizeof(char *));
		*p = strdup(text_mimes[i]);
	}

	wlr_seat_set_selection(s->seat, &src->base,
		wl_display_next_serial(s->display));
}

int clipboard_init(struct server *s) {
	s->wayland_to_snarf.notify = on_copy;
	wl_signal_add(&s->seat->events.request_set_selection, &s->wayland_to_snarf);

	s->wayland_to_snarf_primary.notify = on_primary;
	wl_signal_add(&s->seat->events.request_set_primary_selection,
		&s->wayland_to_snarf_primary);

	reclaim_selection(s);
	return 0;
}

void clipboard_cleanup(struct server *s) {
	wl_list_remove(&s->wayland_to_snarf.link);
	wl_list_remove(&s->wayland_to_snarf_primary.link);
}
