#define _POSIX_C_SOURCE 200809L

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <pthread.h>
#include <fcntl.h>

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
#include <freerdp/freerdp.h>
#include <freerdp/listener.h>
#include "types.h"
#include <winpr/wlog.h>
#include <winpr/winpr.h>

#define TAG FREERDP_TAG("p9wl.main")
#define DEFAULT_WIDTH 1280
#define DEFAULT_HEIGHT 800

static void *browser_input_thread(void *arg) {
    struct server *s = arg;
    mkfifo("/tmp/p9wl_input.fifo", 0666);
    while (s->running) {
        int fd = open("/tmp/p9wl_input.fifo", O_RDONLY);
        if (fd < 0) {
            usleep(100000);
            continue;
        }
        char buf[256];
        int n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            int x, y, buttons, rune, pressed;
            if (sscanf(buf, "MOUSE %d %d %d", &x, &y, &buttons) == 3) {
                struct input_event ev = { .type = INPUT_MOUSE, .mouse = { .x = x, .y = y, .buttons = buttons } };
                input_queue_push(&s->input_queue, &ev);
            } else if (sscanf(buf, "KEY %d %d", &rune, &pressed) == 2) {
                struct input_event ev = { .type = INPUT_KEY, .key = { .rune = rune, .pressed = pressed } };
                input_queue_push(&s->input_queue, &ev);
            }
        }
    }
    return NULL;
}

static int output_poll_timer(void *data) {
    wlr_log(WLR_INFO, "MAIN_DEBUG: output_poll_timer called");
    struct server *s = data;
    if (s->output) {
        wlr_output_schedule_frame(s->output);
    }
    return 1;
}

static void print_usage(const char *prog) {
    wlr_log(WLR_INFO, "MAIN_DEBUG: print_usage called");
    fprintf(stderr, "Usage: %s [options] [command [args...]]\n", prog);
    fprintf(stderr, "\nDisplay options:\n");
    fprintf(stderr, "  -w <width>     Output screen width (default: %d)\n", DEFAULT_WIDTH);
    fprintf(stderr, "  -h <height>    Output screen height (default: %d)\n", DEFAULT_HEIGHT);
    fprintf(stderr, "  -S <scale>     Output scale factor (1.0-4.0, default: 1.0)\n");
    fprintf(stderr, "\nLogging options:\n");
    fprintf(stderr, "  -q             Quiet mode (errors only, default)\n");
    fprintf(stderr, "  -v             Verbose mode (info + errors)\n");
    fprintf(stderr, "  -d             Debug mode (all messages)\n");
}

static int parse_args(int argc, char *argv[], int *width, int *height,
                      float *scale, enum wlr_log_importance *log_level,
                      char ***exec_argv, int *exec_argc) {
    wlr_log(WLR_INFO, "MAIN_DEBUG: parse_args called");
    *width = DEFAULT_WIDTH;
    *height = DEFAULT_HEIGHT;
    *scale = 1.0f;
    *log_level = WLR_ERROR;
    *exec_argv = NULL;
    *exec_argc = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            *width = atoi(argv[++i]);
            if (*width < 640) *width = 640;
            wlr_log(WLR_INFO, "MAIN_DEBUG: Parsed option -w width=%d", *width);
        } else if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
            *height = atoi(argv[++i]);
            if (*height < 480) *height = 480;
            wlr_log(WLR_INFO, "MAIN_DEBUG: Parsed option -h height=%d", *height);
        } else if (strcmp(argv[i], "-S") == 0 && i + 1 < argc) {
            *scale = strtof(argv[++i], NULL);
            if (*scale < 1.0f) *scale = 1.0f;
            if (*scale > 4.0f) *scale = 4.0f;
            wlr_log(WLR_INFO, "MAIN_DEBUG: Parsed option -S scale=%.2f", *scale);
        } else if (strcmp(argv[i], "-q") == 0) {
            *log_level = WLR_ERROR;
            wlr_log(WLR_INFO, "MAIN_DEBUG: Parsed option -q (Quiet mode)");
        } else if (strcmp(argv[i], "-v") == 0) {
            *log_level = WLR_INFO;
            wlr_log(WLR_INFO, "MAIN_DEBUG: Parsed option -v (Verbose mode)");
        } else if (strcmp(argv[i], "-d") == 0) {
            *log_level = WLR_DEBUG;
            wlr_log(WLR_INFO, "MAIN_DEBUG: Parsed option -d (Debug mode)");
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-help") == 0) {
            wlr_log(WLR_INFO, "MAIN_DEBUG: Parsed help flag");
            return -1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            wlr_log(WLR_ERROR, "MAIN_DEBUG: Encountered unknown option: %s", argv[i]);
            return -1;
        } else {
            *exec_argv = &argv[i];
            *exec_argc = argc - i;
            wlr_log(WLR_INFO, "MAIN_DEBUG: Parsed command execution target: %s (argc=%d)", argv[i], *exec_argc);
            break;
        }
    }
    return 0;
}

static int init_wayland(struct server *s) {
    wlr_log(WLR_INFO, "MAIN_DEBUG: init_wayland called");
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

    wlr_compositor_create(s->display, 5, s->renderer);
    wlr_subcompositor_create(s->display);
    wlr_data_device_manager_create(s->display);
    wlr_viewporter_create(s->display);
    wlr_primary_selection_v1_device_manager_create(s->display);
    wlr_idle_notifier_v1_create(s->display);

    s->output_layout = wlr_output_layout_create(s->display);
    wlr_xdg_output_manager_v1_create(s->display, s->output_layout);

    s->scene = wlr_scene_create();
    if (!s->scene)
        return -1;
    wlr_scene_attach_output_layout(s->scene, s->output_layout);

    int logical_w = focus_phys_to_logical(s->visible_width, s->scale);
    int logical_h = focus_phys_to_logical(s->visible_height, s->scale);
    float gray[4] = { 0.3f, 0.3f, 0.3f, 1.0f };
    s->background = wlr_scene_rect_create(&s->scene->tree, logical_w, logical_h, gray);
    if (s->background)
        wlr_scene_node_lower_to_bottom(&s->background->node);

    s->xdg_shell = wlr_xdg_shell_create(s->display, 5);
    if (!s->xdg_shell)
        return -1;
    s->new_xdg_toplevel.notify = new_toplevel;
    wl_signal_add(&s->xdg_shell->events.new_toplevel, &s->new_xdg_toplevel);
    s->new_xdg_popup.notify = new_popup;
    wl_signal_add(&s->xdg_shell->events.new_popup, &s->new_xdg_popup);

    s->decoration_mgr = wlr_xdg_decoration_manager_v1_create(s->display);
    if (s->decoration_mgr) {
        s->new_decoration.notify = handle_new_decoration;
        wl_signal_add(&s->decoration_mgr->events.new_toplevel_decoration, &s->new_decoration);
    }

    wlr_presentation_create(s->display, s->backend, 2);

    s->cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(s->cursor, s->output_layout);

    s->seat = wlr_seat_create(s->display, "seat0");
    wlr_seat_set_capabilities(s->seat, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);

    wlr_keyboard_init(&s->virtual_kb, NULL, "virtual-keyboard");
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
    wlr_keyboard_set_keymap(&s->virtual_kb, km);
    xkb_keymap_unref(km);
    xkb_context_unref(ctx);
    wlr_seat_set_keyboard(s->seat, &s->virtual_kb);

    s->new_output.notify = new_output;
    wl_signal_add(&s->backend->events.new_output, &s->new_output);
    s->new_input.notify = new_input;
    wl_signal_add(&s->backend->events.new_input, &s->new_input);

    wlr_headless_add_output(s->backend, s->visible_width, s->visible_height);

    return 0;
}

static const char *setup_socket(struct server *s) {
    wlr_log(WLR_INFO, "MAIN_DEBUG: setup_socket called");
    const char *sock = wl_display_add_socket_auto(s->display);
    if (!sock)
        return NULL;
    setenv("WAYLAND_DISPLAY", sock, 1);
    wlr_log(WLR_INFO, "WAYLAND_DISPLAY=%s (%dx%d visible, %dx%d padded)",
            sock, s->visible_width, s->visible_height, s->width, s->height);
    WLog_INFO(TAG, "WAYLAND_DISPLAY=%s (%dx%d visible, %dx%d padded)",
              sock, s->visible_width, s->visible_height, s->width, s->height);
    fprintf(stdout, "WAYLAND_DISPLAY=%s\n", sock);
    return sock;
}

int main(int argc, char *argv[]) {
    wlr_log(WLR_INFO, "MAIN_DEBUG: main called");
    const char *runtime_dir = "/tmp/xdg";
    struct stat st = {0};
    if (stat(runtime_dir, &st) == -1) {
        mkdir(runtime_dir, 0700);
    } else {
        chmod(runtime_dir, 0700);
    }
    setenv("XDG_RUNTIME_DIR", runtime_dir, 1);
    int width, height, exec_argc, ret = 1;
    float scale;
    enum wlr_log_importance log_level;
    char **exec_argv;

    if (parse_args(argc, argv, &width, &height, &scale, &log_level,
                   &exec_argv, &exec_argc) < 0) {
        print_usage(argv[0]);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    wlr_log_init(log_level, NULL);
    
    WLog_SetLogLevel(WLog_Get(TAG), WLOG_INFO);

    struct server s = {0};
    wl_list_init(&s.toplevels);
    focus_manager_init(&s.focus, &s);
    s.running = 1;
    s.scale = scale;
    s.log_level = log_level;
    s.width = width;
    s.height = height;
    s.visible_width = width;
    s.visible_height = height;
    s.tiles_x = s.width / TILE_SIZE;
    s.tiles_y = s.height / TILE_SIZE;

    s.framebuf = calloc(s.width * s.height, 4);
    s.prev_framebuf = calloc(s.width * s.height, 4);
    s.send_buf[0] = calloc(s.width * s.height, 4);
    s.send_buf[1] = calloc(s.width * s.height, 4);
    if (!s.framebuf || !s.prev_framebuf || !s.send_buf[0] || !s.send_buf[1]) {
        wlr_log(WLR_ERROR, "Memory allocation failed");
        WLog_ERR(TAG, "Memory allocation failed");
        goto cleanup;
    }

    s.force_full_frame = 1;
    s.frame_dirty = 1;
    s.pending_buf = -1;
    s.active_buf = -1;
    pthread_mutex_init(&s.send_lock, NULL);
    pthread_cond_init(&s.send_cond, NULL);

    input_queue_init(&s.input_queue);

    wlr_log(WLR_INFO, "MAIN_DEBUG: Starting FreeRDP send thread");
    pthread_create(&s.send_thread, NULL, send_thread_func, &s);

    pthread_t browser_input_th;
    pthread_create(&browser_input_th, NULL, browser_input_thread, &s);

    if (init_wayland(&s) < 0)
        goto cleanup;

    clipboard_init(&s);

    if (!setup_socket(&s))
        goto cleanup;

    if (exec_argc > 0) {
        wlr_log(WLR_INFO, "MAIN_DEBUG: About to fork child process for executable: %s", exec_argv[0]);
        pid_t pid = fork();
        if (pid < 0) {
            wlr_log(WLR_ERROR, "fork: %s", strerror(errno));
            WLog_ERR(TAG, "fork: %s", strerror(errno));
            goto cleanup;
        } else if (pid == 0) {
            wlr_log(WLR_INFO, "MAIN_DEBUG: Child process running execvp for: %s", exec_argv[0]);
            execvp(exec_argv[0], exec_argv);
            fprintf(stderr, "exec %s: %s\n", exec_argv[0], strerror(errno));
            _exit(1);
        }
        wlr_log(WLR_INFO, "Spawned child %d: %s", pid, exec_argv[0]);
        WLog_INFO(TAG, "Spawned child %d: %s", pid, exec_argv[0]);
    }

    s.input_event = wl_event_loop_add_fd(wl_display_get_event_loop(s.display),
                                          s.input_queue.pipe_fd[0],
                                          WL_EVENT_READABLE,
                                          handle_input_events, &s);
    s.send_timer = wl_event_loop_add_timer(wl_display_get_event_loop(s.display),
                                            send_timer_callback, &s);
    wl_event_loop_add_timer(wl_display_get_event_loop(s.display),
                            output_poll_timer, &s);

    if (!wlr_backend_start(s.backend)) {
        wlr_log(WLR_ERROR, "Backend start failed");
        WLog_ERR(TAG, "Backend start failed");
        goto cleanup;
    }

    if (s.output) {
        wlr_output_schedule_frame(s.output);
    }

    wlr_log(WLR_INFO, "Running Standalone Wayland-to-RDP Compositor");
    WLog_INFO(TAG, "Running Standalone Wayland-to-RDP Compositor");
    wl_display_run(s.display);
    ret = 0;

cleanup:
    wlr_log(WLR_INFO, "MAIN_DEBUG: Entering server cleanup sequence");
    if (s.display) {
        clipboard_cleanup(&s);
        wl_display_destroy(s.display);
    }
    server_cleanup(&s);
    return ret;
}