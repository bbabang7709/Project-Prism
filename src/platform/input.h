/*
 * Terminal input: turns standard input bytes into key and mouse events.
 *
 * - In Windows Terminal's win32 input mode (ESC [ Vk;Sc;Uc;Kd;Cs;Rc _) presses and releases are reported exactly.
 * - Other terminals fake "held" from key repeat (kept 200 ms at first, 90 ms while repeating).
 *   Walking keys (WASD) act once while held; every other key acts on each press.
 *
 * It never calls game code directly; it reports through the callbacks given to input_init.
 * Key numbers are Windows virtual-key codes (letters are upper case 'A'..'Z').
 */
#ifndef PRISM_PLATFORM_INPUT_H
#define PRISM_PLATFORM_INPUT_H

#include <stdint.h>

enum {
    VK_BACK = 0x08, VK_TAB = 0x09, VK_ENTER = 0x0D, VK_SHIFT = 0x10, VK_ESC = 0x1B, VK_SPACE = 0x20,
    VK_LEFT = 0x25, VK_UP = 0x26, VK_RIGHT = 0x27, VK_DOWN = 0x28,
    VK_PLUS = 0xBB, VK_MINUS = 0xBD,
};

typedef struct {
    void (*press)(int vk);    /* a key was pressed */
    void (*click)(void);      /* left mouse click */
    void (*wheel)(int dir);   /* mouse wheel: up -1, down +1 */
    void (*quit)(void);       /* Ctrl+C */
    void (*text)(int ch);     /* one character (Unicode, >= 32; for text fields, may be NULL) */
} InputHandlers;

void input_init(const InputHandlers *h);
void input_set_time(int64_t now_ms);   /* time of this tick; call before reading and before input_held */
void input_read(void);                 /* read what standard input has and report the events */
int input_held(int vk);                /* is the key held now */
int input_esc_expired(void);           /* 1 if a lone ESC byte has waited over 60 ms (consumes it: the ESC key by itself) */
int input_win32(void);                 /* has win32 input mode been seen */

#endif
