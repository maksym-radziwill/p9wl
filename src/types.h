#ifndef P9WL_TYPES_H
#define P9WL_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_keyboard_shortcuts_inhibit_v1.h>
#include <wlr/util/log.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <freerdp/freerdp.h>
#include <freerdp/input.h>

#define TILE_SIZE           16
#define MAX_SCROLL_REGIONS  128
#define MAX_SCREEN_DIM      8192
#define FRAME_INTERVAL_MS   0
#define INPUT_QUEUE_SIZE    256
#define SCROLL_REGION_SIZE  512
#define MAX_WORKERS         8
#define FFT_SIZE            256
#define MAX_SCROLL_DETECT   (FFT_SIZE / 2)
#define MIN_SCROLL_PIXELS   1
#define TLS_PORT            10001

#define ALPHA_DELTA_OVERHEAD 45

#ifndef P9WL_ALIGN
#define P9WL_ALIGN 64
#endif

#if defined(__GNUC__) || defined(__clang__)
#define P9WL_ALIGNED_STRUCT __attribute__((aligned(P9WL_ALIGN)))
#define P9WL_API __attribute__((visibility("default")))
#else
#define P9WL_ALIGNED_STRUCT
#define P9WL_API
#endif

/* Forward declarations */
struct server;
struct draw_state;
struct toplevel;
struct wlr_surface;
struct wlr_scene_node;
struct wlr_scene_tree;
struct wlr_xdg_popup;

enum input_type {
    INPUT_MOUSE,
    INPUT_KEY,
    INPUT_WAKEUP
};

struct input_event {
    int type;
    union {
        struct {
            int x, y;
            int buttons;
        } mouse;
        struct {
            int rune;
            int pressed;
        } key;
    };
};

struct input_queue {
    struct input_event events[INPUT_QUEUE_SIZE];
    int head, tail;
    pthread_mutex_t lock;
    int pipe_fd[2];
};

struct draw_state {
    int client_id;
    int screen_id;
    int image_id;
    int opaque_id;
    int delta_id;
    int width, height;
    int visible_width;
    int visible_height;
    int win_minx, win_miny;
    int logical_width;
    int logical_height;
    float scale;
    char winname[64];
    int winimage_id;
    int xor_enabled;
    uint32_t iounit;
};

struct subsurface_track {
    struct wl_list link;
    struct wlr_subsurface *subsurface;
    struct wl_listener destroy;
    struct wl_listener commit;
    struct server *server;
    struct toplevel *toplevel;
    bool mapped;
};

struct toplevel {
    struct wl_list link;
    struct wlr_xdg_toplevel *xdg;
    struct wlr_scene_tree *scene_tree;
    struct wlr_surface *surface;
    struct wl_listener commit;
    struct wl_listener destroy;
    struct wl_listener xdg_destroy;
    struct wl_listener request_fullscreen;
    struct wl_listener request_maximize;
    struct wl_listener request_minimize;
    struct wl_list subsurfaces;
    struct server *server;
    bool configured;
    bool mapped;
    int commit_count;
};

/* ============== Focus Manager Data Structures ============== */

struct popup_data {
    struct wlr_xdg_popup *popup;
    struct wlr_surface *surface;
    struct wlr_scene_tree *scene_tree;
    struct server *server;

    /* Lifecycle state */
    int configured;             /* Initial configure sent */
    int commit_count;           /* Number of commits received */
    bool mapped;                /* Surface has buffer and is visible */
    bool has_grab;              /* Popup requested keyboard grab */

    /* Wayland listeners */
    struct wl_listener commit;
    struct wl_listener destroy;
    struct wl_listener grab;

    /* Link in focus_manager.popup_stack (most recent first) */
    struct wl_list link;
};

enum focus_reason {
    FOCUS_REASON_NONE,
    FOCUS_REASON_POINTER_MOTION,    /* Cursor moved over new surface */
    FOCUS_REASON_POINTER_CLICK,     /* User clicked on surface */
    FOCUS_REASON_SURFACE_MAP,       /* Surface became visible */
    FOCUS_REASON_SURFACE_UNMAP,     /* Surface became invisible */
    FOCUS_REASON_SURFACE_DESTROY,   /* Surface is being destroyed */
    FOCUS_REASON_POPUP_GRAB,        /* Popup requested keyboard grab */
    FOCUS_REASON_POPUP_DISMISS,     /* Popup was dismissed */
    FOCUS_REASON_EXPLICIT,          /* Programmatic focus change */
};

struct focus_manager {
    struct server *server;

    /* Current focus targets (may lag seat state during transitions) */
    struct wlr_surface *pointer_focus;
    struct wlr_surface *keyboard_focus;

    /* Popup grab stack: head is most recent (topmost) popup */
    struct wl_list popup_stack;

    /* Deferred pointer focus (while buttons held) */
    bool pointer_focus_deferred;
    struct wlr_surface *deferred_pointer_target;
    double deferred_sx, deferred_sy;

    /* Cached keyboard modifier state */
    uint32_t modifier_state;

    /* Debug statistics */
    int focus_change_count;
};

/* ============== Server Data Structure ============== */

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
    struct wlr_keyboard_shortcuts_inhibit_manager_v1 *kb_shortcuts_inhibit;
    struct wl_listener new_kb_shortcut_inhibitor;
    struct wl_listener kb_inhibitor_destroy;
    struct wlr_keyboard_shortcuts_inhibitor_v1 *active_kb_inhibitor;
    struct wlr_scene_rect *background;
    struct wlr_seat *seat;
    struct wlr_cursor *cursor;
    struct wlr_keyboard virtual_kb;

    struct wl_listener new_output, output_frame, output_destroy;
    struct wl_listener new_xdg_toplevel;
    struct wl_listener new_xdg_popup;
    struct wl_listener new_input;
    struct wl_list toplevels;

    struct focus_manager focus;

    struct draw_state draw;

    volatile int window_changed;
    volatile int resize_pending;
    volatile int pending_width, pending_height;
    volatile int pending_visible_width, pending_visible_height;
    volatile int pending_minx, pending_miny;
    char pending_winname[64];

    int width, height;
    int visible_width, visible_height;
    uint32_t *framebuf;
    uint32_t *prev_framebuf;

    int tiles_x, tiles_y;
    int force_full_frame;
    int scene_dirty;
    int frame_dirty;
    int timer_armed;
    uint32_t last_frame_ms;
    struct wl_event_source *send_timer;

    pthread_t send_thread;
    pthread_mutex_t send_lock;
    pthread_cond_t send_cond;
    uint32_t *send_buf[2];
    int pending_buf;
    int active_buf;
    int send_full;

    uint8_t *dirty_staging;
    int dirty_staging_valid;
    uint8_t *dirty_tiles[2];
    int dirty_valid[2];

    struct {
        int x1, y1, x2, y2;
        int detected;
        int dx, dy;
    } scroll_regions[MAX_SCROLL_REGIONS];
    int num_scroll_regions;
    int scroll_regions_x, scroll_regions_y;

    struct input_queue input_queue;
    struct wl_event_source *input_event;
    pthread_t mouse_thread;
    pthread_t kbd_thread;
    volatile int running;

    struct wl_listener wayland_to_snarf;
    struct wl_listener wayland_to_snarf_primary;

    int has_toplevel;
    int had_toplevel;

    const char *host;
    int port;
    int use_tls;
    char *tls_cert_file;
    char *tls_fingerprint;
    int tls_insecure;
    float scale;
    enum wlr_log_importance log_level;
};

static inline uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static inline uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

#define server_popup_stack(s)         ((s)->focus.popup_stack)
#define server_needs_focus_recheck(s) ((s)->focus.pointer_focus_deferred)

/* ============== Compress Types ============== */

struct P9WL_ALIGNED_STRUCT tile_result {
    uint8_t data[TILE_SIZE * TILE_SIZE * 4 + 256];
    int size;
    int is_delta;
};

struct tile_work {
    uint32_t *pixels;
    int stride;
    uint32_t *prev_pixels;
    int prev_stride;
    int x1, y1, w, h;
};

P9WL_API int compress_tile_data(uint8_t *dst, int dst_max, 
                                uint8_t *raw, int bytes_per_row, int h);
P9WL_API int compress_tile_direct(uint8_t *dst, int dst_max, 
                                  uint32_t *pixels, int stride, 
                                  int x1, int y1, int w, int h);
P9WL_API int compress_tile_alpha_delta(uint8_t *dst, int dst_max,
                                       uint32_t *pixels, int stride,
                                       uint32_t *prev_pixels, int prev_stride,
                                       int x1, int y1, int w, int h);
P9WL_API int compress_tile_adaptive(uint8_t *dst, int dst_max,
                                    uint32_t *pixels, int stride,
                                    uint32_t *prev_pixels, int prev_stride,
                                    int x1, int y1, int w, int h);
P9WL_API int compress_pool_init(int nthreads);
P9WL_API void compress_pool_shutdown(void);
P9WL_API int compress_tiles_parallel(struct tile_work *tiles, 
                                     struct tile_result *results, 
                                     int count);

/* ============== Draw / RDP API ============== */

int init_draw(struct server *s);
int relookup_window(struct server *s);
void delete_rio_window(void *p9);

typedef void (*parallel_fn)(void *ctx, int idx);
void parallel_for(int count, parallel_fn fn, void *ctx);
void parallel_cleanup(void);

struct phase_result {
    int dx;
    int dy;
    int valid;
};
struct phase_result phase_correlate_detect(
    uint32_t *curr_buf, uint32_t *prev_buf, int buf_width,
    int rx1, int ry1, int rx2, int ry2,
    int max_shift
);
void phase_correlate_cleanup(void);

void detect_scroll(struct server *s, uint32_t *send_buf);
int apply_scroll_to_prevbuf(struct server *s);
int write_scroll_commands(struct server *s, uint8_t *batch, size_t max_size);
struct scroll_timing {
    double total_us;
    int regions_processed;
    int regions_detected;
};
const struct scroll_timing *scroll_get_timing(void);
void scroll_init(void);
void scroll_cleanup(void);

#ifndef PUT32
#define PUT32(p, v) do { \
    uint32_t _v = (v); \
    (p)[0] = (uint8_t)(_v); \
    (p)[1] = (uint8_t)(_v >> 8); \
    (p)[2] = (uint8_t)(_v >> 16); \
    (p)[3] = (uint8_t)(_v >> 24); \
} while(0)
#endif

#ifndef PUT16
#define PUT16(p, v) do { \
    uint16_t _v = (v); \
    (p)[0] = (uint8_t)(_v); \
    (p)[1] = (uint8_t)(_v >> 8); \
} while(0)
#endif

int cmd_copy(uint8_t *dst, uint32_t dst_id, uint32_t src_id, uint32_t clip_id, 
             int dst_x1, int dst_y1, int dst_x2, int dst_y2, 
             int src_x1, int src_y1);

static inline int rdp_cmd_draw(uint8_t *buf,
                               uint32_t surface_id, uint32_t codec_id,
                               int dx1, int dy1, int dx2, int dy2) {
    if (!buf) return 24;
    PUT32(buf + 0, surface_id);
    PUT32(buf + 4, codec_id);
    PUT16(buf + 8, (uint16_t)dx1);
    PUT16(buf + 10, (uint16_t)dy1);
    PUT16(buf + 12, (uint16_t)(dx2 - dx1));
    PUT16(buf + 14, (uint16_t)(dy2 - dy1));
    return 24;
}

static inline int rdp_cmd_copy(uint8_t *buf,
                               uint32_t surface_id, uint32_t codec_id,
                               int dx1, int dy1, int dx2, int dy2,
                               int sx, int sy) {
    (void)sx;
    (void)sy;
    return rdp_cmd_draw(buf, surface_id, codec_id, dx1, dy1, dx2, dy2);
}

static inline int rdp_cmd_fill(uint8_t *buf,
                               uint32_t surface_id, uint32_t codec_id,
                               int x1, int y1, int x2, int y2) {
    return rdp_cmd_draw(buf, surface_id, codec_id, x1, y1, x2, y2);
}

static inline int rdp_cmd_load_hdr(uint8_t *buf, uint32_t surface_id,
                                   int x1, int y1, int x2, int y2) {
    if (!buf) return 20;
    PUT32(buf + 0, surface_id);
    PUT16(buf + 4, (uint16_t)x1);
    PUT16(buf + 6, (uint16_t)y1);
    PUT16(buf + 8, (uint16_t)(x2 - x1));
    PUT16(buf + 10, (uint16_t)(y2 - y1));
    return 20;
}

static inline int rdp_cmd_loadraw_hdr(uint8_t *buf, uint32_t surface_id,
                                      int x1, int y1, int x2, int y2) {
    return rdp_cmd_load_hdr(buf, surface_id, x1, y1, x2, y2);
}

static inline int rdp_cmd_flush(uint8_t *buf) {
    if (buf) buf[0] = 0;
    return 1;
}

static inline void tile_bounds(int tx, int ty, int frame_w, int frame_h,
                               int *x1, int *y1, int *w, int *h) {
    *x1 = tx * TILE_SIZE;
    *y1 = ty * TILE_SIZE;
    int x2 = *x1 + TILE_SIZE;
    int y2 = *y1 + TILE_SIZE;
    if (x2 > frame_w) x2 = frame_w;
    if (y2 > frame_h) y2 = frame_h;
    *w = x2 - *x1;
    *h = y2 - *y1;
}

static inline int tile_changed(uint32_t *curr, uint32_t *prev, int stride,
                               int x1, int y1, int w, int h) {
    if (!curr || !prev) return 1;
    for (int y = 0; y < h; y++) {
        if (memcmp(&curr[(y1 + y) * stride + x1],
                   &prev[(y1 + y) * stride + x1], w * sizeof(uint32_t)) != 0) {
            return 1;
        }
    }
    return 0;
}

struct scroll_rects {
    int src_x1, src_y1, src_x2, src_y2;  
    int dst_x1, dst_y1, dst_x2, dst_y2;  
    int exp_x1, exp_y1, exp_x2, exp_y2;  
    int valid;  
};

static inline void compute_scroll_rects(int rx1, int ry1, int rx2, int ry2,
                                        int dx, int dy, struct scroll_rects *r) {
    if (!r) return;
    int rw = rx2 - rx1;
    int rh = ry2 - ry1;
    int abs_dx = dx < 0 ? -dx : dx;
    int abs_dy = dy < 0 ? -dy : dy;
    
    if (rw < 16 || rh < 16 || abs_dx >= rw || abs_dy >= rh) {
        r->valid = 0;
        return;
    }
    r->valid = 1;
    
    r->dst_x1 = rx1;
    r->dst_x2 = rx2;
    r->dst_y1 = ry1;
    r->dst_y2 = ry2;
    
    r->src_x1 = rx1;
    r->src_x2 = rx2;
    r->src_y1 = ry1;
    r->src_y2 = ry2;
    
    if (dy < 0) {
        r->src_y1 = ry1 + abs_dy;
        r->dst_y2 = ry2 - abs_dy;
    } else if (dy > 0) {
        r->src_y2 = ry2 - abs_dy;
        r->dst_y1 = ry1 + abs_dy;
    }
    
    if (dx < 0) {
        r->src_x1 = rx1 + abs_dx;
        r->dst_x2 = rx2 - abs_dx;
    } else if (dx > 0) {
        r->src_x2 = rx2 - abs_dx;
        r->dst_x1 = rx1 + abs_dx;
    }
    
    r->exp_x1 = r->exp_x2 = 0;
    r->exp_y1 = r->exp_y2 = 0;
    
    if (dy < 0) {
        r->exp_y1 = (r->dst_y2 / TILE_SIZE) * TILE_SIZE;
        r->exp_y2 = ry2;
    } else if (dy > 0) {
        r->exp_y1 = ry1;
        r->exp_y2 = ((r->dst_y1 + TILE_SIZE - 1) / TILE_SIZE) * TILE_SIZE;
    }
    
    if (dx < 0) {
        r->exp_x1 = (r->dst_x2 / TILE_SIZE) * TILE_SIZE;
        r->exp_x2 = rx2;
    } else if (dx > 0) {
        r->exp_x1 = rx1;
        r->exp_x2 = ((r->dst_x1 + TILE_SIZE - 1) / TILE_SIZE) * TILE_SIZE;
    }
}

struct tls_config {
    char *cert_file;
    char *cert_fingerprint;
    int insecure;
};

int tls_init(void);
void tls_cleanup(void);
int tls_connect(int fd, SSL **ssl_out, struct tls_config *cfg);
void tls_disconnect(SSL *ssl);
int tls_verify_pinned(SSL *ssl, struct tls_config *cfg);
int tls_read_full(SSL *ssl, uint8_t *buf, int n);
int tls_write_full(SSL *ssl, const uint8_t *buf, int len);
int tls_cert_file_fingerprint(const char *path, char *out, int outlen);

/* ============== Input Handlers ============== */

void input_queue_init(struct input_queue *q);
void input_queue_push(struct input_queue *q, struct input_event *ev);
int input_queue_pop(struct input_queue *q, struct input_event *ev);
void rdp_handle_mouse(struct server *s, uint16_t flags, uint16_t x, uint16_t y);
void rdp_handle_keyboard(struct server *s, uint16_t flags, uint8_t keycode);

struct key_map {
    uint32_t keycode;
    uint8_t shift;
    uint8_t ctrl;
};

uint32_t keymapmod(uint32_t rune);
const struct key_map *keymap_lookup(uint32_t rune);

void new_popup(struct wl_listener *l, void *d);
void new_toplevel(struct wl_listener *l, void *d);

void handle_key(struct server *s, uint32_t rune, int pressed);
void handle_mouse(struct server *s, int mx, int my, int buttons);
int handle_input_events(int fd, uint32_t mask, void *data);

/* ============== Client / Cleanup API ============== */

void handle_new_decoration(struct wl_listener *l, void *d);
void handle_new_kb_inhibitor(struct wl_listener *l, void *d);
void server_cleanup(struct server *s);

/* ============== Output / Input Attachment ============== */

void new_output(struct wl_listener *l, void *d);
void new_input(struct wl_listener *l, void *d);

/* ============== Render / Send Pipeline ============== */

void send_frame(struct server *s);
int send_timer_callback(void *data);
void *send_thread_func(void *arg);

/* ============== Clipboard Interop ============== */

int clipboard_init(struct server *s);
void rdp_to_snarf_write(struct server *s, const void *data, size_t size);
void clipboard_cleanup(struct server *s);

/* ============== Focus Manager API ============== */

void focus_manager_init(struct focus_manager *fm, struct server *server);
void focus_manager_cleanup(struct focus_manager *fm);
struct wlr_surface *focus_surface_at_cursor(
    struct focus_manager *fm, double *out_sx, double *out_sy);
struct toplevel *focus_toplevel_from_surface(
    struct focus_manager *fm, struct wlr_surface *surface);
struct toplevel *focus_toplevel_at_cursor(struct focus_manager *fm);

void focus_pointer_set(
    struct focus_manager *fm, struct wlr_surface *surface,
    double sx, double sy, enum focus_reason reason);
void focus_pointer_motion(
    struct focus_manager *fm, double sx, double sy, uint32_t time_msec);
void focus_pointer_recheck(struct focus_manager *fm);
void focus_pointer_button_pressed(struct focus_manager *fm);
void focus_pointer_button_released(struct focus_manager *fm);

void focus_keyboard_set(
    struct focus_manager *fm, struct wlr_surface *surface, enum focus_reason reason);
void focus_keyboard_set_modifiers(
    struct focus_manager *fm, uint32_t modifiers);
uint32_t focus_keyboard_get_modifiers(struct focus_manager *fm);

void focus_toplevel(
    struct focus_manager *fm, struct toplevel *tl, enum focus_reason reason);
struct toplevel *focus_get_focused_toplevel(struct focus_manager *fm);

void focus_popup_register(struct focus_manager *fm, struct popup_data *pd);
void focus_popup_unregister(struct focus_manager *fm, struct popup_data *pd);
void focus_popup_mapped(struct focus_manager *fm, struct popup_data *pd);
void focus_popup_unmapped(struct focus_manager *fm, struct popup_data *pd);
struct popup_data *focus_popup_get_topmost(struct focus_manager *fm);
struct popup_data *focus_popup_from_surface(
    struct focus_manager *fm, struct wlr_surface *surface);
void focus_popup_dismiss_all(struct focus_manager *fm);
bool focus_popup_dismiss_topmost_grabbed(struct focus_manager *fm);
bool focus_popup_stack_empty(struct focus_manager *fm);

void focus_on_surface_map(
    struct focus_manager *fm, struct wlr_surface *surface, bool is_toplevel);
void focus_on_surface_unmap(
    struct focus_manager *fm, struct wlr_surface *surface);
void focus_on_surface_destroy(
    struct focus_manager *fm, struct wlr_surface *surface);

struct wlr_surface *focus_handle_click(
    struct focus_manager *fm, struct wlr_surface *clicked_surface,
    double sx, double sy, uint32_t button);

static inline int focus_phys_to_logical(int phys, float scale) {
    return (int)(phys / scale + 0.5f);
}
static inline int focus_logical_to_phys(int logical, float scale) {
    return (int)(logical * scale + 0.5f);
}

#endif /* P9WL_TYPES_H */