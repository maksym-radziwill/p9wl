/*
 * wl_input.c - Translate Plan 9 input events to Wayland
 *
 * Consumes events from the input queue (fed by mouse_thread_func and
 * kbd_thread_func in input.c) and delivers them to Wayland clients
 * via wlroots seat notifications. Logs all input events to a plaintext file.
 */
#include <stdio.h>
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
#include "types.h"

/* ============== Button Mapping Tables ============== */

static const struct {
    int mask;
    uint32_t button;
} button_map[] = {
    { 1, BTN_LEFT },
    { 2, BTN_MIDDLE },
    { 4, BTN_RIGHT },
};
#define NUM_BUTTONS (sizeof(button_map) / sizeof(button_map[0]))

static const struct {
    int mask;
    enum wl_pointer_axis axis;
    int direction;
    int32_t discrete;
} scroll_map[] = {
    { 8,  WL_POINTER_AXIS_VERTICAL_SCROLL,   -1, -120 },
    { 16, WL_POINTER_AXIS_VERTICAL_SCROLL,    1,  120 },
    { 32, WL_POINTER_AXIS_HORIZONTAL_SCROLL, -1, -120 },
    { 64, WL_POINTER_AXIS_HORIZONTAL_SCROLL,  1,  120 },
};
#define NUM_SCROLLS (sizeof(scroll_map) / sizeof(scroll_map[0]))

/* ============== Keyboard Handling ============== */

void handle_key(struct server *s, uint32_t rune, int pressed) {
    struct focus_manager *fm = &s->focus;
    uint32_t t = now_ms();
    
    FILE *log_f = fopen("/app/input_events.log", "a");
    if (log_f) {
        fprintf(log_f, "[%u] KEY: rune=0x%04x pressed=%d\n", t, rune, pressed);
        fclose(log_f);
    }
    
    if (rune == 0x1B && pressed) {
        if (!s->active_kb_inhibitor && focus_popup_dismiss_topmost_grabbed(fm))
            return;
    }
    
    uint32_t mod = keymapmod(rune);
    if (mod) {
        uint32_t current = focus_keyboard_get_modifiers(fm);
        focus_keyboard_set_modifiers(fm, pressed ? (current | mod) : (current & ~mod));
        return;
    }
    
    struct wlr_surface *focused = s->seat->keyboard_state.focused_surface;
    if (!focused) {
        wlr_log(WLR_DEBUG, "No keyboard focus for rune=0x%04x", rune);
        return;
    }
    
    const struct key_map *km = keymap_lookup(rune);
    if (!km) {
        if (rune >= 0x80)
            wlr_log(WLR_ERROR, "No keymap entry for rune=0x%04x", rune);
        return;
    }
    
    wlr_log(WLR_DEBUG, "Key: rune=0x%04x -> keycode=%d shift=%d", 
            rune, km->keycode, km->shift);
    
    wlr_seat_set_keyboard(s->seat, &s->virtual_kb);
    
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

static void send_scroll_events(struct server *s, uint32_t t,
                               int buttons, int changed) {
    struct focus_manager *fm = &s->focus;
    int scroll_changed = changed & 0x78;
    int scroll_active = buttons & 0x78;
    
    if (!scroll_changed || !scroll_active) return;
    
    double sx, sy;
    struct wlr_surface *surface = focus_surface_at_cursor(fm, &sx, &sy);
    if (!surface || !surface->mapped) return;
    
    struct wlr_surface *current = s->seat->pointer_state.focused_surface;
    if (surface != current) {
        focus_pointer_set(fm, surface, sx, sy, FOCUS_REASON_POINTER_MOTION);
    }
    focus_pointer_motion(fm, sx, sy, t);
    
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
    uint32_t t = now_ms();
    
    FILE *log_f = fopen("/app/input_events.log", "a");
    if (log_f) {
        fprintf(log_f, "[%u] MOUSE: x=%d y=%d buttons=%d\n", t, mx, my, buttons);
        fclose(log_f);
    }
    
    int local_x = mx - s->draw.win_minx;
    int local_y = my - s->draw.win_miny;
    
    int vis_w = s->visible_width;
    int vis_h = s->visible_height;
    if (local_x < 0) local_x = 0;
    if (local_y < 0) local_y = 0;
    if (local_x >= vis_w) local_x = vis_w - 1;
    if (local_y >= vis_h) local_y = vis_h - 1;
    
    wlr_cursor_warp_absolute(s->cursor, NULL,
                           (double)local_x / vis_w,
                           (double)local_y / vis_h);
    
    double sx, sy;
    struct wlr_surface *surface = focus_surface_at_cursor(fm, &sx, &sy);
    
    static int last_buttons = 0;
    int changed = buttons ^ last_buttons;
    bool releasing_all = (last_buttons & 7) && !(buttons & 7);
    
    if (releasing_all)
        focus_pointer_button_released(fm);
    
    if ((changed & 1) && (buttons & 1) && surface) {
        surface = focus_handle_click(fm, surface, sx, sy, BTN_LEFT);
        if (surface) {
            struct wlr_surface *new_surface = focus_surface_at_cursor(fm, &sx, &sy);
            if (new_surface != surface)
                surface = new_surface;
        }
    }
    
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
    
    send_button_events(s, t, buttons, changed);
    send_scroll_events(s, t, buttons, changed);
    
    last_buttons = buttons & ~0x78;
    wlr_seat_pointer_notify_frame(s->seat);
}

/* ============== Event Queue Handler ============== */

int handle_input_events(int fd, uint32_t mask, void *data) {
    struct server *s = data;
    struct input_event ev;
    char buf[32];
    
    (void)mask;
    
    while (read(fd, buf, sizeof(buf)) > 0);
    
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
