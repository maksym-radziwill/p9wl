/*
 * wayland.h - Wayland compositor components (convenience header)
 *
 * This header provides a single include for all Wayland-related modules
 * in the p9wl compositor. It is intended for source files that need
 * access to multiple Wayland subsystems.
 *
 * Included Modules:
 *
 *   types.h        - Core type definitions (includes focus_manager.h)
 *   popup.h        - XDG popup lifecycle management
 *   toplevel.h     - XDG toplevel window management
 *   wl_input.h     - Input event processing (mouse, keyboard)
 *   output.h       - Output creation and frame rendering
 *   client.h       - Decoration handling and server cleanup
 *
 * Focus Management:
 *
 *   The focus_manager API (via types.h -> focus_manager.h) is the
 *   primary interface for focus-related operations. The old focus.h
 *   header is deprecated and retained only for compatibility.
 *
 * Usage:
 *
 *   For source files that need multiple Wayland subsystems:
 *
 *     #include "wayland/wayland.h"
 *
 *   For new code, prefer including only the specific headers you need
 *   rather than this catch-all header:
 *
 *     #include "wayland/toplevel.h"
 *     #include "wayland/popup.h"
 */

#ifndef P9WL_WAYLAND_H
#define P9WL_WAYLAND_H

/* ============== Included Headers ============== */

#include "../types.h"
#include "popup.h"
#include "toplevel.h"
#include "wl_input.h"
#include "output.h"
#include "client.h"

#endif /* P9WL_WAYLAND_H */
