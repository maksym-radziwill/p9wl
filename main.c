/*
 * main.c - p9wl application entry point
 *
 * Argument parsing, 9P connection setup with optional TLS,
 * wlroots initialization, and main event loop.
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
    fprintf(stderr, "  -k             Insecure mode: skip certificate verification (DANGEROUS)\n");
    fprintf(stderr, "  -u <user>      9P username (default: $P9USER, $USER, or 'glenda')\n");
    fprintf(stderr, "\nDisplay options:\n");
    fprintf(stderr, "  -S <scale>     Output scale factor for HiDPI displays (1.0-4.0, default: 1.0)\n");
    fprintf(stderr, "                 Supports fractional values like 1.5, 1.25, 2.0, etc.\n");
    fprintf(stderr, "                 Use -S 2 if fonts appear too small\n");
    fprintf(stderr, "  -W             Use Wayland-side scaling instead of 9front scaling\n");
    fprintf(stderr, "                 (may look sharper but uses more bandwidth)\n");
    fprintf(stderr, "\nLogging options:\n");;
    fprintf(stderr, "  -q             Quiet mode (errors only, default)\n");
    fprintf(stderr, "  -v             Verbose mode (info + errors)\n");
    fprintf(stderr, "  -d             Debug mode (all messages)\n");
    fprintf(stderr, "\nCommand execution:\n");
    fprintf(stderr, "  [command]      Command to execute after Wayland socket is ready\n");
    fprintf(stderr, "                 WAYLAND_DISPLAY will be set to the socket path\n");
    fprintf(stderr, "\nTLS modes:\n");
    fprintf(stderr, "  No TLS flags   Plaintext connection (default port %d)\n", P9_PORT);
    fprintf(stderr, "  -c or -f       TLS with certificate pinning (default port %d)\n", P9_TLS_PORT);
    fprintf(stderr, "  -k             TLS without verification (default port %d)\n", P9_TLS_PORT);
    fprintf(stderr, "\n");
    fprintf(stderr, "═══════════════════════════════════════════════════════════════════\n");
    fprintf(stderr, "EXAMPLES\n");
    fprintf(stderr, "═══════════════════════════════════════════════════════════════════\n");
    fprintf(stderr, "\n  Plaintext (trusted network only!):\n");
    fprintf(stderr, "    %s 192.168.1.100\n", prog);
    fprintf(stderr, "\n  With 2x scaling for HiDPI:\n");
    fprintf(stderr, "    %s -S 2 192.168.1.100\n", prog);
    fprintf(stderr, "\n  TLS with certificate file:\n");
    fprintf(stderr, "    %s -c 9front.pem 192.168.1.100\n", prog);
    fprintf(stderr, "\n  TLS with SHA256 fingerprint:\n");
    fprintf(stderr, "    %s -f aa11bb22cc33... 192.168.1.100\n", prog);
    fprintf(stderr, "\n  TLS insecure mode (logs fingerprint for later pinning):\n");
    fprintf(stderr, "    %s -k 192.168.1.100\n", prog);
    fprintf(stderr, "\n  Launch a Wayland application:\n");
    fprintf(stderr, "    %s 192.168.1.100 foot\n", prog);
    fprintf(stderr, "    %s -S 2 192.168.1.100 firefox --no-remote\n", prog);
    fprintf(stderr, "\n");
    fprintf(stderr, "═══════════════════════════════════════════════════════════════════\n");
    fprintf(stderr, "9FRONT SERVER SETUP\n");
    fprintf(stderr, "═══════════════════════════════════════════════════════════════════\n");
    fprintf(stderr, "\n  1. Generate TLS certificate (PEM format required by tlssrv):\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "     auth/rsagen -t 'service=tls owner=*' > /sys/lib/tls/key\n");
    fprintf(stderr, "     auth/rsa2x509 -e 3650 'CN=myhost' /sys/lib/tls/key | \\\n");
    fprintf(stderr, "         auth/pemencode CERTIFICATE > /sys/lib/tls/cert\n");
    fprintf(stderr, "     cat /sys/lib/tls/key > /mnt/factotum/ctl\n");
    fprintf(stderr, "\n  2. Start TLS listener (port %d):\n", P9_TLS_PORT);
    fprintf(stderr, "\n");
    fprintf(stderr, "     aux/listen1 -t tcp!*!%d tlssrv -c /sys/lib/tls/cert /bin/exportfs -r /dev\n", P9_TLS_PORT);
    fprintf(stderr, "\n  Or plaintext listener (port %d, trusted networks only!):\n", P9_PORT);
    fprintf(stderr, "\n");
    fprintf(stderr, "     aux/listen1 -t tcp!*!%d /bin/exportfs -r /dev\n", P9_PORT);
    fprintf(stderr, "\n  3. Copy cert to Linux (it's already PEM format):\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "     cp /sys/lib/tls/cert /tmp/9front.pem\n");
    fprintf(stderr, "     # Then copy via drawterm, 9pfuse, or other method\n");
    fprintf(stderr, "\n  4. Get fingerprint (alternative to copying cert):\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "     openssl x509 -in 9front.pem -noout -fingerprint -sha256\n");
    fprintf(stderr, "\n");
}

static int parse_args(int argc, char *argv[], const char **host, int *port,
                      const char **uname, float *scale, int *wl_scaling,
                      enum wlr_log_importance *log_level,
                      struct tls_config *tls_cfg,
                      char ***exec_argv, int *exec_argc) {
    static char host_buf[256];

    *host = NULL;
    *port = -1;  /* Will set default based on TLS config */
    *uname = NULL;
    *scale = 1.0f;  /* Default scale factor */
    *wl_scaling = 0; /* Default: use 9front scaling */
    *log_level = WLR_ERROR;  /* Default: errors only */
    memset(tls_cfg, 0, sizeof(*tls_cfg));
    *exec_argv = NULL;
    *exec_argc = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            tls_cfg->cert_file = argv[++i];
        } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            tls_cfg->cert_fingerprint = argv[++i];
        } else if (strcmp(argv[i], "-k") == 0 || strcmp(argv[i], "--insecure") == 0) {
            tls_cfg->insecure = 1;
        } else if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) {
            *uname = argv[++i];
        } else if (strcmp(argv[i], "-S") == 0 && i + 1 < argc) {
            *scale = strtof(argv[++i], NULL);
            if (*scale < 1.0f) *scale = 1.0f;
            if (*scale > 4.0f) *scale = 4.0f;
        } else if (strcmp(argv[i], "-W") == 0) {
            *wl_scaling = 1;
        } else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            *log_level = WLR_ERROR;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            *log_level = WLR_INFO;
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--debug") == 0) {
            *log_level = WLR_DEBUG;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            return -1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return -1;
        } else if (!*host) {
            *host = argv[i];
            /* Check if host contains :port */
            char *colon = strchr(argv[i], ':');
            if (colon) {
                *port = atoi(colon + 1);
                size_t len = colon - argv[i];
                if (len >= sizeof(host_buf)) len = sizeof(host_buf) - 1;
                memcpy(host_buf, argv[i], len);
                host_buf[len] = '\0';
                *host = host_buf;
            }
        } else if (*exec_argc == 0) {
            /* After host, remaining arguments are the command to execute */
            *exec_argv = &argv[i];
            *exec_argc = argc - i;
            break;  /* Stop parsing, rest is the command */
        }
    }

    if (!*host) {
        return -1;
    }

    /* Set default port based on TLS config */
    if (*port < 0) {
        if (tls_cfg->cert_file || tls_cfg->cert_fingerprint || tls_cfg->insecure) {
            *port = P9_TLS_PORT;  /* Default TLS port: 10001 */
        } else {
            *port = P9_PORT;      /* Default plaintext port: 564 */
        }
    }

    /* Validate TLS configuration */
    if (tls_cfg->insecure && (tls_cfg->cert_file || tls_cfg->cert_fingerprint)) {
        fprintf(stderr, "Warning: -k (insecure) ignores -c and -f options\n");
        tls_cfg->cert_file = NULL;
        tls_cfg->cert_fingerprint = NULL;
    }

    return 0;
}

static int connect_9p_sessions(struct server *s, struct tls_config *tls_cfg) {
    wlr_log(WLR_INFO, "Connecting to %s:%d%s", s->host, s->port,
            tls_cfg->cert_file ? " (TLS with pinned cert)" :
            tls_cfg->cert_fingerprint ? " (TLS with pinned fingerprint)" :
            tls_cfg->insecure ? " (TLS insecure)" : " (plaintext)");

    if (p9_connect(&s->p9_draw, s->host, s->port, tls_cfg) < 0) {
        wlr_log(WLR_ERROR, "Failed to connect (draw)");
        if (!tls_cfg->cert_file && !tls_cfg->cert_fingerprint && !tls_cfg->insecure) {
            wlr_log(WLR_ERROR, "Make sure 9front is running:");
            wlr_log(WLR_ERROR, "  aux/listen1 -t tcp!*!%d /bin/exportfs -r /dev", s->port);
        } else {
            wlr_log(WLR_ERROR, "For TLS, make sure 9front is running:");
            wlr_log(WLR_ERROR, "  aux/listen1 -t tcp!*!%d tlssrv -c /sys/lib/tls/cert /bin/exportfs -r /dev", s->port);
        }
        return -1;
    }

    if (p9_connect(&s->p9_mouse, s->host, s->port, tls_cfg) < 0) {
        wlr_log(WLR_ERROR, "Failed to connect (mouse)");
        p9_disconnect(&s->p9_draw);
        return -1;
    }

    if (p9_connect(&s->p9_kbd, s->host, s->port, tls_cfg) < 0) {
        wlr_log(WLR_ERROR, "Failed to connect (kbd)");
        p9_disconnect(&s->p9_draw);
        p9_disconnect(&s->p9_mouse);
        return -1;
    }

    if (p9_connect(&s->p9_wctl, s->host, s->port, tls_cfg) < 0) {
        wlr_log(WLR_ERROR, "Failed to connect (wctl)");
        p9_disconnect(&s->p9_draw);
        p9_disconnect(&s->p9_mouse);
        p9_disconnect(&s->p9_kbd);
        return -1;
    }

    if (p9_connect(&s->p9_snarf, s->host, s->port, tls_cfg) < 0) {
        wlr_log(WLR_ERROR, "Failed to connect (snarf)");
        p9_disconnect(&s->p9_draw);
        p9_disconnect(&s->p9_mouse);
        p9_disconnect(&s->p9_kbd);
        p9_disconnect(&s->p9_wctl);
        return -1;
    }

    wlr_log(WLR_INFO, "All 9P connections established");
    return 0;
}

static int init_wayland(struct server *s) {
    setenv("WLR_RENDERER", "pixman", 1);
    setenv("WLR_SCENE_DISABLE_DIRECT_SCANOUT", "1", 1);

    s->display = wl_display_create();
    if (!s->display) {
        wlr_log(WLR_ERROR, "Failed to create display");
        return -1;
    }

    s->backend = wlr_headless_backend_create(wl_display_get_event_loop(s->display));
    if (!s->backend) {
        wlr_log(WLR_ERROR, "Backend failed");
        return -1;
    }

    s->renderer = wlr_renderer_autocreate(s->backend);
    if (!s->renderer) {
        wlr_log(WLR_ERROR, "Renderer failed");
        return -1;
    }
    wlr_renderer_init_wl_display(s->renderer, s->display);

    s->allocator = wlr_allocator_autocreate(s->backend, s->renderer);
    if (!s->allocator) {
        wlr_log(WLR_ERROR, "Allocator failed");
        return -1;
    }

    /* Core protocols */
    wlr_compositor_create(s->display, 5, s->renderer);
    wlr_subcompositor_create(s->display);
    wlr_data_device_manager_create(s->display);

    /* Additional protocols for browser support */
    wlr_viewporter_create(s->display);
    wlr_primary_selection_v1_device_manager_create(s->display);
    wlr_idle_notifier_v1_create(s->display);

    /* Output layout */
    s->output_layout = wlr_output_layout_create(s->display);
    wlr_xdg_output_manager_v1_create(s->display, s->output_layout);

    /* Scene graph */
    s->scene = wlr_scene_create();
    if (!s->scene) {
        wlr_log(WLR_ERROR, "Failed to create scene!");
        return -1;
    }
    wlr_scene_attach_output_layout(s->scene, s->output_layout);

    /* Gray background (uses logical dimensions for scene graph) */
    int logical_w = (int)(s->width + 0.5f); // Divided by s->scale before
    int logical_h = (int)(s->height + 0.5f); // ditto
    float gray[4] = { 0.3f, 0.3f, 0.3f, 1.0f };
    s->background = wlr_scene_rect_create(&s->scene->tree, logical_w, logical_h, gray);
    if (s->background) {
        wlr_scene_node_lower_to_bottom(&s->background->node);
        wlr_log(WLR_INFO, "Created gray background %dx%d (logical)", logical_w, logical_h);
    } else {
        wlr_log(WLR_ERROR, "Failed to create background rect!");
    }

    /* XDG shell */
    s->xdg_shell = wlr_xdg_shell_create(s->display, 5);
    if (!s->xdg_shell) {
        wlr_log(WLR_ERROR, "Failed to create xdg_shell!");
        return -1;
    }
    wlr_log(WLR_INFO, "XDG shell created");
    s->new_xdg_toplevel.notify = new_toplevel;
    wl_signal_add(&s->xdg_shell->events.new_toplevel, &s->new_xdg_toplevel);
    s->new_xdg_popup.notify = new_popup;
    wl_signal_add(&s->xdg_shell->events.new_popup, &s->new_xdg_popup);

    /* XDG decoration manager */
    s->decoration_mgr = wlr_xdg_decoration_manager_v1_create(s->display);
    if (s->decoration_mgr) {
        s->new_decoration.notify = handle_new_decoration;
        wl_signal_add(&s->decoration_mgr->events.new_toplevel_decoration, &s->new_decoration);
        wlr_log(WLR_INFO, "XDG decoration manager created");
    }

    /* Presentation time for video sync */
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

    /* Create the headless output */
    wlr_headless_add_output(s->backend, s->width, s->height);

    return 0;
}

static int setup_socket(struct server *s) {
    const char *sock = wl_display_add_socket_auto(s->display);
    if (!sock) {
        wlr_log(WLR_ERROR, "Socket failed");
        return -1;
    }

    wlr_log(WLR_INFO, "WAYLAND_DISPLAY=%s (%dx%d)", sock, s->width, s->height);
    setenv("WAYLAND_DISPLAY", sock, 1);

    return 0;
}

int main(int argc, char *argv[]) {
    const char *uname = NULL;
    const char *host = NULL;
    int port = P9_PORT;
    float scale = 1.0f;
    int wl_scaling = 0;  /* 0 = 9front scaling (default), 1 = Wayland scaling */
    enum wlr_log_importance log_level = WLR_ERROR;
    struct tls_config tls_cfg = {0};
    char **exec_argv = NULL;
    int exec_argc = 0;

    /* Parse arguments */
    if (parse_args(argc, argv, &host, &port, &uname, &scale, &wl_scaling,
                   &log_level, &tls_cfg, &exec_argv, &exec_argc) < 0) {
        print_usage(argv[0]);
        return 1;
    }

    /* Set username in environment for p9_attach to use */
    if (uname) {
        setenv("P9USER", uname, 1);
    }

    signal(SIGPIPE, SIG_IGN);
    wlr_log_init(log_level, NULL);

    /* Initialize TLS if any TLS option is specified */
    int using_tls = tls_cfg.cert_file || tls_cfg.cert_fingerprint || tls_cfg.insecure;
    if (using_tls) {
        if (tls_init() < 0) {
            wlr_log(WLR_ERROR, "Failed to initialize TLS");
            return 1;
        }

        /* Log TLS mode */
        if (tls_cfg.cert_file) {
            wlr_log(WLR_INFO, "TLS mode: certificate pinning (file: %s)", tls_cfg.cert_file);

            /* Verify cert file exists and show fingerprint */
            char fp[65];
            if (tls_cert_file_fingerprint(tls_cfg.cert_file, fp, sizeof(fp)) == 0) {
                wlr_log(WLR_INFO, "Pinned certificate fingerprint: %s", fp);
            }
        } else if (tls_cfg.cert_fingerprint) {
            wlr_log(WLR_INFO, "TLS mode: fingerprint pinning");
        } else if (tls_cfg.insecure) {
            wlr_log(WLR_ERROR, "");
            wlr_log(WLR_ERROR, "╔══════════════════════════════════════════════════════════════╗");
            wlr_log(WLR_ERROR, "║  WARNING: TLS CERTIFICATE VERIFICATION DISABLED              ║");
            wlr_log(WLR_ERROR, "║                                                              ║");
            wlr_log(WLR_ERROR, "║  The connection will be encrypted, but the server's         ║");
            wlr_log(WLR_ERROR, "║  identity will NOT be verified. This is vulnerable to       ║");
            wlr_log(WLR_ERROR, "║  man-in-the-middle attacks!                                 ║");
            wlr_log(WLR_ERROR, "║                                                              ║");
            wlr_log(WLR_ERROR, "║  For production use, obtain the server's certificate and    ║");
            wlr_log(WLR_ERROR, "║  use -c <cert.pem> instead.                                 ║");
            wlr_log(WLR_ERROR, "╚══════════════════════════════════════════════════════════════╝");
            wlr_log(WLR_ERROR, "");
        }
    }

    /* Initialize server structure */
    struct server s = {0};
    wl_list_init(&s.toplevels);
    focus_manager_init(&s.focus, &s);

    s.host = host;
    s.port = port;
    s.running = 1;
    s.has_toplevel = 0;
    s.use_tls = using_tls;
    
    /* Store TLS configuration for reference */
    if (tls_cfg.cert_file) {
        s.tls_cert_file = strdup(tls_cfg.cert_file);
    }
    if (tls_cfg.cert_fingerprint) {
        s.tls_fingerprint = strdup(tls_cfg.cert_fingerprint);
    }
    s.tls_insecure = tls_cfg.insecure;
    s.scale = (scale > 0.0f) ? scale : 1.0f;  /* HiDPI scale factor, default 1.0 */
    s.wl_scaling = wl_scaling;  /* 0 = 9front scaling, 1 = Wayland scaling */
    s.log_level = log_level;

    wlr_log(WLR_INFO, "Connecting to %s:%d", s.host, s.port);

    /* Connect 9P sessions (with TLS if configured) */
    if (connect_9p_sessions(&s, &tls_cfg) < 0) {
        if (using_tls) tls_cleanup();
        return 1;
    }

    /* Initialize draw device */
    if (init_draw(&s) < 0) {
        wlr_log(WLR_ERROR, "Failed to initialize draw device");
        p9_disconnect(&s.p9_draw);
        p9_disconnect(&s.p9_mouse);
        p9_disconnect(&s.p9_kbd);
        p9_disconnect(&s.p9_wctl);
        p9_disconnect(&s.p9_snarf);
        if (using_tls) tls_cleanup();
        return 1;
    }

    /* Set dimensions from draw device.
     * Use physical dimensions initially - new_output() will reconfigure
     * to logical resolution for 9front scaling mode.
     */
    s.width = s.draw.width;
    s.height = s.draw.height;
    s.tiles_x = (s.width + TILE_SIZE - 1) / TILE_SIZE;
    s.tiles_y = (s.height + TILE_SIZE - 1) / TILE_SIZE;

    /* Log scaling info */
    if (s.scale > 1.001f) {
        wlr_log(WLR_INFO, "Scale mode: %s, Physical: %dx%d, Scale: %.2f (new_output will reconfigure)",
                s.wl_scaling ? "Wayland" : "9front",
                s.draw.width, s.draw.height, s.scale);
    }

    /* Allocate framebuffers */
    s.framebuf = calloc(s.width * s.height, 4);
    s.prev_framebuf = calloc(s.width * s.height, 4);
    s.send_buf[0] = calloc(s.width * s.height, 4);
    s.send_buf[1] = calloc(s.width * s.height, 4);

    if (!s.framebuf || !s.prev_framebuf || !s.send_buf[0] || !s.send_buf[1]) {
        wlr_log(WLR_ERROR, "Memory allocation failed");
        server_cleanup(&s);
        if (using_tls) tls_cleanup();
        return 1;
    }

    /* Don't set force_full_frame yet - wait until Wayland is initialized
     * so new_output() can reallocate buffers at logical resolution first.
     */
    s.force_full_frame = 0;
    s.frame_dirty = 0;
    s.last_frame_ms = 0;

    /* Initialize send thread synchronization */
    pthread_mutex_init(&s.send_lock, NULL);
    pthread_cond_init(&s.send_cond, NULL);
    s.pending_buf = -1;
    s.active_buf = -1;
    s.send_full = 0;
    s.window_changed = 0;
    s.resize_pending = 0;
    s.pending_winname[0] = '\0';

    /* Initialize input queue */
    input_queue_init(&s.input_queue);

    /* Start background threads */
    pthread_create(&s.mouse_thread, NULL, mouse_thread_func, &s);
    pthread_create(&s.kbd_thread, NULL, kbd_thread_func, &s);
    pthread_create(&s.wctl_thread, NULL, wctl_thread_func, &s);
    pthread_create(&s.send_thread, NULL, send_thread_func, &s);

    /* Initialize Wayland/wlroots */
    if (init_wayland(&s) < 0) {
        server_cleanup(&s);
        if (using_tls) tls_cleanup();
        return 1;
    }

    /* Initialize clipboard/snarf integration */
    clipboard_init(&s);

    /* Set up socket */
    if (setup_socket(&s) < 0) {
        server_cleanup(&s);
        if (using_tls) tls_cleanup();
        return 1;
    }

    /* Fork and execute command if specified */
    if (exec_argc > 0 && exec_argv != NULL) {
        pid_t pid = fork();
        if (pid < 0) {
            wlr_log(WLR_ERROR, "Failed to fork: %s", strerror(errno));
            server_cleanup(&s);
            if (using_tls) tls_cleanup();
            return 1;
        } else if (pid == 0) {
            /* Child process - WAYLAND_DISPLAY is already set by setup_socket() */
            /* Create null-terminated argv array for execvp */
            char **child_argv = malloc((exec_argc + 1) * sizeof(char *));
            if (!child_argv) {
                _exit(1);
            }
            for (int i = 0; i < exec_argc; i++) {
                child_argv[i] = exec_argv[i];
            }
            child_argv[exec_argc] = NULL;

            wlr_log(WLR_INFO, "Executing: %s", exec_argv[0]);
            execvp(exec_argv[0], child_argv);
            /* If we get here, exec failed */
            fprintf(stderr, "Failed to execute %s: %s\n", exec_argv[0], strerror(errno));
            _exit(1);
        } else {
            /* Parent process continues */
            wlr_log(WLR_INFO, "Spawned child process %d: %s", pid, exec_argv[0]);
        }
    }

    /* Add input event handler to main loop */
    s.input_event = wl_event_loop_add_fd(wl_display_get_event_loop(s.display),
                                          s.input_queue.pipe_fd[0],
                                          WL_EVENT_READABLE,
                                          handle_input_events, &s);

    /* Add throttled send timer */
    s.send_timer = wl_event_loop_add_timer(wl_display_get_event_loop(s.display),
                                            send_timer_callback, &s);

    /* Start backend */
    if (!wlr_backend_start(s.backend)) {
        wlr_log(WLR_ERROR, "Start failed");
        server_cleanup(&s);
        if (using_tls) tls_cleanup();
        return 1;
    }

    wlr_log(WLR_INFO, "Running (9P%s mode with threads)",
            using_tls ? " over TLS" : " direct");

    /* Main event loop */
    wl_display_run(s.display);

    /* Cleanup */
    clipboard_cleanup(&s);
    wl_display_destroy(s.display);
    server_cleanup(&s);
    if (using_tls) tls_cleanup();

    return 0;
}
