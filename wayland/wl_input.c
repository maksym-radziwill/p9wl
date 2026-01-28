/*
 * wl_input.c - Wayland input event handling (refactored)
 *
 * Translates Plan 9 input events to Wayland seat events.
 * Focus-related logic has been consolidated into focus_manager.c
 *
 * Key definitions from 9front /sys/include/keyboard.h:
 *   KF = 0xF000 (function key base)
 *   Kalt = KF|0x17 = 0xF017
 *   Kctl = KF|0x18 = 0xF018
 *   Kshift = KF|0x19 = 0xF019
 *   Kmod4 = KF|0x1A = 0xF01A
 *   Kaltgr = KF|0x33 = 0xF033
 */

#include <unistd.h>
#include <linux/input-event-codes.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_compositor.h>  /* For struct wlr_surface (->mapped) */
#include <wlr/util/log.h>

#include "wl_input.h"
#include "../types.h"  /* Includes focus_manager.h */
#include "../input/input.h"
#include "../input/clipboard.h"

/* 9front modifier key rune values - from /sys/include/keyboard.h */
#define KMOD_ALT     0xF015  /* Kalt = KF|0x15 */
#define KMOD_SHIFT   0xF016  /* Kshift = KF|0x16 */
#define KMOD_CTL     0xF017  /* Kctl = KF|0x17 */
#define KMOD_CAPS    0xF864  /* Kcaps = Spec|0x64 */
#define KMOD_NUM     0xF865  /* Knum = Spec|0x65 */
#define KMOD_ALTGR   0xF867  /* Kaltgr = Spec|0x67 */
#define KMOD_SUPER   0xF868  /* Kmod4 = Spec|0x68 */

/* ============== Keyboard Handling ============== */

void handle_key(struct server *s, uint32_t rune, int pressed) {
    static int key_count = 0;
    key_count++;
    
    /* Always log arrow keys for debugging */
    if (rune >= 0x80 || rune >= 0xF000) {
        wlr_log(WLR_INFO, "handle_key: rune=0x%04x (%d) %s", 
                rune, rune, pressed ? "press" : "release");
    }
    
    struct focus_manager *fm = &s->focus;
    
    /* Handle Escape key for popup dismissal */
    if (rune == 0x1B && pressed) {  /* ESC = 0x1B */
        if (focus_popup_dismiss_topmost_grabbed(fm)) {
            return;
        }
    }
    
    /* Handle modifier keys via focus manager */
    if (rune >= 0xF000) {
        uint32_t mod = 0;
        switch (rune) {
        case KMOD_SHIFT:
            mod = WLR_MODIFIER_SHIFT;
            break;
        case KMOD_CTL:
            mod = WLR_MODIFIER_CTRL;
            break;
        case KMOD_ALT:
            mod = WLR_MODIFIER_ALT;
            break;
        case KMOD_ALTGR:
            mod = WLR_MODIFIER_ALT;  /* Treat AltGr as Alt */
            break;
        case KMOD_SUPER:
            mod = WLR_MODIFIER_LOGO;
            break;
        case KMOD_CAPS:
            mod = WLR_MODIFIER_CAPS;
            break;
        case KMOD_NUM:
            mod = WLR_MODIFIER_MOD2;  /* Num Lock */
            break;
        }
        
        if (mod) {
            uint32_t current = focus_keyboard_get_modifiers(fm);
            if (pressed) {
                focus_keyboard_set_modifiers(fm, current | mod);
            } else {
                focus_keyboard_set_modifiers(fm, current & ~mod);
            }
            return;
        }
    }
    
    /* Check keyboard focus */
    struct wlr_surface *focused = s->seat->keyboard_state.focused_surface;
    if (!focused) {
        if (key_count <= 5) {
            wlr_log(WLR_ERROR, "handle_key: No keyboard focus! rune=0x%04x", rune);
        }
        return;
    }
    
    if (key_count <= 20 || key_count % 100 == 0) {
        wlr_log(WLR_INFO, "handle_key #%d: rune=0x%04x '%c' %s", 
                key_count, rune, (rune >= 32 && rune < 127) ? rune : '?',
                pressed ? "press" : "release");
    }
    
    /* Look up key mapping - try dynamic kbmap first, fall back to static */
    const struct key_map *km = keymap_lookup_dynamic(&s->kbmap, rune);
    if (km) {
        wlr_log(WLR_DEBUG, "handle_key: rune=0x%04x -> keycode=%d shift=%d", 
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
        return;
    }
    
    wlr_log(WLR_ERROR, "handle_key: UNHANDLED rune=0x%04x (%d) - no keymap entry!", rune, rune);
}

/* ============== Mouse Handling ============== */

void handle_mouse(struct server *s, int mx, int my, int buttons) {
    struct focus_manager *fm = &s->focus;
    
    /* Mouse coordinates from 9front are in PHYSICAL screen coordinates.
     * Compositor operates at LOGICAL resolution (s->width, s->height).
     * Convert physical mouse coords to logical using draw->input_scale.
     *
     * In 9front scaling mode: input_scale = user's scale (e.g., 1.5)
     * In Wayland scaling mode: input_scale = 1.0 (no conversion needed)
     * In fractional Wayland mode: input_scale = user's scale
     */
    float scale = s->draw.input_scale;
    if (scale <= 0.0f) scale = 1.0f;
    
    /* Convert to window-relative physical, then to logical */
    int phys_local_x = mx - s->draw.win_minx;
    int phys_local_y = my - s->draw.win_miny;
    
    int local_x = (int)(phys_local_x / scale + 0.5f);
    int local_y = (int)(phys_local_y / scale + 0.5f);
    
    /* s->width, s->height are the compositor's dimensions (logical in 9front mode) */
    int logical_w = s->width;
    int logical_h = s->height;
    
    /* Clamp to logical bounds */
    if (local_x < 0) local_x = 0;
    if (local_y < 0) local_y = 0;
    if (local_x >= logical_w) local_x = logical_w - 1;
    if (local_y >= logical_h) local_y = logical_h - 1;
    
    /* Update cursor position (using logical coordinates) */
    wlr_cursor_warp_absolute(s->cursor, NULL,
                             (double)local_x / logical_w,
                             (double)local_y / logical_h);
    
    /* Find surface under cursor */
    double sx, sy;
    struct wlr_surface *surface = focus_surface_at_cursor(fm, &sx, &sy);
    
    uint32_t t = now_ms();
    static int last_buttons = 0;
    int changed = buttons ^ last_buttons;
    bool releasing_all = (last_buttons & 7) && !(buttons & 7);
    
    /* Track button state for deferred focus */
    if (changed & 7) {
        if (buttons & 7) {
            focus_pointer_button_pressed(fm);
        }
        if (releasing_all) {
            focus_pointer_button_released(fm);
        }
    }
    
    /* Handle click for focus changes */
    if ((changed & 1) && (buttons & 1) && surface) {
        surface = focus_handle_click(fm, surface, sx, sy, BTN_LEFT);
        /* Re-query coordinates after potential popup dismissal */
        if (surface) {
            struct wlr_surface *new_surface = focus_surface_at_cursor(fm, &sx, &sy);
            if (new_surface != surface) {
                surface = new_surface;
            }
        }
    }
    
    /* Handle pointer focus and motion */
    if (surface) {
        struct wlr_surface *focused = s->seat->pointer_state.focused_surface;
        
        /* Update pointer focus if surface changed (respecting drag state) */
        if (surface != focused) {
            focus_pointer_set(fm, surface, sx, sy, FOCUS_REASON_POINTER_MOTION);
        }
        
        /* Send motion event */
        focus_pointer_motion(fm, sx, sy, t);
    } else {
        /* No surface under cursor */
        if ((changed & 1) && (buttons & 1) && !focus_popup_stack_empty(fm)) {
            focus_popup_dismiss_all(fm);
        }
        focus_pointer_clear(fm);
    }
    
    /* Button events - send to currently focused surface (REVERTED TO OLD LOGIC) */
    struct wlr_surface *btn_surface = s->seat->pointer_state.focused_surface;
    if (btn_surface && btn_surface->mapped) {
        if (changed & 1) {
            uint32_t state = (buttons & 1) ? WL_POINTER_BUTTON_STATE_PRESSED 
                                           : WL_POINTER_BUTTON_STATE_RELEASED;
            wlr_seat_pointer_notify_button(s->seat, t, BTN_LEFT, state);
        }
        if (changed & 2) {
            uint32_t state = (buttons & 2) ? WL_POINTER_BUTTON_STATE_PRESSED 
                                           : WL_POINTER_BUTTON_STATE_RELEASED;
            wlr_seat_pointer_notify_button(s->seat, t, BTN_MIDDLE, state);
        }
        if (changed & 4) {
            uint32_t state = (buttons & 4) ? WL_POINTER_BUTTON_STATE_PRESSED 
                                           : WL_POINTER_BUTTON_STATE_RELEASED;
            wlr_seat_pointer_notify_button(s->seat, t, BTN_RIGHT, state);
        }
    }
    
    /* Scroll events - use WHEEL source for discrete mouse wheel scrolling */
    if ((changed & (8|16|32|64)) && (buttons & (8|16|32|64))) {
        double scroll_sx, scroll_sy;
        struct wlr_surface *scroll_surface = focus_surface_at_cursor(fm, &scroll_sx, &scroll_sy);
        
        if (scroll_surface && scroll_surface->mapped) {
            /* Ensure focus on scroll target */
            struct wlr_surface *current = s->seat->pointer_state.focused_surface;
            if (scroll_surface != current) {
                focus_pointer_set(fm, scroll_surface, scroll_sx, scroll_sy, 
                                  FOCUS_REASON_POINTER_MOTION);
            }
            
            focus_pointer_motion(fm, scroll_sx, scroll_sy, t);
            
            /* Send scroll axis events using FINGER source for smooth scrolling */
            if ((changed & 8) && (buttons & 8)) {
                wlr_seat_pointer_notify_axis(s->seat, t, WL_POINTER_AXIS_VERTICAL_SCROLL,
                    -4.0, 0, WL_POINTER_AXIS_SOURCE_FINGER, 
                    WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
            }
            if ((changed & 16) && (buttons & 16)) {
                wlr_seat_pointer_notify_axis(s->seat, t, WL_POINTER_AXIS_VERTICAL_SCROLL,
                    4.0, 0, WL_POINTER_AXIS_SOURCE_FINGER,
                    WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
            }
            if ((changed & 32) && (buttons & 32)) {
                wlr_seat_pointer_notify_axis(s->seat, t, WL_POINTER_AXIS_HORIZONTAL_SCROLL,
                    -4.0, 0, WL_POINTER_AXIS_SOURCE_FINGER,
                    WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
            }
            if ((changed & 64) && (buttons & 64)) {
                wlr_seat_pointer_notify_axis(s->seat, t, WL_POINTER_AXIS_HORIZONTAL_SCROLL,
                    4.0, 0, WL_POINTER_AXIS_SOURCE_FINGER,
                    WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
            }
            
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
    static int call_count = 0;
    int events_processed = 0;
    
    (void)mask;
    call_count++;
    
    /* Drain pipe */
    while (read(fd, buf, sizeof(buf)) > 0);
    
    /* Process all queued events */
    while (input_queue_pop(&s->input_queue, &ev)) {
        events_processed++;
        switch (ev.type) {
        case INPUT_MOUSE:
            handle_mouse(s, ev.mouse.x, ev.mouse.y, ev.mouse.buttons);
            break;
        case INPUT_KEY:
            handle_key(s, ev.key.rune, ev.key.pressed);
            break;
        }
    }
    
    if (events_processed > 0 && (call_count <= 20 || call_count % 100 == 0)) {
        wlr_log(WLR_INFO, "handle_input_events call #%d: processed %d events", 
                call_count, events_processed);
    }
    
    return 0;
}
