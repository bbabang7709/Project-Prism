/*
 * Start menu, drawn over a slowly turning (solved) room:
 *
 *   Solo            < start room >   (two-player rooms left out)
 *   Host            < start room >   (all rooms)
 *   Join            address field
 *   Make a room     < room to edit / new room >
 *   Controls
 *   Quit
 *
 * It only turns keys into a MenuChoice; starting the game is main's job.
 */
#ifndef PRISM_GAME_MENU_H
#define PRISM_GAME_MENU_H

enum { MENU_NONE, MENU_SOLO, MENU_HOST, MENU_JOIN, MENU_EDIT, MENU_QUIT };

typedef struct {
    int what;                           /* MENU_* */
    int level;                          /* start room (from 0; for solo, in the list without two-player rooms). Editor: -1 = new room */
    const char *addr;                   /* address to join */
} MenuChoice;

/* open the menu. g_levels must hold every room. msg is a notice shown at the bottom (may be NULL) */
void menu_open(const char *msg);
void menu_message(const char *msg);

MenuChoice menu_key(int vk);            /* arrow keys, Enter, ESC, Backspace */
void menu_text(int ch);                 /* characters for the address field */
void menu_draw(int frame);

#endif
