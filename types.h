/*
 * types.h - Shared type definitions for p9wl
 */

#ifndef P9WL_TYPES_H
#define P9WL_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

/* Include p9.h for struct p9conn definition (with TLS support) */
#include "p9/p9.h"

/* Include focus_manager for unified focus state machine */
#include "wayland/focus_manager.h"

/* Include kbmap for dynamic keyboard mapping */
#include "input/kbmap.h"

/* Configuration */
#define TILE_SIZE           16
#define MAX_SCREEN_DIM      8192
#define FRAME_INTERVAL_MS   0   /* No throttle - render every frame */
#define INPUT_QUEUE_SIZE    256
#define SCROLL_REGION_SIZE  512   /* Per-region scroll detection grid size */
#define MAX_SCROLL_REGIONS  128   /* Max scroll regions (screen size dependent) */

/* Forward declarations */
struct server;
struct draw_state;
struct toplevel;
/* NOTE: struct popup_data is defined in focus_manager.h */

/* Input event types */
enum input_type { INPUT_MOUSE, INPUT_KEY };

struct input_event {
    int type;
    union {
        struct { int x, y, buttons; } mouse;
        struct { int rune; int pressed; } key;  /* pressed: 1=press, 0=release */
    };
};

/* Thread-safe input queue */
struct input_queue {
    struct input_event events[INPUT_QUEUE_SIZE];
    int head, tail;
    pthread_mutex_t lock;
    int pipe_fd[2];  /* For waking up main loop */
};

/* NOTE: struct p9conn is defined in p9.h with TLS support */

/* Draw state */
struct draw_state {
    struct p9conn *p9;
    uint32_t draw_fid;
    uint32_t drawnew_fid;
    uint32_t drawdata_fid;
    uint32_t drawctl_fid;
    uint32_t winname_fid;  /* fid for /dev/winname - kept open to re-read on resize */
    int client_id;
    int screen_id;
    int image_id;  /* our offscreen buffer image (accumulates via XOR) */
    int opaque_id; /* 1x1 white replicated image for mask */
    int delta_id;  /* temp image for receiving XOR deltas */
    int border_id; /* 1x1 border color replicated image */
    int width, height;
    int win_minx, win_miny;  /* window origin for coordinate translation */
    
    /* Actual window bounds (for border drawing with equal margins).
     * When dimensions are aligned to TILE_SIZE, there's excess space.
     * These track the real window bounds so we can draw equal borders
     * on all sides by centering the content. */
    int actual_minx, actual_miny;
    int actual_maxx, actual_maxy;
    
    char winname[64];  /* window name for re-querying geometry */
    int winimage_id;   /* the image ID assigned to the window */
    int xor_enabled;   /* whether XOR delta mode is active (after first frame) */
    uint32_t iounit;


    int logical_width;
    int logical_height;
    float scale;        /* Scale factor for 'a' command matrix: matrix = 128/scale */
    float input_scale;  /* Scale factor for mouse coord conversion: logical = physical / input_scale */
    int scene_width;    /* Scene/logical dimensions (what Wayland apps see) */
    int scene_height;
};

/* Subsurface tracking */
struct subsurface_track {
    struct wl_list link;
    struct wlr_subsurface *subsurface;
    struct wl_listener destroy;
    struct wl_listener commit;  /* Track content changes and map/unmap */
    struct server *server;
    struct toplevel *toplevel;  /* Parent toplevel for list access */
    bool mapped;  /* Track mapped state for commit-based map/unmap */
};

/* Toplevel window */
struct toplevel {
    struct wl_list link;
    struct wlr_xdg_toplevel *xdg;
    struct wlr_scene_tree *scene_tree;
    struct wlr_surface *surface;  /* Store surface for focus cleanup in destroy */
    struct wl_listener commit, destroy;
    struct wl_list subsurfaces;         /* List of tracked subsurfaces */
    struct server *server;
    bool configured;  /* Have we sent our initial configure? */
    bool mapped;      /* Track mapped state for commit-based map/unmap */
    int commit_count; /* Per-toplevel commit counter */
};

/* Main server state */
struct server {
    struct wl_display *display;
    struct wlr_backend *backend;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;
    struct wlr_scene *scene;
    struct wlr_scene_output *scene_output;
    struct wlr_output_layout *output_layout;
    struct wlr_output *output;
    struct wlr_xdg_shell *xdg_shell;
    struct wlr_xdg_decoration_manager_v1 *decoration_mgr;
    struct wl_listener new_decoration;
    struct wlr_scene_rect *background;    /* Gray background rect - resized with window */
    struct wlr_seat *seat;
    struct wlr_cursor *cursor;
    struct wlr_keyboard virtual_kb;
    
    struct wl_listener new_output, output_frame, output_destroy;
    struct wl_listener new_xdg_toplevel;  /* Signal-based: creation */
    struct wl_listener new_xdg_popup;     /* Signal-based: creation */
    struct wl_listener new_input;
    struct wl_list toplevels;
    
    /*
     * Unified focus state machine.
     * Consolidates: popup stack, pointer/keyboard focus, deferred focus.
     * Replaces: popup_stack, needs_focus_recheck
     */
    struct focus_manager focus;
    
    /* 9P connections (separate for display and input) */
    struct p9conn p9_draw;
    struct p9conn p9_mouse;
    struct p9conn p9_kbd;
    struct p9conn p9_wctl;  /* For window control watching */
    struct p9conn p9_snarf; /* For clipboard/snarf operations */
    
    struct draw_state draw;
    
    /* Window change detection */
    pthread_t wctl_thread;
    volatile int window_changed;  /* Flag set by wctl thread when geometry changes */
    volatile int resize_pending;  /* Flag: need to resize wlroots output in main thread */
    volatile int pending_width, pending_height;  /* New dimensions for resize */
    volatile int pending_minx, pending_miny;  /* New window position */
    char pending_winname[64];  /* New window name for resize */
    
    int width, height;
    uint32_t *framebuf;
    uint32_t *prev_framebuf;
    
    int tiles_x, tiles_y;
    int force_full_frame;
    int frame_dirty;
    int timer_armed;
    uint32_t last_frame_ms;
    struct wl_event_source *send_timer;
    
    /* Send thread state - double buffered */
    pthread_t send_thread;
    pthread_mutex_t send_lock;
    pthread_cond_t send_cond;
    uint32_t *send_buf[2];      /* Double buffer for send thread */
    int pending_buf;            /* Which buffer has new data (-1 = none) */
    int active_buf;             /* Which buffer send thread is using (-1 = idle) */
    int send_full;              /* Flag: force full frame */
    
    /* Per-region scroll detection */
    struct {
        int x1, y1, x2, y2;  /* Region bounds */
        int detected;         /* Scroll detected for this region? */
        int dx, dy;           /* Scroll vector */
    } scroll_regions[MAX_SCROLL_REGIONS];
    int num_scroll_regions;
    int scroll_regions_x, scroll_regions_y;  /* Grid dimensions */
    
    /* Input handling */
    struct input_queue input_queue;
    struct wl_event_source *input_event;
    pthread_t mouse_thread;
    pthread_t kbd_thread;
    volatile int running;
    
    /* Dynamic keyboard mapping (loaded from /dev/kbmap) */
    struct kbmap kbmap;
    
    /* Clipboard/snarf integration (Wayland → Snarf) */
    struct wl_listener wayland_to_snarf;
    struct wl_listener wayland_to_snarf_primary;
    
    /* Toplevel tracking for exit on last destroy */
    int has_toplevel;                  /* Have any toplevel? */
    int had_toplevel;                  /* Ever had a toplevel? (for exit on last destroy) */

    const char *host;
    int port;
    int use_tls;                       /* TLS mode enabled */
    char *tls_cert_file;               /* Path to certificate file (-c option) */
    char *tls_fingerprint;             /* SHA256 fingerprint string (-f option) */
    int tls_insecure;                  /* Insecure mode (-k option) */
    float scale;                       /* Output scale factor for HiDPI (default: 1.0) */
    enum wlr_log_importance log_level; /* Log level */

    float wl_scaling;

};

/* Utility functions */
static inline uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static inline uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/*
 * Legacy compatibility macros.
 * These allow existing code to compile during migration.
 * Remove after fully migrating to focus_manager API.
 */
#define server_popup_stack(s)         ((s)->focus.popup_stack)
#define server_needs_focus_recheck(s) ((s)->focus.pointer_focus_deferred)

#endif /* P9WL_TYPES_H */
