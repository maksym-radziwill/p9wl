/*
 * wl_input.c - Translate Plan 9 input events to Wayland
 *
 * Consumes events from the input queue (fed by mouse_thread_func and
 * kbd_thread_func in input.c) and delivers them to Wayland clients
 * via wlroots seat notifications.
 *
 * Keyboard: Plan 9 runes are mapped to Linux keycodes via keymap_lookup().
 * Modifier state is tracked through the focus manager.
 *
 * Mouse: Plan 9 absolute coordinates and button bitmask are translated
 * to Wayland pointer motion, button, and scroll axis events.
 */

#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

#include "wl_input.h"
#include "../types.h"
#include "../input/input.h"
#include "../input/clipboard.h"

/* ============== Button Mapping Tables ============== */

/* Mouse button mapping: bitmask -> Linux button code */
static const struct {
    int mask;
    uint32_t button;
} button_map[] = {
    { 1, BTN_LEFT },
    { 2, BTN_MIDDLE },
    { 4, BTN_RIGHT },
};
#define NUM_BUTTONS (sizeof(button_map) / sizeof(button_map[0]))

/* Scroll axis mapping: bitmask -> axis, direction, discrete step */
static const struct {
    int mask;
    enum wl_pointer_axis axis;
    int direction;       /* -1 or +1 */
    int32_t discrete;    /* ±120 per notch (Wayland axis_value120 convention) */
} scroll_map[] = {
    { 8,  WL_POINTER_AXIS_VERTICAL_SCROLL,   -1, -120 },  /* Scroll up */
    { 16, WL_POINTER_AXIS_VERTICAL_SCROLL,    1,  120 },  /* Scroll down */
    { 32, WL_POINTER_AXIS_HORIZONTAL_SCROLL, -1, -120 },  /* Scroll left */
    { 64, WL_POINTER_AXIS_HORIZONTAL_SCROLL,  1,  120 },  /* Scroll right */
};
#define NUM_SCROLLS (sizeof(scroll_map) / sizeof(scroll_map[0]))

/*
 * Scroll source.
 *
 * Plan 9's /dev/mouse delivers scroll as discrete button bits (8/16/32/64)
 * regardless of the physical device. Even trackpad swipes arrive as
 * individual button events. We always report SOURCE_WHEEL with discrete
 * step counts since that's what the events look like by the time they
 * reach us.
 */

/* ============== Keyboard Handling ============== */

void handle_key(struct server *s, uint32_t rune, int pressed) {
    struct focus_manager *fm = &s->focus;
    
    /* Handle Escape for popup dismissal (unless keyboard shortcuts are inhibited,
     * e.g. during fullscreen video — let the client handle Escape itself) */
    if (rune == 0x1B && pressed) {
        if (!s->active_kb_inhibitor && focus_popup_dismiss_topmost_grabbed(fm))
            return;
    }
    
    /* Handle modifier keys - use keymapmod() as single source of truth */
    uint32_t mod = keymapmod(rune);
    if (mod) {
        uint32_t current = focus_keyboard_get_modifiers(fm);
        focus_keyboard_set_modifiers(fm, pressed ? (current | mod) : (current & ~mod));
        return;
    }
    
    /* Check keyboard focus */
    struct wlr_surface *focused = s->seat->keyboard_state.focused_surface;
    if (!focused) {
        wlr_log(WLR_DEBUG, "No keyboard focus for rune=0x%04x", rune);
        return;
    }
    
    /* Look up key mapping */
    const struct key_map *km = keymap_lookup(rune);
    if (!km) {
        if (rune >= 0x80)
            wlr_log(WLR_ERROR, "No keymap entry for rune=0x%04x", rune);
        return;
    }
    
    wlr_log(WLR_DEBUG, "Key: rune=0x%04x -> keycode=%d shift=%d", 
            rune, km->keycode, km->shift);
    
    uint32_t t = now_ms();
    wlr_seat_set_keyboard(s->seat, &s->virtual_kb);
    
    /* Handle temporary modifiers from keymap */
    uint32_t key_mods = 0;
    if (km->shift) key_mods |= WLR_MODIFIER_SHIFT;
    if (km->ctrl) key_mods |= WLR_MODIFIER_CTRL;
    
    if (key_mods && pressed) {
        uint32_t current = focus_keyboard_get_modifiers(fm);
        focus_keyboard_set_modifiers(fm, current | key_mods);
    }
    
    uint32_t state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED 
                             : WL_KEYBOARD_KEY_STATE_RELEASED;
    wlr_seat_keyboard_notify_key(s->seat, t, km->keycode, state);
    
    if (key_mods && !pressed) {
        uint32_t current = focus_keyboard_get_modifiers(fm);
        focus_keyboard_set_modifiers(fm, current & ~key_mods);
    }
}

/* ============== Mouse Handling ============== */

/*
 * Send button events for all changed buttons.
 * Uses table-driven approach for cleaner code.
 */
static void send_button_events(struct server *s, uint32_t t, 
                               int buttons, int changed) {
    struct wlr_surface *surface = s->seat->pointer_state.focused_surface;
    if (!surface || !surface->mapped) return;
    
    for (size_t i = 0; i < NUM_BUTTONS; i++) {
        if (changed & button_map[i].mask) {
            uint32_t state = (buttons & button_map[i].mask)
                ? WL_POINTER_BUTTON_STATE_PRESSED
                : WL_POINTER_BUTTON_STATE_RELEASED;
            wlr_seat_pointer_notify_button(s->seat, t, button_map[i].button, state);
        }
    }
}

/*
 * Send scroll events for all active scroll buttons.
 */
static void send_scroll_events(struct server *s, uint32_t t,
                               int buttons, int changed) {
    struct focus_manager *fm = &s->focus;
    int scroll_changed = changed & 0x78;
    int scroll_active = buttons & 0x78;
    
    if (!scroll_changed || !scroll_active) return;
    
    double sx, sy;
    struct wlr_surface *surface = focus_surface_at_cursor(fm, &sx, &sy);
    if (!surface || !surface->mapped) return;
    
    /* Ensure focus is on scroll target */
    struct wlr_surface *current = s->seat->pointer_state.focused_surface;
    if (surface != current) {
        focus_pointer_set(fm, surface, sx, sy, FOCUS_REASON_POINTER_MOTION);
    }
    focus_pointer_motion(fm, sx, sy, t);
    
    /* Send scroll events */
    for (size_t i = 0; i < NUM_SCROLLS; i++) {
        if ((changed & scroll_map[i].mask) && (buttons & scroll_map[i].mask)) {
            wlr_seat_pointer_notify_axis(s->seat, t, scroll_map[i].axis,
                scroll_map[i].direction * 15.0,
                scroll_map[i].discrete,
                WL_POINTER_AXIS_SOURCE_WHEEL,
                WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
        }
    }
}

void handle_mouse(struct server *s, int mx, int my, int buttons) {
    struct focus_manager *fm = &s->focus;
    
    /* Translate to window-local coordinates */
    int local_x = mx - s->draw.win_minx;
    int local_y = my - s->draw.win_miny;
    
    /* Clamp to window bounds */
    if (local_x < 0) local_x = 0;
    if (local_y < 0) local_y = 0;
    if (local_x >= s->width) local_x = s->width - 1;
    if (local_y >= s->height) local_y = s->height - 1;
    
    /* Update cursor */
    wlr_cursor_warp_absolute(s->cursor, NULL,
                             (double)local_x / s->width,
                             (double)local_y / s->height);
    
    /* Find surface under cursor */
    double sx, sy;
    struct wlr_surface *surface = focus_surface_at_cursor(fm, &sx, &sy);
    
    uint32_t t = now_ms();
    static int last_buttons = 0;
    int changed = buttons ^ last_buttons;
    bool releasing_all = (last_buttons & 7) && !(buttons & 7);
    
    /* Handle button release for deferred focus */
    if (releasing_all)
        focus_pointer_button_released(fm);
    
    /* Handle click for focus changes */
    if ((changed & 1) && (buttons & 1) && surface) {
        surface = focus_handle_click(fm, surface, sx, sy, BTN_LEFT);
        if (surface) {
            struct wlr_surface *new_surface = focus_surface_at_cursor(fm, &sx, &sy);
            if (new_surface != surface)
                surface = new_surface;
        }
    }
    
    /* Handle pointer focus and motion */
    if (surface) {
        struct wlr_surface *focused = s->seat->pointer_state.focused_surface;
        if (surface != focused)
            focus_pointer_set(fm, surface, sx, sy, FOCUS_REASON_POINTER_MOTION);
        focus_pointer_motion(fm, sx, sy, t);
    } else {
        if ((changed & 1) && (buttons & 1) && !focus_popup_stack_empty(fm))
            focus_popup_dismiss_all(fm);
        focus_pointer_set(fm, NULL, 0, 0, FOCUS_REASON_EXPLICIT);
    }
    
    /* Button and scroll events */
    send_button_events(s, t, buttons, changed);
    send_scroll_events(s, t, buttons, changed);
    
    last_buttons = buttons & ~0x78;  /* Scroll bits are instantaneous, not holdable */
    wlr_seat_pointer_notify_frame(s->seat);
}

/* ============== Event Queue Handler ============== */

int handle_input_events(int fd, uint32_t mask, void *data) {
    struct server *s = data;
    struct input_event ev;
    char buf[32];
    
    (void)mask;
    
    /* Drain pipe */
    while (read(fd, buf, sizeof(buf)) > 0);
    
    /* Process all queued events */
    while (input_queue_pop(&s->input_queue, &ev)) {
        switch (ev.type) {
        case INPUT_MOUSE:
            handle_mouse(s, ev.mouse.x, ev.mouse.y, ev.mouse.buttons);
            break;
        case INPUT_KEY:
            handle_key(s, ev.key.rune, ev.key.pressed);
            break;
        case INPUT_WAKEUP:
            wlr_output_schedule_frame(s->output);
            break;
        }
    }
    
    return 0;
}
