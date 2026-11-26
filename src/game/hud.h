/*
 * Builds one screen: moves the rendered pixels into block-character cells, then adds
 * the status line, what you hold or aim at, the help line and messages, and sends it to the terminal.
 */
#ifndef PRISM_GAME_HUD_H
#define PRISM_GAME_HUD_H

void hud_draw(int frame, int fps, const char *net);   /* net: network status text (NULL when playing alone) */
void hud_toggle_help(void);
void hud_toggle_perf(void);             /* P: performance overlay (render threads, light, terminal output) */
void hud_set_autores(int on);           /* dynamic resolution on/off (--full-res turns it off) */

/* display mode: 1 = 2x2 pixels per cell (quadrant blocks), 0 = 1x2 pixels (half block) */
int hud_quad(void);
void hud_set_quad(int on);

/* pixel size to render in the current mode, and the pixel height/width ratio */
void hud_frame_size(int *W, int *H, float *paspect);

/* move g_pix into terminal cells (g_back) at brightness k/256 (256 = unchanged) */
void hud_blit(int k);

/* development: redraw the block characters in g_back into g_pix, cw x ch pixels per cell,
 * to see as an image what a real terminal would show */
void hud_rasterize(int cw, int ch);

#endif
