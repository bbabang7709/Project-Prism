/*
 * Level editor: in the top-down view, pick a cell with the cursor and place, erase and turn blocks and objects.
 * Light is recomputed after every change and shown at once.
 *
 *   arrows      cursor (screen up/down/left/right)    Tab / ]  next tool,  [  previous tool
 *   Enter / F   place          Backspace / X  erase (to floor)
 *   Q / E       turn (mirrors 22.5 degrees, emitters 45)
 *   , / .       turn the camera      + / -  zoom
 *   N           rename (Hangul input works)     V  verify (solve)     T  test play
 *   S           save (levels folder: temp file -> fsync -> rename)
 *   ESC         leave (press twice if not saved)
 *
 * The room being edited lives in g_levels[0] and load_level builds the world from it. Back in the menu, main reloads the level files.
 */
#ifndef PRISM_GAME_EDITOR_H
#define PRISM_GAME_EDITOR_H

#include "world/world.h"

enum { EDIT_NONE, EDIT_QUIT, EDIT_TEST };

void editor_open(const Level *src);     /* edit src; NULL = a new room */
void editor_resume(void);               /* back from test play */
const Level *editor_level(void);        /* the room being edited (for test play) */

int editor_key(int vk);                 /* EDIT_* */
void editor_text(int ch);               /* [, ], comma, period, name input */
void editor_draw(int frame);

#endif
