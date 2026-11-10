#include "optics/optics.h"

#include <math.h>
#include <string.h>
#include "world/world.h"

/* ---------- 빛 시뮬레이션 ---------- */

Seg g_seg[MAXSEG];
int g_nseg;

static Body g_body[8];                 /* 이번 계산에서 빛을 가리는 몸들 */
static int g_nbody;

static int beam_blocked(int x, int z)
{
    if (x < 0 || z < 0 || x >= RW || z >= RD) return 1;
    int b = g_blk[1][z][x];
    return b == B_WALL || b == B_DOOR;
}
static int is_glass(int x, int z) { return x >= 0 && z >= 0 && x < RW && z < RD && g_blk[1][z][x] == B_GLASS; }

static void add_seg(float x0, float z0, float x1, float z1, float r, float g, float b)
{
    if (g_nseg < MAXSEG && (fabsf(x1 - x0) + fabsf(z1 - z0)) > 1e-4f) g_seg[g_nseg++] = (Seg){ x0, z0, x1, z1, r, g, b };
}

static void rot2(float *dx, float *dz, float a)
{
    float c = cosf(a), s = sinf(a), x = *dx, z = *dz;
    *dx = x * c - z * s;
    *dz = x * s + z * c;
}

/* 빛줄기 하나를 칸 단위로 따라간다. 갈라지는 곳에서만 재귀 */
static void beam(float x, float z, float dx, float dz, float r, float g, float b, int depth)
{
    if (depth > 28 || r + g + b < 0.05f || g_nseg >= MAXSEG) return;
    float sx = x, sz = z;
    int cx = (int)floorf(x), cz = (int)floorf(z);
    float nm = is_glass(cx, cz) ? 1.5f : 1.0f;
    for (int it = 0; it < 400; it++) {
        float tx = dx > 1e-6f ? ((float)cx + 1 - x) / dx : dx < -1e-6f ? ((float)cx - x) / dx : 1e30f;
        float tz = dz > 1e-6f ? ((float)cz + 1 - z) / dz : dz < -1e-6f ? ((float)cz - z) / dz : 1e30f;
        int axis = tx < tz ? 0 : 1;
        float t = axis ? tz : tx;
        float bx = x + dx * t, bz = z + dz * t;
        int nx = cx + (axis == 0 ? (dx > 0 ? 1 : -1) : 0), nz = cz + (axis == 1 ? (dz > 0 ? 1 : -1) : 0);
        {                                                 /* 플레이어 몸(반지름 PR인 원)에 가려짐: 가장 가까운 몸에서 끊긴다 */
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
        if (beam_blocked(nx, nz)) { add_seg(sx, sz, bx, bz, r, g, b); return; }

        float n2 = is_glass(nx, nz) ? 1.5f : 1.0f;
        if (n2 != nm) {                                   /* 유리 경계: 스넬 법칙, 임계각을 넘으면 전반사 */
            float ux = axis == 0 ? (dx > 0 ? -1.f : 1.f) : 0, uz = axis == 1 ? (dz > 0 ? -1.f : 1.f) : 0;
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
        float tq = ((float)cx + 0.5f - x) * dx + ((float)cz + 0.5f - z) * dz;   /* 칸 가운데에 가장 가까운 점에서 반응 */
        if (tq < 0) tq = 0;
        float qx = x + dx * tq, qz = z + dz * tq;
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
            if (fabsf(dn) < 1e-3f) break;                /* 거울면과 나란하면 그냥 지나감 */
            add_seg(sx, sz, qx, qz, r, g, b);
            float rdx = dx - 2 * dn * nnx, rdz = dz - 2 * dn * nnz;
            if (o->kind == O_SPLIT) {                     /* 반은 반사, 반은 통과 */
                beam(qx, qz, rdx, rdz, r * 0.5f, g * 0.5f, b * 0.5f, depth + 1);
                r *= 0.5f; g *= 0.5f; b *= 0.5f;
            } else {
                dx = rdx; dz = rdz;
            }
            sx = x = qx; sz = z = qz;
            break;
        }
        case O_PRISM: {                                   /* 분광: 빨강은 왼쪽으로, 파랑은 오른쪽으로 꺾인다 */
            add_seg(sx, sz, qx, qz, r, g, b);
            if (r > 0.02f) { float ex = dx, ez = dz; rot2(&ex, &ez, PI / 4); beam(qx, qz, ex, ez, r, 0, 0, depth + 1); }
            if (b > 0.02f) { float ex = dx, ez = dz; rot2(&ex, &ez, -PI / 4); beam(qx, qz, ex, ez, 0, 0, b, depth + 1); }
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
            float dx = cosf((float)o->rot * PI / 4), dz = sinf((float)o->rot * PI / 4);
            beam((float)o->x + 0.5f + dx * 0.3f, (float)o->z + 0.5f + dz * 0.3f, dx, dz, o->col[0], o->col[1], o->col[2], 0);
        }
    int all = 1, n = 0;
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
