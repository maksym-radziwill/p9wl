/*
 * kbmap.c - Dynamic keyboard map loading from Plan 9 /dev/kbmap
 *
 * Reads /dev/kbmap to get the scancode→rune mapping, then builds
 * the inverse rune→scancode mapping needed for translating Plan 9
 * keyboard events to Linux/Wayland keycodes.
 *
 * /dev/kbmap format (space-separated, variable width):
 *   modifier  scancode  rune
 *   none      30        97
 *   shift     30        65
 *   none      123       61454
 *
 * We read the "none" and "shift" entries to build our mapping.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <linux/input-event-codes.h>
#include <wlr/util/log.h>

#include "kbmap.h"

/*
 * PC/AT scancode to Linux keycode mapping.
 * This maps the scancodes in /dev/kbmap to Linux KEY_* codes.
 * Based on linux/input-event-codes.h and standard PC keyboard layout.
 */
static int scancode_to_keycode[256] = {
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
    [84] = KEY_RESERVED,
    [85] = KEY_ZENKAKUHANKAKU,
    [86] = KEY_102ND,
    [87] = KEY_F11,
    [88] = KEY_F12,
    [89] = KEY_RO,
    [90] = KEY_KATAKANA,
    [91] = KEY_HIRAGANA,
    [92] = KEY_HENKAN,
    [93] = KEY_KATAKANAHIRAGANA,
    [94] = KEY_MUHENKAN,
    [95] = KEY_KPJPCOMMA,
    [96] = KEY_KPENTER,
    [97] = KEY_RIGHTCTRL,
    [98] = KEY_KPSLASH,
    [99] = KEY_SYSRQ,
    [100] = KEY_RIGHTALT,
    [101] = KEY_LINEFEED,
    [102] = KEY_HOME,
    [103] = KEY_UP,
    [104] = KEY_PAGEUP,
    [105] = KEY_LEFT,
    [106] = KEY_RIGHT,
    [107] = KEY_END,
    [108] = KEY_DOWN,
    [109] = KEY_PAGEDOWN,
    [110] = KEY_INSERT,
    [111] = KEY_DELETE,
    [112] = KEY_MACRO,
    [113] = KEY_MUTE,
    [114] = KEY_VOLUMEDOWN,
    [115] = KEY_VOLUMEUP,
    [116] = KEY_POWER,
    [117] = KEY_KPEQUAL,
    [118] = KEY_KPPLUSMINUS,
    [119] = KEY_PAUSE,
    [120] = KEY_SCALE,
    [121] = KEY_KPCOMMA,
    [122] = KEY_HANGEUL,
    [123] = KEY_HANJA,
    [124] = KEY_YEN,
    [125] = KEY_LEFTMETA,
    [126] = KEY_RIGHTMETA,
    [127] = KEY_COMPOSE,
};

/* Check if a rune is already in the keymap */
static int kbmap_find(struct kbmap *km, int rune) {
    for (int i = 0; i < km->count; i++) {
        if (km->entries[i].rune == rune) {
            return i;
        }
    }
    return -1;
}

/* Add or update an entry in the keymap */
static void kbmap_add(struct kbmap *km, int rune, int keycode, int shift, int ctrl) {
    if (rune == 0 || keycode == 0) return;
    if (km->count >= KBMAP_MAX_ENTRIES) return;
    
    /* Check if already exists */
    int idx = kbmap_find(km, rune);
    if (idx >= 0) {
        /* Update existing - prefer non-shifted version */
        if (!shift && km->entries[idx].shift) {
            km->entries[idx].keycode = keycode;
            km->entries[idx].shift = 0;
        }
        return;
    }
    
    /* Add new entry */
    km->entries[km->count].rune = rune;
    km->entries[km->count].keycode = keycode;
    km->entries[km->count].shift = shift;
    km->entries[km->count].ctrl = ctrl;
    km->count++;
}

/* Parse a single line from /dev/kbmap */
static void parse_kbmap_line(struct kbmap *km, const char *line) {
    char modifier[32];
    int scancode, rune;
    
    /* Parse: "modifier scancode rune" */
    if (sscanf(line, "%31s %d %d", modifier, &scancode, &rune) != 3) {
        return;
    }
    
    /* Skip if scancode out of range */
    if (scancode < 0 || scancode > 127) return;
    
    /* Skip if rune is 0 (no mapping) */
    if (rune == 0) return;
    
    /* Get Linux keycode for this scancode */
    int keycode = scancode_to_keycode[scancode];
    if (keycode == 0) return;
    
    /* Determine modifiers */
    int shift = 0, ctrl = 0;
    
    if (strcmp(modifier, "none") == 0) {
        /* Base mapping, no modifiers */
    } else if (strcmp(modifier, "shift") == 0) {
        shift = 1;
    } else if (strcmp(modifier, "ctrl") == 0 || strcmp(modifier, "ctl") == 0) {
        ctrl = 1;
    } else {
        /* Skip other modifiers (altgr, etc.) for now */
        return;
    }
    
    kbmap_add(km, rune, keycode, shift, ctrl);
}

int kbmap_load(struct kbmap *km, struct p9conn *p9) {
    uint32_t kbmap_fid;
    const char *wnames[] = { "kbmap" };
    uint8_t buf[8192];
    
    memset(km, 0, sizeof(*km));
    
    /* Walk to /dev/kbmap */
    kbmap_fid = p9->next_fid++;
    if (p9_walk(p9, p9->root_fid, kbmap_fid, 1, wnames) < 0) {
        wlr_log(WLR_INFO, "kbmap: /dev/kbmap not found, using static keymap");
        return -1;
    }
    
    /* Open for reading */
    if (p9_open(p9, kbmap_fid, OREAD, NULL) < 0) {
        wlr_log(WLR_ERROR, "kbmap: failed to open /dev/kbmap");
        return -1;
    }
    
    wlr_log(WLR_INFO, "kbmap: reading /dev/kbmap...");
    
    /* Read the entire kbmap file */
    uint64_t offset = 0;
    size_t total = 0;
    char *kbmap_data = NULL;
    size_t kbmap_size = 0;
    
    while (1) {
        int n = p9_read(p9, kbmap_fid, offset, sizeof(buf), buf);
        if (n < 0) {
            wlr_log(WLR_ERROR, "kbmap: read error");
            if (kbmap_data) free(kbmap_data);
            p9_clunk(p9, kbmap_fid);
            return -1;
        }
        if (n == 0) break;  /* EOF */
        
        /* Append to buffer */
        kbmap_data = realloc(kbmap_data, kbmap_size + n + 1);
        if (!kbmap_data) {
            wlr_log(WLR_ERROR, "kbmap: out of memory");
            p9_clunk(p9, kbmap_fid);
            return -1;
        }
        memcpy(kbmap_data + kbmap_size, buf, n);
        kbmap_size += n;
        offset += n;
        total += n;
    }
    
    p9_clunk(p9, kbmap_fid);
    
    if (!kbmap_data || kbmap_size == 0) {
        wlr_log(WLR_ERROR, "kbmap: empty file");
        if (kbmap_data) free(kbmap_data);
        return -1;
    }
    
    kbmap_data[kbmap_size] = '\0';
    
    wlr_log(WLR_INFO, "kbmap: read %zu bytes", total);
    
    /* Parse line by line */
    char *line = kbmap_data;
    char *end = kbmap_data + kbmap_size;
    
    while (line < end) {
        /* Find end of line */
        char *eol = strchr(line, '\n');
        if (eol) {
            *eol = '\0';
        }
        
        /* Skip empty lines and whitespace-only lines */
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        
        if (*p) {
            parse_kbmap_line(km, p);
        }
        
        if (eol) {
            line = eol + 1;
        } else {
            break;
        }
    }
    
    free(kbmap_data);
    
    /* Add control characters that may not be in kbmap */
    /* These are generated by Ctrl+letter, producing ASCII 0x01-0x1A */
    for (int i = 1; i <= 26; i++) {
        int keycode = KEY_A + (i - 1);
        if (i == 8) keycode = KEY_BACKSPACE;  /* Ctrl+H = backspace */
        if (i == 9) keycode = KEY_TAB;        /* Tab */
        if (i == 10 || i == 13) keycode = KEY_ENTER;  /* Enter */
        kbmap_add(km, i, keycode, 0, (i != 8 && i != 9 && i != 10 && i != 13) ? 1 : 0);
    }
    
    /* Add ESC */
    kbmap_add(km, 0x1B, KEY_ESC, 0, 0);
    
    /* Add DEL */
    kbmap_add(km, 0x7F, KEY_DELETE, 0, 0);
    
    km->loaded = 1;
    wlr_log(WLR_INFO, "kbmap: loaded %d key mappings", km->count);
    
    /* Log some key mappings for debugging */
    for (int i = 0; i < km->count && i < 10; i++) {
        wlr_log(WLR_DEBUG, "kbmap: rune 0x%04x ('%c') -> keycode %d %s%s",
                km->entries[i].rune,
                (km->entries[i].rune >= 32 && km->entries[i].rune < 127) ? 
                    km->entries[i].rune : '?',
                km->entries[i].keycode,
                km->entries[i].shift ? "+shift" : "",
                km->entries[i].ctrl ? "+ctrl" : "");
    }
    
    return 0;
}

const struct kbmap_entry *kbmap_lookup(struct kbmap *km, int rune) {
    if (!km || !km->loaded) return NULL;
    
    for (int i = 0; i < km->count; i++) {
        if (km->entries[i].rune == rune) {
            return &km->entries[i];
        }
    }
    return NULL;
}

void kbmap_cleanup(struct kbmap *km) {
    if (km) {
        memset(km, 0, sizeof(*km));
    }
}
