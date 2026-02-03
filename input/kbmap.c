/*
 * kbmap.c - Dynamic keyboard map loading from Plan 9 /dev/kbmap
 *
 * /dev/kbmap format (whitespace-separated, fields padded to 11 chars):
 *   layer  scancode  rune
 *
 * Layer names: none, shift, esc, altgr, ctl, ctlesc, shiftesc,
 *              shiftaltgr, mod4, altgrmod4
 *
 * Rune representations:
 *   'x     -> literal character x (UTF-8 encoded)
 *   ^X     -> control character (X - 0x40), e.g. ^A = 0x01
 *   0xNNNN -> hex value
 *   0NNN   -> octal value
 *   NNN    -> decimal value
 *
 * We build the INVERSE mapping: rune -> keycode.
 *
 * Only layer 0 (none) is loaded. The 'k'/'K' messages from /dev/kbd
 * provide keys in unshifted form with modifiers as separate runes
 * (e.g. Shift+A arrives as Kshift + 'a', not 'A'). We forward these
 * as raw keycodes to Wayland, and the client's XKB keymap handles
 * all composition (shift, altgr, ctrl combinations, dead keys, etc.).
 *
 * This means the shift, ctl, altgr, and other modifier layers in
 * /dev/kbmap are irrelevant to us — they describe compositions that
 * Wayland clients perform on their own.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <linux/input-event-codes.h>
#include <wlr/util/log.h>

#include "kbmap.h"
#include "input.h"

/* PC/AT scancode to Linux keycode */
static int scancode_to_keycode[128] = {
	[1]  = KEY_ESC,
	[2]  = KEY_1,       [3]  = KEY_2,       [4]  = KEY_3,
	[5]  = KEY_4,       [6]  = KEY_5,       [7]  = KEY_6,
	[8]  = KEY_7,       [9]  = KEY_8,       [10] = KEY_9,
	[11] = KEY_0,       [12] = KEY_MINUS,   [13] = KEY_EQUAL,
	[14] = KEY_BACKSPACE, [15] = KEY_TAB,
	[16] = KEY_Q,       [17] = KEY_W,       [18] = KEY_E,
	[19] = KEY_R,       [20] = KEY_T,       [21] = KEY_Y,
	[22] = KEY_U,       [23] = KEY_I,       [24] = KEY_O,
	[25] = KEY_P,       [26] = KEY_LEFTBRACE, [27] = KEY_RIGHTBRACE,
	[28] = KEY_ENTER,   [29] = KEY_LEFTCTRL,
	[30] = KEY_A,       [31] = KEY_S,       [32] = KEY_D,
	[33] = KEY_F,       [34] = KEY_G,       [35] = KEY_H,
	[36] = KEY_J,       [37] = KEY_K,       [38] = KEY_L,
	[39] = KEY_SEMICOLON, [40] = KEY_APOSTROPHE, [41] = KEY_GRAVE,
	[42] = KEY_LEFTSHIFT, [43] = KEY_BACKSLASH,
	[44] = KEY_Z,       [45] = KEY_X,       [46] = KEY_C,
	[47] = KEY_V,       [48] = KEY_B,       [49] = KEY_N,
	[50] = KEY_M,       [51] = KEY_COMMA,   [52] = KEY_DOT,
	[53] = KEY_SLASH,   [54] = KEY_RIGHTSHIFT,
	[55] = KEY_KPASTERISK, [56] = KEY_LEFTALT, [57] = KEY_SPACE,
	[58] = KEY_CAPSLOCK,
	[59] = KEY_F1,      [60] = KEY_F2,      [61] = KEY_F3,
	[62] = KEY_F4,      [63] = KEY_F5,      [64] = KEY_F6,
	[65] = KEY_F7,      [66] = KEY_F8,      [67] = KEY_F9,
	[68] = KEY_F10,
	[69] = KEY_NUMLOCK, [70] = KEY_SCROLLLOCK,
	[71] = KEY_KP7,     [72] = KEY_KP8,     [73] = KEY_KP9,
	[74] = KEY_KPMINUS, [75] = KEY_KP4,     [76] = KEY_KP5,
	[77] = KEY_KP6,     [78] = KEY_KPPLUS,
	[79] = KEY_KP1,     [80] = KEY_KP2,     [81] = KEY_KP3,
	[82] = KEY_KP0,     [83] = KEY_KPDOT,
	[87] = KEY_F11,     [88] = KEY_F12,
};

static void kbmap_add(struct kbmap *km, uint32_t rune, int keycode) {
	if (rune == 0 || keycode == 0) return;
	if (km->count >= KBMAP_MAX_ENTRIES) return;

	/* First mapping wins — layer 0 is parsed first */
	for (int i = 0; i < km->count; i++)
		if (km->entries[i].rune == rune)
			return;

	km->entries[km->count++] = (struct kbmap_entry){
		.rune = rune, .keycode = keycode,
	};
}

/*
 * Parse Plan 9 rune representation.
 * See kbdfs(8) for the format.
 */
static int parse_rune(const char *s) {
	if (!s || !*s) return 0;

	while (*s && isspace((unsigned char)*s)) s++;

	if (s[0] == '\'' && s[1]) {
		int rune;
		if (utf8_decode((const unsigned char *)s + 1,
		                (const unsigned char *)s + 1 + strlen(s + 1), &rune) > 0)
			return rune;
		return 0;
	}

	if (s[0] == '^' && s[1]) {
		int c = (unsigned char)s[1];
		if (c >= 0x40 && c < 0x80)
			return c - 0x40;
		return 0;
	}

	char *end;
	long val = strtol(s, &end, 0);
	if (end > s && val >= 0 && val <= 0x10FFFF)
		return (int)val;

	return 0;
}

static void parse_kbmap_line(struct kbmap *km, const char *line) {
	char layer_str[32];
	int scancode;
	char rune_str[64];

	if (sscanf(line, "%31s %d %63s", layer_str, &scancode, rune_str) != 3)
		return;

	/*
	 * Only load layer 0 (none). We forward raw unshifted keycodes
	 * to Wayland and let XKB handle all modifier composition.
	 */
	if (strcmp(layer_str, "none") != 0 && strcmp(layer_str, "0") != 0)
		return;

	if (scancode < 0 || scancode >= 128) return;

	int rune = parse_rune(rune_str);
	if (rune == 0) return;

	/* Skip Plan 9 special keys (0xF000+), handled by static keymap */
	if (rune >= 0xF000) return;

	int keycode = scancode_to_keycode[scancode];
	if (keycode == 0) return;

	kbmap_add(km, rune, keycode);
}

int kbmap_load(struct kbmap *km, struct p9conn *p9) {
	uint32_t kbmap_fid;
	const char *wnames[] = { "kbmap" };

	memset(km, 0, sizeof(*km));

	kbmap_fid = p9->next_fid++;
	if (p9_walk(p9, p9->root_fid, kbmap_fid, 1, wnames) < 0) {
		wlr_log(WLR_INFO, "kbmap: /dev/kbmap not found");
		return -1;
	}

	uint32_t iounit = 8192;
	if (p9_open(p9, kbmap_fid, OREAD, &iounit) < 0) {
		wlr_log(WLR_ERROR, "kbmap: failed to open");
		p9_clunk(p9, kbmap_fid);
		return -1;
	}

	uint8_t *buf = malloc(iounit);
	if (!buf) {
		p9_clunk(p9, kbmap_fid);
		return -1;
	}

	uint64_t offset = 0;
	char *data = NULL;
	size_t data_size = 0;

	while (1) {
		int n = p9_read(p9, kbmap_fid, offset, iounit, buf);
		if (n < 0) goto fail;
		if (n == 0) break;

		char *tmp = realloc(data, data_size + n + 1);
		if (!tmp) goto fail;
		data = tmp;
		memcpy(data + data_size, buf, n);
		data_size += n;
		offset += n;
	}

	free(buf);
	buf = NULL;
	p9_clunk(p9, kbmap_fid);

	if (!data) return -1;
	data[data_size] = '\0';

	char *line = data;
	while (line && *line) {
		char *eol = strchr(line, '\n');
		if (eol) *eol = '\0';
		if (*line)
			parse_kbmap_line(km, line);
		line = eol ? eol + 1 : NULL;
	}

	free(data); 
	km->loaded = 1; 
	wlr_log(WLR_INFO, "kbmap: loaded %d mappings", km->count);
	return 0;

fail:
	free(buf);
	free(data);
	p9_clunk(p9, kbmap_fid);
	return -1;
}

const struct kbmap_entry *kbmap_lookup(struct kbmap *km, uint32_t rune) {
	if (!km || !km->loaded) return NULL;

	for (int i = 0; i < km->count; i++){
		if (km->entries[i].rune == rune){
		    wlr_log(WLR_DEBUG, "kbmap_lookup: rune=%d -> keycode=%d shift=%d ctrl=%d",
                	rune, km->entries[i].keycode, km->entries[i].shift, km->entries[i].ctrl);
			return &km->entries[i];
		}
		
	}
	return NULL;
}

void kbmap_cleanup(struct kbmap *km) {
	if (km) memset(km, 0, sizeof(*km));
}
