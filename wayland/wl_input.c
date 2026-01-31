/*
 * wl_input.c - Wayland input event handling (streamlined)
 *
 * Removed:
 * - Duplicate KMOD_* definitions (use keymapmod() from input.c)
 * - Redundant modifier switch statement
 */

#include <unistd.h>
#include <linux/input-event-codes.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/util/log.h>

#include "wl_input.h"
#include "../types.h"
#include "../input/input.h"
#include "../input/clipboard.h"

/* ============== Keyboard Handling ============== */

void handle_key(struct server *s, uint32_t rune, int pressed) {
    struct focus_manager *fm = &s->focus;
    
    /* Handle Escape for popup dismissal */
    if (rune == 0x1B && pressed) {
        if (focus_popup_dismiss_topmost_grabbed(fm))
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
    const struct key_map *km = keymap_lookup_dynamic(&s->kbmap, rune);
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
    
    /* Button events */
    struct wlr_surface *btn_surface = s->seat->pointer_state.focused_surface;
    if (btn_surface && btn_surface->mapped) {
        if (changed & 1) {
            wlr_seat_pointer_notify_button(s->seat, t, BTN_LEFT,
                (buttons & 1) ? WL_POINTER_BUTTON_STATE_PRESSED 
                              : WL_POINTER_BUTTON_STATE_RELEASED);
        }
        if (changed & 2) {
            wlr_seat_pointer_notify_button(s->seat, t, BTN_MIDDLE,
                (buttons & 2) ? WL_POINTER_BUTTON_STATE_PRESSED 
                              : WL_POINTER_BUTTON_STATE_RELEASED);
        }
        if (changed & 4) {
            wlr_seat_pointer_notify_button(s->seat, t, BTN_RIGHT,
                (buttons & 4) ? WL_POINTER_BUTTON_STATE_PRESSED 
                              : WL_POINTER_BUTTON_STATE_RELEASED);
        }
    }
    
    /* Scroll events */
    if ((changed & 0x78) && (buttons & 0x78)) {
        double scroll_sx, scroll_sy;
        struct wlr_surface *scroll_surface = focus_surface_at_cursor(fm, &scroll_sx, &scroll_sy);
        
        if (scroll_surface && scroll_surface->mapped) {
            struct wlr_surface *current = s->seat->pointer_state.focused_surface;
            if (scroll_surface != current)
                focus_pointer_set(fm, scroll_surface, scroll_sx, scroll_sy, 
                                  FOCUS_REASON_POINTER_MOTION);
            
            focus_pointer_motion(fm, scroll_sx, scroll_sy, t);
            
            if ((changed & 8) && (buttons & 8))
                wlr_seat_pointer_notify_axis(s->seat, t, WL_POINTER_AXIS_VERTICAL_SCROLL,
                    -4.0, 0, WL_POINTER_AXIS_SOURCE_FINGER, 
                    WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
            if ((changed & 16) && (buttons & 16))
                wlr_seat_pointer_notify_axis(s->seat, t, WL_POINTER_AXIS_VERTICAL_SCROLL,
                    4.0, 0, WL_POINTER_AXIS_SOURCE_FINGER,
                    WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
            if ((changed & 32) && (buttons & 32))
                wlr_seat_pointer_notify_axis(s->seat, t, WL_POINTER_AXIS_HORIZONTAL_SCROLL,
                    -4.0, 0, WL_POINTER_AXIS_SOURCE_FINGER,
                    WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
            if ((changed & 64) && (buttons & 64))
                wlr_seat_pointer_notify_axis(s->seat, t, WL_POINTER_AXIS_HORIZONTAL_SCROLL,
                    4.0, 0, WL_POINTER_AXIS_SOURCE_FINGER,
                    WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
            
            wlr_seat_pointer_notify_frame(s->seat);
        }
    }
    
    last_buttons = buttons;
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
        }
    }
    
    return 0;
}
