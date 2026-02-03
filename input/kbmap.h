/*
 * kbmap.h - Dynamic keyboard map loading from Plan 9 /dev/kbmap
 *
 * Reads the keyboard mapping from /dev/kbmap at runtime via 9P,
 * providing layout-correct key translation for non-US keyboards.
 *
 * /dev/kbmap Format (see kbdfs(8)):
 *
 *   layer  scancode  rune
 *
 *   Where:
 *     layer:    none, shift, esc, altgr, ctl, ctlesc, shiftesc,
 *               shiftaltgr, mod4, altgrmod4
 *     scancode: PC/AT scancode (0-127)
 *     rune:     'x (UTF-8 literal), ^X (control), 0xNNNN (hex),
 *               0NNN (octal), or NNN (decimal)
 *
 * Inverse Mapping:
 *
 *   This module builds an INVERSE mapping: given an unshifted rune
 *   from a 'k'/'K' message, find the physical keycode that produces it.
 *
 *   Only layer 0 (none) is loaded. The 'k'/'K' messages from /dev/kbd
 *   always provide keys in unshifted form, with modifiers delivered as
 *   separate runes (e.g. Shift+A arrives as Kshift + 'a'). We forward
 *   raw keycodes and modifier keys independently to Wayland, and the
 *   client's XKB keymap handles all composition — shift, altgr, ctrl
 *   combinations, dead keys, etc. The other kbmap layers describe
 *   these compositions and are therefore not needed.
 *
 *   Plan 9 special keys (runes >= 0xF000: arrows, function keys,
 *   modifiers, etc.) are not stored here. They are handled by the
 *   static keymap table in input.c.
 *
 * Usage:
 *
 *   struct kbmap km;
 *   if (kbmap_load(&km, &p9conn) == 0) {
 *       const struct kbmap_entry *e = kbmap_lookup(&km, rune);
 *       if (e) use e->keycode;
 *   }
 *   // Falls back to static keymap in input.c if load fails
 *   kbmap_cleanup(&km);
 */

#ifndef P9WL_KBMAP_H
#define P9WL_KBMAP_H

#include <stdint.h>
#include "../p9/p9.h"

/* ============== Constants ============== */

#define KBMAP_MAX_ENTRIES 512

/* ============== Data Structures ============== */

/*
 * Keyboard map entry: maps a Plan 9 unshifted rune to a Linux keycode.
 *
 * The shift and ctrl fields are vestigial (always 0) since we only
 * load layer 0 and let XKB handle modifier composition. They are
 * retained for structural compatibility with struct key_map in input.h.
 */
struct kbmap_entry {
    uint32_t rune;      /* Plan 9 rune (Unicode codepoint) */
    int keycode;        /* Linux keycode (KEY_* from input-event-codes.h) */
    int shift;          /* Always 0 — XKB handles shift composition */
    int ctrl;           /* Always 0 — XKB handles ctrl composition */
};

/*
 * Dynamic keyboard map state.
 *
 * Populated by kbmap_load() from /dev/kbmap.
 */
struct kbmap {
    struct kbmap_entry entries[KBMAP_MAX_ENTRIES];
    int count;          /* Number of valid entries */
};

/* ============== Functions ============== */

/*
 * Load keyboard map from /dev/kbmap via 9P.
 *
 * Reads the file and builds an inverse mapping (rune -> keycode)
 * from layer 0 (none) only. First mapping per rune wins.
 *
 * km: keymap to populate (zeroed first)
 * p9: 9P connection for reading /dev/kbmap
 *
 * Returns 0 on success, -1 on failure. On failure km->loaded is 0
 * and the static keymap in input.c should be used as fallback.
 */
int kbmap_load(struct kbmap *km, struct p9conn *p9);

/*
 * Look up a rune in the dynamic keymap.
 *
 * km:   keymap to search
 * rune: Plan 9 rune to look up
 *
 * Returns pointer to entry, or NULL if not found or km not loaded.
 * Special keys (>= 0xF000) are never stored here — use the static
 * keymap for those.
 */
const struct kbmap_entry *kbmap_lookup(struct kbmap *km, uint32_t rune);

/*
 * Reset keymap state. Safe to call on NULL or uninitialized km.
 */
void kbmap_cleanup(struct kbmap *km);

#endif /* P9WL_KBMAP_H */
