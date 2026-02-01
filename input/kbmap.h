/*
 * kbmap.h - Dynamic keyboard map loading from Plan 9 /dev/kbmap
 *
 * Reads the keyboard mapping from the Plan 9 system at runtime,
 * providing a robust solution that works with any keyboard layout.
 *
 * /dev/kbmap Format:
 *   layer  scancode  rune
 *
 * Where:
 *   layer:    none, shift, ctl, altgr, mod4, etc.
 *   scancode: PC/AT scancode (0-127)
 *   rune:     'x (literal), ^X (control), 0xNNNN (hex), or decimal
 *
 * This module builds an INVERSE mapping: given a rune, find the
 * keycode and modifiers needed to produce it. This is needed because
 * Plan 9 sends characters (runes) but Wayland expects physical keys.
 *
 * Usage:
 *   struct kbmap km;
 *   if (kbmap_load(&km, p9conn) == 0) {
 *       // Use km for lookups
 *   }
 *   // Falls back to static table if load fails
 *   kbmap_cleanup(&km);
 */

#ifndef P9WL_KBMAP_H
#define P9WL_KBMAP_H

#include <stdint.h>
#include "../p9/p9.h"

/* Maximum entries in dynamic keymap */
#define KBMAP_MAX_ENTRIES 512

/*
 * Keyboard map entry: maps Plan 9 rune to Linux keycode.
 *
 * This is similar to struct key_map in input.h but used for
 * dynamically loaded mappings from /dev/kbmap.
 */
struct kbmap_entry {
    uint32_t rune;      /* Plan 9 rune value (Unicode codepoint) */
    int keycode;        /* Linux keycode (KEY_* from input-event-codes.h) */
    int shift;          /* 1 if shift modifier required */
    int ctrl;           /* 1 if ctrl modifier required */
};

/*
 * Dynamic keyboard map state.
 *
 * Contains the mapping table loaded from /dev/kbmap.
 * The 'loaded' flag indicates whether the table is valid.
 */
struct kbmap {
    struct kbmap_entry entries[KBMAP_MAX_ENTRIES];
    int count;          /* Number of valid entries */
    int loaded;         /* 1 if successfully loaded from /dev/kbmap, 0 otherwise */
};

/*
 * Load keyboard map from /dev/kbmap via 9P connection.
 *
 * Reads the entire /dev/kbmap file and parses it to build the
 * inverse mapping table. Only processes 'none', 'shift', and 'ctl'
 * layers; other layers (altgr, mod4, etc.) are skipped.
 *
 * For duplicate runes, prefers the unshifted version.
 *
 * @param km  Keymap structure to populate (will be zeroed first)
 * @param p9  9P connection to use for reading /dev/kbmap
 * @return    0 on success, -1 on failure (file not found, read error, etc.)
 *
 * On failure, km->loaded will be 0 and the static fallback keymap
 * in input.c should be used instead.
 */
int kbmap_load(struct kbmap *km, struct p9conn *p9);

/*
 * Look up a rune in the dynamic keymap.
 *
 * @param km    Keymap to search (must not be NULL)
 * @param rune  Plan 9 rune value to look up
 * @return      Pointer to entry if found, NULL if not found or km not loaded
 *
 * Note: Modifier keys (runes >= 0xF000) are not stored in the dynamic
 * keymap and will always return NULL. Use the static keymap for those.
 */
const struct kbmap_entry *kbmap_lookup(struct kbmap *km, uint32_t rune);

/*
 * Free resources associated with keymap.
 *
 * Currently just zeros the structure. Safe to call multiple times
 * or on an uninitialized structure.
 *
 * @param km  Keymap to clean up (may be NULL)
 */
void kbmap_cleanup(struct kbmap *km);

#endif /* P9WL_KBMAP_H */
