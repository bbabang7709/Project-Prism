/*
 * PRISM - 레이저 광선 퍼즐 프로토타입 (방 하나)
 *
 * 빛 시뮬레이션: 거울 반사, 반투명 거울 분할, 프리즘 분광, 유리 굴절(스넬 법칙, 전반사),
 *               색 필터, 수정에서 빛 더하기. 물체가 바뀔 때만 다시 계산한다.
 * 렌더러: 방을 바깥에서 비스듬히 내려다보는 카메라로 복셀 레이캐스팅.
 *         물체는 칸 안에서 거리 함수(SDF)로 모양을 그리고, 빛줄기는 화면 광선과의 거리로 빛나게 더한다.
 *         윤곽선과 선택 테두리는 깊이·물체 번호 버퍼로 후처리한다.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "common/config.h"
#include "common/mathutil.h"
#include "common/types.h"
#include "platform/terminal.h"
#include "world/world.h"

/* 퍼즐 상태: 모든 수정이 켜졌는가 */
static int g_solved;

/* 플레이어: 방 안을 걸어 다니며 물체를 들고, 놓고, 돌린다. 몸이 빛을 가린다 */
static float g_px = 2.5f, g_pz = 8.5f, g_pyaw = PI / 2, g_ppitch = -0.35f;
static int g_fp = 1;           /* 1: 1인칭, 0: 내려다보기 */
static int g_carry = -1;       /* 들고 있는 물체 */
static int g_tobj = -1, g_tx = -1, g_tz = -1;   /* 겨누는 물체, 놓을 수 있는 칸 */

/* ---------- 빛 시뮬레이션 ---------- */

static Seg g_seg[MAXSEG];
static int g_nseg;

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
        {
            float fx = x - g_px, fz = z - g_pz, bb = fx * dx + fz * dz, cc = fx * fx + fz * fz - PR * PR, disc = bb * bb - cc;
            if (cc > 0 && disc > 0) {
                float th = -bb - sqrtf(disc);
                if (th >= 0 && th <= t) { add_seg(sx, sz, x + dx * th, z + dz * th, r, g, b); return; }
            }
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

static void simulate(void)
{
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
    g_solved = n > 0 && all;
    world_set_door(g_solved);
}

static int count_lit(int *total)
{
    int n = 0, t = 0;
    for (int i = 0; i < g_nobj; i++)
        if (g_obj[i].kind == O_CRYSTAL) { t++; n += g_obj[i].lit; }
    *total = t;
    return n;
}

/* 회전 가능한 물체의 모든 각도를 시험해 푸는 방법을 찾는다 (시험과 스크린샷용) */
static int solve(void)
{
    int idx[8], k = 0;
    for (int i = 0; i < g_nobj && k < 8; i++)
        if (g_obj[i].movable && (g_obj[i].kind == O_MIRROR || g_obj[i].kind == O_SPLIT)) idx[k++] = i;
    long total = 1;
    for (int i = 0; i < k; i++) total *= 8;
    for (long c = 0; c < total; c++) {
        long v = c;
        for (int i = 0; i < k; i++) { g_obj[idx[i]].rot = (int)(v % 8); v /= 8; }
        simulate();
        if (g_solved) return 1;
    }
    return 0;
}

/* ---------- 렌더러 ---------- */

typedef struct {
    float ox, oy, oz, fx, fy, fz, rx, ry, rz, ux, uy, uz, tanh;
    float nearx, nearz;       /* 카메라 쪽 방향 (앞 벽 잘라내기) */
    int W, H, frame, fp, tx, tz;
} Frame;
static Frame g_fr;
static uint32_t g_pix[MAX_H * 2][MAX_W];
static float g_depth[MAX_H * 2][MAX_W];
static uint16_t g_id[MAX_H * 2][MAX_W];

typedef struct { float r, g, b, T; } Acc;
static inline void layer(Acc *a, float r, float g, float b, float alpha)
{
    float k = a->T * alpha;
    a->r += k * r; a->g += k * g; a->b += k * b;
    a->T *= 1 - alpha;
}

static inline float sd_box(float x, float y, float z, float hx, float hy, float hz)
{
    float qx = fabsf(x) - hx, qy = fabsf(y) - hy, qz = fabsf(z) - hz;
    float ox = fmaxf(qx, 0), oy = fmaxf(qy, 0), oz = fmaxf(qz, 0);
    return sqrtf(ox * ox + oy * oy + oz * oz) + fminf(fmaxf(qx, fmaxf(qy, qz)), 0);
}

/* 물체 모양: 칸 가운데 바닥 기준 좌표 (x, y, z). mat에 재질 번호 */
enum { M_BASE, M_SILVER, M_FRAME, M_GLASSY, M_PRISM, M_FILTER, M_BODY, M_NOZZLE, M_GEM };

static float obj_sdf(const Obj *o, float x, float y, float z, int *mat)
{
    float d = sd_box(x, y - 0.05f, z, 0.36f, 0.05f, 0.36f), e;
    *mat = M_BASE;
    switch (o->kind) {
    case O_MIRROR:
    case O_SPLIT: {
        float a = (float)o->rot * PI / 8, c = cosf(a), s = sinf(a);
        float u = x * c + z * s, w = -x * s + z * c;
        e = sd_box(u, y - 0.55f, w, 0.38f, 0.34f, 0.05f);
        if (e < d) { d = e; *mat = (fabsf(u) > 0.32f || fabsf(y - 0.55f) > 0.28f || fabsf(w) > 0.035f) ? M_FRAME : o->kind == O_MIRROR ? M_SILVER : M_GLASSY; }
        e = sd_box(u, y - 0.16f, w, 0.05f, 0.12f, 0.05f);
        if (e < d) { d = e; *mat = M_FRAME; }
        break;
    }
    case O_PRISM: {
        float t = -1e9f;
        for (int k = 0; k < 3; k++) {
            float a = PI / 2 + (float)k * 2 * PI / 3;
            t = fmaxf(t, x * cosf(a) + z * sinf(a) - 0.15f);
        }
        e = fmaxf(t, fabsf(y - 0.5f) - 0.34f);
        if (e < d) { d = e; *mat = M_PRISM; }
        break;
    }
    case O_FILTER:
        e = sd_box(x, y - 0.52f, z, 0.03f, 0.3f, 0.36f);
        if (e < d) { d = e; *mat = (fabsf(z) > 0.31f || fabsf(y - 0.52f) > 0.25f) ? M_FRAME : M_FILTER; }
        break;
    case O_EMIT:
        e = sd_box(x + 0.04f, y - 0.42f, z, 0.25f, 0.24f, 0.24f);
        if (e < d) { d = e; *mat = M_BODY; }
        e = sd_box(x - 0.29f, y - 0.55f, z, 0.07f, 0.07f, 0.07f);
        if (e < d) { d = e; *mat = M_NOZZLE; }
        break;
    case O_CRYSTAL:
        e = (fabsf(x) + fabsf(y - 0.62f) * 0.7f + fabsf(z) - 0.36f) * 0.57f;
        if (e < d) { d = e; *mat = M_GEM; }
        break;
    }
    return d;
}

/* 표면 한 점이 받는 빛: 은은한 위쪽 조명 + 빛줄기 주변의 색 + 켜진 수정 */
static void surface_light(float px, float py, float pz, float nx, float ny, float nz, float *lr, float *lg, float *lb)
{
    float ndl = fmaxf(0, nx * 0.35f + ny * 0.85f + nz * 0.4f);
    float base = 0.22f + 0.38f * ndl;
    *lr = base; *lg = base; *lb = base * 1.1f;
    for (int i = 0; i < g_nseg; i++) {
        const Seg *s = &g_seg[i];
        float ex = s->x1 - s->x0, ez = s->z1 - s->z0, l2 = ex * ex + ez * ez;
        float t = l2 > 0 ? clampf(((px - s->x0) * ex + (pz - s->z0) * ez) / l2, 0, 1) : 0;
        float dx = px - (s->x0 + ex * t), dy = py - BY, dz = pz - (s->z0 + ez * t);
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > 6) continue;
        float f = 1 / (1 + d2 / 0.18f);
        f *= f * 1.6f;
        *lr += s->r * f; *lg += s->g * f; *lb += s->b * f;
    }
    for (int i = 0; i < g_nobj; i++) {
        const Obj *o = &g_obj[i];
        if (o->kind != O_CRYSTAL && o->kind != O_EMIT) continue;
        float k = o->kind == O_EMIT ? 0.5f : o->lit ? 1.6f : 0.12f;
        float dx = px - ((float)o->x + 0.5f), dy = py - 1.6f, dz = pz - ((float)o->z + 0.5f);
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > 9) continue;
        float f = k / (1 + d2 / 0.5f);
        f *= f;
        *lr += o->col[0] * f; *lg += o->col[1] * f; *lb += o->col[2] * f;
    }
}

/* 플레이어 몸(원기둥 + 머리 구) 과 화면 광선의 교차 */
static float player_hit(float ox, float oy, float oz, float dx, float dy, float dz, float *nx, float *ny, float *nz, int *head)
{
    float best = 1e9f, fx = ox - g_px, fz = oz - g_pz;
    float a = dx * dx + dz * dz, b = fx * dx + fz * dz, c = fx * fx + fz * fz - 0.2f * 0.2f, disc = b * b - a * c;
    if (a > 1e-8f && disc > 0) {
        float t = (-b - sqrtf(disc)) / a, y = oy + dy * t;
        if (t > 0 && y >= 1.0f && y <= 2.0f) { best = t; *nx = (fx + dx * t) / 0.2f; *ny = 0; *nz = (fz + dz * t) / 0.2f; *head = 0; }
    }
    if (dy < 0) {
        float t = (2.0f - oy) / dy, qx = fx + dx * t, qz = fz + dz * t;
        if (t > 0 && t < best && qx * qx + qz * qz < 0.04f) { best = t; *nx = 0; *ny = 1; *nz = 0; *head = 0; }
    }
    float hx = fx, hy = oy - 2.17f, hz = fz, B = hx * dx + hy * dy + hz * dz, C = hx * hx + hy * hy + hz * hz - 0.17f * 0.17f, D = B * B - C;
    if (D > 0) {
        float t = -B - sqrtf(D);
        if (t > 0 && t < best) { best = t; *nx = (hx + dx * t) / 0.17f; *ny = (hy + dy * t) / 0.17f; *nz = (hz + dz * t) / 0.17f; *head = 1; }
    }
    return best;
}

static inline uint32_t pack(float r, float g, float b)
{
    int R = (int)r, G = (int)g, B = (int)b;
    return (uint32_t)((R > 255 ? 255 : R < 0 ? 0 : R) << 16 | (G > 255 ? 255 : G < 0 ? 0 : G) << 8 | (B > 255 ? 255 : B < 0 ? 0 : B));
}

static int near_wall(const Frame *F, int x, int z)
{
    if (x != 0 && z != 0 && x != RW - 1 && z != RD - 1) return 0;
    float cx = (float)x + 0.5f - RW / 2.0f, cz = (float)z + 0.5f - RD / 2.0f;
    return cx * F->nearx + cz * F->nearz > 2.0f;
}

/* 화면 광선과 빛줄기 선분 사이의 가장 가까운 거리 */
static float ray_seg_dist(float ox, float oy, float oz, float dx, float dy, float dz, float tmax, const Seg *s)
{
    float ax = s->x0, ay = BY, az = s->z0, ex = s->x1 - s->x0, ez = s->z1 - s->z0;
    float rx = ox - ax, ry = oy - ay, rz = oz - az;
    float a = dx * dx + dy * dy + dz * dz, e = ex * ex + ez * ez, f = ex * rx + ez * rz;
    float c = dx * rx + dy * ry + dz * rz, b = dx * ex + dz * ez, den = a * e - b * b;
    float t = den > 1e-8f ? clampf((b * f - c * e) / den, 0, tmax) : 0;
    float u = e > 1e-8f ? clampf((b * t + f) / e, 0, 1) : 0;
    t = clampf((b * u - c) / a, 0, tmax);
    float qx = ox + dx * t - (ax + ex * u), qy = oy + dy * t - ay, qz = oz + dz * t - (az + ez * u);
    return sqrtf(qx * qx + qy * qy + qz * qz);
}

static uint32_t trace(const Frame *F, float dx, float dy, float dz, float *zdep, uint16_t *id)
{
    float inv = 1 / sqrtf(dx * dx + dy * dy + dz * dz);
    dx *= inv; dy *= inv; dz *= inv;
    float ox = F->ox, oy = F->oy, oz = F->oz;
    *zdep = 1e9f;
    *id = 0;
    /* 배경: 어두운 신전 바깥 */
    float bgk = clampf(0.5f + dy * 0.6f, 0, 1);
    float bgr = 10 + 10 * bgk, bgg = 11 + 11 * bgk, bgb = 18 + 18 * bgk;

    /* 방 상자와 만나는 구간 */
    float t0 = 0, t1 = 1e9f, o3[3] = { ox, oy, oz }, d3[3] = { dx, dy, dz }, mx[3] = { RW, RH, RD };
    for (int k = 0; k < 3; k++) {
        if (fabsf(d3[k]) < 1e-8f) { if (o3[k] < 0 || o3[k] > mx[k]) t1 = -1; continue; }
        float a = (0 - o3[k]) / d3[k], b = (mx[k] - o3[k]) / d3[k];
        if (a > b) { float tmp = a; a = b; b = tmp; }
        t0 = fmaxf(t0, a);
        t1 = fminf(t1, b);
    }
    Acc acc = { 0, 0, 0, 1 };
    float thit = 1e9f, pnx = 0, pny = 0, pnz = 0;
    int phead = 0;
    float tp = F->fp ? 1e9f : player_hit(ox, oy, oz, dx, dy, dz, &pnx, &pny, &pnz, &phead);
    if (t1 > t0) {
        float sx0 = ox + dx * (t0 + 1e-4f), sy0 = oy + dy * (t0 + 1e-4f), sz0 = oz + dz * (t0 + 1e-4f);
        int x = (int)floorf(sx0), y = (int)floorf(sy0), z = (int)floorf(sz0);
        x = x < 0 ? 0 : x >= RW ? RW - 1 : x;
        y = y < 0 ? 0 : y >= RH ? RH - 1 : y;
        z = z < 0 ? 0 : z >= RD ? RD - 1 : z;
        int sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1, sz = dz > 0 ? 1 : -1;
        float tdx = fabsf(1 / (fabsf(dx) > 1e-8f ? dx : 1e-8f)), tdy = fabsf(1 / (fabsf(dy) > 1e-8f ? dy : 1e-8f)),
              tdz = fabsf(1 / (fabsf(dz) > 1e-8f ? dz : 1e-8f));
        float tmx = t0 + (dx > 0 ? (float)x + 1 - sx0 : sx0 - (float)x) * tdx;
        float tmy = t0 + (dy > 0 ? (float)y + 1 - sy0 : sy0 - (float)y) * tdy;
        float tmz = t0 + (dz > 0 ? (float)z + 1 - sz0 : sz0 - (float)z) * tdz;
        float tcell = t0;
        int axis = -1;
        for (int it = 0; it < 120; it++) {
            float texit = fminf(tmx, fminf(tmy, tmz));
            if (tp < texit) {                                                    /* 내려다보기에서 플레이어 몸 */
                float hx = ox + dx * tp, hy = oy + dy * tp, hz = oz + dz * tp, lr, lg, lb;
                surface_light(hx + pnx * 0.02f, hy + pny * 0.02f, hz + pnz * 0.02f, pnx, pny, pnz, &lr, &lg, &lb);
                float ar = phead ? 0.85f : 0.26f, ag = phead ? 0.42f : 0.22f, ab = phead ? 0.17f : 0.36f;
                layer(&acc, ar * lr * 255, ag * lg * 255, ab * lb * 255, 1);
                thit = tp;
                *zdep = tp * (dx * F->fx + dy * F->fy + dz * F->fz);
                *id = 99;
                break;
            }
            int b = g_blk[y][z][x];
            if (b == B_WALL && y >= 2 && !F->fp && near_wall(F, x, z)) b = B_AIR;          /* 카메라 쪽 벽은 잘라서 안이 보이게 */
            if (b == B_FLOOR || b == B_WALL || b == B_DOOR) {
                float hx = ox + dx * tcell, hy = oy + dy * tcell, hz = oz + dz * tcell;
                if (axis < 0) {                                                  /* 상자 경계에서 바로 맞음: 가장 가까운 면 */
                    float fx = hx - floorf(hx), fy = hy - floorf(hy), fz = hz - floorf(hz);
                    float m = fminf(fminf(fminf(fx, 1 - fx), fminf(fy, 1 - fy)), fminf(fz, 1 - fz));
                    axis = (m == fx || m == 1 - fx) ? 0 : (m == fy || m == 1 - fy) ? 1 : 2;
                }
                float nx = axis == 0 ? (float)-sx : 0, ny = axis == 1 ? (float)-sy : 0, nz = axis == 2 ? (float)-sz : 0;
                float u = axis == 0 ? hz - floorf(hz) : hx - floorf(hx), v = axis == 1 ? hz - floorf(hz) : hy - floorf(hy);
                float ar, ag, ab;
                int hl = 0;
                if (b == B_FLOOR) {
                    float k = ((x + z) & 1) ? 1.0f : 0.86f;
                    ar = 0.26f * k; ag = 0.27f * k; ab = 0.32f * k;
                    if (u < 0.04f || u > 0.96f || v < 0.04f || v > 0.96f) { ar *= 0.6f; ag *= 0.6f; ab *= 0.6f; }
                    if (x == F->tx && z == F->tz && axis == 1 && (u < 0.09f || u > 0.91f || v < 0.09f || v > 0.91f)) hl = 1;
                } else if (b == B_DOOR) {
                    ar = 0.42f; ag = 0.30f; ab = 0.18f;
                    if (fabsf(u - 0.5f) < 0.04f || fabsf(v - 0.5f) < 0.04f) { ar = 0.9f; ag = 0.75f; ab = 0.35f; }
                } else {
                    float k = ny > 0 ? 1.15f : 1.0f;
                    ar = 0.33f * k; ag = 0.33f * k; ab = 0.40f * k;
                    float vv = hy - floorf(hy), row = floorf(hy * 2);
                    float uu = (axis == 0 ? hz : hx) + ((int)row % 2) * 0.5f;
                    if (ny == 0 && (fmodf(vv * 2, 1) < 0.07f || fmodf(uu, 1) < 0.04f)) { ar *= 0.7f; ag *= 0.7f; ab *= 0.7f; }
                }
                float lr, lg, lb;
                surface_light(hx + nx * 0.01f, hy + ny * 0.01f, hz + nz * 0.01f, nx, ny, nz, &lr, &lg, &lb);
                if (hl) layer(&acc, 255, 205, 80, 1);                            /* 물체를 놓을 칸 */
                else layer(&acc, ar * lr * 255, ag * lg * 255, ab * lb * 255, 1);
                thit = tcell;
                *zdep = tcell * (dx * F->fx + dy * F->fy + dz * F->fz);
                *id = (uint16_t)(b * 8 + axis * 2 + (nx + ny + nz > 0) + 1);
                break;
            }
            if (b == B_GLASS) {                                                  /* 유리 블록: 옅게 비치고 모서리는 밝게 */
                float hx = ox + dx * tcell, hy = oy + dy * tcell, hz = oz + dz * tcell;
                float fx = hx - floorf(hx), fy = hy - floorf(hy), fz = hz - floorf(hz);
                int edges = (fx < 0.05f || fx > 0.95f) + (fy < 0.05f || fy > 0.95f) + (fz < 0.05f || fz > 0.95f);
                if (edges >= 2) layer(&acc, 190, 235, 255, 0.55f);
                else layer(&acc, 120, 190, 230, 0.14f);
            }
            if (y == 1 && g_objat[z][x]) {                                       /* 물체: 칸 안에서 거리 함수로 구 추적 */
                const Obj *o = &g_obj[g_objat[z][x] - 1];
                float cx = (float)x + 0.5f, cz = (float)z + 0.5f;
                float t = tcell;
                int mat = 0, hit = 0;
                for (int k = 0; k < 48 && t < texit; k++) {
                    float px = ox + dx * t - cx, py = oy + dy * t - 1, pz = oz + dz * t - cz;
                    float d = obj_sdf(o, px, py, pz, &mat);
                    if (d < 0.004f) { hit = 1; break; }
                    t += fmaxf(d, 0.004f);
                }
                if (hit) {
                    float px = ox + dx * t - cx, py = oy + dy * t - 1, pz = oz + dz * t - cz, e = 0.01f;
                    int m2;
                    float nx = obj_sdf(o, px + e, py, pz, &m2) - obj_sdf(o, px - e, py, pz, &m2);
                    float ny = obj_sdf(o, px, py + e, pz, &m2) - obj_sdf(o, px, py - e, pz, &m2);
                    float nz = obj_sdf(o, px, py, pz + e, &m2) - obj_sdf(o, px, py, pz - e, &m2);
                    float nl = sqrtf(nx * nx + ny * ny + nz * nz) + 1e-9f;
                    nx /= nl; ny /= nl; nz /= nl;
                    float ar = 0.3f, ag = 0.3f, ab = 0.35f, er = 0, eg = 0, eb = 0, spec = 0;
                    switch (mat) {
                    case M_BASE:                                              /* 움직일 수 있는 물체는 구리색 받침 */
                        if (o->movable) { ar = 0.62f; ag = 0.40f; ab = 0.22f; spec = 0.4f; }
                        else { ar = 0.20f; ag = 0.21f; ab = 0.26f; }
                        break;
                    case M_FRAME: ar = 0.85f; ag = 0.66f; ab = 0.32f; spec = 0.5f; break;
                    case M_SILVER: ar = 0.80f; ag = 0.86f; ab = 0.95f; spec = 1; break;
                    case M_GLASSY: ar = 0.45f; ag = 0.80f; ab = 0.95f; spec = 0.7f; break;
                    case M_PRISM: {
                        float a = atan2f(nz, nx);
                        ar = 0.7f + 0.3f * cosf(a); ag = 0.7f + 0.3f * cosf(a + 2.1f); ab = 0.75f + 0.25f * cosf(a + 4.2f);
                        spec = 0.8f;
                        break;
                    }
                    case M_FILTER: ar = o->col[0] * 0.8f; ag = o->col[1] * 0.8f; ab = o->col[2] * 0.9f; er = o->col[0] * 40; eg = o->col[1] * 40; eb = o->col[2] * 60; break;
                    case M_BODY: ar = 0.25f; ag = 0.26f; ab = 0.32f; break;
                    case M_NOZZLE: er = 255; eg = 250; eb = 235; ar = ag = ab = 0; break;
                    case M_GEM: {
                        float k = o->lit ? 1.0f : 0.35f, flick = o->lit ? 0.85f + 0.15f * sinf((float)F->frame * 0.15f + (float)x) : 1;
                        ar = o->col[0] * k * 0.6f + 0.05f; ag = o->col[1] * k * 0.6f + 0.05f; ab = o->col[2] * k * 0.6f + 0.05f;
                        float em = o->lit ? 190 * flick : 25;
                        er = o->col[0] * em; eg = o->col[1] * em; eb = o->col[2] * em;
                        spec = 0.6f;
                        break;
                    }
                    }
                    float lr, lg, lb, wx = px + cx, wy = py + 1, wz = pz + cz;
                    surface_light(wx + nx * 0.02f, wy + ny * 0.02f, wz + nz * 0.02f, nx, ny, nz, &lr, &lg, &lb);
                    if (spec > 0) {                                              /* 반짝이는 재질은 빛줄기 색에 덜 물든다 */
                        float m = (lr + lg + lb) / 3;
                        lr = fminf(0.5f * lr + 0.5f * m, 1.15f); lg = fminf(0.5f * lg + 0.5f * m, 1.15f); lb = fminf(0.5f * lb + 0.5f * m, 1.15f);
                    }
                    float rr = ar * lr * 255 + er, gg = ag * lg * 255 + eg, bb = ab * lb * 255 + eb;
                    if (spec > 0) {                                              /* 반짝임 */
                        float hx = 0.35f - dx, hy = 0.85f - dy, hz = 0.4f - dz, hl = sqrtf(hx * hx + hy * hy + hz * hz);
                        float s = powf(fmaxf(0, (nx * hx + ny * hy + nz * hz) / hl), 24) * 200 * spec;
                        rr += s; gg += s; bb += s;
                    }
                    layer(&acc, rr, gg, bb, 1);
                    thit = t;
                    *zdep = t * (dx * F->fx + dy * F->fy + dz * F->fz);
                    *id = (uint16_t)(100 + g_objat[z][x]);
                    break;
                }
            }
            /* 다음 칸 */
            tcell = texit;
            if (tmx <= tmy && tmx <= tmz) { x += sx; tmx += tdx; axis = 0; }
            else if (tmy <= tmz) { y += sy; tmy += tdy; axis = 1; }
            else { z += sz; tmz += tdz; axis = 2; }
            if ((unsigned)x >= RW || (unsigned)y >= RH || (unsigned)z >= RD) break;
        }
    }
    if (acc.T > 0.001f) layer(&acc, bgr, bgg, bgb, 1);

    /* 빛줄기: 화면 광선과 가까울수록 밝게 (가려진 부분은 thit에서 끊김) */
    float tm = thit < 1e8f ? thit : 200;
    for (int i = 0; i < g_nseg; i++) {
        const Seg *s = &g_seg[i];
        float d = ray_seg_dist(ox, oy, oz, dx, dy, dz, tm, s);
        if (d > 0.45f) continue;
        float core = expf(-(d * d) / (0.045f * 0.045f)), halo = expf(-(d * d) / (0.16f * 0.16f));
        float k = core * 1.4f + halo * 0.45f;
        acc.r += (s->r * 255 * 0.85f + core * 120) * k;
        acc.g += (s->g * 255 * 0.85f + core * 120) * k;
        acc.b += (s->b * 255 * 0.85f + core * 120) * k;
    }
    return pack(acc.r, acc.g, acc.b);
}

static int g_nthreads = 1;
static pthread_barrier_t g_go, g_done;
static volatile int g_wquit;
static pthread_t g_tid[MAX_THREADS];

static void render_rows(int id)
{
    const Frame *F = &g_fr;
    float aspect = (float)F->H / (float)F->W;
    for (int j = id; j < F->H; j += g_nthreads) {
        float sy = (1 - 2 * ((float)j + 0.5f) / (float)F->H) * F->tanh * aspect;
        for (int i = 0; i < F->W; i++) {
            float sx = (2 * ((float)i + 0.5f) / (float)F->W - 1) * F->tanh;
            g_pix[j][i] = trace(F, F->fx + F->rx * sx + F->ux * sy, F->fy + F->ry * sx + F->uy * sy,
                                F->fz + F->rz * sx + F->uz * sy, &g_depth[j][i], &g_id[j][i]);
        }
    }
}

static void *worker(void *arg)
{
    int id = (int)(intptr_t)arg;
    for (;;) {
        pthread_barrier_wait(&g_go);
        if (g_wquit) break;
        render_rows(id);
        pthread_barrier_wait(&g_done);
    }
    return NULL;
}

static inline uint32_t scale_c(uint32_t c, float k)
{
    return pack((float)(c >> 16 & 255) * k, (float)(c >> 8 & 255) * k, (float)(c & 255) * k);
}

/* 윤곽선과 선택 테두리 */
static void post_pass(int W, int H, int sel_id, int frame)
{
    float pulse = 0.75f + 0.25f * sinf((float)frame * 0.25f);
    for (int j = 1; j < H - 1; j++)
        for (int i = 1; i < W - 1; i++) {
            uint16_t id = g_id[j][i];
            if (!id) continue;
            if (sel_id && id == sel_id &&
                (g_id[j][i + 1] != id || g_id[j][i - 1] != id || g_id[j + 1][i] != id || g_id[j - 1][i] != id)) {
                g_pix[j][i] = pack(255 * pulse, 210 * pulse, 80 * pulse);
                continue;
            }
            float z = g_depth[j][i], zl = g_depth[j][i - 1], zr = g_depth[j][i + 1], zu = g_depth[j - 1][i], zd = g_depth[j + 1][i];
            float iz = 1 / z, lap = fabsf(1 / zl + 1 / zr - 2 * iz) + fabsf(1 / zu + 1 / zd - 2 * iz);
            float far = fmaxf(fmaxf(zl, zr), fmaxf(zu, zd));
            if (lap > iz * 0.15f && far > z * 1.04f) g_pix[j][i] = scale_c(g_pix[j][i], 0.45f);
            else if (g_id[j][i + 1] != id || g_id[j + 1][i] != id) g_pix[j][i] = scale_c(g_pix[j][i], 0.72f);
        }
}

/* ---------- 카메라 ---------- */

static float cam_yaw = 0.62f, cam_yaw_target = 0.62f, cam_pitch = -0.78f, cam_dist = 19;

static void setup_frame(int W, int H, int frame)
{
    Frame *F = &g_fr;
    float yaw = g_fp ? g_pyaw : cam_yaw, pitch = g_fp ? g_ppitch : cam_pitch;
    float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
    F->fx = sy * cp; F->fy = sp; F->fz = cy * cp;
    F->rx = cy; F->ry = 0; F->rz = -sy;
    F->ux = -sy * sp; F->uy = cp; F->uz = -cy * sp;
    if (g_fp) {
        F->ox = g_px; F->oy = EYE_Y; F->oz = g_pz;
        F->tanh = 0.72f;
    } else {
        F->ox = RW / 2.0f - F->fx * cam_dist;
        F->oy = 1.0f - F->fy * cam_dist;
        F->oz = RD / 2.0f - F->fz * cam_dist;
        F->tanh = 0.58f;
    }
    float nl = sqrtf(F->fx * F->fx + F->fz * F->fz) + 1e-6f;
    F->nearx = -F->fx / nl; F->nearz = -F->fz / nl;
    F->W = W; F->H = H;
    F->frame = frame;
    F->fp = g_fp;
    F->tx = g_tx; F->tz = g_tz;
}

static void render_pixels(int W, int H, int frame)
{
    setup_frame(W, H, frame);
    pthread_barrier_wait(&g_go);
    render_rows(0);
    pthread_barrier_wait(&g_done);
    post_pass(W, H, g_tobj >= 0 ? 101 + g_tobj : 0, frame);
}

/* ---------- 플레이어 조작 ---------- */

static uint8_t g_kdown[256];
static int64_t g_ktap[256], g_now;
static int g_win32;
static unsigned char g_in[4096];
static int g_in_len;
static int64_t g_esc_since;
static int g_help = 1, g_moves;
static char g_msg[160];
static int g_msg_t;

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void message(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_msg, sizeof g_msg, fmt, ap);
    va_end(ap);
    g_msg_t = 90;
}

static int kheld(int vk) { return g_kdown[vk] || g_ktap[vk] > g_now; }

static void after_change(void)
{
    int was = g_solved;
    simulate();
    g_moves++;
    if (g_solved && !was) message("모든 수정이 빛났어요! 문이 열렸습니다 (%d번 조작)", g_moves);
}

static int cell_free(int x, int z)
{
    return x > 0 && z > 0 && x < RW - 1 && z < RD - 1 && g_blk[1][z][x] == B_AIR && !g_objat[z][x];
}

/* 몸(원)이 벽, 유리, 물체 칸과 겹치는가 */
static int circle_hits(float x, float z)
{
    for (int cz = (int)floorf(z - PR); cz <= (int)floorf(z + PR); cz++)
        for (int cx = (int)floorf(x - PR); cx <= (int)floorf(x + PR); cx++) {
            int solid = cx < 0 || cz < 0 || cx >= RW || cz >= RD || g_blk[1][cz][cx] != B_AIR || g_objat[cz][cx];
            if (!solid) continue;
            float qx = clampf(x, (float)cx, (float)cx + 1), qz = clampf(z, (float)cz, (float)cz + 1);
            if ((qx - x) * (qx - x) + (qz - z) * (qz - z) < PR * PR) return 1;
        }
    return 0;
}

/* 겨누기: 1인칭은 화면 가운데 광선(4.5칸까지), 내려다보기는 플레이어 바로 앞 칸 */
static void find_target(void)
{
    g_tobj = -1;
    g_tx = g_tz = -1;
    if (g_fp) {
        float cp = cosf(g_ppitch), dx = sinf(g_pyaw) * cp, dy = sinf(g_ppitch), dz = cosf(g_pyaw) * cp;
        float ox = g_px, oy = EYE_Y, oz = g_pz;
        int x = (int)floorf(ox), y = (int)floorf(oy), z = (int)floorf(oz);
        int sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1, sz = dz > 0 ? 1 : -1;
        float tdx = fabsf(dx) > 1e-6f ? fabsf(1 / dx) : 1e30f, tdy = fabsf(dy) > 1e-6f ? fabsf(1 / dy) : 1e30f,
              tdz = fabsf(dz) > 1e-6f ? fabsf(1 / dz) : 1e30f;
        float tmx = fabsf(dx) > 1e-6f ? (dx > 0 ? (float)x + 1 - ox : ox - (float)x) * tdx : 1e30f;
        float tmy = fabsf(dy) > 1e-6f ? (dy > 0 ? (float)y + 1 - oy : oy - (float)y) * tdy : 1e30f;
        float tmz = fabsf(dz) > 1e-6f ? (dz > 0 ? (float)z + 1 - oz : oz - (float)z) * tdz : 1e30f;
        for (int i = 0; i < 24; i++) {
            int axis;
            float t;
            if (tmx < tmy && tmx < tmz) { t = tmx; x += sx; tmx += tdx; axis = 0; }
            else if (tmy < tmz) { t = tmy; y += sy; tmy += tdy; axis = 1; }
            else { t = tmz; z += sz; tmz += tdz; axis = 2; }
            if (t > 4.5f || x < 0 || z < 0 || x >= RW || z >= RD || y < 0) return;
            if (y >= RH) continue;
            if (y == 1 && g_objat[z][x]) { g_tobj = g_objat[z][x] - 1; return; }
            int b = g_blk[y][z][x];
            if (b != B_AIR) {
                if (b == B_FLOOR && y == 0 && axis == 1 && cell_free(x, z)) { g_tx = x; g_tz = z; }
                break;
            }
        }
    } else {
        int x = (int)floorf(g_px + sinf(g_pyaw) * 0.85f), z = (int)floorf(g_pz + cosf(g_pyaw) * 0.85f);
        if (x < 0 || z < 0 || x >= RW || z >= RD) return;
        if (g_objat[z][x]) g_tobj = g_objat[z][x] - 1;
        else if (cell_free(x, z)) { g_tx = x; g_tz = z; }
    }
    if (g_tx >= 0) {                                      /* 내가 서 있는 칸에는 못 놓는다 */
        float qx = clampf(g_px, (float)g_tx, (float)g_tx + 1), qz = clampf(g_pz, (float)g_tz, (float)g_tz + 1);
        if ((qx - g_px) * (qx - g_px) + (qz - g_pz) * (qz - g_pz) < PR * PR) g_tx = g_tz = -1;
    }
}

static void act(void)
{
    if (g_carry >= 0) {
        if (g_tx < 0) { message("빈 바닥을 겨누고 F를 누르면 내려놓아요"); return; }
        Obj *o = &g_obj[g_carry];
        o->x = g_tx; o->z = g_tz; o->held = 0;
        g_objat[g_tz][g_tx] = g_carry + 1;
        message("%s을(를) 내려놓았어요", KIND_NAME[o->kind]);
        g_carry = -1;
        after_change();
        find_target();
        return;
    }
    if (g_tobj < 0) { message("물체를 겨누고 F를 누르면 들 수 있어요"); return; }
    Obj *o = &g_obj[g_tobj];
    if (!o->movable) { message("%s은(는) 고정되어 있어요", KIND_NAME[o->kind]); return; }
    g_objat[o->z][o->x] = 0;
    o->held = 1;
    g_carry = g_tobj;
    message("%s을(를) 들었어요. 빈 바닥을 겨누고 F로 내려놓으세요", KIND_NAME[o->kind]);
    after_change();
    find_target();
}

static void rotate_target(int d)
{
    Obj *o = g_tobj >= 0 ? &g_obj[g_tobj] : g_carry >= 0 ? &g_obj[g_carry] : NULL;
    if (!o) { message("돌릴 거울을 겨누세요"); return; }
    if (o->kind != O_MIRROR && o->kind != O_SPLIT) { message("%s은(는) 돌릴 수 없어요", KIND_NAME[o->kind]); return; }
    o->rot = (o->rot + d + 8) % 8;
    after_change();
}

static void reset_room(void)
{
    load_level();
    g_carry = -1;
    g_px = 2.5f; g_pz = 8.5f; g_pyaw = PI / 2; g_ppitch = -0.35f;
    simulate();
    g_moves = 0;
    find_target();
    message("처음 상태로 되돌렸어요");
}

static void on_press(int vk)
{
    switch (vk) {
    case 0x09: g_fp = !g_fp; message(g_fp ? "1인칭 시점" : "내려다보기 시점 (←→ 카메라 회전)"); break;
    case 'F': act(); break;
    case 'Q': rotate_target(-1); break;
    case 'E': rotate_target(1); break;
    case 'R': reset_room(); break;
    case 'H': g_help = !g_help; break;
    case 0xBB: cam_dist = fmaxf(10, cam_dist - 1.5f); break;
    case 0xBD: cam_dist = fminf(30, cam_dist + 1.5f); break;
    case 0x25: if (!g_fp) cam_yaw_target -= PI / 4; break;
    case 0x27: if (!g_fp) cam_yaw_target += PI / 4; break;
    case 0x26: if (!g_fp) cam_pitch = fmaxf(-1.35f, cam_pitch - 0.08f); break;
    case 0x28: if (!g_fp) cam_pitch = fminf(-0.3f, cam_pitch + 0.08f); break;
    case 0x1B: g_quit = 1; break;
    }
}

static void key_event(int vk, int down)
{
    vk &= 255;
    if (down) {
        if (!g_kdown[vk]) on_press(vk);
        g_kdown[vk] = 1;
    } else {
        g_kdown[vk] = 0;
    }
}

/* 키를 뗀 순간을 모르는 터미널용: 키 반복으로 누르고 있음을 흉내 */
static void key_tap(int vk)
{
    int was = g_ktap[vk] > g_now, walk = vk == 'W' || vk == 'A' || vk == 'S' || vk == 'D';
    if (!was || !walk) on_press(vk);                    /* 걷기만 '누르고 있음', 나머지는 누를 때마다 실행 */
    g_ktap[vk] = g_now + (was ? 90 : 200);
}

static void plain_char(int c)
{
    if (c == 3) g_quit = 1;
    else if (c >= 'a' && c <= 'z') key_tap(c - 32);
    else if (c >= 'A' && c <= 'Z') key_tap(c);
    else if (c == '\t') key_tap(0x09);
    else if (c == '+' || c == '=') key_tap(0xBB);
    else if (c == '-' || c == '_') key_tap(0xBD);
}

static void csi(const unsigned char *p, int n, char fin)
{
    int prm[8] = { 0, 0, 0, 0, 0, 1, 0, 0 }, np = 0, priv = 0, any = 0;
    if (n > 0 && p[0] == '<') { priv = 1; p++; n--; }
    for (int i = 0; i < n && np < 8; i++) {
        if (p[i] == ';') { np++; any = 0; continue; }
        if (p[i] >= '0' && p[i] <= '9') {
            if (!any) prm[np] = 0;
            prm[np] = prm[np] * 10 + (p[i] - '0');
            any = 1;
        }
    }
    if (fin == '_') {                                    /* win32-input-mode: Vk;Sc;Uc;Kd;Cs;Rc */
        g_win32 = 1;
        if (prm[0] == 'C' && prm[3] && (prm[4] & 0x0C)) g_quit = 1;
        if (prm[0] > 0) key_event(prm[0], prm[3]);
    } else if (priv && fin == 'M') {                     /* 마우스: 왼쪽 클릭 = 들기/놓기, 휠 = 돌리기 */
        if (prm[0] == 0) act();
        else if (prm[0] == 64) rotate_target(-1);
        else if (prm[0] == 65) rotate_target(1);
    } else if (fin >= 'A' && fin <= 'D' && !priv) {
        static const int vk[4] = { 0x26, 0x28, 0x27, 0x25 };
        key_tap(vk[fin - 'A']);
    } else if (fin == 'O' && n == 0) {
        memset(g_kdown, 0, sizeof g_kdown);
    }
}

static void handle_stdin(void)
{
    ssize_t n = read(STDIN_FILENO, g_in + g_in_len, sizeof g_in - (size_t)g_in_len);
    if (n > 0) g_in_len += (int)n;
    int i = 0;
    while (i < g_in_len) {
        unsigned char c = g_in[i];
        if (c != 27) { plain_char(c); i++; continue; }
        if (i + 1 >= g_in_len) { if (!g_esc_since) g_esc_since = now_ms(); break; }
        g_esc_since = 0;
        if (g_in[i + 1] != '[') { i++; continue; }
        int j = i + 2;
        while (j < g_in_len && !(g_in[j] >= 0x40 && g_in[j] <= 0x7E)) j++;
        if (j >= g_in_len) { if (g_in_len - i > 64) i = g_in_len; break; }
        csi(g_in + i + 2, j - (i + 2), (char)g_in[j]);
        i = j + 1;
    }
    memmove(g_in, g_in + i, (size_t)(g_in_len - i));
    g_in_len -= i;
}

/* 한 틱: 걷기 (벽에 미끄러지며), 몸이 움직이면 빛 다시 계산, 겨누기 갱신 */
static void player_tick(float dt)
{
    float f = (float)(kheld('W') - kheld('S')), s = (float)(kheld('D') - kheld('A')), vx, vz;
    if (g_fp) {
        g_pyaw += (float)(kheld(0x27) - kheld(0x25)) * 2.2f * dt;
        g_ppitch = clampf(g_ppitch + (float)(kheld(0x26) - kheld(0x28)) * 1.6f * dt, -1.3f, 0.9f);
        vx = sinf(g_pyaw) * f + cosf(g_pyaw) * s;
        vz = cosf(g_pyaw) * f - sinf(g_pyaw) * s;
    } else {
        vx = sinf(cam_yaw) * f + cosf(cam_yaw) * s;
        vz = cosf(cam_yaw) * f - sinf(cam_yaw) * s;
        if (vx != 0 || vz != 0) g_pyaw = atan2f(vx, vz);
    }
    float l = sqrtf(vx * vx + vz * vz);
    if (l > 0) {
        int moved = 0;
        vx = vx / l * 3.0f * dt;
        vz = vz / l * 3.0f * dt;
        if (!circle_hits(g_px + vx, g_pz)) { g_px += vx; moved = 1; }
        if (!circle_hits(g_px, g_pz + vz)) { g_pz += vz; moved = 1; }
        if (moved) {
            int was = g_solved;
            simulate();                                   /* 몸이 빛줄기를 가릴 수 있다 */
            if (g_solved && !was) message("모든 수정이 빛났어요! 문이 열렸습니다");
        }
    }
    find_target();
}

/* ---------- 화면 ---------- */

static int g_fps, g_fps_n;
static int64_t g_fps_t0;

static void render(int frame)
{
    if (g_cols < 80 || g_rows < 24) {
        for (int r = 0; r < g_rows; r++) term_bar(r, 0x101010);
        term_text(1, 1, 0xFFD060, 0x101010, "창이 작아요: 80x24 이상 (지금 %dx%d)", g_cols, g_rows);
        term_present();
        return;
    }
    int W = g_cols, H = g_rows * 2;
    render_pixels(W, H, frame);
    if (g_fp) {                                           /* 조준점 */
        int cx = W / 2, cy = H / 2;
        for (int k = -2; k <= 2; k++) {
            g_pix[cy][cx + k] = k ? 0xE8E8E8 : 0xFFFFFF;
            g_pix[cy + k][cx] = 0xE8E8E8;
        }
    }
    static const char HALF[4] = "\xE2\x96\x80";
    for (int r = 0; r < g_rows; r++)
        for (int c = 0; c < W; c++) {
            VCell *v = &g_back[r][c];
            v->fg = g_pix[2 * r][c];
            v->bg = g_pix[2 * r + 1][c];
            memcpy(v->ch, HALF, 4);
            v->ch[4] = 0;
            v->wide = 0;
        }
    int total, lit = count_lit(&total);
    term_bar(0, 0x161822);
    int x = term_text(1, 0, 0xFFD27A, 0x161822, " PRISM ") + 2;
    x += term_text(x, 0, 0xC8CCD8, 0x161822, "빛의 신전  |  수정 %d/%d", lit, total);
    x += term_text(x, 0, g_solved ? 0x7CE07C : 0xC8CCD8, 0x161822, g_solved ? "  |  문 열림" : "  |  문 닫힘");
    x += term_text(x, 0, 0xC8CCD8, 0x161822, "  |  %s", g_fp ? "1인칭" : "내려다보기");
    term_text(g_cols - 34, 0, 0x7A8090, 0x161822, "키 %s  빛줄기 %d  %2d fps", g_win32 ? "누름/뗌" : "탭", g_nseg, g_fps);

    /* 아래쪽: 손에 든 것, 겨누는 것 */
    int iy = g_rows - (g_help ? 3 : 2);
    char info[160];
    if (g_carry >= 0)
        snprintf(info, sizeof info, " 손에 든 것: %s  —  %s ", KIND_NAME[g_obj[g_carry].kind], g_tx >= 0 ? "F: 여기에 내려놓기" : "빈 바닥을 겨누세요");
    else if (g_tobj >= 0)
        snprintf(info, sizeof info, " %s  —  %s ", KIND_NAME[g_obj[g_tobj].kind],
                 !g_obj[g_tobj].movable ? "고정됨" : (g_obj[g_tobj].kind == O_MIRROR || g_obj[g_tobj].kind == O_SPLIT) ? "F: 들기   Q/E: 돌리기" : "F: 들기");
    else
        info[0] = 0;
    if (info[0]) term_text(g_cols / 2 - term_str_w(info) / 2, iy, 0xFFE6A0, 0x22252F, "%s", info);
    if (g_help) {
        term_bar(g_rows - 1, 0x161822);
        term_text(1, g_rows - 1, 0x9AA0B0, 0x161822,
             g_fp ? "WASD 걷기   방향키 시선   F 들기/놓기   Q/E·휠 거울 돌리기   Tab 내려다보기   R 처음부터   H 도움말   ESC 종료"
                  : "WASD 걷기   ←→ 카메라 회전   ↑↓ 기울기   +/- 확대   F 들기/놓기   Q/E 돌리기   Tab 1인칭   R 처음부터   ESC 종료");
    }
    if (g_msg_t > 0) term_text(g_cols / 2 - term_str_w(g_msg) / 2 - 1, 2, g_solved ? 0x101010 : 0xFFFFFF, g_solved ? 0x7CE07C : 0x2A2F3C, " %s ", g_msg);
    term_present();
}

/* ---------- 시험용 ---------- */

static void print_state(const char *title)
{
    int total, lit = count_lit(&total);
    printf("%s: %d segments, crystals %d/%d, door %s\n", title, g_nseg, lit, total, g_solved ? "open" : "closed");
    for (int i = 0; i < g_nobj; i++) {
        const Obj *o = &g_obj[i];
        if (o->kind == O_CRYSTAL)
            printf("  crystal (%d,%d) wants %.0f%.0f%.0f got %.2f %.2f %.2f -> %s\n", o->x, o->z, o->col[0], o->col[1], o->col[2],
                   o->got[0], o->got[1], o->got[2], o->lit ? "LIT" : "dark");
        else if (o->movable)
            printf("  %s (%d,%d) rot %d\n", o->kind == O_MIRROR ? "mirror" : o->kind == O_SPLIT ? "splitter" : o->kind == O_PRISM ? "prism" : "filter",
                   o->x, o->z, o->rot);
    }
}

static void write_ppm(const char *path, int W, int H, int scale)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", W * scale, H * scale);
    for (int y = 0; y < H * scale; y++)
        for (int x = 0; x < W * scale; x++) {
            uint32_t c = g_pix[y / scale][x / scale];
            unsigned char rgb[3] = { (unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c };
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

int main(int argc, char **argv)
{
    int check = 0, do_solve = 0, sel = -1, fp_shot = 0;
    float fx = 0, fz = 0, fyaw = 0, fpitch = 0;
    const char *shot = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--check")) check = 1;
        else if (!strcmp(argv[i], "--solve")) do_solve = 1;
        else if (!strcmp(argv[i], "--select") && i + 1 < argc) sel = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fp") && i + 4 < argc) {
            fp_shot = 1;
            fx = strtof(argv[++i], NULL); fz = strtof(argv[++i], NULL);
            fyaw = strtof(argv[++i], NULL); fpitch = strtof(argv[++i], NULL);
        } else if (!strcmp(argv[i], "--shot") && i + 1 < argc) {
            shot = argv[++i];
            if (i + 2 < argc && argv[i + 1][0] != '-') { cam_yaw = cam_yaw_target = strtof(argv[++i], NULL); cam_pitch = strtof(argv[++i], NULL); }
            if (i + 1 < argc && argv[i + 1][0] != '-') cam_dist = strtof(argv[++i], NULL);
        } else {
            fprintf(stderr, "usage: %s [--check] [--solve] [--shot file.ppm [yaw pitch [dist]]] [--fp x z yaw pitch] [--select n]\n", argv[0]);
            return 1;
        }
    }
    load_level();
    simulate();
    int nthreads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    nthreads = nthreads < 1 ? 1 : nthreads > MAX_THREADS ? MAX_THREADS : nthreads;
    g_nthreads = nthreads;
    pthread_barrier_init(&g_go, NULL, (unsigned)nthreads);
    pthread_barrier_init(&g_done, NULL, (unsigned)nthreads);
    for (int i = 1; i < nthreads; i++) pthread_create(&g_tid[i], NULL, worker, (void *)(intptr_t)i);

    if (check || shot) {
        g_fp = fp_shot;
        if (fp_shot) { g_px = fx; g_pz = fz; g_pyaw = fyaw; g_ppitch = fpitch; }
        if (check) print_state("start");
        if (do_solve) {
            int ok = solve();
            if (check) print_state(ok ? "solved" : "no solution found");
        }
        find_target();
        if (sel >= 0 && sel < g_nobj) g_tobj = sel;
        if (shot) {
            int W = 240, H = 140;
            int64_t t0 = now_us();
            render_pixels(W, H, 0);
            double ms = (double)(now_us() - t0) / 1000.0;
            if (g_fp)
                for (int k = -2; k <= 2; k++) { g_pix[H / 2][W / 2 + k] = 0xE8E8E8; g_pix[H / 2 + k][W / 2] = 0xE8E8E8; }
            write_ppm(shot, W, H, 3);
            printf("wrote %s (%dx%d px, %.1f ms with %d threads)\n", shot, W, H, ms, nthreads);
        }
        g_wquit = 1;
        pthread_barrier_wait(&g_go);
        for (int i = 1; i < nthreads; i++) pthread_join(g_tid[i], NULL);
        return 0;
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "prism: run this in a terminal\n");
        return 1;
    }

    term_install_signals();
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    struct itimerspec its = { { 0, 1000000000L / 30 }, { 0, 1000000000L / 30 } };
    timerfd_settime(tfd, 0, &its, NULL);
    term_setup();
    term_get_size();
    g_now = now_ms();
    find_target();
    message("거울을 들고, 놓고, 돌려서 모든 수정에 맞는 색의 빛을 보내세요");
    g_fps_t0 = g_now;
    int frame = 0;
    struct pollfd pf[2] = { { STDIN_FILENO, POLLIN, 0 }, { tfd, POLLIN, 0 } };
    while (!g_quit) {
        if (g_resize) { g_resize = 0; term_get_size(); }
        if (poll(pf, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        g_now = now_ms();
        if (pf[0].revents & POLLIN) handle_stdin();
        if (g_in_len == 1 && g_in[0] == 27 && g_esc_since && g_now - g_esc_since > 60) g_quit = 1;
        if (pf[1].revents & POLLIN) {
            uint64_t t;
            if (read(tfd, &t, sizeof t) != sizeof t) continue;
            player_tick(1.0f / 30);
            cam_yaw += (cam_yaw_target - cam_yaw) * 0.25f;            /* 카메라는 부드럽게 돈다 */
            if (g_msg_t > 0) g_msg_t--;
            render(frame++);
            g_fps_n++;
            if (g_now - g_fps_t0 >= 1000) {
                g_fps = (int)(g_fps_n * 1000 / (g_now - g_fps_t0));
                g_fps_n = 0;
                g_fps_t0 = g_now;
            }
        }
    }
    term_restore();
    g_wquit = 1;
    pthread_barrier_wait(&g_go);
    for (int i = 1; i < nthreads; i++) pthread_join(g_tid[i], NULL);
    close(tfd);
    return 0;
}
