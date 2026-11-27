#include "game/game.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/config.h"
#include "common/mathutil.h"
#include "game/guide.h"
#include "game/progress.h"
#include "optics/optics.h"
#include "platform/bridge.h"
#include "platform/clock.h"
#include "platform/input.h"
#include "world/world.h"

Player g_pl[MAXPLAYER] = {
    { .on = 1, .x = 2.5f, .z = 8.5f, .yaw = PI / 2, .pitch = -0.35f, .fp = 1, .walk_yaw = 0.62f,
      .carry = -1, .tobj = -1, .tx = -1, .tz = -1 },
    { .carry = -1, .tobj = -1, .tx = -1, .tz = -1 },
};
int g_me;

float cam_yaw = 0.62f, cam_yaw_target = 0.62f, cam_pitch = -0.98f, cam_dist = 17.5f;

int g_solved;
int g_moves;
int g_cleared;
int g_ticks;
int g_total_moves;
int g_record;

/* undo: object state and what each player carried before a move (cleared on room change or reset) */
#define MAXUNDO 64
typedef struct { int x[MAXOBJ], z[MAXOBJ], rot[MAXOBJ], held[MAXOBJ], carry[MAXPLAYER]; } Snap;
static Snap s_undo[MAXUNDO];
static int s_nundo;

void game_reset(void)
{
    for (int i = 0; i < MAXPLAYER; i++)
        g_pl[i] = (Player){ .on = i == 0, .yaw = PI / 2, .pitch = -0.35f, .fp = 1, .walk_yaw = 0.62f,
                            .carry = -1, .tobj = -1, .tx = -1, .tz = -1 };
    g_me = 0;
    cam_yaw = cam_yaw_target = 0.62f;
    cam_pitch = -0.98f;
    cam_dist = 17.5f;
    g_solved = g_moves = g_cleared = g_ticks = g_total_moves = 0;
    g_record = 0;
    s_nundo = 0;
}

/* ---------- light and doors ---------- */

/* does a player body (circle) overlap cell (x, z) */
static int body_on_cell(const Player *p, int x, int z)
{
    float qx = clampf(p->x, (float)x, (float)x + 1), qz = clampf(p->z, (float)z, (float)z + 1);
    return (qx - p->x) * (qx - p->x) + (qz - p->z) * (qz - p->z) < PR * PR;
}

/* set of lit sensors (one bit per object). A sensor's lit is the previous light result */
static unsigned sensor_mask(int *count)
{
    unsigned m = 0;
    int n = 0;
    for (int i = 0; i < g_nobj; i++)
        if (g_obj[i].kind == O_SENSOR) { n++; if (g_obj[i].lit) m |= 1u << i; }
    if (count) *count = n;
    return m;
}

/* plates, sensors and gates: a plate is pressed by a person standing on it or an object on it; a sensor is lit by light.
 * A gate is open while every plate with its link number (0-3) is pressed and every such sensor is lit.
 * A gate cell with a person on it never closes (no one gets stuck). */
static void update_plates(void)
{
    const Level *lv = &g_levels[g_cur_level];
    int plates[4] = { 0 }, all[4] = { 1, 1, 1, 1 };
    for (int i = 0; i < g_nobj; i++)                      /* sensors (previous light result) */
        if (g_obj[i].kind == O_SENSOR) {
            int c = lv->chan[g_obj[i].z][g_obj[i].x] & 3;
            plates[c]++;
            all[c] &= g_obj[i].lit;
        }
    for (int z = 0; z < RD; z++)
        for (int x = 0; x < RW; x++) {
            if (lv->map[z][x] != '_') continue;
            int pressed = g_objat[z][x] != 0, c = lv->chan[z][x] & 3;
            for (int i = 0; i < MAXPLAYER && !pressed; i++)
                pressed = g_pl[i].on && (int)floorf(g_pl[i].x) == x && (int)floorf(g_pl[i].z) == z;
            world_set_plate(x, z, pressed);
            plates[c]++;
            all[c] &= pressed;
        }
    for (int z = 0; z < RD; z++)
        for (int x = 0; x < RW; x++) {
            if (lv->map[z][x] != '=') continue;
            int c = lv->chan[z][x] & 3, open = plates[c] > 0 && all[c];
            for (int i = 0; i < MAXPLAYER && !open; i++)
                open = g_pl[i].on && body_on_cell(&g_pl[i], x, z);
            world_set_gate(x, z, open);
        }
}

/* Update plates and gates, recompute the light (every player body blocks it), then open doors from the result.
 * Gate and door blocks stop light, so the order must stay "plates and gates -> light -> doors".
 * Sensors switching gates changes the light paths, so repeat until no sensor changes
 * (stops after 4 passes in a room where a gate blocks the light to its own sensor) */
int g_sim_us;

void game_simulate(void)
{
    int64_t t0 = now_us();
    Body b[MAXPLAYER];
    int n = 0;
    for (int i = 0; i < MAXPLAYER; i++)
        if (g_pl[i].on) b[n++] = (Body){ g_pl[i].x, g_pl[i].z, i, g_pl[i].y };
    for (int pass = 0; pass < 4; pass++) {
        unsigned before = sensor_mask(NULL);
        update_plates();
        g_solved = optics_simulate(b, n);
        if (sensor_mask(NULL) == before) break;
    }
    world_set_door(g_solved);
    g_sim_us = (int)(now_us() - t0);
}

/* stand in the centre of the room's start cell, facing the given way.
 * The second player stands on empty floor around the start cell */
static void place_player(int p)
{
    static const int NB[9][2] = { { 0, 0 }, { 0, 1 }, { 0, -1 }, { -1, 0 }, { 1, 0 }, { -1, 1 }, { 1, 1 }, { -1, -1 }, { 1, -1 } };
    const Level *lv = &g_levels[g_cur_level];
    int own = p == 1 && lv->has_start2;                   /* split room: player 2 stands on their own start cell */
    int bx = own ? lv->sx2 : lv->sx, bz = own ? lv->sz2 : lv->sz, x = bx, z = bz;
    char face = own ? lv->face2 : lv->face;
    for (int k = p && !own ? 1 : 0; k < 9; k++) {
        int cx = bx + NB[k][0], cz = bz + NB[k][1];
        if (lv->map[cz][cx] == '.') { x = cx; z = cz; break; }
    }
    Player *pl = &g_pl[p];
    pl->x = (float)x + 0.5f;
    pl->z = (float)z + 0.5f;
    pl->y = pl->vy = 0;                                   /* land on the start cell */
    pl->yaw = face == 'E' ? PI / 2 : face == 'W' ? -PI / 2 : face == 'N' ? PI : 0;
    pl->pitch = -0.35f;
    pl->carry = -1;
}

void game_init(int level)
{
    s_nundo = 0;
    g_cur_level = level;
    load_level();
    for (int i = 0; i < MAXPLAYER; i++)
        if (g_pl[i].on) place_player(i);
    game_simulate();
}

void game_solve_setup(void)
{
    const Level *lv = &g_levels[g_cur_level];
    if (lv->has_stand) {                                  /* rooms solved only while someone stands on a given cell (two-player rooms, plate rooms) */
        g_pl[1].on = 1;
        g_pl[1].x = (float)lv->stand_x + 0.5f;
        g_pl[1].z = (float)lv->stand_z + 0.5f;
    }
    if (lv->has_stand2) {                                 /* rooms where two people must stand */
        g_pl[0].on = 1;
        g_pl[0].x = (float)lv->stand2_x + 0.5f;
        g_pl[0].z = (float)lv->stand2_z + 0.5f;
    }
    for (int i = 0; i < lv->nmove; i++) {                 /* rooms solved only after moving objects (boxes onto plates or into beams) */
        const signed char *m = lv->move[i];
        int oi = g_objat[m[1]][m[0]];
        if (!oi || g_objat[m[3]][m[2]]) continue;
        g_objat[m[1]][m[0]] = 0;
        g_obj[oi - 1].x = m[2]; g_obj[oi - 1].z = m[3];
        g_objat[m[3]][m[2]] = oi;
    }
}

/* try every angle of the turnable objects to find a solution (tests and screenshots) */
int game_solve(void)
{
    int idx[SOLVE_MAX_MIRRORS], k = 0;
    for (int i = 0; i < g_nobj && k < SOLVE_MAX_MIRRORS; i++)   /* more mirrors keep their file angle */
        if (g_obj[i].movable && (g_obj[i].kind == O_MIRROR || g_obj[i].kind == O_SPLIT)) idx[k++] = i;
    long total = 1;
    for (int i = 0; i < k; i++) total *= 8;
    for (long c = 0; c < total; c++) {
        long v = c;
        for (int i = 0; i < k; i++) { g_obj[idx[i]].rot = (int)(v % 8); v /= 8; }
        game_simulate();
        if (g_solved) return 1;
    }
    return 0;
}

/* ---------- messages ---------- */

/* Korean particle after an object name: a ("을", "은", "이") if the last syllable has a final consonant, else b ("를", "는", "가") */
static const char *josa(int kind, const char *a, const char *b)
{
    const unsigned char *s = (const unsigned char *)KIND_NAME[kind];
    size_t n = strlen((const char *)s);
    if (n < 3) return a;
    unsigned c = (unsigned)(s[n - 3] & 0x0F) << 12 | (unsigned)(s[n - 2] & 0x3F) << 6 | (unsigned)(s[n - 1] & 0x3F);
    return c >= 0xAC00 && c <= 0xD7A3 && (c - 0xAC00) % 28 == 0 ? b : a;
}

static void vmessage(Player *p, const char *fmt, va_list ap)
{
    vsnprintf(p->msg, sizeof p->msg, fmt, ap);
    p->msg_t = 90;
    p->msg_seq++;
}

void game_message(int p, const char *fmt, ...)
{
    for (int i = 0; i < MAXPLAYER; i++) {
        if (p >= 0 ? i != p : !g_pl[i].on) continue;
        va_list ap;
        va_start(ap, fmt);
        vmessage(&g_pl[i], fmt, ap);
        va_end(ap);
    }
}

/* ---------- player actions ---------- */

static void after_change(void)
{
    for (int i = 0; i < MAXPLAYER; i++) g_pl[i].hint_t = 0;   /* after a change the hint may no longer fit */
    int was = g_solved;
    game_simulate();
    g_moves++;
    g_total_moves++;
    if (g_solved && !was) game_message(-1, "모든 수정이 빛났어요! 문이 열렸습니다 (%d번 조작)", g_moves);
}

/* remember the state before a move; when full, drop the oldest */
static void push_undo(void)
{
    if (s_nundo == MAXUNDO) {
        memmove(s_undo, s_undo + 1, sizeof s_undo[0] * (MAXUNDO - 1));
        s_nundo--;
    }
    Snap *u = &s_undo[s_nundo++];
    for (int i = 0; i < g_nobj; i++) { u->x[i] = g_obj[i].x; u->z[i] = g_obj[i].z; u->rot[i] = g_obj[i].rot; u->held[i] = g_obj[i].held; }
    for (int i = 0; i < MAXPLAYER; i++) u->carry[i] = g_pl[i].carry;
}

/* undo the last move. Refuse if a person now stands where an object would return,
 * or the person who carried it then has left (no one stuck in an object, no object lost) */
static void undo(int p)
{
    if (!s_nundo) { game_message(p, "되돌릴 조작이 없어요"); return; }
    const Snap *u = &s_undo[s_nundo - 1];
    for (int i = 0; i < MAXPLAYER; i++)
        if (u->carry[i] >= 0 && !g_pl[i].on) { game_message(p, "그 물체를 들고 있던 사람이 나가서 되돌릴 수 없어요"); return; }
    for (int i = 0; i < g_nobj; i++) {
        if (u->held[i] || (u->x[i] == g_obj[i].x && u->z[i] == g_obj[i].z && !g_obj[i].held)) continue;
        for (int k = 0; k < MAXPLAYER; k++)
            if (g_pl[k].on && body_on_cell(&g_pl[k], u->x[i], u->z[i])) {
                game_message(p, "%s%s 돌아갈 자리에 누가 서 있어요. 비켜 서면 되돌릴 수 있어요", KIND_NAME[g_obj[i].kind], josa(g_obj[i].kind, "이", "가"));
                return;
            }
    }
    s_nundo--;
    for (int i = 0; i < MAXPLAYER; i++) g_pl[i].hint_t = 0;
    memset(g_objat, 0, sizeof g_objat);
    for (int i = 0; i < g_nobj; i++) {
        Obj *o = &g_obj[i];
        o->x = u->x[i]; o->z = u->z[i]; o->rot = u->rot[i]; o->held = u->held[i];
        if (!o->held) g_objat[o->z][o->x] = i + 1;
    }
    for (int i = 0; i < MAXPLAYER; i++) g_pl[i].carry = u->carry[i];
    int was = g_solved;
    game_simulate();
    if (g_solved && !was) game_message(-1, "모든 수정이 빛났어요! 문이 열렸습니다");
    else game_message(-1, "한 번 되돌렸어요 (더 되돌릴 수 있는 횟수 %d)", s_nundo);
    for (int i = 0; i < MAXPLAYER; i++) if (g_pl[i].on) game_find_target(i);
}

static int cell_free(int x, int z)
{
    return x > 0 && z > 0 && x < RW - 1 && z < RD - 1 && g_blk[1][z][x] == B_AIR && !g_objat[z][x] &&
           g_levels[g_cur_level].map[z][x] != '=';       /* nothing can be placed on an open gate cell */
}

/* with self's body (circle) at (x, z), does it overlap a wall, glass, an object cell or another player */
static int circle_hits(int self, float x, float z)
{
    for (int cz = (int)floorf(z - PR); cz <= (int)floorf(z + PR); cz++)
        for (int cx = (int)floorf(x - PR); cx <= (int)floorf(x + PR); cx++) {
            int solid = cx < 0 || cz < 0 || cx >= RW || cz >= RD || g_blk[1][cz][cx] != B_AIR || g_objat[cz][cx] ||
                        g_blk[0][cz][cx] == B_HATCH;      /* people cannot pass a hatch */
            if (!solid) continue;
            float qx = clampf(x, (float)cx, (float)cx + 1), qz = clampf(z, (float)cz, (float)cz + 1);
            if ((qx - x) * (qx - x) + (qz - z) * (qz - z) < PR * PR) return 1;
        }
    for (int i = 0; i < MAXPLAYER; i++) {
        if (i == self || !g_pl[i].on) continue;
        float dx = g_pl[i].x - x, dz = g_pl[i].z - z;
        if (dx * dx + dz * dz < 4 * PR * PR) return 1;
    }
    return 0;
}

/* aiming: first person uses the centre ray (up to 4.5 cells), top-down the cell right in front */
void game_find_target(int p)
{
    Player *pl = &g_pl[p];
    pl->tobj = -1;
    pl->tx = pl->tz = -1;
    if (pl->fp) {
        float cp = cosf(pl->pitch), dx = sinf(pl->yaw) * cp, dy = sinf(pl->pitch), dz = cosf(pl->yaw) * cp;
        float ox = pl->x, oy = EYE_Y + pl->y, oz = pl->z;
        int x = (int)floorf(ox), y = (int)floorf(oy), z = (int)floorf(oz);
        int sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1, sz = dz > 0 ? 1 : -1;
        float tdx = fabsf(dx) > 1e-6f ? fabsf(1 / dx) : 1e30f, tdy = fabsf(dy) > 1e-6f ? fabsf(1 / dy) : 1e30f,
              tdz = fabsf(dz) > 1e-6f ? fabsf(1 / dz) : 1e30f;
        float tmx = fabsf(dx) > 1e-6f ? (dx > 0 ? (float)x + 1 - ox : ox - (float)x) * tdx : 1e30f;
        float tmy = fabsf(dy) > 1e-6f ? (dy > 0 ? (float)y + 1 - oy : oy - (float)y) * tdy : 1e30f;
        float tmz = fabsf(dz) > 1e-6f ? (dz > 0 ? (float)z + 1 - oz : oz - (float)z) * tdz : 1e30f;
        for (int i = 0; i < 24; i++) {
            int axis, in_hatch = x >= 0 && z >= 0 && x < RW && z < RD && g_blk[0][z][x] == B_HATCH;
            float t;
            if (tmx < tmy && tmx < tmz) { t = tmx; x += sx; tmx += tdx; axis = 0; }
            else if (tmy < tmz) { t = tmy; y += sy; tmy += tdy; axis = 1; }
            else { t = tmz; z += sz; tmz += tdz; axis = 2; }
            if (t > 4.5f || x < 0 || z < 0 || x >= RW || z >= RD || y < 0) return;
            if (in_hatch && axis != 1) return;            /* objects and floor beyond a hatch (the other room) cannot be aimed at */
            if (y >= RH) continue;
            if (y == 1 && g_objat[z][x]) { pl->tobj = g_objat[z][x] - 1; return; }
            int b = g_blk[y][z][x];
            if (b != B_AIR) {
                if ((b == B_FLOOR || b == B_PLATE || b == B_PLATE_ON || b == B_HATCH) && y == 0 && axis == 1 && cell_free(x, z)) { pl->tx = x; pl->tz = z; }
                break;
            }
        }
    } else {
        int x = (int)floorf(pl->x + sinf(pl->yaw) * 0.85f), z = (int)floorf(pl->z + cosf(pl->yaw) * 0.85f);
        if (x < 0 || z < 0 || x >= RW || z >= RD) return;
        if (g_objat[z][x]) pl->tobj = g_objat[z][x] - 1;
        else if (cell_free(x, z)) { pl->tx = x; pl->tz = z; }
    }
    if (pl->tx >= 0)                                      /* nothing can be placed where someone stands */
        for (int i = 0; i < MAXPLAYER; i++)
            if (g_pl[i].on && body_on_cell(&g_pl[i], pl->tx, pl->tz)) { pl->tx = pl->tz = -1; break; }
}

static void take_or_place(int p)
{
    Player *pl = &g_pl[p];
    if (pl->carry >= 0) {
        if (pl->tx < 0) { game_message(p, "빈 바닥을 겨누고 F를 누르면 내려놓아요"); return; }
        push_undo();
        Obj *o = &g_obj[pl->carry];
        o->x = pl->tx; o->z = pl->tz; o->held = 0;
        g_objat[pl->tz][pl->tx] = pl->carry + 1;
        game_message(p, "%s%s 내려놓았어요", KIND_NAME[o->kind], josa(o->kind, "을", "를"));
        pl->carry = -1;
        after_change();
        for (int i = 0; i < MAXPLAYER; i++) if (g_pl[i].on) game_find_target(i);
        return;
    }
    if (pl->tobj < 0) { game_message(p, "물체를 겨누고 F를 누르면 들 수 있어요"); return; }
    Obj *o = &g_obj[pl->tobj];
    if (!o->movable) { game_message(p, "%s%s 고정되어 있어요", KIND_NAME[o->kind], josa(o->kind, "은", "는")); return; }
    push_undo();
    g_objat[o->z][o->x] = 0;
    o->held = 1;
    pl->carry = pl->tobj;
    game_message(p, "%s%s 들었어요. 빈 바닥을 겨누고 F로 내려놓으세요", KIND_NAME[o->kind], josa(o->kind, "을", "를"));
    after_change();
    for (int i = 0; i < MAXPLAYER; i++) if (g_pl[i].on) game_find_target(i);
}

static void rotate(int p, int d)
{
    Player *pl = &g_pl[p];
    Obj *o = pl->tobj >= 0 ? &g_obj[pl->tobj] : pl->carry >= 0 ? &g_obj[pl->carry] : NULL;
    if (!o) { game_message(p, "돌릴 거울을 겨누세요"); return; }
    if (o->kind != O_MIRROR && o->kind != O_SPLIT) { game_message(p, "%s%s 돌릴 수 없어요", KIND_NAME[o->kind], josa(o->kind, "은", "는")); return; }
    push_undo();
    o->rot = (o->rot + d + 8) % 8;
    after_change();
}

/* put the room back in its starting state, everyone on the start cells */
static void restart_room(void)
{
    s_nundo = 0;
    for (int i = 0; i < MAXPLAYER; i++) g_pl[i].hint_t = 0;
    load_level();
    for (int i = 0; i < MAXPLAYER; i++)
        if (g_pl[i].on) place_player(i);
    game_simulate();
    g_moves = 0;
    for (int i = 0; i < MAXPLAYER; i++)
        if (g_pl[i].on) game_find_target(i);
}

/* went through the open door: next room, or the end after the last one */
static void through_door(void)
{
    int rec = g_record ? progress_record(g_levels[g_cur_level].file, g_moves) : 0, moves = g_moves;
    if (g_cur_level + 1 < g_nlevels) {
        g_cur_level++;
        restart_room();
        if (rec == 1) game_message(-1, "방 %d/%d: %s   (지난 방 새 기록: %d번 조작)", g_cur_level + 1, g_nlevels, g_levels[g_cur_level].name, moves);
        else game_message(-1, "방 %d/%d: %s", g_cur_level + 1, g_nlevels, g_levels[g_cur_level].name);
    } else if (!g_cleared) {
        g_cleared = 1;
        game_message(-1, "모든 방을 풀었어요! 빛의 신전을 빠져나왔습니다");
    }
}

/* ---------- hints ---------- */

static void set_hint(int p, int obj, int x, int z, const char *fmt, ...) __attribute__((format(printf, 5, 6)));
static void set_hint(int p, int obj, int x, int z, const char *fmt, ...)
{
    Player *pl = &g_pl[p];
    va_list ap;
    va_start(ap, fmt);
    vmessage(pl, fmt, ap);
    va_end(ap);
    pl->msg_t = pl->hint_t = 240;                         /* 8 seconds */
    pl->hint_obj = obj; pl->hint_x = x; pl->hint_z = z;
}

/* Solve from the starting state (with the level's stand and move lines), compare with now, and name one next step.
 * Everything changed in the world and the players while solving is put back */
static void hint(int p)
{
    static Obj obj[MAXOBJ];
    static int objat[RD][RW];
    static uint8_t blk[RH][RD][RW];
    static Player pl[MAXPLAYER];
    int nobj = g_nobj;
    memcpy(obj, g_obj, sizeof obj);
    memcpy(objat, g_objat, sizeof objat);
    memcpy(blk, g_blk, sizeof blk);
    memcpy(pl, g_pl, sizeof pl);

    load_level();                                         /* solve from the start with nobody in the room (or only on the stand cells) */
    for (int i = 0; i < MAXPLAYER; i++) g_pl[i].on = 0;
    game_solve_setup();
    int ok = game_solve(), sx[MAXOBJ], sz[MAXOBJ], srot[MAXOBJ];
    for (int i = 0; i < g_nobj; i++) { sx[i] = g_obj[i].x; sz[i] = g_obj[i].z; srot[i] = g_obj[i].rot; }

    g_nobj = nobj;
    memcpy(g_obj, obj, sizeof obj);
    memcpy(g_objat, objat, sizeof objat);
    memcpy(g_blk, blk, sizeof blk);
    memcpy(g_pl, pl, sizeof pl);
    game_simulate();

    const Level *lv = &g_levels[g_cur_level];
    Player *me = &g_pl[p];
    if (g_solved) { set_hint(p, -1, -1, -1, "문이 열려 있어요. 문으로 걸어 나가세요"); return; }
    if (!ok) { set_hint(p, -1, -1, -1, "이 방은 힌트를 찾지 못했어요"); return; }
    if (me->carry >= 0) {                                 /* where to put what you carry */
        int c = me->carry;
        set_hint(p, -1, sx[c], sz[c], "들고 있는 %s%s 노란 칸에 내려놓으세요", KIND_NAME[g_obj[c].kind], josa(g_obj[c].kind, "을", "를"));
        return;
    }
    for (int i = 0; i < g_nobj; i++) {                    /* an object in the wrong place (not one someone else carries) */
        const Obj *o = &g_obj[i];
        if (o->held || (o->x == sx[i] && o->z == sz[i])) continue;
        set_hint(p, i, sx[i], sz[i], "테두리 친 %s%s 노란 칸으로 옮기세요", KIND_NAME[o->kind], josa(o->kind, "을", "를"));
        return;
    }
    for (int i = 0; i < g_nobj; i++) {                    /* a mirror at the wrong angle: turn it the short way (8 steps = 180 degrees) */
        const Obj *o = &g_obj[i];
        if (o->held || (o->kind != O_MIRROR && o->kind != O_SPLIT)) continue;
        int d = ((srot[i] - o->rot) % 8 + 8) % 8;
        if (!d) continue;
        if (d > 4) set_hint(p, i, -1, -1, "테두리 친 %s%s Q로 %d번 돌리세요", KIND_NAME[o->kind], josa(o->kind, "을", "를"), 8 - d);
        else set_hint(p, i, -1, -1, "테두리 친 %s%s E로 %d번 돌리세요", KIND_NAME[o->kind], josa(o->kind, "을", "를"), d);
        return;
    }
    for (int k = 0; k < 2; k++) {                         /* rooms where someone must stand to block light or press a plate */
        int has = k ? lv->has_stand2 : lv->has_stand, x = k ? lv->stand2_x : lv->stand_x, z = k ? lv->stand2_z : lv->stand_z;
        if (!has) continue;
        int taken = g_objat[z][x] != 0;
        for (int i = 0; i < MAXPLAYER; i++) taken |= g_pl[i].on && body_on_cell(&g_pl[i], x, z);
        if (taken) continue;
        set_hint(p, -1, x, z, lv->coop ? "한 사람이 노란 칸에 서 있어야 해요" : "노란 칸에 서 있거나 물체를 올려 두세요");
        return;
    }
    set_hint(p, -1, -1, -1, "물체는 제자리예요. 빛을 가리고 있지 않은지 확인해 보세요");
}

void game_action(int p, int act)
{
    if (p < 0 || p >= MAXPLAYER || !g_pl[p].on) return;
    switch (act) {
    case ACT_TAKE: take_or_place(p); break;
    case ACT_ROT_LEFT: rotate(p, -1); break;
    case ACT_ROT_RIGHT: rotate(p, 1); break;
    case ACT_RESET: restart_room(); game_message(-1, "처음 상태로 되돌렸어요"); break;
    case ACT_UNDO: undo(p); break;
    case ACT_HINT: hint(p); break;
    case ACT_JUMP:                                        /* only from the floor; not a puzzle move */
        if (g_pl[p].y <= 0) g_pl[p].vy = JUMP_V;
        break;
    }
}

void game_join(int p)
{
    Player *pl = &g_pl[p];
    *pl = (Player){ .on = 1, .fp = 1, .walk_yaw = cam_yaw, .carry = -1, .tobj = -1, .tx = -1, .tz = -1 };
    place_player(p);
    game_simulate();
    game_find_target(p);
    game_message(-1, "%d번 플레이어가 들어왔어요", p + 1);
}

void game_leave(int p)
{
    Player *pl = &g_pl[p];
    if (!pl->on) return;
    if (pl->carry >= 0) {                                 /* put what they carried on the floor under them or an empty cell around */
        int px = (int)floorf(pl->x), pz = (int)floorf(pl->z);
        for (int k = 0; k < 9; k++) {
            int x = px + k % 3 - 1, z = pz + k / 3 - 1;
            if (cell_free(x, z)) {
                Obj *o = &g_obj[pl->carry];
                o->x = x; o->z = z; o->held = 0;
                g_objat[z][x] = pl->carry + 1;
                break;
            }
        }
    }
    pl->on = 0;
    pl->carry = -1;
    game_simulate();
    game_message(-1, "%d번 플레이어가 나갔어요", p + 1);
}

/* ---------- this terminal's keys ---------- */

void game_view_key(int vk)
{
    Player *me = &g_pl[g_me];
    switch (vk) {
    case VK_TAB: me->fp = !me->fp; game_message(g_me, me->fp ? "1인칭 시점" : "내려다보기 시점 (←→ 카메라 회전)"); break;
    case VK_PLUS: cam_dist = fmaxf(10, cam_dist - 1.5f); break;
    case VK_MINUS: cam_dist = fminf(30, cam_dist + 1.5f); break;
    case VK_LEFT: if (!me->fp) cam_yaw_target -= PI / 4; break;
    case VK_RIGHT: if (!me->fp) cam_yaw_target += PI / 4; break;
    case VK_UP: if (!me->fp) cam_pitch = fmaxf(-1.35f, cam_pitch - 0.08f); break;
    case VK_DOWN: if (!me->fp) cam_pitch = fminf(-0.3f, cam_pitch + 0.08f); break;
    }
}

/* view set by the mouse (mouse-lock helper connected, first person) */
int g_look_local;

void game_mouse_look(float dx, float dy)
{
    Player *me = &g_pl[g_me];
    if (!me->fp) return;
    me->yaw = fmodf(me->yaw + dx, 2 * PI);
    me->pitch = clampf(me->pitch - dy, -1.3f, 0.9f);
}

unsigned game_local_keys(void)
{
    unsigned k = (input_held('W') ? K_W : 0) | (input_held('A') ? K_A : 0) | (input_held('S') ? K_S : 0) | (input_held('D') ? K_D : 0) |
           (input_held(VK_LEFT) ? K_LEFT : 0) | (input_held(VK_RIGHT) ? K_RIGHT : 0) |
           (input_held(VK_UP) ? K_UP : 0) | (input_held(VK_DOWN) ? K_DOWN : 0) | (input_held(VK_SHIFT) ? K_RUN : 0);
    if (bridge_connected()) {                              /* the real key state the helper reads (counts only while the game window is in front) */
        uint32_t b = bridge_keys();
        k |= (b & KB_W ? K_W : 0) | (b & KB_A ? K_A : 0) | (b & KB_S ? K_S : 0) | (b & KB_D ? K_D : 0) |
             (b & KB_LEFT ? K_LEFT : 0) | (b & KB_RIGHT ? K_RIGHT : 0) | (b & KB_UP ? K_UP : 0) | (b & KB_DOWN ? K_DOWN : 0) |
             (b & KB_SHIFT ? K_RUN : 0);
    }
    return k;
}

/* ---------- tick ---------- */

/* walk (sliding along walls), recompute light when a body moves, next room when walking into an open door */
static void player_tick(int p, float dt)
{
    Player *pl = &g_pl[p];
    unsigned k = pl->keys;
    float f = (float)(!!(k & K_W) - !!(k & K_S)), s = (float)(!!(k & K_D) - !!(k & K_A)), vx, vz;
    if (pl->fp) {
        pl->yaw += (float)(!!(k & K_RIGHT) - !!(k & K_LEFT)) * 2.2f * dt;
        pl->pitch = clampf(pl->pitch + (float)(!!(k & K_UP) - !!(k & K_DOWN)) * 1.6f * dt, -1.3f, 0.9f);
        vx = sinf(pl->yaw) * f + cosf(pl->yaw) * s;
        vz = cosf(pl->yaw) * f - sinf(pl->yaw) * s;
    } else {
        vx = sinf(pl->walk_yaw) * f + cosf(pl->walk_yaw) * s;
        vz = cosf(pl->walk_yaw) * f - sinf(pl->walk_yaw) * s;
        if (vx != 0 || vz != 0) pl->yaw = atan2f(vx, vz);
    }
    if (pl->y > 0 || pl->vy > 0) {                        /* jump: simple ballistic arc, land on the floor */
        pl->vy -= GRAVITY * dt;
        pl->y += pl->vy * dt;
        if (pl->y <= 0) pl->y = pl->vy = 0;
    }
    float l = sqrtf(vx * vx + vz * vz);
    if (l > 0) {
        int moved = 0;
        float speed = (k & K_RUN) ? RUN_SPEED : WALK_SPEED;
        vx = vx / l * speed * dt;
        vz = vz / l * speed * dt;
        if (!circle_hits(p, pl->x + vx, pl->z)) { pl->x += vx; moved = 1; }
        if (!circle_hits(p, pl->x, pl->z + vz)) { pl->z += vz; moved = 1; }
        if (moved) {
            int was = g_solved;
            game_simulate();                              /* the body may block a beam */
            if (g_solved && !was) game_message(-1, "모든 수정이 빛났어요! 문이 열렸습니다");
            if (g_solved && world_is_door((int)floorf(pl->x), (int)floorf(pl->z))) through_door();
        }
    }
}

void game_tick(float dt)
{
    Player *me = &g_pl[g_me];
    me->keys = game_local_keys();
    me->walk_yaw = cam_yaw;
    for (int i = 0; i < MAXPLAYER; i++)
        if (g_pl[i].on) { player_tick(i, dt); game_find_target(i); }
    cam_yaw += (cam_yaw_target - cam_yaw) * 0.25f;        /* the camera turns smoothly */
    for (int i = 0; i < MAXPLAYER; i++)
        if (g_pl[i].msg_t > 0) g_pl[i].msg_t--;
    for (int i = 0; i < MAXPLAYER; i++)
        if (g_pl[i].hint_t > 0) g_pl[i].hint_t--;
    guide_tick(dt);
    if (!g_cleared) g_ticks++;
}

void game_local_tick(void)
{
    cam_yaw += (cam_yaw_target - cam_yaw) * 0.25f;
    if (g_pl[g_me].msg_t > 0) g_pl[g_me].msg_t--;
    if (g_pl[g_me].hint_t > 0) g_pl[g_me].hint_t--;
    guide_tick(1.0f / 30);                                /* the guide follows the hint the host sent */
}

/* ---------- renderer and tests ---------- */

void game_view(View *v)
{
    static Body bodies[MAXPLAYER + 1];
    const Player *me = &g_pl[g_me];
    int n = 0;
    for (int i = 0; i < MAXPLAYER; i++)                   /* in first person your own body is not drawn */
        if (g_pl[i].on && !(i == g_me && me->fp)) bodies[n++] = (Body){ g_pl[i].x, g_pl[i].z, i, g_pl[i].y };
    n += guide_body(&bodies[n]);                          /* the hint guide, if shown */
    int tx = me->tx, tz = me->tz, sel = me->tobj;
    if (me->hint_t > 0 && (me->hint_t / 10) % 3 != 0) {   /* hint: blink the object outline and the cell mark */
        if (me->hint_obj >= 0) sel = me->hint_obj;
        if (me->hint_x >= 0) { tx = me->hint_x; tz = me->hint_z; }
    }
    *v = (View){ me->fp, me->x, me->z, me->yaw, me->pitch, cam_yaw, cam_pitch, cam_dist, tx, tz, sel,
                 g_seg, g_nseg, bodies, n, 1, me->y };
}

void game_print_state(const char *title)
{
    int total, lit = optics_count_lit(&total);
    printf("%s: %d segments, crystals %d/%d, door %s\n", title, g_nseg, lit, total, g_solved ? "open" : "closed");
    for (int i = 0; i < g_nobj; i++) {
        const Obj *o = &g_obj[i];
        if (o->kind == O_CRYSTAL)
            printf("  crystal (%d,%d) wants %.0f%.0f%.0f got %.2f %.2f %.2f -> %s\n", o->x, o->z, o->col[0], o->col[1], o->col[2],
                   o->got[0], o->got[1], o->got[2], o->lit ? "LIT" : "dark");
        else if (o->kind == O_SENSOR)
            printf("  sensor (%d,%d) got %.2f -> %s\n", o->x, o->z, o->got[0] + o->got[1] + o->got[2], o->lit ? "ON" : "off");
        else if (o->movable)
            printf("  %s (%d,%d) rot %d\n", o->kind == O_MIRROR ? "mirror" : o->kind == O_SPLIT ? "splitter" : o->kind == O_PRISM ? "prism" :
                   o->kind == O_BOX ? "box" : o->kind == O_LIFT ? "lift" : "filter", o->x, o->z, o->rot);
    }
}
