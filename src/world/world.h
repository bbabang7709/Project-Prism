/*
 * World: level files, voxel blocks, the object list, and the object number of each cell.
 *
 * Ownership
 *   g_levels the rooms read from level files; filled by world_load_levels / world_use_builtin.
 *   g_blk    written by load_level and world_set_door; other modules only read it.
 *   g_obj    position and angle (x, z, rot, held) belong to the game; got and lit to the light simulation.
 *   g_objat  object number + 1 (0 = empty cell); written by load_level and the game (take/place).
 *
 * Level file format (.lvl files in the levels folder; rooms 1, 2, ... in file name order)
 *   # comment            lines starting with # before the map
 *   name 빛의 신전        room name
 *   start 2 8 E          start cell (x z) and facing E S W N
 *   rot 6 2 2            starting angle 0-7 of the object at (x, z) (mirrors in 22.5 degree steps, emitters 45)
 *   coop 11 9            two-player room: solved while someone stands at (x, z) blocking light (skipped when solo). "coop" alone also works
 *   start2 12 4 W        player 2's start cell and facing (split rooms start each player in their own half)
 *   link 4 5 1           ties the plate/sensor/gate at (x, z) to link 1 (0-3, default 0).
 *                        a gate is open while every plate of its link is pressed and every sensor of it is lit
 *   stand 4 8            for the solver (--solve): assume someone stands on this cell (plate rooms). Twice = two people
 *   move 3 9 13 7        for the solver: move the object at (3, 9) to (13, 7) first (up to 4 lines, box rooms)
 *   tutorial             a tutorial room: a card walks each player through the basics step by step
 *   map                  the next 12 lines are the 16-column map
 *   map characters: . floor  # wall  | pillar  D door  g glass  E emitter  P prism  M mirror  S half mirror
 *             b blue filter  r red filter  R G B Y C V crystals (red, green, blue, yellow, cyan = green+blue, magenta = red+blue)
 *             _ plate  = gate (open while every plate is pressed and every sensor is lit; people or objects press plates)
 *             O sensor (lit by any light)  T portal (0 or 2 per room; light entering one leaves the other in the same direction)
 *             K box (can be carried; stops light and presses plates)
 *             h hatch (people cannot pass, light can, objects put on it can be taken from either side)
 *             w low wall (knee high: stops people and low beams, high beams pass over)
 *             U light lift (moves a beam between the low and the high height, same direction)
 *             W white crystal (needs red, green and blue)
 */
#ifndef PRISM_WORLD_H
#define PRISM_WORLD_H

#include <stddef.h>
#include <stdint.h>
#include "common/config.h"

#define MAXLEVEL 32

enum { B_AIR, B_FLOOR, B_WALL, B_GLASS, B_DOOR, B_PLATE, B_PLATE_ON, B_GATE, B_HATCH };   /* plate (up/pressed), gate, hatch floor */
enum { O_EMIT = 1, O_MIRROR, O_SPLIT, O_PRISM, O_FILTER, O_CRYSTAL, O_SENSOR, O_PORTAL, O_BOX, O_LIFT };

typedef struct {
    int kind, x, z, rot, movable, held;
    float col[3];             /* emitter colour, filter colour, colour a crystal wants */
    float got[3];             /* light that reached a crystal */
    int lit;
} Obj;

typedef struct {
    char name[64];
    char map[RD][RW + 1];
    signed char rot[RD][RW];  /* starting angle of each object */
    int sx, sz;               /* start cell */
    char face;                /* starting facing: 'E' 'S' 'W' 'N' */
    int coop;                 /* two-player room (skipped when solo) */
    int tutorial;             /* step-by-step tutorial room (tutorial line) */
    int has_stand;            /* for the solver: solved only while someone stands on (stand_x, stand_z) */
    int stand_x, stand_z;
    int nmove;                /* for the solver: solved after moving objects (move x1 z1 x2 z2) */
    signed char move[4][4];
    char file[64];            /* file name it was read from (no folder); empty for the built-in room or a new room */
    signed char chan[RD][RW]; /* link number 0-3 of plates, sensors and gates (link lines) */
    int has_start2, sx2, sz2; /* player 2's start cell (start2 line) */
    char face2;
    int has_stand2, stand2_x, stand2_z;   /* for the solver: a second person (second stand line) */
    char note[512];           /* description comment lines at the top of the file ('#' included, '\n' after each); the editor writes them back */
} Level;

extern const char *KIND_NAME[];          /* object names, indexed by O_* */
extern const float CHAN_COL[4][3];       /* colour of each link number (0 cyan, 1 orange, 2 pink, 3 sky blue) */

extern Level g_levels[MAXLEVEL];
extern int g_nlevels, g_cur_level;       /* number of rooms, current room (from 0) */

extern uint8_t g_blk[RH][RD][RW];
extern Obj g_obj[MAXOBJ];
extern int g_nobj, g_objat[RD][RW];

/* read the *.lvl files in dir in name order.
 * Returns the number of rooms: 0 if the folder is missing, -1 with "file:line: reason" in err if a file is wrong */
int world_load_levels(const char *dir, char *err, size_t errlen);

/* remove two-player (coop) rooms from the list (solo play) */
void world_drop_coop(void);

/* folder the level files came from ("levels" when the built-in room is used) */
extern char g_level_dir[256];

/* check that a room follows the rules (has an emitter and a crystal, starts on empty floor, border is wall/door, ...).
 * 0 if it does, otherwise -1 with the reason in err */
int world_check_level(const Level *lv, char *err, size_t errlen);

/* save the room as g_level_dir/file: write a temp file in the same folder, fsync, then rename over the old one,
 * so a crash while saving never corrupts the original. 0 on success */
int world_save_level(const Level *lv, const char *file, char *err, size_t errlen);

/* name for a new level file: the largest number in the folder + 1 ("07-custom.lvl") */
void world_new_file_name(char *out, size_t n);

/* use the one built-in room (the original prototype's room) when there are no level files */
void world_use_builtin(void);

/* build the current room (g_cur_level) in its starting state */
void load_level(void);

/* open (air) or close (door blocks) the door (D) cells of the current room */
void world_set_door(int open);

/* show plates as pressed, open or close gate cells (when is the game's decision) */
void world_set_plate(int x, int z, int pressed);
void world_set_gate(int x, int z, int open);

/* is (x, z) a door cell of the current room */
int world_is_door(int x, int z);

#endif
