#include "optics/optics.h"

#include <math.h>
#include <string.h>
#include "world/world.h"

/* ---------- light simulation ---------- */

Seg g_seg[MAXSEG];
int g_nseg;

/* distance at which light counts as touching an object (matched to the shapes the renderer draws) */
#define MIRROR_HALF 0.36f   /* half width of a mirror panel (0.38 with its frame) */
#define PRISM_REACH 0.24f   /* light must pass this close to a prism's centre to hit it */
#define OBJ_REACH   0.30f   /* emitters, crystals, filters, sensors, portals */
#define BOX_HALF    0.30f   /* half width of a box: light stops at its surface */
#define MAX_HOP     8       /* how many portals one beam may pass (light bouncing between two portals) */

/* the other portal of the pair (NULL if none) */
static const Obj *other_portal(const Obj *o)
{
    for (int i = 0; i < g_nobj; i++)
        if (g_obj[i].kind == O_PORTAL && &g_obj[i] != o && !g_obj[i].held) return &g_obj[i];
    return NULL;
}

static Body g_body[8];                 /* bodies that block light in this pass */
static int g_nbody;
static int s_layer;                    /* height of the beam being traced: 0 low, 1 high (changed by light lifts) */

static int beam_blocked(int x, int z)
{
    if (x < 0 || z < 0 || x >= RW || z >= RD) return 1;
    int b = g_blk[1 + s_layer][z][x];                    /* the voxel row the beam runs through */
    return b == B_WALL || b == B_DOOR || b == B_GATE;
}
static int is_glass(int x, int z) { return x >= 0 && z >= 0 && x < RW && z < RD && g_blk[1 + s_layer][z][x] == B_GLASS; }

static void add_seg(float x0, float z0, float x1, float z1, float r, float g, float b)
{
    if (g_nseg < MAXSEG && (fabsf(x1 - x0) + fabsf(z1 - z0)) > 1e-4f) g_seg[g_nseg++] = (Seg){ x0, z0, x1, z1, r, g, b, s_layer };
}

static void rot2(float *dx, float *dz, float a)
{
    float c = cosf(a), s = sinf(a), x = *dx, z = *dz;
    *dx = x * c - z * s;
    *dz = x * s + z * c;
}

/* light entered a cell holding one of the newer devices. st = { x, z, sx, sz }: current point and the start of this segment, cell = current cell.
 * 0: misses (keep going)  1: stops here  2: teleported (st and cell moved to the paired portal)
 * Kept apart from beam() so the order of its float math stays the same (results must be bit-identical under -ffast-math) */
static __attribute__((noinline)) int new_device(Obj *o, float *st, int *cell, float dx, float dz, float r, float g, float b, int *hops)
{
    float x = st[0], z = st[1], ccx = (float)cell[0] + 0.5f, ccz = (float)cell[1] + 0.5f, qx, qz;
    if (o->kind == O_BOX) {                               /* box: first point on its square outline (slab method) */
        float lo = 0, hi = 1e30f, p[2] = { x, z }, d[2] = { dx, dz }, c[2] = { ccx, ccz };
        for (int k = 0; k < 2; k++) {
            if (fabsf(d[k]) < 1e-6f) {
                if (fabsf(p[k] - c[k]) > BOX_HALF) return 0;
                continue;
            }
            float t1 = (c[k] - BOX_HALF - p[k]) / d[k], t2 = (c[k] + BOX_HALF - p[k]) / d[k];
            lo = fmaxf(lo, fminf(t1, t2));
            hi = fminf(hi, fmaxf(t1, t2));
        }
        if (lo > hi) return 0;                            /* misses the box corner */
        add_seg(st[2], st[3], x + dx * lo, z + dz * lo, r, g, b);
        return 1;
    }
    float tq = fmaxf((ccx - x) * dx + (ccz - z) * dz, 0);   /* point closest to the cell centre */
    qx = x + dx * tq; qz = z + dz * tq;
    if ((qx - ccx) * (qx - ccx) + (qz - ccz) * (qz - ccz) > OBJ_REACH * OBJ_REACH) return 0;
    add_seg(st[2], st[3], qx, qz, r, g, b);
    if (o->kind == O_LIFT) {                              /* light lift: same point and direction, other height */
        o->got[0] += r; o->got[1] += g; o->got[2] += b;   /* the renderer lights its core with this */
        if (++*hops > MAX_HOP) return 1;
        s_layer ^= 1;
        st[0] = st[2] = qx; st[1] = st[3] = qz;
        return 2;
    }
    if (o->kind == O_SENSOR) {
        o->got[0] += r; o->got[1] += g; o->got[2] += b;
        return 1;
    }
    const Obj *to = other_portal(o);                      /* portal: continue from the same relative point of the paired portal, same direction */
    if (!to || ++*hops > MAX_HOP) return 1;
    st[0] = st[2] = (float)to->x + 0.5f + (qx - ccx);
    st[1] = st[3] = (float)to->z + 0.5f + (qz - ccz);
    cell[0] = to->x; cell[1] = to->z;
    return 2;
}

/* follow one beam cell by cell; recurse only where it splits */
static void beam(float x, float z, float dx, float dz, float r, float g, float b, int depth)
{
    if (depth > 28 || r + g + b < 0.05f || g_nseg >= MAXSEG) return;
    float sx = x, sz = z;
    int cx = (int)floorf(x), cz = (int)floorf(z);
    float nm = is_glass(cx, cz) ? 1.5f : 1.0f;
    int hops = 0;
    for (int it = 0; it < 400; it++) {
        float tx = dx > 1e-6f ? ((float)cx + 1 - x) / dx : dx < -1e-6f ? ((float)cx - x) / dx : 1e30f;
        float tz = dz > 1e-6f ? ((float)cz + 1 - z) / dz : dz < -1e-6f ? ((float)cz - z) / dz : 1e30f;
        /* a beam passing exactly through a cell corner (a diagonal beam) goes straight to the diagonal cell
         * instead of through a side cell, so it never clips an object or glass in a side cell at the corner */
        int corner = fabsf(tx - tz) < 1e-4f && tx < 1e29f;
        int axis = corner ? 2 : tx < tz ? 0 : 1;
        float t = axis == 1 ? tz : tx;
        float bx = x + dx * t, bz = z + dz * t;
        int sxs = dx > 0 ? 1 : -1, szs = dz > 0 ? 1 : -1;
        int nx = cx + (axis != 1 ? sxs : 0), nz = cz + (axis != 0 ? szs : 0);
        if (corner) { bx = (float)(dx > 0 ? cx + 1 : cx); bz = (float)(dz > 0 ? cz + 1 : cz); }
        if (!s_layer) {                                   /* blocked by a player body (circle of radius PR): cut at the nearest body; high beams pass over heads */
            float hit = -1;
            for (int i = 0; i < g_nbody; i++) {
                float fx = x - g_body[i].x, fz = z - g_body[i].z, bb = fx * dx + fz * dz, cc = fx * fx + fz * fz - PR * PR, disc = bb * bb - cc;
                if (cc > 0 && disc > 0) {
                    float th = -bb - sqrtf(disc);
                    if (th >= 0 && th <= t && (hit < 0 || th < hit)) hit = th;
                }
            }
            if (hit >= 0) { add_seg(sx, sz, x + dx * hit, z + dz * hit, r, g, b); return; }
        }
        if (beam_blocked(nx, nz) || (corner && beam_blocked(cx + sxs, cz) && beam_blocked(cx, cz + szs))) {
            add_seg(sx, sz, bx, bz, r, g, b);             /* a wall, or the gap between two wall corners */
            return;
        }

        float n2 = is_glass(nx, nz) ? 1.5f : 1.0f;
        if (n2 != nm) {                                   /* glass boundary: Snell's law, total internal reflection past the critical angle */
            float ux = axis != 1 ? (dx > 0 ? -1.f : 1.f) : 0, uz = axis != 0 ? (dz > 0 ? -1.f : 1.f) : 0;
            if (corner) { ux *= 0.70710678f; uz *= 0.70710678f; }   /* entering through a corner counts as a diagonal face */
            float eta = nm / n2, cosi = -(dx * ux + dz * uz), k = 1 - eta * eta * (1 - cosi * cosi);
            add_seg(sx, sz, bx, bz, r, g, b);
            sx = bx; sz = bz;
            if (k < 0) {
                dx += 2 * cosi * ux; dz += 2 * cosi * uz;
                x = bx - ux * 1e-4f; z = bz - uz * 1e-4f;
                continue;
            }
            float ndx = eta * dx + (eta * cosi - sqrtf(k)) * ux, ndz = eta * dz + (eta * cosi - sqrtf(k)) * uz;
            float l = sqrtf(ndx * ndx + ndz * ndz);
            dx = ndx / l; dz = ndz / l;
            nm = n2;
            r *= 0.96f; g *= 0.96f; b *= 0.96f;
        }
        x = bx; z = bz; cx = nx; cz = nz;

        int oi = g_objat[cz][cx];
        if (!oi) continue;
        Obj *o = &g_obj[oi - 1];
        if (s_layer && o->kind != O_LIFT) continue;      /* high beams pass over every object but lifts */
        if (o->kind >= O_SENSOR) {                        /* sensors, portals, boxes, lifts */
            float st[4] = { x, z, sx, sz };
            int cell[2] = { cx, cz }, k = new_device(o, st, cell, dx, dz, r, g, b, &hops);
            if (k == 1) return;
            if (k == 2) { x = st[0]; z = st[1]; sx = st[2]; sz = st[3]; cx = cell[0]; cz = cell[1]; nm = 1.0f; }
            continue;
        }
        /* react only where the light meets the object's real shape:
         * for mirrors and half mirrors the point on the mirror plane (through the cell centre), otherwise the point closest to the cell centre */
        float ccx = (float)cx + 0.5f, ccz = (float)cz + 0.5f;
        float tq = (ccx - x) * dx + (ccz - z) * dz;
        if (tq < 0) tq = 0;
        float qx = x + dx * tq, qz = z + dz * tq;
        float miss = sqrtf((qx - ccx) * (qx - ccx) + (qz - ccz) * (qz - ccz));   /* how far the light passes from the centre */
        if (o->kind == O_MIRROR || o->kind == O_SPLIT) {
            float a = (float)o->rot * PI / 8, mx = cosf(a), mz = sinf(a), dn = dx * -mz + dz * mx;
            if (fabsf(dn) < 1e-3f) continue;              /* parallel to the mirror: passes by */
            float tm = ((ccx - x) * -mz + (ccz - z) * mx) / dn;
            float px = x + dx * tm, pz = z + dz * tm;
            float ex = dx > 1e-6f ? ((float)cx + 1 - x) / dx : dx < -1e-6f ? ((float)cx - x) / dx : 1e30f;
            float ez = dz > 1e-6f ? ((float)cz + 1 - z) / dz : dz < -1e-6f ? ((float)cz - z) / dz : 1e30f;
            if (tm < 0 || tm > fminf(ex, ez) || fabsf((px - ccx) * mx + (pz - ccz) * mz) > MIRROR_HALF) continue;   /* misses the panel */
            qx = px; qz = pz;
        } else if (miss > (o->kind == O_PRISM ? PRISM_REACH : OBJ_REACH)) {
            continue;                                     /* grazes past the object */
        }
        switch (o->kind) {
        case O_EMIT:
            add_seg(sx, sz, qx, qz, r, g, b);
            return;
        case O_CRYSTAL:
            add_seg(sx, sz, qx, qz, r, g, b);
            o->got[0] += r; o->got[1] += g; o->got[2] += b;
            return;
        case O_FILTER: {
            add_seg(sx, sz, qx, qz, r, g, b);
            r *= o->col[0]; g *= o->col[1]; b *= o->col[2];
            if (r + g + b < 0.05f) return;
            sx = x = qx; sz = z = qz;
            break;
        }
        case O_MIRROR:
        case O_SPLIT: {
            float a = (float)o->rot * PI / 8, nnx = -sinf(a), nnz = cosf(a), dn = dx * nnx + dz * nnz;
            add_seg(sx, sz, qx, qz, r, g, b);
            float rdx = dx - 2 * dn * nnx, rdz = dz - 2 * dn * nnz;
            if (o->kind == O_SPLIT) {                     /* half reflected, half passes through */
                int l = s_layer;
                beam(qx, qz, rdx, rdz, r * 0.5f, g * 0.5f, b * 0.5f, depth + 1);
                s_layer = l;
                r *= 0.5f; g *= 0.5f; b *= 0.5f;
            } else {
                dx = rdx; dz = rdz;
            }
            sx = x = qx; sz = z = qz;
            break;
        }
        case O_PRISM: {                                   /* dispersion: red bends left, blue bends right */
            add_seg(sx, sz, qx, qz, r, g, b);
            int l = s_layer;                              /* split beams start at this height; restore it after each */
            if (r > 0.02f) { float ex = dx, ez = dz; rot2(&ex, &ez, PI / 4); beam(qx, qz, ex, ez, r, 0, 0, depth + 1); s_layer = l; }
            if (b > 0.02f) { float ex = dx, ez = dz; rot2(&ex, &ez, -PI / 4); beam(qx, qz, ex, ez, 0, 0, b, depth + 1); s_layer = l; }
            if (g <= 0.02f) return;
            r = b = 0;
            sx = x = qx; sz = z = qz;
            break;
        }
        }
    }
    add_seg(sx, sz, x, z, r, g, b);
}

static int crystal_ok(const Obj *o)
{
    for (int k = 0; k < 3; k++) {
        if (o->col[k] > 0 && o->got[k] < 0.3f) return 0;
        if (o->col[k] == 0 && o->got[k] > 0.15f) return 0;
    }
    return 1;
}

int optics_simulate(const Body *bodies, int nbody)
{
    g_nbody = nbody < 0 ? 0 : nbody > 8 ? 8 : nbody;
    for (int i = 0; i < g_nbody; i++) g_body[i] = bodies[i];
    g_nseg = 0;
    for (int i = 0; i < g_nobj; i++) memset(g_obj[i].got, 0, sizeof g_obj[i].got);
    for (int i = 0; i < g_nobj; i++)
        if (g_obj[i].kind == O_EMIT) {
            Obj *o = &g_obj[i];
            s_layer = 0;                                  /* emitters shoot low beams */
            float dx = cosf((float)o->rot * PI / 4), dz = sinf((float)o->rot * PI / 4);
            beam((float)o->x + 0.5f + dx * 0.3f, (float)o->z + 0.5f + dz * 0.3f, dx, dz, o->col[0], o->col[1], o->col[2], 0);
        }
    int all = 1, n = 0;
    for (int i = 0; i < g_nobj; i++)
        if (g_obj[i].kind == O_SENSOR) g_obj[i].lit = g_obj[i].got[0] + g_obj[i].got[1] + g_obj[i].got[2] > 0.3f;
    for (int i = 0; i < g_nobj; i++)
        if (g_obj[i].kind == O_CRYSTAL) {
            g_obj[i].lit = crystal_ok(&g_obj[i]);
            all &= g_obj[i].lit;
            n++;
        }
    return n > 0 && all;
}

int optics_count_lit(int *total)
{
    int n = 0, t = 0;
    for (int i = 0; i < g_nobj; i++)
        if (g_obj[i].kind == O_CRYSTAL) { t++; n += g_obj[i].lit; }
    *total = t;
    return n;
}
