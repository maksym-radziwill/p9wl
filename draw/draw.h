/*
 * draw.h - Plan 9 draw device initialization and window management
 */

#ifndef P9WL_DRAW_H
#define P9WL_DRAW_H

#include "../types.h"

/* Draw device initialization */
int init_draw(struct server *s);

/* Window management */
int relookup_window(struct server *s);
void delete_rio_window(struct p9conn *p9);

#endif /* P9WL_DRAW_H */
