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
#include <linux/input-event-codes.h>

#include <wlr/util/log.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_keyboard.h>

#include "input.h"
#include "kbmap.h"
#include "../p9/p9.h"

/* Forward declarations for functions defined in wayland.c */
extern bool dismiss_topmost_grabbed_popup(struct server *s);

/*
 * 9front keyboard.h key definitions
 * KF = 0xF000 is the base for function/special keys
 *
 * From 9front /sys/include/keyboard.h:
 *   KF=0xF000
 *   KF|1 through KF|0xC are F1-F12
 *   Khome=KF|0x0D, Kup=KF|0x0E, Kpgup=KF|0x0F, Kprint=KF|0x10
 *   Kleft=KF|0x11, Kright=KF|0x12, Kdown=KF|0x13, Kview=KF|0x14
 *   Kpgdown=KF|0x15, Kins=KF|0x16, Kalt=KF|0x17, Kctl=KF|0x18
 *   Kshift=KF|0x19, Kmod4=KF|0x1A, Kend=KF|0x1B
 *   Kscrollonedown=KF|0x20, Kscrolloneup=KF|0x21
 *   Kbreak=KF|0x2F, Kesc=KF|0x30
 *   Kcaps=KF|0x31, Knum=KF|0x32, Kaltgr=KF|0x33
 */

/* Redefine key constants to match 9front keyboard.h */
#undef KF
#undef Khome
#undef Kup
#undef Kpgup
#undef Kprint
#undef Kleft
#undef Kright
#undef Kdown
#undef Kview
#undef Kpgdown
#undef Kins
#undef Kalt
#undef Kctl
#undef Kshift
#undef Kmod4
#undef Kend
#undef Kdel
#undef Kscrollonedown
#undef Kscrolloneup
#undef Kbreak
#undef Kesc
#undef Kcaps
#undef Knum
#undef Kaltgr
#undef KF1
#undef KF2
#undef KF3
#undef KF4
#undef KF5
#undef KF6
#undef KF7
#undef KF8
#undef KF9
#undef KF10
#undef KF11
#undef KF12

/*
 * Key definitions from Plan 9 /sys/include/keyboard.h
 */
#define KF              0xF000  /* Beginning of private Unicode space */
#define Spec            0xF800  /* Special keys base */
#define PF              (Spec|0x20)  /* Num pad function key */

/* Function keys F1-F12 */
#define KF1             (KF|0x01)  /* 0xF001 */
#define KF2             (KF|0x02)  /* 0xF002 */
#define KF3             (KF|0x03)  /* 0xF003 */
#define KF4             (KF|0x04)  /* 0xF004 */
#define KF5             (KF|0x05)  /* 0xF005 */
#define KF6             (KF|0x06)  /* 0xF006 */
#define KF7             (KF|0x07)  /* 0xF007 */
#define KF8             (KF|0x08)  /* 0xF008 */
#define KF9             (KF|0x09)  /* 0xF009 */
#define KF10            (KF|0x0A)  /* 0xF00A */
#define KF11            (KF|0x0B)  /* 0xF00B */
#define KF12            (KF|0x0C)  /* 0xF00C */

/* Navigation keys - ACTUAL Plan 9 values */
#define Kview           (Spec|0x00)  /* 0xF800 - view (shift window up) */
#define Kdown           Kview        /* 0xF800 - Down arrow = Kview */
#define Khome           (KF|0x0D)    /* 0xF00D */
#define Kup             (KF|0x0E)    /* 0xF00E - Up arrow */
#define Kpgup           (KF|0x0F)    /* 0xF00F - Page Up */
#define Kprint          (KF|0x10)    /* 0xF010 - Print Screen */
#define Kleft           (KF|0x11)    /* 0xF011 - Left arrow */
#define Kright          (KF|0x12)    /* 0xF012 - Right arrow */
#define Kpgdown         (KF|0x13)    /* 0xF013 - Page Down */
#define Kins            (KF|0x14)    /* 0xF014 - Insert */

/* Modifier keys - ACTUAL Plan 9 values */
#define Kalt            (KF|0x15)    /* 0xF015 - Alt */
#define Kshift          (KF|0x16)    /* 0xF016 - Shift */
#define Kctl            (KF|0x17)    /* 0xF017 - Control */
#define Kend            (KF|0x18)    /* 0xF018 - End */
#define Kscroll         (KF|0x19)    /* 0xF019 - Scroll Lock */

/* Scroll keys */
#define Kscrolloneup    (KF|0x20)    /* 0xF020 */
#define Kscrollonedown  (KF|0x21)    /* 0xF021 */

/* Multimedia keys */
#define Ksbwd           (KF|0x22)    /* 0xF022 - skip backwards */
#define Ksfwd           (KF|0x23)    /* 0xF023 - skip forward */
#define Kpause          (KF|0x24)    /* 0xF024 - play/pause */
#define Kvoldn          (KF|0x25)    /* 0xF025 - volume down */
#define Kvolup          (KF|0x26)    /* 0xF026 - volume up */
#define Kmute           (KF|0x27)    /* 0xF027 - mute */
#define Kbrtdn          (KF|0x28)    /* 0xF028 - brightness down */
#define Kbrtup          (KF|0x29)    /* 0xF029 - brightness up */

/* Special keys - Spec base (0xF800) */
#define Kbreak          (Spec|0x61)  /* 0xF861 - Break/Pause */
#define Kcaps           (Spec|0x64)  /* 0xF864 - Caps Lock */
#define Knum            (Spec|0x65)  /* 0xF865 - Num Lock */
#define Kmiddle         (Spec|0x66)  /* 0xF866 - Middle (mouse?) */
#define Kaltgr          (Spec|0x67)  /* 0xF867 - AltGr */
#define Kmod4           (Spec|0x68)  /* 0xF868 - Super/Windows/Mod4 */

/* ASCII control characters */
#define Ksoh            0x01
#define Kstx            0x02
#define Ketx            0x03
#define Keof            0x04
#define Kenq            0x05
#define Kack            0x06
#define Kbs             0x08
#define Knack           0x15
#define Ketb            0x17
#define Kdel            0x7F
#define Kesc            0x1B
#define Kdel            0x7F       /* Delete key sends ASCII DEL */

/* Key mapping table */
const struct key_map keymap[] = {
    /* Control characters - Ctrl+A through Ctrl+Z */
    {0x01, KEY_A, 0, 1}, {0x02, KEY_B, 0, 1}, {0x03, KEY_C, 0, 1}, {0x04, KEY_D, 0, 1},
    {0x05, KEY_E, 0, 1}, {0x06, KEY_F, 0, 1}, {0x07, KEY_G, 0, 1}, {0x08, KEY_BACKSPACE, 0, 0}, /* Ctrl+H = backspace */
    {0x09, KEY_TAB, 0, 0}, {0x0A, KEY_ENTER, 0, 0}, {0x0B, KEY_K, 0, 1}, {0x0C, KEY_L, 0, 1},
    {0x0D, KEY_ENTER, 0, 0}, {0x0E, KEY_N, 0, 1}, {0x0F, KEY_O, 0, 1},
    {0x10, KEY_P, 0, 1}, {0x11, KEY_Q, 0, 1}, {0x12, KEY_R, 0, 1}, {0x13, KEY_S, 0, 1},
    {0x14, KEY_T, 0, 1}, {0x15, KEY_U, 0, 1}, {0x16, KEY_V, 0, 1}, {0x17, KEY_W, 0, 1},
    {0x18, KEY_X, 0, 1}, {0x19, KEY_Y, 0, 1}, {0x1A, KEY_Z, 0, 1},
    /* Special control chars */
    {0x1B, KEY_ESC, 0, 0}, /* ESC (ASCII) */
    {0x1C, KEY_BACKSLASH, 0, 1}, /* Ctrl+\ */
    {0x1D, KEY_RIGHTBRACE, 0, 1}, /* Ctrl+] */
    {0x1F, KEY_SLASH, 0, 1}, /* Ctrl+/ or Ctrl+_ */
    
    /* 
     * Special keys - values from 9front /sys/include/keyboard.h
     * These are the rune values that Plan 9 sends for special keys
     */
    
    /* Navigation keys - using correct Plan 9 values */
    {Kdel, KEY_DELETE, 0, 0},           /* 0x7F - ASCII DEL */
    {Khome, KEY_HOME, 0, 0},            /* 0xF00D */
    {Kend, KEY_END, 0, 0},              /* 0xF018 */
    {Kup, KEY_UP, 0, 0},                /* 0xF00E */
    {Kdown, KEY_DOWN, 0, 0},            /* 0xF800 (= Kview) */
    {Kleft, KEY_LEFT, 0, 0},            /* 0xF011 */
    {Kright, KEY_RIGHT, 0, 0},          /* 0xF012 */
    {Kpgup, KEY_PAGEUP, 0, 0},          /* 0xF00F */
    {Kpgdown, KEY_PAGEDOWN, 0, 0},      /* 0xF013 */
    {Kins, KEY_INSERT, 0, 0},           /* 0xF014 */
    {Kprint, KEY_SYSRQ, 0, 0},          /* 0xF010 */
    {Kbreak, KEY_PAUSE, 0, 0},          /* 0xF861 */
    
    /* Scroll keys (rio scroll commands) */
    {Kscrolloneup, KEY_PAGEUP, 0, 0},   /* 0xF020 */
    {Kscrollonedown, KEY_PAGEDOWN, 0, 0}, /* 0xF021 */
    
    /* Modifier keys - correct Plan 9 values */
    {Kshift, KEY_LEFTSHIFT, 0, 0},      /* 0xF016 */
    {Kctl, KEY_LEFTCTRL, 0, 0},         /* 0xF017 */
    {Kalt, KEY_LEFTALT, 0, 0},          /* 0xF015 */
    {Kmod4, KEY_LEFTMETA, 0, 0},        /* 0xF868 */
    {Kcaps, KEY_CAPSLOCK, 0, 0},        /* 0xF864 */
    {Knum, KEY_NUMLOCK, 0, 0},          /* 0xF865 */
    {Kaltgr, KEY_RIGHTALT, 0, 0},       /* 0xF867 */
    {Kscroll, KEY_SCROLLLOCK, 0, 0},    /* 0xF019 */
    
    /* Function keys */
    {KF1, KEY_F1, 0, 0},                /* 0xF001 */
    {KF2, KEY_F2, 0, 0},                /* 0xF002 */
    {KF3, KEY_F3, 0, 0},                /* 0xF003 */
    {KF4, KEY_F4, 0, 0},                /* 0xF004 */
    {KF5, KEY_F5, 0, 0},                /* 0xF005 */
    {KF6, KEY_F6, 0, 0},                /* 0xF006 */
    {KF7, KEY_F7, 0, 0},                /* 0xF007 */
    {KF8, KEY_F8, 0, 0},                /* 0xF008 */
    {KF9, KEY_F9, 0, 0},                /* 0xF009 */
    {KF10, KEY_F10, 0, 0},              /* 0xF00A */
    {KF11, KEY_F11, 0, 0},              /* 0xF00B */
    {KF12, KEY_F12, 0, 0},              /* 0xF00C */
    
    /* Multimedia keys */
    {Ksbwd, KEY_PREVIOUSSONG, 0, 0},    /* 0xF022 - skip backwards */
    {Ksfwd, KEY_NEXTSONG, 0, 0},        /* 0xF023 - skip forward */
    {Kpause, KEY_PLAYPAUSE, 0, 0},      /* 0xF024 - play/pause */
    {Kvoldn, KEY_VOLUMEDOWN, 0, 0},     /* 0xF025 - volume down */
    {Kvolup, KEY_VOLUMEUP, 0, 0},       /* 0xF026 - volume up */
    {Kmute, KEY_MUTE, 0, 0},            /* 0xF027 - mute */
    {Kbrtdn, KEY_BRIGHTNESSDOWN, 0, 0}, /* 0xF028 - brightness down */
    {Kbrtup, KEY_BRIGHTNESSUP, 0, 0},   /* 0xF029 - brightness up */
    
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

/* Look up a key mapping by rune (static fallback table) */
const struct key_map *keymap_lookup(uint32_t rune) {
    for (int i = 0; keymap[i].rune || keymap[i].keycode; i++) {
        if ((uint32_t)keymap[i].rune == rune) {
            return &keymap[i];
        }
    }
    /* Log when special keys are not found */
    if (rune >= 0x80 || rune >= KF) {
        wlr_log(WLR_ERROR, "keymap_lookup: NO ENTRY for rune=0x%04x (%d)", rune, rune);
    }
    return NULL;
}

/*
 * Look up a key mapping by rune, using dynamic kbmap if available.
 * Falls back to static keymap table.
 *
 * This is the preferred lookup function when you have access to the
 * server's kbmap (loaded from /dev/kbmap).
 */
static struct key_map dynamic_result;  /* Static storage for dynamic lookup result */

const struct key_map *keymap_lookup_dynamic(struct kbmap *km, uint32_t rune) {
    /* Try dynamic keymap first */
    if (km && km->loaded) {
        const struct kbmap_entry *entry = kbmap_lookup(km, (int)rune);
        if (entry) {
            dynamic_result.rune = entry->rune;
            dynamic_result.keycode = entry->keycode;
            dynamic_result.shift = entry->shift;
            dynamic_result.ctrl = entry->ctrl;
            return &dynamic_result;
        }
    }
    
    /* Fall back to static keymap */
    return keymap_lookup(rune);
}

/* Decode UTF-8 rune from buffer, return bytes consumed (1-4) or 0 on error */
int utf8_decode(const unsigned char *p, const unsigned char *end, int *rune) {
    if (p >= end) return 0;
    
    unsigned char c = *p;
    if (c < 0x80) {
        *rune = c;
        return 1;
    } else if ((c & 0xE0) == 0xC0) {
        /* 2-byte sequence */
        if (p + 1 >= end) return 0;
        *rune = ((c & 0x1F) << 6) | (p[1] & 0x3F);
        return 2;
    } else if ((c & 0xF0) == 0xE0) {
        /* 3-byte sequence */
        if (p + 2 >= end) return 0;
        *rune = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        return 3;
    } else if ((c & 0xF8) == 0xF0) {
        /* 4-byte sequence */
        if (p + 3 >= end) return 0;
        *rune = ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        return 4;
    }
    return 0;  /* Invalid */
}

/* Get modifier mask for a rune - used for tracking held modifiers */
uint32_t keymapmod(int rune) {
    switch (rune) {
    case Kshift: return WLR_MODIFIER_SHIFT;
    case Kctl:   return WLR_MODIFIER_CTRL;
    case Kalt:   return WLR_MODIFIER_ALT;
    case Kmod4:  return WLR_MODIFIER_LOGO;
    case Kcaps:  return WLR_MODIFIER_CAPS;
    case Knum:   return WLR_MODIFIER_MOD2;  /* Num lock */
    case Kaltgr: return WLR_MODIFIER_ALT;   /* AltGr treated as Alt */
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
    
    /* Debug: verify arrow key mappings */
    wlr_log(WLR_INFO, "Arrow key rune values: Up=0x%04x Down=0x%04x Left=0x%04x Right=0x%04x",
            Kup, Kdown, Kleft, Kright);
    const struct key_map *up = keymap_lookup(Kup);
    const struct key_map *down = keymap_lookup(Kdown);
    const struct key_map *left = keymap_lookup(Kleft);
    const struct key_map *right = keymap_lookup(Kright);
    wlr_log(WLR_INFO, "Keymap lookup: Up=%s Down=%s Left=%s Right=%s",
            up ? "OK" : "MISSING", down ? "OK" : "MISSING",
            left ? "OK" : "MISSING", right ? "OK" : "MISSING");
    if (down) {
        wlr_log(WLR_INFO, "Down arrow: rune=0x%04x keycode=%d", down->rune, down->keycode);
    }
}

void input_queue_push(struct input_queue *q, struct input_event *ev) {
    pthread_mutex_lock(&q->lock);
    int next = (q->tail + 1) % INPUT_QUEUE_SIZE;
    if (next != q->head) {
        q->events[q->tail] = *ev;
        q->tail = next;
        /* Wake up main loop */
        char c = 1;
        if (write(q->pipe_fd[1], &c, 1) < 0) {
            /* Ignore - pipe might be full which is fine */
        }
    }
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

/* Mouse thread - reads /dev/mouse and queues events */
void *mouse_thread_func(void *arg) {
    struct server *s = arg;
    struct p9conn *p9 = &s->p9_mouse;
    uint32_t mouse_fid;
    const char *wnames[] = { "mouse" };
    uint8_t buf[64];
    int mice_read = 0;
    
    /* Walk to /dev/mouse */
    mouse_fid = p9->next_fid++;
    if (p9_walk(p9, p9->root_fid, mouse_fid, 1, wnames) < 0) {
        wlr_log(WLR_ERROR, "Mouse thread: failed to walk to /dev/mouse");
        return NULL;
    }
    if (p9_open(p9, mouse_fid, OREAD, NULL) < 0) {
        wlr_log(WLR_ERROR, "Mouse thread: failed to open /dev/mouse");
        return NULL;
    }
    
    wlr_log(WLR_INFO, "Mouse thread started - reading /dev/mouse");
    
    while (s->running) {
        int n = p9_read(p9, mouse_fid, 0, sizeof(buf) - 1, buf);
        if (n <= 0) {
            if (s->running) {
                wlr_log(WLR_ERROR, "Mouse thread: read failed");
            }
            break;
        }
        
        buf[n] = '\0';
        mice_read++;
        
        if (buf[0] == 'm') {
            int x, y, buttons;
            if (sscanf((char*)buf + 1, "%d %d %d", &x, &y, &buttons) == 3) {
                struct input_event ev;
                ev.type = INPUT_MOUSE;
                ev.mouse.x = x;
                ev.mouse.y = y;
                ev.mouse.buttons = buttons;
                input_queue_push(&s->input_queue, &ev);
                
                if (mice_read <= 20 || mice_read % 500 == 0) {
                    wlr_log(WLR_DEBUG, "Mouse #%d: x=%d y=%d buttons=%d", 
                            mice_read, x, y, buttons);
                }
            }
        } else if (buf[0] == 'r') {
            /* Resize event - handled by wctl thread */
            wlr_log(WLR_INFO, "Mouse: resize notification");
        }
    }
    
    wlr_log(WLR_INFO, "Mouse thread exiting (read %d events)", mice_read);
    return NULL;
}

/* Keyboard thread - reads /dev/kbd (kbdfs) for keyboard events.
 * Uses 'k'/'K' messages for special keys, 'c' for regular characters.
 * Falls back to /dev/cons with rawon if /dev/kbd is not available.
 */
void *kbd_thread_func(void *arg) {
    struct server *s = arg;
    struct p9conn *p9 = &s->p9_kbd;
    uint32_t kbd_fid;
    const char *wnames[2];
    uint8_t buf[256];
    int keys_read = 0;
    int use_cons_fallback = 0;
    
    /* Track previously held keys for detecting press/release */
    int prev_keys[16];
    int prev_nkeys = 0;
    uint32_t prev_mods = 0;
    
    /* First try /dev/kbd (kbdfs) which provides proper key messages */
    kbd_fid = p9->next_fid++;
    wnames[0] = "kbd";
    if (p9_walk(p9, p9->root_fid, kbd_fid, 1, wnames) < 0) {
        wlr_log(WLR_INFO, "Kbd thread: /dev/kbd not found, trying /dev/cons fallback");
        use_cons_fallback = 1;
    } else if (p9_open(p9, kbd_fid, OREAD, NULL) < 0) {
        wlr_log(WLR_INFO, "Kbd thread: failed to open /dev/kbd, trying /dev/cons fallback");
        use_cons_fallback = 1;
    }
    
    if (use_cons_fallback) {
        uint32_t consctl_fid;
        
        /* Walk to /dev/consctl */
        consctl_fid = p9->next_fid++;
        wnames[0] = "consctl";
        if (p9_walk(p9, p9->root_fid, consctl_fid, 1, wnames) < 0) {
            wlr_log(WLR_ERROR, "Kbd thread: failed to walk to /dev/consctl");
            return NULL;
        }
        if (p9_open(p9, consctl_fid, OWRITE, NULL) < 0) {
            wlr_log(WLR_ERROR, "Kbd thread: failed to open /dev/consctl");
            return NULL;
        }
        
        /* Enable raw mode */
        const char *rawon = "rawon";
        if (p9_write(p9, consctl_fid, 0, (uint8_t*)rawon, strlen(rawon)) < 0) {
            wlr_log(WLR_ERROR, "Kbd thread: failed to write rawon");
            return NULL;
        }
        wlr_log(WLR_INFO, "Kbd: rawon mode enabled");
        
        /* Walk to /dev/cons */
        kbd_fid = p9->next_fid++;
        wnames[0] = "cons";
        if (p9_walk(p9, p9->root_fid, kbd_fid, 1, wnames) < 0) {
            wlr_log(WLR_ERROR, "Kbd thread: failed to walk to /dev/cons");
            return NULL;
        }
        if (p9_open(p9, kbd_fid, OREAD, NULL) < 0) {
            wlr_log(WLR_ERROR, "Kbd thread: failed to open /dev/cons");
            return NULL;
        }
        
        wlr_log(WLR_INFO, "Keyboard thread started - reading /dev/cons (raw mode)");
    } else {
        wlr_log(WLR_INFO, "Keyboard thread started - reading /dev/kbd");
    }
    
    while (s->running) {
        int n = p9_read(p9, kbd_fid, 0, sizeof(buf) - 1, buf);
        if (n <= 0) {
            if (s->running) {
                wlr_log(WLR_ERROR, "Kbd thread: read failed");
            }
            break;
        }
        
        /* Log raw data for debugging */
        if (keys_read < 50) {
            wlr_log(WLR_INFO, "Kbd: received %d bytes, first byte=0x%02x '%c'", 
                    n, buf[0], (buf[0] >= 32 && buf[0] < 127) ? buf[0] : '?');
        }
        
        /* Process keyboard input */
        unsigned char *p = buf;
        unsigned char *end = buf + n;
        
        if (use_cons_fallback) {
            /* /dev/cons raw mode: just UTF-8 characters, no message framing */
            while (p < end) {
                int rune = 0;
                int consumed = utf8_decode(p, end, &rune);
                if (consumed > 0 && rune != 0) {
                    keys_read++;
                    if (keys_read <= 30 || keys_read % 100 == 0) {
                        wlr_log(WLR_INFO, "Kbd cons: char 0x%04x '%c'", 
                                rune, (rune >= 32 && rune < 127) ? rune : '?');
                    }
                    struct input_event ev;
                    ev.type = INPUT_KEY;
                    ev.key.rune = rune;
                    ev.key.pressed = 1;
                    input_queue_push(&s->input_queue, &ev);
                    ev.key.pressed = 0;
                    input_queue_push(&s->input_queue, &ev);
                    p += consumed;
                } else {
                    p++;  /* Skip invalid byte */
                }
            }
            continue;  /* Next read */
        }
        
        /* /dev/kbd (kbdfs) mode: null-terminated messages with type prefix */
        while (p < end) {
            /* Find end of message (null-terminated) */
            unsigned char *msg_end = memchr(p, 0, end - p);
            if (!msg_end) {
                /* No null terminator - incomplete message, skip */
                wlr_log(WLR_DEBUG, "Kbd: incomplete message, skipping %ld bytes", (long)(end - p));
                break;
            }
            
            unsigned char msg_type = *p++;
            size_t msg_len = msg_end - p;
            
            /* Log raw message for debugging */
            if (msg_type == 'k' || msg_type == 'K') {
                wlr_log(WLR_INFO, "Kbd msg: type='%c' len=%zu bytes", msg_type, msg_len);
            }
            
            if (msg_type == 'c') {
                /* Character event - regular characters only, special keys via k/K */
                if (p < msg_end) {
                    int rune = 0;
                    int consumed = utf8_decode(p, msg_end, &rune);
                    if (consumed > 0 && rune != 0 && rune < KF) {
                        /* Regular character (below KF range) - send press+release */
                        keys_read++;
                        wlr_log(WLR_DEBUG, "Kbd 'c': char 0x%04x '%c'", 
                                rune, (rune >= 32 && rune < 127) ? rune : '?');
                        struct input_event ev;
                        ev.type = INPUT_KEY;
                        ev.key.rune = rune;
                        ev.key.pressed = 1;
                        input_queue_push(&s->input_queue, &ev);
                        ev.key.pressed = 0;
                        input_queue_push(&s->input_queue, &ev);
                    }
                }
            } else if (msg_type == 'k' || msg_type == 'K') {
                /* Key state message - use for special keys (>= KF) only */
                int curr_keys[16];
                int curr_nkeys = 0;
                int is_press = (msg_type == 'k');
                
                /* Parse UTF-8 runes until message end */
                while (p < msg_end && curr_nkeys < 16) {
                    int rune = 0;
                    int consumed = utf8_decode(p, msg_end, &rune);
                    if (consumed > 0 && rune != 0) {
                        curr_keys[curr_nkeys++] = rune;
                        p += consumed;
                    } else {
                        break;
                    }
                }
                
                /* Calculate current modifier state */
                uint32_t curr_mods = 0;
                for (int i = 0; i < curr_nkeys; i++) {
                    curr_mods |= keymapmod(curr_keys[i]);
                }
                
                if (is_press) {
                    /* 'k' message: find newly pressed SPECIAL keys (>= KF) */
                    for (int i = 0; i < curr_nkeys; i++) {
                        int rune = curr_keys[i];
                        if (rune < KF) continue;  /* Regular chars handled by 'c' */
                        int found = 0;
                        for (int j = 0; j < prev_nkeys; j++) {
                            if (prev_keys[j] == rune) { found = 1; break; }
                        }
                        if (!found && !keymapmod(rune)) {
                            keys_read++;
                            wlr_log(WLR_INFO, "Kbd PRESS: rune=0x%04x prev_nkeys=%d", rune, prev_nkeys);
                            struct input_event ev;
                            ev.type = INPUT_KEY;
                            ev.key.rune = rune;
                            ev.key.pressed = 1;
                            input_queue_push(&s->input_queue, &ev);
                        }
                    }
                } else {
                    /* 'K' message: find released SPECIAL keys (>= KF) */
                    for (int i = 0; i < prev_nkeys; i++) {
                        int rune = prev_keys[i];
                        if (rune < KF) continue;  /* Regular chars handled by 'c' */
                        int found = 0;
                        for (int j = 0; j < curr_nkeys; j++) {
                            if (curr_keys[j] == rune) { found = 1; break; }
                        }
                        if (!found && !keymapmod(rune)) {
                            wlr_log(WLR_INFO, "Kbd RELEASE: rune=0x%04x", rune);
                            struct input_event ev;
                            ev.type = INPUT_KEY;
                            ev.key.rune = rune;
                            ev.key.pressed = 0;
                            input_queue_push(&s->input_queue, &ev);
                        }
                    }
                }
                
                /* Update modifier state */
                if (curr_mods != prev_mods) {
                    prev_mods = curr_mods;
                }
                
                /* Update previous key state */
                memcpy(prev_keys, curr_keys, curr_nkeys * sizeof(int));
                prev_nkeys = curr_nkeys;
            }
            /* Skip to next message (past the null terminator) */
            p = msg_end + 1;
        }
    }
    
    wlr_log(WLR_INFO, "Keyboard thread exiting (read %d keys)", keys_read);
    return NULL;
}

/* Window control thread - watches /dev/wctl for geometry changes.
 * Opens/reads/closes each iteration to allow other programs (like riow) to access wctl.
 */
void *wctl_thread_func(void *arg) {
    struct server *s = arg;
    struct p9conn *p9 = &s->p9_wctl;
    char buf[128];
    int last_x0 = -1, last_y0 = -1, last_x1 = -1, last_y1 = -1;
    
    wlr_log(WLR_INFO, "Wctl thread started - polling /dev/wctl for window changes");
    
    while (s->running) {
        /* Open wctl fresh each time to allow other programs to access it */
        uint32_t wctl_fid = p9->next_fid++;
        const char *wnames[] = { "wctl" };
        
        if (p9_walk(p9, p9->root_fid, wctl_fid, 1, wnames) < 0) {
            wlr_log(WLR_DEBUG, "Wctl thread: walk failed, retrying...");
            usleep(100000);
            continue;
        }
        
        if (p9_open(p9, wctl_fid, OREAD, NULL) < 0) {
            p9_clunk(p9, wctl_fid);
            wlr_log(WLR_DEBUG, "Wctl thread: open failed, retrying...");
            usleep(100000);
            continue;
        }
        
        /* Read wctl: "x0 y0 x1 y1 current|notcurrent hidden|visible" */
        int n = p9_read(p9, wctl_fid, 0, sizeof(buf) - 1, (uint8_t*)buf);
        
        /* Close immediately to release the file for other programs */
        p9_clunk(p9, wctl_fid);
        
        if (n <= 0) {
            if (s->running) {
                wlr_log(WLR_DEBUG, "Wctl thread: read failed, retrying...");
            }
            usleep(100000);
            continue;
        }
        
        buf[n] = '\0';
        
        /* Parse the wctl data: "x0 y0 x1 y1 ..." */
        int x0, y0, x1, y1;
        if (sscanf(buf, "%d %d %d %d", &x0, &y0, &x1, &y1) == 4) {
            /* Check if geometry changed (skip first read - just initialize) */
            if (last_x0 < 0) {
                /* First read - just store initial values */
                last_x0 = x0; last_y0 = y0;
                last_x1 = x1; last_y1 = y1;
                wlr_log(WLR_INFO, "Wctl initial geometry: (%d,%d)-(%d,%d)", x0, y0, x1, y1);
            } else if (x0 != last_x0 || y0 != last_y0 || x1 != last_x1 || y1 != last_y1) {
                wlr_log(WLR_INFO, "Wctl geometry changed: (%d,%d)-(%d,%d) -> (%d,%d)-(%d,%d)",
                        last_x0, last_y0, last_x1, last_y1, x0, y0, x1, y1);
                
                last_x0 = x0; last_y0 = y0;
                last_x1 = x1; last_y1 = y1;
                
                /* Signal that window changed - send thread will handle relookup */
                s->window_changed = 1;
                
                /* Wake up send thread if it's waiting */
                pthread_mutex_lock(&s->send_lock);
                pthread_cond_signal(&s->send_cond);
                pthread_mutex_unlock(&s->send_lock);
            }
        }
        
        /* Poll interval - 50ms gives responsive resize detection */
        usleep(50000);
    }
    
    wlr_log(WLR_INFO, "Wctl thread exiting");
    return NULL;
}
