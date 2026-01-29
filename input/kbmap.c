/*
 * kbmap.c - Dynamic keyboard map loading from Plan 9 /dev/kbmap
 *
 * /dev/kbmap format (tab-separated):
 *   layer  scancode  rune
 *
 * Where:
 *   layer: 0=none, 1=shift, 2=esc, 3=shiftesc, 4=ctl
 *   scancode: PC/AT scancode (0-127)
 *   rune: 'x (literal), ^X (control), 0xNNNN (hex), or decimal
 *
 * We build the INVERSE mapping: rune → (keycode, shift)
 * The ctrl flag is NEVER set because Ctrl state is tracked
 * separately via Kctl (0xF017) key events from 9front.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <linux/input-event-codes.h>
#include <wlr/util/log.h>

#include "kbmap.h"

/* PC/AT scancode to Linux keycode mapping */
static int scancode_to_keycode[128] = {
    [0] = KEY_RESERVED,
    [1] = KEY_ESC,
    [2] = KEY_1,
    [3] = KEY_2,
    [4] = KEY_3,
    [5] = KEY_4,
    [6] = KEY_5,
    [7] = KEY_6,
    [8] = KEY_7,
    [9] = KEY_8,
    [10] = KEY_9,
    [11] = KEY_0,
    [12] = KEY_MINUS,
    [13] = KEY_EQUAL,
    [14] = KEY_BACKSPACE,
    [15] = KEY_TAB,
    [16] = KEY_Q,
    [17] = KEY_W,
    [18] = KEY_E,
    [19] = KEY_R,
    [20] = KEY_T,
    [21] = KEY_Y,
    [22] = KEY_U,
    [23] = KEY_I,
    [24] = KEY_O,
    [25] = KEY_P,
    [26] = KEY_LEFTBRACE,
    [27] = KEY_RIGHTBRACE,
    [28] = KEY_ENTER,
    [29] = KEY_LEFTCTRL,
    [30] = KEY_A,
    [31] = KEY_S,
    [32] = KEY_D,
    [33] = KEY_F,
    [34] = KEY_G,
    [35] = KEY_H,
    [36] = KEY_J,
    [37] = KEY_K,
    [38] = KEY_L,
    [39] = KEY_SEMICOLON,
    [40] = KEY_APOSTROPHE,
    [41] = KEY_GRAVE,
    [42] = KEY_LEFTSHIFT,
    [43] = KEY_BACKSLASH,
    [44] = KEY_Z,
    [45] = KEY_X,
    [46] = KEY_C,
    [47] = KEY_V,
    [48] = KEY_B,
    [49] = KEY_N,
    [50] = KEY_M,
    [51] = KEY_COMMA,
    [52] = KEY_DOT,
    [53] = KEY_SLASH,
    [54] = KEY_RIGHTSHIFT,
    [55] = KEY_KPASTERISK,
    [56] = KEY_LEFTALT,
    [57] = KEY_SPACE,
    [58] = KEY_CAPSLOCK,
    [59] = KEY_F1,
    [60] = KEY_F2,
    [61] = KEY_F3,
    [62] = KEY_F4,
    [63] = KEY_F5,
    [64] = KEY_F6,
    [65] = KEY_F7,
    [66] = KEY_F8,
    [67] = KEY_F9,
    [68] = KEY_F10,
    [69] = KEY_NUMLOCK,
    [70] = KEY_SCROLLLOCK,
    [71] = KEY_KP7,
    [72] = KEY_KP8,
    [73] = KEY_KP9,
    [74] = KEY_KPMINUS,
    [75] = KEY_KP4,
    [76] = KEY_KP5,
    [77] = KEY_KP6,
    [78] = KEY_KPPLUS,
    [79] = KEY_KP1,
    [80] = KEY_KP2,
    [81] = KEY_KP3,
    [82] = KEY_KP0,
    [83] = KEY_KPDOT,
    [87] = KEY_F11,
    [88] = KEY_F12,
};

/* Add entry if not already present (prefer unshifted) */
static void kbmap_add(struct kbmap *km, int rune, int keycode, int shift) {
    if (rune == 0 || keycode == 0) return;
    if (km->count >= KBMAP_MAX_ENTRIES) return;
    
    /* Check if already exists */
    for (int i = 0; i < km->count; i++) {
        if (km->entries[i].rune == rune) {
            /* Prefer non-shifted version */
            if (!shift && km->entries[i].shift) {
                km->entries[i].keycode = keycode;
                km->entries[i].shift = 0;
            }
            return;
        }
    }
    
    /* Add new entry - ctrl is ALWAYS 0 */
    km->entries[km->count].rune = rune;
    km->entries[km->count].keycode = keycode;
    km->entries[km->count].shift = shift;
    km->entries[km->count].ctrl = 0;  /* NEVER set ctrl - tracked via Kctl */
    km->count++;
}

/*
 * Parse Plan 9 rune representation:
 *   'x  -> literal character x
 *   ^X  -> control character (X - 0x40), e.g. ^A = 0x01
 *   0xNNNN -> hex value
 *   NNN -> decimal value
 */
static int parse_rune(const char *s) {
    if (!s || !*s) return 0;
    
    /* Skip leading whitespace */
    while (*s && isspace((unsigned char)*s)) s++;
    
    if (s[0] == '\'' && s[1]) {
        /* Character literal: 'x */
        return (unsigned char)s[1];
    }
    
    if (s[0] == '^' && s[1]) {
        /* Control character: ^X = X - 0x40 */
        int c = (unsigned char)s[1];
        if (c >= 0x40 && c < 0x80)
            return c - 0x40;
        return 0;
    }
    
    /* Numeric: hex (0x...) or decimal */
    char *end;
    long val = strtol(s, &end, 0);
    if (end > s && val >= 0 && val <= 0x10FFFF)
        return (int)val;
    
    return 0;
}

/* Parse one line from /dev/kbmap */
static void parse_kbmap_line(struct kbmap *km, const char *line) {
    int layer, scancode;
    char rune_str[64];
    
    /* Format: layer scancode rune */
    if (sscanf(line, "%d %d %63s", &layer, &scancode, rune_str) != 3)
        return;
    
    /* Validate */
    if (scancode < 0 || scancode >= 128) return;
    
    int rune = parse_rune(rune_str);
    if (rune == 0) return;
    
    /* Skip modifier keys (they're handled separately) */
    if (rune >= 0xF000) return;
    
    int keycode = scancode_to_keycode[scancode];
    if (keycode == 0) return;
    
    /*
     * Determine shift state from layer:
     *   Layer 0 (none): shift=0
     *   Layer 1 (shift): shift=1
     *   Layer 4 (ctl): shift=0 (Ctrl tracked via Kctl events)
     *   Other layers: skip (altgr, etc. not supported yet)
     */
    int shift = 0;
    switch (layer) {
    case 0:  /* none */
        shift = 0;
        break;
    case 1:  /* shift */
        shift = 1;
        break;
    case 4:  /* ctl - DO NOT set ctrl=1, just map the rune */
        shift = 0;
        break;
    default:
        /* Skip other layers for now */
        return;
    }
    
    kbmap_add(km, rune, keycode, shift);
}

int kbmap_load(struct kbmap *km, struct p9conn *p9) {
    uint32_t kbmap_fid;
    const char *wnames[] = { "kbmap" };
    uint8_t buf[8192];
    
    memset(km, 0, sizeof(*km));
    
    /* Walk to /dev/kbmap */
    kbmap_fid = p9->next_fid++;
    if (p9_walk(p9, p9->root_fid, kbmap_fid, 1, wnames) < 0) {
        wlr_log(WLR_INFO, "kbmap: /dev/kbmap not found");
        return -1;
    }
    
    if (p9_open(p9, kbmap_fid, OREAD, NULL) < 0) {
        wlr_log(WLR_ERROR, "kbmap: failed to open /dev/kbmap");
        p9_clunk(p9, kbmap_fid);
        return -1;
    }
    
    /* Read entire file */
    uint64_t offset = 0;
    char *data = NULL;
    size_t data_size = 0;
    
    while (1) {
        int n = p9_read(p9, kbmap_fid, offset, sizeof(buf), buf);
        if (n < 0) {
            free(data);
            p9_clunk(p9, kbmap_fid);
            return -1;
        }
        if (n == 0) break;
        
        char *tmp = realloc(data, data_size + n + 1);
        if (!tmp) {
            free(data);
            p9_clunk(p9, kbmap_fid);
            return -1;
        }
        data = tmp;
        memcpy(data + data_size, buf, n);
        data_size += n;
        offset += n;
    }
    
    p9_clunk(p9, kbmap_fid);
    
    if (!data) return -1;
    data[data_size] = '\0';
    
    /* Parse line by line */
    char *line = data;
    while (line && *line) {
        char *eol = strchr(line, '\n');
        if (eol) *eol = '\0';
        
        if (*line && *line != '#')
            parse_kbmap_line(km, line);
        
        line = eol ? eol + 1 : NULL;
    }
    
    free(data);
    
    km->loaded = 1;
    wlr_log(WLR_INFO, "kbmap: loaded %d mappings from /dev/kbmap", km->count);
    
    return 0;
}

const struct kbmap_entry *kbmap_lookup(struct kbmap *km, int rune) {
    if (!km || !km->loaded) return NULL;
    
    for (int i = 0; i < km->count; i++) {
        if (km->entries[i].rune == rune)
            return &km->entries[i];
    }
    return NULL;
}

void kbmap_cleanup(struct kbmap *km) {
    if (km) memset(km, 0, sizeof(*km));
}
