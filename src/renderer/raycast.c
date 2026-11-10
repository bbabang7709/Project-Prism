#include "renderer/raycast.h"

#include <math.h>
#include <string.h>
#include "common/mathutil.h"
#include "world/world.h"

/* 프레임 정보와 결과 버퍼. 원본처럼 이 파일의 전역 배열로 두어야
 * 컴파일러가 버퍼 쓰기와 Frame 읽기가 겹치지 않음을 알고, 원본과 같은 계산 순서를 만든다. */
Frame g_fr;
uint32_t g_pix[MAX_H * 2][MAX_W];
float g_depth[MAX_H * 2][MAX_W];
uint16_t g_id[MAX_H * 2][MAX_W];

/* 프레임마다 복사해 두는 빛줄기 선분과 플레이어 몸들.
 * 원본처럼 모듈 안 배열로 읽어야 -ffast-math에서 계산 결과가 픽셀 단위까지 같다. */
#define MAXBODY 8
static Seg s_seg[MAXSEG];
static int s_nseg;
static Body s_body[MAXBODY];
static int s_nbody;

void raycast_prepare(const Seg *seg, int nseg, const Body *body, int nbody)
{
    s_nseg = nseg < 0 ? 0 : nseg > MAXSEG ? MAXSEG : nseg;
    memcpy(s_seg, seg, sizeof *seg * (size_t)s_nseg);
    s_nbody = nbody < 0 ? 0 : nbody > MAXBODY ? MAXBODY : nbody;
    memcpy(s_body, body, sizeof *body * (size_t)s_nbody);
}

/* 플레이어별 색: 몸, 머리 (0번은 원래 색) */
static const float BODY_COL[2][2][3] = {
    { { 0.26f, 0.22f, 0.36f }, { 0.85f, 0.42f, 0.17f } },
    { { 0.18f, 0.34f, 0.30f }, { 0.95f, 0.78f, 0.30f } },
};

/* ---------- 화면 광선 추적 ---------- */

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
        if (o->rot) {                                     /* 동쪽이 아닌 광원: 쏘는 방향으로 돌린다 (45도 단위) */
            float a = (float)o->rot * PI / 4, c = cosf(a), s = sinf(a), u = x * c + z * s, w = -x * s + z * c;
            x = u; z = w;
        }
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
    for (int i = 0; i < s_nseg; i++) {
        const Seg *s = &s_seg[i];
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

/* 플레이어 몸 하나(원기둥 + 머리 구)와 화면 광선의 교차 (원본과 같은 계산) */
static float player_hit(const Body *pb, float ox, float oy, float oz, float dx, float dy, float dz, float *nx, float *ny, float *nz, int *head)
{
    float best = 1e9f, fx = ox - pb->x, fz = oz - pb->z;
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
    int phead = 0, pwho = 0;
    float tp = s_nbody ? player_hit(&s_body[0], ox, oy, oz, dx, dy, dz, &pnx, &pny, &pnz, &phead) : 1e9f;
    if (s_nbody) pwho = s_body[0].who;
    for (int i = 1; i < s_nbody; i++) {                    /* 다른 몸: 더 가까우면 바꾼다 */
        float nx2 = 0, ny2 = 0, nz2 = 0;
        int head2 = 0;
        float t2 = player_hit(&s_body[i], ox, oy, oz, dx, dy, dz, &nx2, &ny2, &nz2, &head2);
        if (t2 < tp) { tp = t2; pnx = nx2; pny = ny2; pnz = nz2; phead = head2; pwho = s_body[i].who; }
    }
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
            if (tp < texit) {                                                    /* 플레이어 몸 */
                float hx = ox + dx * tp, hy = oy + dy * tp, hz = oz + dz * tp, lr, lg, lb;
                surface_light(hx + pnx * 0.02f, hy + pny * 0.02f, hz + pnz * 0.02f, pnx, pny, pnz, &lr, &lg, &lb);
                const float *col = BODY_COL[pwho & 1][phead];
                layer(&acc, col[0] * lr * 255, col[1] * lg * 255, col[2] * lb * 255, 1);
                thit = tp;
                *zdep = tp * (dx * F->fx + dy * F->fy + dz * F->fz);
                *id = (uint16_t)(99 - pwho);
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
    for (int i = 0; i < s_nseg; i++) {
        const Seg *s = &s_seg[i];
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

void raycast_rows(int first, int step)
{
    const Frame *F = &g_fr;
    float aspect = (float)F->H / (float)F->W;
    for (int j = first; j < F->H; j += step) {
        float sy = (1 - 2 * ((float)j + 0.5f) / (float)F->H) * F->tanh * aspect;
        for (int i = 0; i < F->W; i++) {
            float sx = (2 * ((float)i + 0.5f) / (float)F->W - 1) * F->tanh;
            g_pix[j][i] = trace(F, F->fx + F->rx * sx + F->ux * sy, F->fy + F->ry * sx + F->uy * sy,
                                F->fz + F->rz * sx + F->uz * sy, &g_depth[j][i], &g_id[j][i]);
        }
    }
}
