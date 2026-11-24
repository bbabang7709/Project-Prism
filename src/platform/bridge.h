/*
 * bridge.h - TCP input bridge for the optional Windows "mouselock" helper.
 *
 * The game listens on 127.0.0.1:47017. The helper sends 16-byte packets with
 * raw mouse motion and the real held state of the movement keys; the game
 * answers with single control bytes ('L' lock, 'U' unlock, 'Q' quit).
 */
#ifndef PRISM_PLATFORM_BRIDGE_H
#define PRISM_PLATFORM_BRIDGE_H

#include <stdint.h>

#define BRIDGE_PORT 47017

enum {
    KB_W = 1 << 0, KB_A = 1 << 1, KB_S = 1 << 2, KB_D = 1 << 3,
    KB_SPACE = 1 << 4, KB_SHIFT = 1 << 5, KB_LEFT = 1 << 6, KB_RIGHT = 1 << 7,
    KB_UP = 1 << 8, KB_DOWN = 1 << 9, KB_F = 1 << 10, KB_LMB = 1 << 11
};

/* bit 31 of BridgePacket.keys: set by the helper only while it has found the
 * game window and that window is in the foreground. Without it the game keeps
 * using the terminal's own keyboard/mouse input. */
#define KB_ACTIVE 0x80000000u

typedef struct __attribute__((packed)) {
    int32_t  dx, dy;      /* raw mouse counts since the last packet */
    uint32_t keys;        /* KB_* bits currently held */
    uint32_t seq;
} BridgePacket;

int  bridge_start(void);                     /* starts the listener thread */
void bridge_stop(void);
int  bridge_connected(void);                 /* helper connected and alive */
void bridge_take_motion(int *dx, int *dy);   /* consume accumulated motion */
uint32_t bridge_keys(void);                  /* held keys (0 if not connected) */
void bridge_send(char c);                    /* 'L', 'U', 'Q' */

#endif
