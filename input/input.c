/*
 * input.c - Input handling (keyboard, mouse, queue)
 *
 * Key definitions from 9front /sys/include/keyboard.h
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <linux/input-event-codes.h>

#include <wlr/util/log.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_keyboard.h>

#include "input.h"
#include "plan9_keys.h"
#include "../p9/p9.h"

/* Key mapping table */
const struct key_map keymap[] = {
    /* Control characters - Ctrl+A through Ctrl+Z */
    {0x01, KEY_A, 0, 1}, {0x02, KEY_B, 0, 1}, {0x03, KEY_C, 0, 1}, {0x04, KEY_D, 0, 1},
    {0x05, KEY_E, 0, 1}, {0x06, KEY_F, 0, 1}, {0x07, KEY_G, 0, 1}, {0x08, KEY_BACKSPACE, 0, 0},
    {0x09, KEY_TAB, 0, 0}, {0x0A, KEY_ENTER, 0, 0}, {0x0B, KEY_K, 0, 1}, {0x0C, KEY_L, 0, 1},
    {0x0D, KEY_ENTER, 0, 0}, {0x0E, KEY_N, 0, 1}, {0x0F, KEY_O, 0, 1},
    {0x10, KEY_P, 0, 1}, {0x11, KEY_Q, 0, 1}, {0x12, KEY_R, 0, 1}, {0x13, KEY_S, 0, 1},
    {0x14, KEY_T, 0, 1}, {0x15, KEY_U, 0, 1}, {0x16, KEY_V, 0, 1}, {0x17, KEY_W, 0, 1},
    {0x18, KEY_X, 0, 1}, {0x19, KEY_Y, 0, 1}, {0x1A, KEY_Z, 0, 1},
    /* Special control chars */
    {Kesc, KEY_ESC, 0, 0},
    {0x1C, KEY_BACKSLASH, 0, 1}, {0x1D, KEY_RIGHTBRACE, 0, 1}, {0x1F, KEY_SLASH, 0, 1},
    
    /* Navigation keys */
    {Kdel, KEY_DELETE, 0, 0},
    {Khome, KEY_HOME, 0, 0},
    {Kend, KEY_END, 0, 0},
    {Kup, KEY_UP, 0, 0},
    {Kdown, KEY_DOWN, 0, 0},
    {Kleft, KEY_LEFT, 0, 0},
    {Kright, KEY_RIGHT, 0, 0},
    {Kpgup, KEY_PAGEUP, 0, 0},
    {Kpgdown, KEY_PAGEDOWN, 0, 0},
    {Kins, KEY_INSERT, 0, 0},
    {Kprint, KEY_SYSRQ, 0, 0},
    {Kbreak, KEY_PAUSE, 0, 0},
    
    /* Scroll keys */
    {Kscrolloneup, KEY_PAGEUP, 0, 0},
    {Kscrollonedown, KEY_PAGEDOWN, 0, 0},
    
    /* Modifier keys */
    {Kshift, KEY_LEFTSHIFT, 0, 0},
    {Kctl, KEY_LEFTCTRL, 0, 0},
    {Kalt, KEY_LEFTALT, 0, 0},
    {Kmod4, KEY_LEFTMETA, 0, 0},
    {Kcaps, KEY_CAPSLOCK, 0, 0},
    {Knum, KEY_NUMLOCK, 0, 0},
    {Kaltgr, KEY_RIGHTALT, 0, 0},
    {Kscroll, KEY_SCROLLLOCK, 0, 0},
    
    /* Function keys */
    {KF1, KEY_F1, 0, 0}, {KF2, KEY_F2, 0, 0}, {KF3, KEY_F3, 0, 0}, {KF4, KEY_F4, 0, 0},
    {KF5, KEY_F5, 0, 0}, {KF6, KEY_F6, 0, 0}, {KF7, KEY_F7, 0, 0}, {KF8, KEY_F8, 0, 0},
    {KF9, KEY_F9, 0, 0}, {KF10, KEY_F10, 0, 0}, {KF11, KEY_F11, 0, 0}, {KF12, KEY_F12, 0, 0},
    
    /* Multimedia keys */
    {Ksbwd, KEY_PREVIOUSSONG, 0, 0},
    {Ksfwd, KEY_NEXTSONG, 0, 0},
    {Kpause, KEY_PLAYPAUSE, 0, 0},
    {Kvoldn, KEY_VOLUMEDOWN, 0, 0},
    {Kvolup, KEY_VOLUMEUP, 0, 0},
    {Kmute, KEY_MUTE, 0, 0},
    {Kbrtdn, KEY_BRIGHTNESSDOWN, 0, 0},
    {Kbrtup, KEY_BRIGHTNESSUP, 0, 0},
    
    /* Regular letters */
    {'a', KEY_A, 0, 0}, {'b', KEY_B, 0, 0}, {'c', KEY_C, 0, 0}, {'d', KEY_D, 0, 0},
    {'e', KEY_E, 0, 0}, {'f', KEY_F, 0, 0}, {'g', KEY_G, 0, 0}, {'h', KEY_H, 0, 0},
    {'i', KEY_I, 0, 0}, {'j', KEY_J, 0, 0}, {'k', KEY_K, 0, 0}, {'l', KEY_L, 0, 0},
    {'m', KEY_M, 0, 0}, {'n', KEY_N, 0, 0}, {'o', KEY_O, 0, 0}, {'p', KEY_P, 0, 0},
    {'q', KEY_Q, 0, 0}, {'r', KEY_R, 0, 0}, {'s', KEY_S, 0, 0}, {'t', KEY_T, 0, 0},
    {'u', KEY_U, 0, 0}, {'v', KEY_V, 0, 0}, {'w', KEY_W, 0, 0}, {'x', KEY_X, 0, 0},
    {'y', KEY_Y, 0, 0}, {'z', KEY_Z, 0, 0},
    {'A', KEY_A, 1, 0}, {'B', KEY_B, 1, 0}, {'C', KEY_C, 1, 0}, {'D', KEY_D, 1, 0},
    {'E', KEY_E, 1, 0}, {'F', KEY_F, 1, 0}, {'G', KEY_G, 1, 0}, {'H', KEY_H, 1, 0},
    {'I', KEY_I, 1, 0}, {'J', KEY_J, 1, 0}, {'K', KEY_K, 1, 0}, {'L', KEY_L, 1, 0},
    {'M', KEY_M, 1, 0}, {'N', KEY_N, 1, 0}, {'O', KEY_O, 1, 0}, {'P', KEY_P, 1, 0},
    {'Q', KEY_Q, 1, 0}, {'R', KEY_R, 1, 0}, {'S', KEY_S, 1, 0}, {'T', KEY_T, 1, 0},
    {'U', KEY_U, 1, 0}, {'V', KEY_V, 1, 0}, {'W', KEY_W, 1, 0}, {'X', KEY_X, 1, 0},
    {'Y', KEY_Y, 1, 0}, {'Z', KEY_Z, 1, 0},
    
    /* Numbers */
    {'0', KEY_0, 0, 0}, {'1', KEY_1, 0, 0}, {'2', KEY_2, 0, 0}, {'3', KEY_3, 0, 0},
    {'4', KEY_4, 0, 0}, {'5', KEY_5, 0, 0}, {'6', KEY_6, 0, 0}, {'7', KEY_7, 0, 0},
    {'8', KEY_8, 0, 0}, {'9', KEY_9, 0, 0},
    
    /* Symbols */
    {' ', KEY_SPACE, 0, 0}, {'!', KEY_1, 1, 0}, {'@', KEY_2, 1, 0}, {'#', KEY_3, 1, 0},
    {'$', KEY_4, 1, 0}, {'%', KEY_5, 1, 0}, {'^', KEY_6, 1, 0}, {'&', KEY_7, 1, 0},
    {'*', KEY_8, 1, 0}, {'(', KEY_9, 1, 0}, {')', KEY_0, 1, 0},
    {'-', KEY_MINUS, 0, 0}, {'_', KEY_MINUS, 1, 0}, {'=', KEY_EQUAL, 0, 0}, {'+', KEY_EQUAL, 1, 0},
    {'[', KEY_LEFTBRACE, 0, 0}, {'{', KEY_LEFTBRACE, 1, 0},
    {']', KEY_RIGHTBRACE, 0, 0}, {'}', KEY_RIGHTBRACE, 1, 0},
    {'\\', KEY_BACKSLASH, 0, 0}, {'|', KEY_BACKSLASH, 1, 0},
    {';', KEY_SEMICOLON, 0, 0}, {':', KEY_SEMICOLON, 1, 0},
    {'\'', KEY_APOSTROPHE, 0, 0}, {'"', KEY_APOSTROPHE, 1, 0},
    {',', KEY_COMMA, 0, 0}, {'<', KEY_COMMA, 1, 0},
    {'.', KEY_DOT, 0, 0}, {'>', KEY_DOT, 1, 0},
    {'/', KEY_SLASH, 0, 0}, {'?', KEY_SLASH, 1, 0},
    {'`', KEY_GRAVE, 0, 0}, {'~', KEY_GRAVE, 1, 0},
    {0, 0, 0, 0}
};

const struct key_map *keymap_lookup(uint32_t rune) {
    for (int i = 0; keymap[i].rune || keymap[i].keycode; i++) {
        if ((uint32_t)keymap[i].rune == rune)
            return &keymap[i];
    }
    wlr_log(WLR_ERROR, "keymap_lookup: NO ENTRY for rune=0x%04x (%d)", rune, rune);
    return NULL;
}

int utf8_decode(const unsigned char *p, const unsigned char *end, int *rune) {
    if (p >= end) return 0;
    
    unsigned char c = *p;
    if (c < 0x80) {
        *rune = c;
        return 1;
    } else if ((c & 0xE0) == 0xC0) {
        if (p + 1 >= end) return 0;
        *rune = ((c & 0x1F) << 6) | (p[1] & 0x3F);
        return 2;
    } else if ((c & 0xF0) == 0xE0) {
        if (p + 2 >= end) return 0;
        *rune = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        return 3;
    } else if ((c & 0xF8) == 0xF0) {
        if (p + 3 >= end) return 0;
        *rune = ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        return 4;
    }
    return 0;
}

uint32_t keymapmod(int rune) {
    switch (rune) {
    case Kshift: return WLR_MODIFIER_SHIFT;
    case Kctl:   return WLR_MODIFIER_CTRL;
    case Kalt:   return WLR_MODIFIER_ALT;
    case Kmod4:  return WLR_MODIFIER_LOGO;
    case Kcaps:  return WLR_MODIFIER_CAPS;
    case Knum:   return WLR_MODIFIER_MOD2;
    case Kaltgr: return WLR_MODIFIER_ALT;
    default:     return 0;
    }
}

/* ============== Input Queue ============== */

void input_queue_init(struct input_queue *q) {
    q->head = q->tail = 0;
    pthread_mutex_init(&q->lock, NULL);
    if (pipe(q->pipe_fd) < 0) {
        wlr_log(WLR_ERROR, "pipe failed: %s", strerror(errno));
        q->pipe_fd[0] = q->pipe_fd[1] = -1;
    }
    fcntl(q->pipe_fd[0], F_SETFL, O_NONBLOCK);
    
}

void input_queue_push(struct input_queue *q, struct input_event *ev) {
    pthread_mutex_lock(&q->lock);
    int next = (q->tail + 1) % INPUT_QUEUE_SIZE;
    if (next != q->head) {
        q->events[q->tail] = *ev;
        q->tail = next;
        char c = 1;
        if (write(q->pipe_fd[1], &c, 1) < 0) { /* ignore */ }
    }
    /* Drop the event if the ring buffer is full */
    pthread_mutex_unlock(&q->lock);
}

int input_queue_pop(struct input_queue *q, struct input_event *ev) {
    pthread_mutex_lock(&q->lock);
    if (q->head == q->tail) {
        pthread_mutex_unlock(&q->lock);
        return 0;
    }
    *ev = q->events[q->head];
    q->head = (q->head + 1) % INPUT_QUEUE_SIZE;
    pthread_mutex_unlock(&q->lock);
    return 1;
}

/* ============== Input Threads ============== */

static int key_in_set(int key, int *set, int count) {
    for (int i = 0; i < count; i++)
        if (set[i] == key) return 1;
    return 0;
}

void *mouse_thread_func(void *arg) {
    struct server *s = arg;
    struct p9conn *p9 = &s->p9_mouse;
    uint32_t mouse_fid;
    const char *wnames[] = { "mouse" };
    uint8_t buf[64];
    int mice_read = 0;
    
    mouse_fid = p9->next_fid++;
    if (p9_walk(p9, p9->root_fid, mouse_fid, 1, wnames) < 0) {
        wlr_log(WLR_ERROR, "Mouse thread: failed to walk to /dev/mouse");
        return NULL;
    }
    if (p9_open(p9, mouse_fid, OREAD, NULL) < 0) {
        wlr_log(WLR_ERROR, "Mouse thread: failed to open /dev/mouse");
        return NULL;
    }
    
    wlr_log(WLR_INFO, "Mouse thread started");
    
    while (s->running) {
        int n = p9_read(p9, mouse_fid, 0, sizeof(buf) - 1, buf);
        if (n <= 0) {
            if (s->running) wlr_log(WLR_ERROR, "Mouse thread: read failed");
            break;
        }
        
        buf[n] = '\0';
        mice_read++;
        
        if (buf[0] == 'm') {
            int x, y, buttons;
            if (sscanf((char*)buf + 1, "%d %d %d", &x, &y, &buttons) == 3) {
                struct input_event ev = {
                    .type = INPUT_MOUSE,
                    .mouse = { .x = x, .y = y, .buttons = buttons }
                };
                input_queue_push(&s->input_queue, &ev);
            }
        } else if (buf[0] == 'r') {
            wlr_log(WLR_INFO, "Mouse: resize notification");
            s->window_changed = 1;
            s->force_full_frame = 1;
            s->scene_dirty = 1;
            
            pthread_mutex_lock(&s->send_lock);
            pthread_cond_signal(&s->send_cond);
            pthread_mutex_unlock(&s->send_lock);
            
        }
    }
    
    wlr_log(WLR_INFO, "Mouse thread exiting (read %d events)", mice_read);
    return NULL;
}

void *kbd_thread_func(void *arg) {
    struct server *s = arg;
    struct p9conn *p9 = &s->p9_kbd;
    uint32_t kbd_fid;
    const char *wnames[1];
    uint8_t buf[256];
    int keys_read = 0;

    int prev_keys[16];
    int prev_nkeys = 0;

    kbd_fid = p9->next_fid++;
    wnames[0] = "kbd";
    if (p9_walk(p9, p9->root_fid, kbd_fid, 1, wnames) < 0) {
        wlr_log(WLR_INFO, "Kbd thread: /dev/kbd not found");
        return NULL;
    }
    if (p9_open(p9, kbd_fid, OREAD, NULL) < 0) {
        wlr_log(WLR_INFO, "Kbd thread: failed to open /dev/kbd");
        return NULL;
    }

    wlr_log(WLR_INFO, "Keyboard thread started");

    while (s->running) {
        int n = p9_read(p9, kbd_fid, 0, sizeof(buf) - 1, buf);
        if (n <= 0) {
            if (s->running) wlr_log(WLR_ERROR, "Kbd thread: read failed");
            break;
        }
        buf[n] = '\0';

        const unsigned char *p = buf;
        const unsigned char *end = buf + n;

        while (p < end) {
            const unsigned char *msg_end = memchr(p, '\0', end - p);
            if (!msg_end) break;

            char msg_type = *p++;

            if (msg_type == 'k' || msg_type == 'K') {
                int curr_keys[16];
                int curr_nkeys = 0;

                while (p < msg_end && curr_nkeys < 16) {
                    int rune;
                    int len = utf8_decode(p, msg_end, &rune);
                    if (len <= 0) { p++; continue; }
                    p += len;
                    curr_keys[curr_nkeys++] = rune;
                }

                for (int i = 0; i < curr_nkeys; i++) {
                    if (!key_in_set(curr_keys[i], prev_keys, prev_nkeys)) {
                        keys_read++;
                        struct input_event ev = {
                            .type = INPUT_KEY,
                            .key = { .rune = curr_keys[i], .pressed = 1 }
                        };
                        input_queue_push(&s->input_queue, &ev);
                    }
                }

                for (int i = 0; i < prev_nkeys; i++) {
                    if (!key_in_set(prev_keys[i], curr_keys, curr_nkeys)) {
                        struct input_event ev = {
                            .type = INPUT_KEY,
                            .key = { .rune = prev_keys[i], .pressed = 0 }
                        };
                        input_queue_push(&s->input_queue, &ev);
                    }
                }

                memcpy(prev_keys, curr_keys, curr_nkeys * sizeof(int));
                prev_nkeys = curr_nkeys;
            }

            p = msg_end + 1;
        }
    }

    wlr_log(WLR_INFO, "Keyboard thread exiting (read %d keys)", keys_read);
    return NULL;
}

