/*
 * Hint guide: after a hint (I), a glowing figure appears at this terminal's player,
 * walks a breadth-first-search path over floor cells to the hinted object or cell,
 * and floats there while the hint lasts.
 *
 * It lives only on this terminal: it is driven by the hint fields every player already
 * receives (so a joiner gets one too), never blocks light, never collides, and is never
 * sent over the network.
 */
#ifndef PRISM_GAME_GUIDE_H
#define PRISM_GAME_GUIDE_H

#include "common/types.h"

void guide_tick(float dt);              /* follow this terminal's hint: start, walk, or vanish */
int guide_body(Body *b);                /* 1 and the body to draw while the guide is shown */

#endif
