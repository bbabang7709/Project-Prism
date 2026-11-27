/*
 * ui.c - panels, frames, chips, key caps and the logo, drawn into the terminal cell buffer.
 */
#include "game/ui.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "platform/terminal.h"

void ui_fill(int x0, int y0, int w, int h, uint32_t bg)
{
    for (int y = y0; y < y0 + h && y < g_rows; y++)
        for (int x = x0; x < x0 + w && x < g_cols; x++)
            if (x >= 0 && y >= 0) g_back[y][x] = (VCell){ UI_TEXT, bg, " ", 0 };
}

static void put(int x, int y, uint32_t fg, uint32_t bg, const char *ch)
{
    if (x < 0 || y < 0 || x >= g_cols || y >= g_rows) return;
    VCell *v = &g_back[y][x];
    memset(v->ch, 0, sizeof v->ch);
    memcpy(v->ch, ch, strlen(ch) < sizeof v->ch ? strlen(ch) : sizeof v->ch - 1);
    v->fg = fg;
    v->bg = bg;
    v->wide = 0;
}

void ui_frame(int x, int y, int w, int h, uint32_t fg, uint32_t bg, const char *title)
{
    if (w < 2 || h < 2) return;
    ui_fill(x, y, w, h, bg);
    ui_border(x, y, w, h, fg, bg, title);
}

void ui_border(int x, int y, int w, int h, uint32_t fg, uint32_t bg, const char *title)
{
    if (w < 2 || h < 2) return;
    for (int i = 1; i < w - 1; i++) { put(x + i, y, fg, bg, "─"); put(x + i, y + h - 1, fg, bg, "─"); }
    for (int j = 1; j < h - 1; j++) { put(x, y + j, fg, bg, "│"); put(x + w - 1, y + j, fg, bg, "│"); }
    put(x, y, fg, bg, "╭"); put(x + w - 1, y, fg, bg, "╮");
    put(x, y + h - 1, fg, bg, "╰"); put(x + w - 1, y + h - 1, fg, bg, "╯");
    if (title && title[0]) term_text(x + 2, y, UI_GOLD, bg, " %s ", title);
}

int ui_chip(int x, int y, uint32_t fg, uint32_t bg, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return term_text(x, y, fg, bg, " %s ", buf);
}

int ui_keys_w(const char *const *pairs, int n)
{
    int w = 0;
    for (int i = 0; i < n; i++) w += term_str_w(pairs[2 * i]) + 2 + 1 + term_str_w(pairs[2 * i + 1]) + 3;
    return w;
}

int ui_keys(int x, int y, uint32_t bg, const char *const *pairs, int n)
{
    int x0 = x;
    for (int i = 0; i < n; i++) {
        x += term_text(x, y, 0xFFFFFF, UI_KEY, " %s ", pairs[2 * i]);
        x += term_text(x, y, UI_DIM, bg, " %s   ", pairs[2 * i + 1]);
    }
    return x - x0;
}

void ui_center(int y, uint32_t fg, uint32_t bg, const char *s)
{
    term_text(g_cols / 2 - term_str_w(s) / 2, y, fg, bg, "%s", s);
}

/* 5x5 block letters, one bit per pixel (bit 4 = left column) */
static const unsigned char LOGO[5][5] = {
    { 0x1E, 0x11, 0x1E, 0x10, 0x10 },    /* P */
    { 0x1E, 0x11, 0x1E, 0x14, 0x13 },    /* R */
    { 0x1F, 0x04, 0x04, 0x04, 0x1F },    /* I */
    { 0x0F, 0x10, 0x0E, 0x01, 0x1E },    /* S */
    { 0x11, 0x1B, 0x15, 0x11, 0x11 },    /* M */
};

/* a colour along the visible spectrum, t in 0..1 (red -> violet), as a prism would split white light */
static uint32_t spectrum(float t)
{
    float h = t * 5.0f, r, g, b;
    if (h < 1) { r = 1; g = h; b = 0; }
    else if (h < 2) { r = 2 - h; g = 1; b = 0; }
    else if (h < 3) { r = 0; g = 1; b = h - 2; }
    else if (h < 4) { r = 0; g = 4 - h; b = 1; }
    else { r = h - 4; g = 0; b = 1; }
    return (uint32_t)(60 + r * 195) << 16 | (uint32_t)(60 + g * 195) << 8 | (uint32_t)(60 + b * 195);
}

int ui_logo(int x, int y, uint32_t bg, int frame)
{
    /* two pixel rows per cell with half blocks: rows (0,1), (2,3), (4,-) */
    static const char *HALF[4] = { " ", "▀", "▄", "█" };
    int w = 5 * 6 - 1;
    for (int L = 0; L < 5; L++)
        for (int c = 0; c < 5; c++) {
            int col = L * 6 + c;
            float t = fmodf((float)col / (float)w + (float)frame * 0.004f, 1.0f);
            uint32_t fg = spectrum(t);
            for (int r = 0; r < 3; r++) {
                int top = (LOGO[L][r * 2] >> (4 - c)) & 1;
                int bot = r * 2 + 1 < 5 ? (LOGO[L][r * 2 + 1] >> (4 - c)) & 1 : 0;
                put(x + col, y + r, fg, bg, HALF[top | bot << 1]);
            }
        }
    return w;
}
