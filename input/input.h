/*
 * input.h - Input handling declarations
 */

#ifndef P9WL_INPUT_H
#define P9WL_INPUT_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <wlr/types/wlr_keyboard.h>

#include "../types.h"  /* For struct server, input_queue, input_event */

/* Forward declaration */
struct kbmap;

/*
 * Key definitions from Plan 9 /sys/include/keyboard.h
 */
#define KF              0xF000  /* Beginning of private Unicode space */
#define Spec            0xF800  /* Special keys base */

/* Function keys F1-F12 */
#define KF1             (KF|0x01)
#define KF2             (KF|0x02)
#define KF3             (KF|0x03)
#define KF4             (KF|0x04)
#define KF5             (KF|0x05)
#define KF6             (KF|0x06)
#define KF7             (KF|0x07)
#define KF8             (KF|0x08)
#define KF9             (KF|0x09)
#define KF10            (KF|0x0A)
#define KF11            (KF|0x0B)
#define KF12            (KF|0x0C)

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
#define Kscrolloneup    (KF|0x20)
#define Kscrollonedown  (KF|0x21)

/* Special keys - Spec base (0xF800) */
#define Kbreak          (Spec|0x61)  /* 0xF861 - Break/Pause */
#define Kcaps           (Spec|0x64)  /* 0xF864 - Caps Lock */
#define Knum            (Spec|0x65)  /* 0xF865 - Num Lock */
#define Kaltgr          (Spec|0x67)  /* 0xF867 - AltGr */
#define Kmod4           (Spec|0x68)  /* 0xF868 - Super/Windows/Mod4 */

/* ASCII */
#define Kdel            0x7F
#define Kesc            0x1B

/* Key mapping entry: maps Plan 9 rune to Linux keycode */
struct key_map {
    int rune;           /* Plan 9 rune value */
    int keycode;        /* Linux keycode (KEY_*) */
    int shift;          /* Requires shift modifier */
    int ctrl;           /* Requires ctrl modifier */
};

/* Static keymap table (fallback) */
extern const struct key_map keymap[];

/* ============== Function Declarations ============== */

/*
 * Look up a key mapping by rune (static table only).
 * For dynamic lookup, use keymap_lookup_dynamic() instead.
 */
const struct key_map *keymap_lookup(uint32_t rune);

/*
 * Look up a key mapping by rune, using dynamic kbmap if available.
 * Falls back to static keymap table.
 *
 * @param km   Pointer to dynamic kbmap (can be NULL)
 * @param rune Plan 9 rune value to look up
 * @return     Pointer to key_map entry, or NULL if not found
 */
const struct key_map *keymap_lookup_dynamic(struct kbmap *km, uint32_t rune);

/* Decode UTF-8 rune from buffer, return bytes consumed (1-4) or 0 on error */
int utf8_decode(const unsigned char *p, const unsigned char *end, int *rune);

/* Get modifier mask for a rune - used for tracking held modifiers */
uint32_t keymapmod(int rune);

/* Input queue functions */
void input_queue_init(struct input_queue *q);
void input_queue_push(struct input_queue *q, struct input_event *ev);
int input_queue_pop(struct input_queue *q, struct input_event *ev);

/* Input threads */
void *mouse_thread_func(void *arg);
void *kbd_thread_func(void *arg);
void *wctl_thread_func(void *arg);

/* Time helpers */
uint32_t now_ms(void);
uint64_t now_us(void);

#endif /* P9WL_INPUT_H */
