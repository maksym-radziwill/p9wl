/*
 * kbmap.h - Dynamic keyboard map loading from Plan 9 /dev/kbmap
 *
 * Reads the keyboard mapping from the Plan 9 system at runtime,
 * providing a robust solution that works with any keyboard layout.
 */

#ifndef P9WL_KBMAP_H
#define P9WL_KBMAP_H

#include <stdint.h>
#include "../p9/p9.h"

/* Maximum entries in dynamic keymap */
#define KBMAP_MAX_ENTRIES 512

/* Keyboard map entry: maps Plan 9 rune to Linux keycode */
struct kbmap_entry {
    int rune;           /* Plan 9 rune value */
    int keycode;        /* Linux keycode (KEY_*) */
    int shift;          /* Requires shift modifier */
    int ctrl;           /* Requires ctrl modifier */
};

/* Dynamic keyboard map state */
struct kbmap {
    struct kbmap_entry entries[KBMAP_MAX_ENTRIES];
    int count;
    int loaded;         /* Successfully loaded from /dev/kbmap? */
};

/*
 * Load keyboard map from /dev/kbmap via 9P connection.
 * Returns 0 on success, -1 on failure.
 * On failure, the static fallback keymap should be used.
 */
int kbmap_load(struct kbmap *km, struct p9conn *p9);

/*
 * Look up a rune in the dynamic keymap.
 * Returns pointer to entry, or NULL if not found.
 */
const struct kbmap_entry *kbmap_lookup(struct kbmap *km, int rune);

/*
 * Free resources associated with keymap.
 */
void kbmap_cleanup(struct kbmap *km);

#endif /* P9WL_KBMAP_H */
