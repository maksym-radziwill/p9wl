/*
 * types.h - Shared type definitions for p9wl
 *
 * This is the AUTHORITATIVE source for:
 *   - TILE_SIZE constant (used by compression, scroll detection, rendering)
 *   - MAX_SCROLL_REGIONS constant
 *   - Core structs: server, draw_state, toplevel, input_queue, etc.
 *   - Time utility functions: now_ms(), now_us()
 *
 * Include this header (directly or indirectly) before any draw/compress headers.
 *
 * Dependency Notes:
 *
 *   - This header includes p9.h, focus_manager.h, and kbmap.h
 *   - Do not include input.h from here (would create circular dependency)
 *   - Input-related code should include input.h which includes this file
 *
 * Usage:
 *
 *   Most source files should include this header indirectly through
 *   other headers (e.g., wayland/wayland.h). Direct inclusion is
 *   appropriate for core infrastructure code.
 */

#ifndef P9WL_TYPES_H
#define P9WL_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>   /* Required for clock_gettime, struct timespec in now_ms/now_us */
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

/* ============== Configuration Constants ============== */

/*
 * TILE_SIZE - Tile dimension for compression and change detection.
 *
 * This is the ONLY definition. Do not redefine elsewhere.
 * Value of 16 balances compression efficiency with granularity:
 *   - Smaller values = finer change detection but more overhead
 *   - Larger values = coarser detection but less metadata
 */
#define TILE_SIZE           16

/*
 * MAX_SCROLL_REGIONS - Maximum number of scroll detection regions.
 *
 * Screen is divided into a grid; each cell is a scroll region.
 * This caps memory usage for the scroll_regions array in struct server.
 */
#define MAX_SCROLL_REGIONS  128

/*
 * MAX_SCREEN_DIM - Maximum supported screen dimension.
 *
 * Limits framebuffer allocation. 8K resolution should be sufficient
 * for current and near-future displays.
 */
#define MAX_SCREEN_DIM      8192

/*
 * FRAME_INTERVAL_MS - Minimum milliseconds between frames.
 *
 * Set to 0 to disable throttling (render every frame).
 * Non-zero values can reduce CPU usage at cost of latency.
 */
#define FRAME_INTERVAL_MS   0

/*
 * INPUT_QUEUE_SIZE - Maximum pending input events.
 *
 * Ring buffer size for mouse/keyboard events from input threads.
 * Should be large enough to handle bursts without dropping.
 */
#define INPUT_QUEUE_SIZE    256

/*
 * SCROLL_REGION_SIZE - Dimension of each scroll detection region.
 *
 * Used to divide the screen into a grid for regional scroll detection.
 */
#define SCROLL_REGION_SIZE  512

/* ============== Forward Declarations ============== */

struct server;
struct draw_state;
struct toplevel;
/* Note: struct popup_data is defined in focus_manager.h */

/* ============== Input Event Types ============== */

/*
 * Input event types for the cross-thread input queue.
 */
enum input_type {
    INPUT_MOUSE,    /* Mouse movement or button event */
    INPUT_KEY       /* Keyboard press or release */
};

/*
 * Input event structure for cross-thread communication.
 *
 * Pushed by mouse_thread and kbd_thread, popped by main loop.
 */
struct input_event {
    int type;       /* INPUT_MOUSE or INPUT_KEY */
    union {
        struct {
            int x, y;       /* Absolute position in window coordinates */
            int buttons;    /* Button mask: bit 0=left, 1=middle, 2=right */
        } mouse;
        struct {
            int rune;       /* Plan 9 rune (Unicode codepoint or Kxxx) */
            int pressed;    /* 1 = key press, 0 = key release */
        } key;
    };
};

/*
 * Thread-safe input queue.
 *
 * Uses a ring buffer with mutex protection. The pipe_fd is used
 * to wake the main Wayland event loop when events are available.
 */
struct input_queue {
    struct input_event events[INPUT_QUEUE_SIZE];
    int head, tail;         /* Ring buffer indices */
    pthread_mutex_t lock;   /* Protects head/tail */
    int pipe_fd[2];         /* [0]=read, [1]=write; for waking handle_input_events */
};

/* ============== Draw State ============== */

/*
 * Draw state for Plan 9 /dev/draw operations.
 *
 * Tracks all the file descriptors and image IDs needed to render
 * frames to a Plan 9 window.
 */
struct draw_state {
    struct p9conn *p9;          /* 9P connection for draw operations */
    uint32_t draw_fid;          /* fid for /dev/draw directory */
    uint32_t drawnew_fid;       /* fid for /dev/draw/new */
    uint32_t drawdata_fid;      /* fid for /dev/draw/N/data */
    uint32_t drawctl_fid;       /* fid for /dev/draw/N/ctl */
    uint32_t winname_fid;       /* fid for /dev/winname (kept open for resize) */
    int client_id;              /* Draw client ID (from /dev/draw/new) */
    int screen_id;              /* Screen image ID */
    int image_id;               /* Our offscreen buffer (accumulates via XOR) */
    int opaque_id;              /* 1x1 white replicated image for mask */
    int delta_id;               /* Temp image for receiving XOR deltas */
    int border_id;              /* 1x1 border color replicated image */
    int width, height;          /* Current buffer dimensions */
    int win_minx, win_miny;     /* Window origin for coordinate translation */

    /*
     * Actual window bounds (for border drawing with equal margins).
     *
     * When dimensions are aligned to TILE_SIZE, there may be excess
     * space. These track the real window bounds so we can draw equal
     * borders on all sides by centering the content.
     */
    int actual_minx, actual_miny;
    int actual_maxx, actual_maxy;

    char winname[64];           /* Window name for re-querying geometry */
    int winimage_id;            /* Image ID assigned to the window */
    int xor_enabled;            /* Whether XOR delta mode is active */
    uint32_t iounit;            /* Maximum I/O size for this connection */
};

/* ============== Surface Tracking ============== */

/*
 * Subsurface tracking for Wayland subsurfaces.
 *
 * Wayland subsurfaces are child surfaces that move with their parent.
 * We track them to ensure proper rendering and cleanup.
 */
struct subsurface_track {
    struct wl_list link;            /* Link in toplevel->subsurfaces list */
    struct wlr_subsurface *subsurface;
    struct wl_listener destroy;     /* Cleanup when subsurface destroyed */
    struct wl_listener commit;      /* Track content changes and map/unmap */
    struct server *server;
    struct toplevel *toplevel;      /* Parent toplevel */
    bool mapped;                    /* Track mapped state */
};

/*
 * Toplevel window tracking.
 *
 * Represents a top-level application window (xdg_toplevel in Wayland).
 */
struct toplevel {
    struct wl_list link;            /* Link in server->toplevels list */
    struct wlr_xdg_toplevel *xdg;   /* The wlroots toplevel object */
    struct wlr_scene_tree *scene_tree;
    struct wlr_surface *surface;    /* Stored for focus cleanup in destroy */
    struct wl_listener commit;      /* Handle surface commits */
    struct wl_listener destroy;     /* Cleanup on destroy */
    struct wl_list subsurfaces;     /* List of subsurface_track */
    struct server *server;
    bool configured;                /* Have we sent initial configure? */
    bool mapped;                    /* Is surface currently mapped? */
    int commit_count;               /* Per-toplevel commit counter */
};

/* ============== Main Server State ============== */

/*
 * Main compositor server state.
 *
 * This is the central structure containing all state for the p9wl
 * compositor, including Wayland objects, 9P connections, rendering
 * state, and input handling.
 */
struct server {
    /* ---- Wayland/wlroots core ---- */
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
    struct wlr_scene_rect *background;  /* Gray background, resized with window */
    struct wlr_seat *seat;
    struct wlr_cursor *cursor;
    struct wlr_keyboard virtual_kb;

    /* ---- Event listeners ---- */
    struct wl_listener new_output, output_frame, output_destroy;
    struct wl_listener new_xdg_toplevel;
    struct wl_listener new_xdg_popup;
    struct wl_listener new_input;
    struct wl_list toplevels;           /* List of struct toplevel */

    /*
     * Unified focus state machine.
     *
     * Consolidates: popup stack, pointer/keyboard focus, deferred focus.
     * See focus_manager.h for API.
     */
    struct focus_manager focus;

    /* ---- 9P connections ---- */
    struct p9conn p9_draw;      /* For /dev/draw rendering */
    struct p9conn p9_mouse;     /* For /dev/mouse input */
    struct p9conn p9_kbd;       /* For /dev/cons keyboard input */
    struct p9conn p9_wctl;      /* For /dev/wctl window monitoring */
    struct p9conn p9_snarf;     /* For /dev/snarf clipboard */

    struct draw_state draw;

    /* ---- Window change detection ---- */
    volatile int window_changed;    /* Set by wctl thread on geometry change */
    volatile int resize_pending;    /* Need to resize wlroots output */
    volatile int pending_width, pending_height;
    volatile int pending_minx, pending_miny;
    char pending_winname[64];

    /* ---- Framebuffers ---- */
    int width, height;              /* Current framebuffer dimensions */
    uint32_t *framebuf;             /* Current frame */
    uint32_t *prev_framebuf;        /* Previous frame (for delta detection) */

    /* ---- Tile-based rendering ---- */
    int tiles_x, tiles_y;           /* Number of tiles in each dimension */
    int force_full_frame;           /* Force complete redraw */
    int scene_dirty;                /* Scene content changed, needs render */
    int frame_dirty;                /* Frame has changes to send */
    int timer_armed;                /* Send timer is active */
    uint32_t last_frame_ms;         /* Timestamp of last frame */
    struct wl_event_source *send_timer;

    /* ---- Send thread (double buffered) ---- */
    pthread_t send_thread;
    pthread_mutex_t send_lock;
    pthread_cond_t send_cond;
    uint32_t *send_buf[2];          /* Double buffer for send thread */
    int pending_buf;                /* Buffer with new data (-1 = none) */
    int active_buf;                 /* Buffer send thread is using */
    int send_full;                  /* Force full frame flag */

    /* ---- Damage-based dirty tile tracking ---- */
    uint8_t *dirty_staging;          /* Tile bitmap written by output thread */
    int dirty_staging_valid;         /* 1 if dirty_staging has valid data */
    uint8_t *dirty_tiles[2];         /* Per-send-buffer tile bitmaps */
    int dirty_valid[2];              /* Whether bitmap is valid per buffer */

    /* ---- Per-region scroll detection ---- */
    struct {
        int x1, y1, x2, y2;         /* Region bounds */
        int detected;               /* Scroll detected? */
        int dx, dy;                 /* Scroll vector */
    } scroll_regions[MAX_SCROLL_REGIONS];
    int num_scroll_regions;
    int scroll_regions_x, scroll_regions_y;  /* Grid dimensions */

    /* ---- Input handling ---- */
    struct input_queue input_queue;
    struct wl_event_source *input_event;
    pthread_t mouse_thread;
    pthread_t kbd_thread;
    volatile int running;           /* False to signal threads to exit */

    /* Dynamic keyboard mapping (loaded from /dev/kbmap) */
    struct kbmap kbmap;

    /* ---- Clipboard/snarf integration ---- */
    struct wl_listener wayland_to_snarf;
    struct wl_listener wayland_to_snarf_primary;

    /* ---- Toplevel tracking ---- */
    int has_toplevel;               /* Currently have any toplevel? */
    int had_toplevel;               /* Ever had a toplevel? (for exit logic) */

    /* ---- Connection settings ---- */
    const char *host;               /* 9P server hostname */
    int port;                       /* 9P server port */
    int use_tls;                    /* TLS mode enabled */
    char *tls_cert_file;            /* Path to certificate (-c option) */
    char *tls_fingerprint;          /* SHA256 fingerprint (-f option) */
    int tls_insecure;               /* Skip cert verification (-k option) */
    float scale;                    /* Output scale for HiDPI (default: 1.0) */
    enum wlr_log_importance log_level;
};

/* ============== Utility Functions ============== */

/*
 * Get current time in milliseconds (monotonic clock).
 *
 * Uses CLOCK_MONOTONIC for consistent timing unaffected by
 * system time changes.
 *
 * Returns milliseconds since arbitrary epoch.
 */
static inline uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/*
 * Get current time in microseconds (monotonic clock).
 *
 * Higher precision version of now_ms() for timing-sensitive code.
 *
 * Returns microseconds since arbitrary epoch.
 */
static inline uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* ============== Legacy Compatibility ============== */

/*
 * Legacy compatibility macros.
 *
 * These allow existing code to compile during migration to the
 * focus_manager API. Remove after migration is complete.
 */
#define server_popup_stack(s)         ((s)->focus.popup_stack)
#define server_needs_focus_recheck(s) ((s)->focus.pointer_focus_deferred)

#endif /* P9WL_TYPES_H */
