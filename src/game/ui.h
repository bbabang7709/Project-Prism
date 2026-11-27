/*
 * UI helpers shared by the HUD, the start menu and the editor: panels, rounded frames,
 * chips and key caps drawn into the terminal cell buffer (g_back), plus one colour palette
 * so every screen looks the same.
 */
#ifndef PRISM_GAME_UI_H
#define PRISM_GAME_UI_H

#include <stdint.h>

/* palette */
#define UI_BAR      0x161822u           /* top and bottom bars */
#define UI_PANEL    0x1A1E2Au           /* panels and cards */
#define UI_PANEL_HI 0x2E3550u           /* selected row */
#define UI_KEY      0x3A4160u           /* key cap background */
#define UI_TEXT     0xD8DCE8u
#define UI_DIM      0x8A90A0u
#define UI_GOLD     0xFFD27Au           /* titles, accents */
#define UI_OK       0x7CE07Cu           /* solved, open */
#define UI_INFO     0x9FD4FFu           /* links, values */
#define UI_WARN     0xFFB86Au

void ui_fill(int x, int y, int w, int h, uint32_t bg);
/* a panel with a rounded frame in colour fg; title (may be NULL) sits in the top border */
void ui_frame(int x, int y, int w, int h, uint32_t fg, uint32_t bg, const char *title);
/* only the frame cells (the inside is left as it is) */
void ui_border(int x, int y, int w, int h, uint32_t fg, uint32_t bg, const char *title);
/* " text " on its own background; returns the width used */
int ui_chip(int x, int y, uint32_t fg, uint32_t bg, const char *fmt, ...) __attribute__((format(printf, 5, 6)));
/* key caps: pairs of { key, label } drawn as [key] label  [key] label ...; returns the width used */
int ui_keys(int x, int y, uint32_t bg, const char *const *pairs, int npairs);
/* width ui_keys would use */
int ui_keys_w(const char *const *pairs, int npairs);
void ui_center(int y, uint32_t fg, uint32_t bg, const char *s);

/* the PRISM logo in block letters with a spectrum gradient (30 x 3 cells); returns its width */
int ui_logo(int x, int y, uint32_t bg, int frame);

#endif
