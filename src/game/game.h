/*
 * Game: players (up to 2) and cameras, aiming, taking/placing/turning, reset, puzzle checks, moving between rooms.
 *
 * Light is recomputed only when an object changes or a body moves (game_simulate).
 * Every player's body blocks light.
 * Take/place/turn remember the state before them so they can be undone (ACT_UNDO, up to 64 per room).
 * A hint (ACT_HINT) compares the current state with a solution from the starting state and names one next step.
 *
 * Solo and host: this module moves every player (game_tick).
 * Joiner (client): only draws the state the host sends, and sends its keys and actions to the host.
 */
#ifndef PRISM_GAME_H
#define PRISM_GAME_H

#include "renderer/renderer.h"

#define MAXPLAYER 2

/* keys being held (Player.keys) */
enum { K_W = 1, K_A = 2, K_S = 4, K_D = 8, K_LEFT = 16, K_RIGHT = 32, K_UP = 64, K_DOWN = 128, K_RUN = 256 };

/* actions (also the unit sent over the network). ACT_JUMP is not counted as a move */
enum { ACT_TAKE = 1, ACT_ROT_LEFT, ACT_ROT_RIGHT, ACT_RESET, ACT_UNDO, ACT_HINT, ACT_JUMP };

#define WALK_SPEED 3.0f                 /* cells per second */
#define RUN_SPEED  5.0f                 /* while K_RUN (Shift) is held */
#define JUMP_V     4.2f                 /* take-off speed: 0.56 cells high, 0.53 s in the air */
#define GRAVITY    15.75f
#define JUMP_MAX   1.0f                 /* largest height accepted from the network */

typedef struct {
    int on;                             /* a player is in this slot */
    float x, z, yaw, pitch;             /* position and view direction */
    float y, vy;                        /* jump height above the floor and vertical speed */
    int fp;                             /* 1: first person, 0: top-down (aiming works differently) */
    float walk_yaw;                     /* top-down: the direction walking is relative to (that player's camera angle) */
    unsigned keys;                      /* keys held, K_* */
    int carry;                          /* object being carried (-1 = none) */
    int tobj, tx, tz;                   /* object aimed at, cell to place on (-1 = none) */
    char msg[160];                      /* message to show this player */
    int msg_t, msg_seq;                 /* ticks left for the message, message number (+1 per new message) */
    int hint_t, hint_obj, hint_x, hint_z;   /* hint: ticks left, object and cell to show (-1 = none) */
} Player;

extern Player g_pl[MAXPLAYER];
extern int g_me;                        /* this terminal's player number */

/* this terminal's top-down camera */
extern float cam_yaw, cam_yaw_target, cam_pitch, cam_dist;

/* puzzle state */
extern int g_solved;                    /* every crystal lit (door open) */
extern int g_moves;                     /* moves made in this room */
extern int g_cleared;                   /* went through the last room's door */
extern int g_ticks;                     /* ticks played (1/30 s; stops at the end) */
extern int g_total_moves;               /* moves made in every room */
extern int g_record;                    /* 1: record solved rooms in the progress file (solo, host) */

void game_reset(void);                  /* new game: players, camera and records back to their starting values */

void game_init(int level);              /* build room number level (from 0), put player 0 on the start cell, compute the light */
void game_simulate(void);               /* light -> doors */
extern int g_sim_us;                    /* time of the last game_simulate (light + plates + door), microseconds */
int game_solve(void);                   /* try every mirror angle to find a solution */
#define SOLVE_MAX_MIRRORS 8             /* game_solve turns at most this many mirrors (8^8 settings) */
void game_solve_setup(void);            /* before solving, apply the level's stand (someone standing) and move (objects moved) lines */
void game_find_target(int p);           /* recompute the object player p aims at and the cell to place on */
void game_message(int p, const char *fmt, ...) __attribute__((format(printf, 2, 3)));   /* p < 0: everyone */

void game_join(int p);                  /* a player joined slot p (stands next to the start cell) */
void game_leave(int p);                 /* player p left (whatever they carried is put down there) */

void game_action(int p, int act);       /* player p's action ACT_* */
void game_view_key(int vk);             /* this terminal's view and camera keys (Tab, arrows, +/-) */
unsigned game_local_keys(void);         /* keys held on this terminal -> K_* (including the mouse-lock helper's key state) */
void game_mouse_look(float dx, float dy);   /* first person: turn the view by mouse motion (radians). dx > 0 right, dy > 0 down */
extern int g_look_local;                /* 1: this terminal's mouse sets my view (a joiner then ignores the view the host sends back) */
void game_tick(float dt);               /* every player walks and aims, messages count down (solo / host) */
void game_local_tick(void);             /* this terminal's camera follow and message time (joiner) */

void game_view(View *v);                /* view to hand to the renderer (this terminal's player) */
void game_print_state(const char *title);   /* --check output */

#endif
