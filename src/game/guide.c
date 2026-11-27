/*
 * guide.c - the hint guide: a glowing figure that walks to what the current hint points at.
 */
#include "game/guide.h"

#include <math.h>
#include <stdlib.h>
#include "game/game.h"
#include "world/world.h"

/*
 * A glowing guide figure that walks from you to whatever the hint points at
 * (the object to take or turn, the cell to put something on or stand on) and
 * floats there while the hint lasts. It lives only on this terminal: it is
 * driven by the hint fields every player already receives, does not block
 * light, does not collide, and is never sent over the network.
 */
#define GUIDE_SPEED 4.0f
static struct {
    int on, n, i, level, tx, tz;         /* tx, tz: the cell it points at */
    float x, z, yaw, bob;
    signed char px[RW * RD], pz[RW * RD];
    int prev_hint_t;
} s_guide;

static int guide_walkable(int x, int z)
{
    if ((unsigned)x >= RW || (unsigned)z >= RD || g_objat[z][x]) return 0;
    int f = g_blk[0][z][x];
    return (f == B_FLOOR || f == B_PLATE || f == B_PLATE_ON) && g_blk[1][z][x] == B_AIR;
}

/* goal cells: next to the hinted object, or the hinted cell itself (or next to it if it is not walkable) */
static int guide_goal(int x, int z)
{
    if (!guide_walkable(x, z)) return 0;
    if (x == s_guide.tx && z == s_guide.tz) return 1;
    int dx = abs(x - s_guide.tx), dz = abs(z - s_guide.tz);
    return dx <= 1 && dz <= 1 && !guide_walkable(s_guide.tx, s_guide.tz);
}

static void guide_start(const Player *me)
{
    s_guide.on = 0;
    if (me->hint_obj >= 0 && me->hint_obj < g_nobj && !g_obj[me->hint_obj].held) { s_guide.tx = g_obj[me->hint_obj].x; s_guide.tz = g_obj[me->hint_obj].z; }
    else if (me->hint_x >= 0) { s_guide.tx = me->hint_x; s_guide.tz = me->hint_z; }
    else return;                                          /* a text-only hint: nothing to point at */
    /* breadth-first search over floor cells from the player's cell */
    static short prev[RD][RW];
    int qx[RW * RD], qz[RW * RD], head = 0, tail = 0, sx = (int)floorf(me->x), sz = (int)floorf(me->z), gx = -1, gz = -1;
    for (int z = 0; z < RD; z++)
        for (int x = 0; x < RW; x++) prev[z][x] = -1;
    if ((unsigned)sx < RW && (unsigned)sz < RD) { prev[sz][sx] = (short)(sz * RW + sx); qx[tail] = sx; qz[tail++] = sz; }
    while (head < tail && gx < 0) {
        int x = qx[head], z = qz[head++];
        if (guide_goal(x, z)) { gx = x; gz = z; break; }
        static const int D[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
        for (int k = 0; k < 4; k++) {
            int nx = x + D[k][0], nz = z + D[k][1];
            if ((unsigned)nx >= RW || (unsigned)nz >= RD || prev[nz][nx] >= 0 || !guide_walkable(nx, nz)) continue;
            prev[nz][nx] = (short)(z * RW + x);
            qx[tail] = nx; qz[tail++] = nz;
        }
    }
    s_guide.n = s_guide.i = 0;
    if (gx >= 0) {                                        /* walk the path back from the goal */
        int len = 0, c = gz * RW + gx;
        while (c != sz * RW + sx && len < RW * RD) {
            s_guide.px[len] = (signed char)(c % RW);
            s_guide.pz[len++] = (signed char)(c / RW);
            c = prev[c / RW][c % RW];
        }
        for (int i = 0; i < len / 2; i++) {               /* reverse: start -> goal */
            signed char t = s_guide.px[i]; s_guide.px[i] = s_guide.px[len - 1 - i]; s_guide.px[len - 1 - i] = t;
            t = s_guide.pz[i]; s_guide.pz[i] = s_guide.pz[len - 1 - i]; s_guide.pz[len - 1 - i] = t;
        }
        s_guide.n = len;
        s_guide.x = me->x; s_guide.z = me->z;
    } else {                                              /* unreachable (the other half of a split room): appear next to it */
        for (int z = s_guide.tz - 1; z <= s_guide.tz + 1 && gx < 0; z++)
            for (int x = s_guide.tx - 1; x <= s_guide.tx + 1 && gx < 0; x++)
                if (guide_goal(x, z)) { gx = x; gz = z; }
        if (gx < 0) return;
        s_guide.x = (float)gx + 0.5f; s_guide.z = (float)gz + 0.5f;
    }
    s_guide.yaw = me->yaw;
    s_guide.level = g_cur_level;
    s_guide.on = 1;
}

void guide_tick(float dt)
{
    const Player *me = &g_pl[g_me];
    if (me->hint_t > s_guide.prev_hint_t + 1) guide_start(me);   /* a new hint was just given */
    s_guide.prev_hint_t = me->hint_t;
    if (!s_guide.on) return;
    if (me->hint_t <= 0 || s_guide.level != g_cur_level) { s_guide.on = 0; return; }
    s_guide.bob += dt;
    float step = GUIDE_SPEED * dt;
    while (step > 0 && s_guide.i < s_guide.n) {           /* follow the path through cell centres */
        float cx = (float)s_guide.px[s_guide.i] + 0.5f - s_guide.x, cz = (float)s_guide.pz[s_guide.i] + 0.5f - s_guide.z;
        float d = sqrtf(cx * cx + cz * cz);
        if (d <= step) { s_guide.x += cx; s_guide.z += cz; step -= d; s_guide.i++; continue; }
        s_guide.x += cx / d * step; s_guide.z += cz / d * step;
        s_guide.yaw = atan2f(cx, cz);
        step = 0;
    }
    if (s_guide.i >= s_guide.n) {                         /* arrived: turn toward what the hint is about */
        float fx = (float)s_guide.tx + 0.5f - s_guide.x, fz = (float)s_guide.tz + 0.5f - s_guide.z;
        if (fx * fx + fz * fz > 0.01f) s_guide.yaw = atan2f(fx, fz);
    }
}

int guide_body(Body *b)
{
    if (!s_guide.on) return 0;
    *b = (Body){ s_guide.x, s_guide.z, BODY_GUIDE, 0.15f + 0.07f * sinf(s_guide.bob * 4) };   /* floats and bobs a little */
    return 1;
}
