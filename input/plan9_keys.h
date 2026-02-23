/*
 * plan9_keys.h - Plan 9 keyboard constants
 *
 * Key definitions from 9front /sys/include/keyboard.h
 *
 * This is the CANONICAL source for Plan 9 key constants in the p9wl
 * project. Do not redefine these values elsewhere; include this header.
 *
 * Plan 9 uses the Unicode Private Use Area (U+F000-U+FFFF) for special
 * keys that don't have standard Unicode codepoints:
 *   - KF base (0xF000): Function keys, navigation, modifiers
 *   - Spec base (0xF800): Special keys (caps lock, num lock, etc.)
 *   - PF base (0xF820): Numpad function keys
 *
 * Regular printable characters use their standard Unicode codepoints.
 * Control characters (Ctrl+A = 0x01, etc.) use ASCII values 0x00-0x1F.
 *
 * Usage:
 *
 *   #include "plan9_keys.h"
 *
 *   if (rune == Kshift) {
 *       // Handle shift key
 *   } else if (rune >= KF1 && rune <= KF12) {
 *       // Handle function keys
 *   }
 */

#ifndef PLAN9_KEYS_H
#define PLAN9_KEYS_H

/* ============== Base Values ============== */

#define KF              0xF000  /* Function/navigation key base */
#define Spec            0xF800  /* Special keys base */
#define PF              (Spec|0x20)  /* Numpad function key base (0xF820) */

/* ============== Function Keys F1-F12 ============== */

#define KF1             (KF|0x01)   /* 0xF001 */
#define KF2             (KF|0x02)   /* 0xF002 */
#define KF3             (KF|0x03)   /* 0xF003 */
#define KF4             (KF|0x04)   /* 0xF004 */
#define KF5             (KF|0x05)   /* 0xF005 */
#define KF6             (KF|0x06)   /* 0xF006 */
#define KF7             (KF|0x07)   /* 0xF007 */
#define KF8             (KF|0x08)   /* 0xF008 */
#define KF9             (KF|0x09)   /* 0xF009 */
#define KF10            (KF|0x0A)   /* 0xF00A */
#define KF11            (KF|0x0B)   /* 0xF00B */
#define KF12            (KF|0x0C)   /* 0xF00C */

/* ============== Navigation Keys ============== */

#define Khome           (KF|0x0D)   /* 0xF00D - Home */
#define Kup             (KF|0x0E)   /* 0xF00E - Up arrow */
#define Kpgup           (KF|0x0F)   /* 0xF00F - Page Up */
#define Kprint          (KF|0x10)   /* 0xF010 - Print Screen */
#define Kleft           (KF|0x11)   /* 0xF011 - Left arrow */
#define Kright          (KF|0x12)   /* 0xF012 - Right arrow */
#define Kpgdown         (KF|0x13)   /* 0xF013 - Page Down */
#define Kins            (KF|0x14)   /* 0xF014 - Insert */

/* ============== Modifier Keys ============== */

#define Kalt            (KF|0x15)   /* 0xF015 - Alt */
#define Kshift          (KF|0x16)   /* 0xF016 - Shift */
#define Kctl            (KF|0x17)   /* 0xF017 - Control */

/* ============== Navigation Keys (continued) ============== */

#define Kend            (KF|0x18)   /* 0xF018 - End */

/* ============== Lock Keys ============== */

#define Kscroll         (KF|0x19)   /* 0xF019 - Scroll Lock */

/* ============== Scroll Keys ============== */

#define Kscrolloneup    (KF|0x20)   /* 0xF020 - Scroll one line up */
#define Kscrollonedown  (KF|0x21)   /* 0xF021 - Scroll one line down */

/* ============== Multimedia Keys ============== */

#define Ksbwd           (KF|0x22)   /* 0xF022 - Skip backward */
#define Ksfwd           (KF|0x23)   /* 0xF023 - Skip forward */
#define Kpause          (KF|0x24)   /* 0xF024 - Play/Pause */
#define Kvoldn          (KF|0x25)   /* 0xF025 - Volume down */
#define Kvolup          (KF|0x26)   /* 0xF026 - Volume up */
#define Kmute           (KF|0x27)   /* 0xF027 - Mute */
#define Kbrtdn          (KF|0x28)   /* 0xF028 - Brightness down */
#define Kbrtup          (KF|0x29)   /* 0xF029 - Brightness up */

/* ============== Special Keys (Spec base) ============== */

/*
 * Kview/Kdown: In Plan 9, the down arrow produces Kview (0xF800),
 * which historically meant "scroll view down". For compatibility,
 * we alias Kdown to Kview.
 */
#define Kview           (Spec|0x00) /* 0xF800 - View/scroll down */
#define Kdown           Kview       /* Down arrow = Kview */

#define Kbreak          (Spec|0x61) /* 0xF861 - Break/Pause */
#define Kcaps           (Spec|0x64) /* 0xF864 - Caps Lock */
#define Knum            (Spec|0x65) /* 0xF865 - Num Lock */
#define Kmiddle         (Spec|0x66) /* 0xF866 - Middle mouse button (chord) */
#define Kaltgr          (Spec|0x67) /* 0xF867 - AltGr (right Alt) */
#define Kmod4           (Spec|0x68) /* 0xF868 - Super/Windows/Mod4 key */

/* ============== ASCII Control Characters ============== */

/*
 * These are standard ASCII values, not Plan 9 specific.
 * Included here for convenience when handling keyboard input.
 */
#define Kbs             0x08        /* Backspace (Ctrl+H) */
#define Ktab            0x09        /* Tab (Ctrl+I) */
#define Kenter          0x0A        /* Enter/Return (Ctrl+J, newline) */
#define Kesc            0x1B        /* Escape */
#define Kdel            0x7F        /* Delete */

#endif /* PLAN9_KEYS_H */
