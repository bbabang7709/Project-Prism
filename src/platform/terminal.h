/*
 * Terminal output: raw mode, a back/front cell buffer, sending only changed cells as 24-bit ANSI colour,
 * UTF-8 and double-width characters (Hangul), window size and signals.
 *
 * One screen cell (VCell) holds a block character with a foreground and a background colour,
 * or one HUD character. wide: 0 normal, 1 left half of a double-width character, 2 its right half (not printed).
 */
#ifndef PRISM_PLATFORM_TERMINAL_H
#define PRISM_PLATFORM_TERMINAL_H

#include <signal.h>
#include <stdint.h>
#include "common/config.h"

typedef struct { uint32_t fg, bg; char ch[5]; uint8_t wide; } VCell;

extern VCell g_back[MAX_H][MAX_W];       /* cells to draw this frame */
extern int g_cols, g_rows;                /* current window size (clipped to MAX_W, MAX_H) */
extern volatile sig_atomic_t g_quit, g_resize;

int term_setup(void);                     /* raw mode, alternate screen, mouse and win32 input modes. -1: not a terminal */
void term_restore(void);
void term_install_signals(void);          /* SIGINT/TERM/HUP → g_quit, SIGWINCH → g_resize */
void term_get_size(void);                 /* read the window size; the next output redraws everything */
void term_present(void);                  /* send g_back to the screen (changed cells only) */
void term_stats(int *bytes, int *changed, int *cells);   /* last term_present: bytes written, cells sent, cells on screen */

/* write text from cell (x, y); returns the width written (cells) */
int term_text(int x, int y, uint32_t fg, uint32_t bg, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));
void term_bar(int y, uint32_t bg);        /* fill row y with blank cells of colour bg */
int term_str_w(const char *s);            /* display width (double-width characters count 2) */

#endif
