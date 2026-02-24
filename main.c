/* 
 * main.c - p9wl application entry point
 *
 * Argument parsing, 9P connection setup with optional TLS,
 * wlroots initialization, child process spawning, and main event loop.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <pthread.h>

#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

#include "types.h"
#include "p9/p9.h"
#include "p9/p9_tls.h"
#include "input/input.h"
#include "input/clipboard.h"
#include "draw/draw.h"
#include "draw/send.h"
#include "wayland/wayland.h"

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s [options] <plan9-ip>[:<port>] [command [args...]]\n", prog);
    fprintf(stderr, "\nConnection options:\n");
    fprintf(stderr, "  -c <cert>      Path to server certificate (PEM format)\n");
    fprintf(stderr, "  -f <fp>        SHA256 fingerprint of server certificate (hex)\n");
    fprintf(stderr, "  -k             Insecure mode: skip certificate verification\n");
    fprintf(stderr, "  -u <user>      9P username (default: $P9USER, $USER, or 'glenda')\n");
    fprintf(stderr, "\nDisplay options:\n");
    fprintf(stderr, "  -S <scale>     Output scale factor (1.0-4.0, default: 1.0)\n");
    fprintf(stderr, "\nLogging options:\n");
    fprintf(stderr, "  -q             Quiet mode (errors only, default)\n");
    fprintf(stderr, "  -v             Verbose mode (info + errors)\n");
    fprintf(stderr, "  -d             Debug mode (all messages)\n");
    fprintf(stderr, "\nDefault port is %d for plaintext, %d for TLS.\n", P9_PORT, P9_TLS_PORT);
    fprintf(stderr, "\nExamples:\n");
    fprintf(stderr, "  %s 192.168.1.100 foot\n", prog);
    fprintf(stderr, "  %s -c 9front.pem 192.168.1.100:10001 firefox\n", prog);
    fprintf(stderr, "  %s -f aa11bb22cc33... 192.168.1.100 chromium\n", prog);
    fprintf(stderr, "  %s -k 192.168.1.100 librewolf\n", prog);
    fprintf(stderr, "\n9front setup (plaintext):\n");
    fprintf(stderr, "  aux/listen1 -t tcp!*!%d /bin/exportfs -r /dev\n", P9_PORT);
    fprintf(stderr, "\n9front setup (TLS):\n");
    fprintf(stderr, "  auth/rsagen -t 'service=tls owner=*' > /sys/lib/tls/key\n");
    fprintf(stderr, "  auth/rsa2x509 -e 3650 'CN=myhost' /sys/lib/tls/key | \\\n");
    fprintf(stderr, "      auth/pemencode CERTIFICATE > /sys/lib/tls/cert\n");
    fprintf(stderr, "  cat /sys/lib/tls/key > /mnt/factotum/ctl\n");
    fprintf(stderr, "  aux/listen1 -t tcp!*!%d tlssrv -c /sys/lib/tls/cert /bin/exportfs -r /dev\n", P9_TLS_PORT);
    fprintf(stderr, "\n");
}

static int parse_args(int argc, char *argv[], const char **host, int *port,
                      const char **uname, float *scale,
                      enum wlr_log_importance *log_level,
                      struct tls_config *tls_cfg,  
                      char ***exec_argv, int *exec_argc) {
    static char host_buf[256];  /* Static: lifetime matches program, not reentrant */

    *host = NULL;
    *port = -1;
    *uname = NULL;
    *scale = 1.0f;
    *log_level = WLR_ERROR;
    memset(tls_cfg, 0, sizeof(*tls_cfg));
    *exec_argv = NULL;
    *exec_argc = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            tls_cfg->cert_file = argv[++i];
        } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            tls_cfg->cert_fingerprint = argv[++i];
        } else if (strcmp(argv[i], "-k") == 0) {
            tls_cfg->insecure = 1;
        } else if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) {
            *uname = argv[++i];
        } else if (strcmp(argv[i], "-S") == 0 && i + 1 < argc) {
            *scale = strtof(argv[++i], NULL);
            if (*scale < 1.0f) *scale = 1.0f;
            if (*scale > 4.0f) *scale = 4.0f;
        } else if (strcmp(argv[i], "-q") == 0) {
            *log_level = WLR_ERROR;
        } else if (strcmp(argv[i], "-v") == 0) {
            *log_level = WLR_INFO;
        } else if (strcmp(argv[i], "-d") == 0) {
            *log_level = WLR_DEBUG;
        } else if (strcmp(argv[i], "-h") == 0) {
            return -1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return -1;
        } else if (!*host) {
            *host = argv[i];
            char *colon = strchr(argv[i], ':');
            if (colon) {
                *port = atoi(colon + 1);
                size_t len = colon - argv[i];
                if (len >= sizeof(host_buf)) len = sizeof(host_buf) - 1;
                memcpy(host_buf, argv[i], len);
                host_buf[len] = '\0';
                *host = host_buf;
            }
        } else {
            *exec_argv = &argv[i];
            *exec_argc = argc - i;
            break;
        }
    }

    if (!*host)
        return -1;

    if (*port < 0)
        *port = (tls_cfg->cert_file || tls_cfg->cert_fingerprint || tls_cfg->insecure)
                ? P9_TLS_PORT : P9_PORT;

    if (tls_cfg->insecure && (tls_cfg->cert_file || tls_cfg->cert_fingerprint)) {
        fprintf(stderr, "Warning: -k (insecure) ignores -c and -f options\n");
        tls_cfg->cert_file = NULL;
        tls_cfg->cert_fingerprint = NULL;
    }

    return 0;
}

/* Connect all six 9P sessions, rolling back on failure. */
static int connect_9p_sessions(struct server *s, struct tls_config *tls_cfg) {
    struct p9conn *conns[] = {
        &s->p9_draw, &s->p9_relookup, &s->p9_mouse, &s->p9_kbd, &s->p9_wctl, &s->p9_snarf
    };
    const char *names[] = { "draw", "relookup", "mouse", "kbd", "wctl", "snarf" };
    int n = sizeof(conns) / sizeof(conns[0]);

    for (int i = 0; i < n; i++) {
        if (p9_connect(conns[i], s->host, s->port, tls_cfg) < 0) {
            wlr_log(WLR_ERROR, "Failed to connect (%s)", names[i]);
            for (int j = 0; j < i; j++)
                p9_disconnect(conns[j]);
            return -1;
        }
    }
    return 0;
}

/*
 * Initialize all wlroots subsystems.
 *
 * Order matters: renderer and allocator must exist before protocols
 * that need them, scene graph before background rect, and the headless
 * output is created last so that new_output fires after everything
 * else is wired up.
 *
 * Sets WLR_RENDERER=pixman to force software rendering (required for
 * headless framebuffer extraction).
 */
static int init_wayland(struct server *s) {
    setenv("WLR_RENDERER", "pixman", 1);
    setenv("WLR_SCENE_DISABLE_DIRECT_SCANOUT", "1", 1);

    s->display = wl_display_create();
    if (!s->display)
        return -1;

    s->backend = wlr_headless_backend_create(wl_display_get_event_loop(s->display));
    if (!s->backend)
        return -1;

    s->renderer = wlr_renderer_autocreate(s->backend);
    if (!s->renderer)
        return -1;
    wlr_renderer_init_wl_display(s->renderer, s->display);

    s->allocator = wlr_allocator_autocreate(s->backend, s->renderer);
    if (!s->allocator)
        return -1;

    /* Core protocols */
    wlr_compositor_create(s->display, 5, s->renderer);
    wlr_subcompositor_create(s->display);
    wlr_data_device_manager_create(s->display);
    wlr_viewporter_create(s->display);
    wlr_primary_selection_v1_device_manager_create(s->display);
    wlr_idle_notifier_v1_create(s->display);

    /* Output layout */
    s->output_layout = wlr_output_layout_create(s->display);
    wlr_xdg_output_manager_v1_create(s->display, s->output_layout);

    /* Scene graph */
    s->scene = wlr_scene_create();
    if (!s->scene)
        return -1;
    wlr_scene_attach_output_layout(s->scene, s->output_layout);

    /* Background — sized to visible (logical) dimensions */
    int logical_w = focus_phys_to_logical(s->visible_width, s->scale);
    int logical_h = focus_phys_to_logical(s->visible_height, s->scale);
    float gray[4] = { 0.3f, 0.3f, 0.3f, 1.0f };
    s->background = wlr_scene_rect_create(&s->scene->tree, logical_w, logical_h, gray);
    if (s->background)
        wlr_scene_node_lower_to_bottom(&s->background->node);

    /* XDG shell */
    s->xdg_shell = wlr_xdg_shell_create(s->display, 5);
    if (!s->xdg_shell)
        return -1;
    s->new_xdg_toplevel.notify = new_toplevel;
    wl_signal_add(&s->xdg_shell->events.new_toplevel, &s->new_xdg_toplevel);
    s->new_xdg_popup.notify = new_popup;
    wl_signal_add(&s->xdg_shell->events.new_popup, &s->new_xdg_popup);

    /* XDG decoration manager */
    s->decoration_mgr = wlr_xdg_decoration_manager_v1_create(s->display);
    if (s->decoration_mgr) {
        s->new_decoration.notify = handle_new_decoration;
        wl_signal_add(&s->decoration_mgr->events.new_toplevel_decoration, &s->new_decoration);
    }

    /* Presentation time */
    wlr_presentation_create(s->display, s->backend, 2);

    /* Cursor */
    s->cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(s->cursor, s->output_layout);

    /* Seat */
    s->seat = wlr_seat_create(s->display, "seat0");
    wlr_seat_set_capabilities(s->seat, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);

    /* Virtual keyboard */
    wlr_keyboard_init(&s->virtual_kb, NULL, "virtual-keyboard");
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
    wlr_keyboard_set_keymap(&s->virtual_kb, km);
    xkb_keymap_unref(km);
    xkb_context_unref(ctx);
    wlr_seat_set_keyboard(s->seat, &s->virtual_kb);

    /* Output and input events */
    s->new_output.notify = new_output;
    wl_signal_add(&s->backend->events.new_output, &s->new_output);
    s->new_input.notify = new_input;
    wl_signal_add(&s->backend->events.new_input, &s->new_input);

    /* Create the headless output at visible (not padded) dimensions */
    wlr_headless_add_output(s->backend, s->visible_width, s->visible_height);

    return 0;
}

static const char *setup_socket(struct server *s) {
    const char *sock = wl_display_add_socket_auto(s->display);
    if (!sock)
        return NULL;
    setenv("WAYLAND_DISPLAY", sock, 1);
    wlr_log(WLR_INFO, "WAYLAND_DISPLAY=%s (%dx%d visible, %dx%d padded)",
            sock, s->visible_width, s->visible_height, s->width, s->height);
    /* Print to stdout for parent process socket discovery */
    fprintf(stdout, "WAYLAND_DISPLAY=%s\n", sock);
    return sock;
}

int main(int argc, char *argv[]) {
    const char *host, *uname;
    int port, exec_argc, ret = 1;
    float scale;
    enum wlr_log_importance log_level;
    struct tls_config tls_cfg;
    char **exec_argv;

    if (parse_args(argc, argv, &host, &port, &uname, &scale, &log_level,
                   &tls_cfg, &exec_argv, &exec_argc) < 0) {
        print_usage(argv[0]);
        return 1;
    }

    if (uname)
        setenv("P9USER", uname, 1);

    signal(SIGPIPE, SIG_IGN);
    wlr_log_init(log_level, NULL);

    int using_tls = tls_cfg.cert_file || tls_cfg.cert_fingerprint || tls_cfg.insecure;
    if (using_tls) {
        if (tls_init() < 0) {
            wlr_log(WLR_ERROR, "Failed to initialize TLS");
            return 1;
        }
        if (tls_cfg.cert_file) {
            wlr_log(WLR_INFO, "TLS mode: certificate pinning (file: %s)", tls_cfg.cert_file);
            char fp[65];
            if (tls_cert_file_fingerprint(tls_cfg.cert_file, fp, sizeof(fp)) == 0)
                wlr_log(WLR_INFO, "Pinned certificate fingerprint: %s", fp);
        } else if (tls_cfg.cert_fingerprint) {
            wlr_log(WLR_INFO, "TLS mode: fingerprint pinning");
        } else if (tls_cfg.insecure) {
            wlr_log(WLR_ERROR, "WARNING: TLS certificate verification disabled (vulnerable to MITM)");
        }
    }

    struct server s = {0};
    wl_list_init(&s.toplevels);
    focus_manager_init(&s.focus, &s);
    s.host = host;
    s.port = port;
    s.running = 1;
    s.use_tls = using_tls;
    s.scale = scale;
    s.log_level = log_level;
    if (tls_cfg.cert_file)
        s.tls_cert_file = strdup(tls_cfg.cert_file);
    if (tls_cfg.cert_fingerprint)
        s.tls_fingerprint = strdup(tls_cfg.cert_fingerprint);
    s.tls_insecure = tls_cfg.insecure;

    wlr_log(WLR_INFO, "Connecting to %s:%d", host, port);

    if (connect_9p_sessions(&s, &tls_cfg) < 0)
        goto cleanup;

    if (init_draw(&s) < 0) {
        wlr_log(WLR_ERROR, "Failed to initialize draw device");
        goto cleanup;
    }

    s.width = s.draw.width;
    s.height = s.draw.height;
    s.visible_width = s.draw.visible_width;
    s.visible_height = s.draw.visible_height;
    s.tiles_x = s.width / TILE_SIZE;
    s.tiles_y = s.height / TILE_SIZE;

    if (s.scale > 1.0f) {
        wlr_log(WLR_INFO, "Visible: %dx%d, Padded: %dx%d, Scale: %.2f, Logical: %dx%d",
                s.visible_width, s.visible_height, s.width, s.height, s.scale,
                focus_phys_to_logical(s.visible_width, s.scale),
                focus_phys_to_logical(s.visible_height, s.scale));
    }

    s.framebuf = calloc(s.width * s.height, 4);
    s.prev_framebuf = calloc(s.width * s.height, 4);
    s.send_buf[0] = calloc(s.width * s.height, 4);
    s.send_buf[1] = calloc(s.width * s.height, 4);
    if (!s.framebuf || !s.prev_framebuf || !s.send_buf[0] || !s.send_buf[1]) {
        wlr_log(WLR_ERROR, "Memory allocation failed");
        goto cleanup;
    }

    s.force_full_frame = 1;
    s.frame_dirty = 1;
    s.pending_buf = -1;
    s.active_buf = -1;
    pthread_mutex_init(&s.send_lock, NULL);
    pthread_cond_init(&s.send_cond, NULL);

    input_queue_init(&s.input_queue);

    pthread_create(&s.mouse_thread, NULL, mouse_thread_func, &s);
    pthread_create(&s.kbd_thread, NULL, kbd_thread_func, &s); 
    pthread_create(&s.send_thread, NULL, send_thread_func, &s);

    if (init_wayland(&s) < 0)
        goto cleanup;

    clipboard_init(&s);

    if (!setup_socket(&s))
        goto cleanup;

    if (exec_argc > 0) {
        pid_t pid = fork();
        if (pid < 0) {
            wlr_log(WLR_ERROR, "fork: %s", strerror(errno));
            goto cleanup;
        } else if (pid == 0) {
            execvp(exec_argv[0], exec_argv);
            fprintf(stderr, "exec %s: %s\n", exec_argv[0], strerror(errno));
            _exit(1);
        }
        wlr_log(WLR_INFO, "Spawned child %d: %s", pid, exec_argv[0]);
    }

    s.input_event = wl_event_loop_add_fd(wl_display_get_event_loop(s.display),
                                          s.input_queue.pipe_fd[0],
                                          WL_EVENT_READABLE,
                                          handle_input_events, &s);
    s.send_timer = wl_event_loop_add_timer(wl_display_get_event_loop(s.display),
                                            send_timer_callback, &s);

    if (!wlr_backend_start(s.backend)) {
        wlr_log(WLR_ERROR, "Backend start failed");
        goto cleanup;
    }

    wlr_log(WLR_INFO, "Running (9P%s)", using_tls ? " over TLS" : "");
    wl_display_run(s.display);
    ret = 0;

cleanup:
    if (s.display) {
        clipboard_cleanup(&s);
        wl_display_destroy(s.display);
    }
    server_cleanup(&s);
    if (using_tls)
        tls_cleanup();
    return ret;
}
