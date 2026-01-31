/*
 * plan9_keys.h - Plan 9 keyboard constants
 *
 * Key definitions from 9front /sys/include/keyboard.h
 * Consolidated here to avoid repetition across files.
 */

#ifndef PLAN9_KEYS_H
#define PLAN9_KEYS_H

/* Base values */
#define KF              0xF000  /* Function key base */
#define Spec            0xF800  /* Special keys base */
#define PF              (Spec|0x20)  /* Num pad function key */

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

/* Navigation keys */
#define Khome           (KF|0x0D)
#define Kup             (KF|0x0E)
#define Kpgup           (KF|0x0F)
#define Kprint          (KF|0x10)
#define Kleft           (KF|0x11)
#define Kright          (KF|0x12)
#define Kpgdown         (KF|0x13)
#define Kins            (KF|0x14)

/* Modifier keys */
#define Kalt            (KF|0x15)
#define Kshift          (KF|0x16)
#define Kctl            (KF|0x17)
#define Kend            (KF|0x18)
#define Kscroll         (KF|0x19)

/* Scroll keys */
#define Kscrolloneup    (KF|0x20)
#define Kscrollonedown  (KF|0x21)

/* Multimedia keys */
#define Ksbwd           (KF|0x22)
#define Ksfwd           (KF|0x23)
#define Kpause          (KF|0x24)
#define Kvoldn          (KF|0x25)
#define Kvolup          (KF|0x26)
#define Kmute           (KF|0x27)
#define Kbrtdn          (KF|0x28)
#define Kbrtup          (KF|0x29)

/* Special keys (Spec base) */
#define Kview           (Spec|0x00)  /* Down arrow = Kview */
#define Kdown           Kview
#define Kbreak          (Spec|0x61)
#define Kcaps           (Spec|0x64)
#define Knum            (Spec|0x65)
#define Kmiddle         (Spec|0x66)
#define Kaltgr          (Spec|0x67)
#define Kmod4           (Spec|0x68)  /* Super/Windows/Mod4 */

/* ASCII control characters */
#define Kbs             0x08
#define Ktab            0x09
#define Kenter          0x0A
#define Kesc            0x1B
#define Kdel            0x7F

#endif /* PLAN9_KEYS_H */
