/*
 * Tutorial and device introductions.
 *
 * Tutorial: a room whose level file has a "tutorial" line walks each player through the basics
 * one step at a time (walk, take, put down, turn, top-down view, light the crystal, leave).
 * The step lives in Player.tut (0 = no tutorial) and is sent to a joiner with the state.
 *
 * Introductions: when a room uses a device that no earlier room in the list had, a short card
 * explains it as the room starts (sent as an ordinary message, so a joiner sees it too).
 */
#ifndef PRISM_GAME_TUTORIAL_H
#define PRISM_GAME_TUTORIAL_H

#define TUT_STEPS 7

extern int g_room_intro;                /* 1 if the current room started with an introduction card */

void tutorial_room_start(void);         /* game_init: reset the steps, show what is new in this room */
void tutorial_join(int p);              /* a player joined during the room */
void tutorial_tick(void);               /* advance each player's step (solo / host) */
const char *tutorial_text(int step);    /* what step (1..TUT_STEPS) asks for */

#endif
