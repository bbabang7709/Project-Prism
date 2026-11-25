#include "renderer/raycast.h"

#include <math.h>
#include <string.h>
#include "common/mathutil.h"
#include "world/world.h"

/* 프레임 정보와 결과 버퍼. 원본처럼 이 파일의 전역 배열로 두어야
 * 컴파일러가 버퍼 쓰기와 Frame 읽기가 겹치지 않음을 알고, 원본과 같은 계산 순서를 만든다. */
Frame g_fr;
uint32_t g_pix[PIX_H][PIX_W];
float g_depth[PIX_H][PIX_W];
uint16_t g_id[PIX_H][PIX_W];

/* 프레임마다 복사해 두는 빛줄기 선분과 플레이어 몸들.
 * 원본처럼 모듈 안 배열로 읽어야 -ffast-math에서 계산 결과가 픽셀 단위까지 같다. */
#define MAXBODY 8
static Seg s_seg[MAXSEG];
static int s_nseg;
static Body s_body[MAXBODY];
static int s_nbody;
static float s_body_oy[MAXBODY];        /* ray origin height used for each body (camera y minus jump height) */
static float s_glow[MAXOBJ][6];         /* 주변을 물들이는 새 장치들: x, z, 세기, r, g, b */
static int s_nglow;

void raycast_prepare(const Seg *seg, int nseg, const Body *body, int nbody, float cam_y)
{
    s_nseg = nseg < 0 ? 0 : nseg > MAXSEG ? MAXSEG : nseg;
    memcpy(s_seg, seg, sizeof *seg * (size_t)s_nseg);
    s_nbody = nbody < 0 ? 0 : nbody > MAXBODY ? MAXBODY : nbody;
    memcpy(s_body, body, sizeof *body * (size_t)s_nbody);
    /* a jumping body is tested by moving the ray origin down by its height, once per frame,
     * so the per-pixel body test stays exactly the same for bodies on the floor */
    for (int i = 0; i < s_nbody; i++) s_body_oy[i] = s_body[i].y > 0 ? cam_y - s_body[i].y : cam_y;
    s_nglow = 0;
    for (int i = 0; i < g_nobj; i++) {
        const Obj *o = &g_obj[i];
        if (o->held || !(o->kind == O_PORTAL || (o->kind == O_SENSOR && o->lit))) continue;
        float k = o->kind == O_PORTAL ? 0.3f : 0.6f;
        float *gl = s_glow[s_nglow++];
        gl[0] = (float)o->x + 0.5f; gl[1] = (float)o->z + 0.5f; gl[2] = k;
        gl[3] = o->col[0]; gl[4] = o->col[1]; gl[5] = o->col[2];
    }
}

/* 픽셀 하나가 덮는 구간 [e - fw/2, e + fw/2]이 띠 [a, b]와 겹치는 비율.
 * 가는 선을 픽셀 크기(fw)에 맞춰 칠한다: 선이 픽셀보다 가늘면 끊기지 않고 옅어진다 */
static inline float band(float e, float a, float b, float fw)
{
    float h = fmaxf(fw * 0.5f, 1e-4f), lo = fmaxf(e - h, a), hi = fminf(e + h, b);
    return clampf((hi - lo) / (2 * h), 0, 1);
}

static inline float mixf(float a, float b, float k) { return a + (b - a) * k; }

/* 플레이어별 색: 몸, 머리 (0번은 원래 색) */
static const float BODY_COL[2][2][3] = {
    { { 0.26f, 0.22f, 0.36f }, { 0.85f, 0.42f, 0.17f } },
    { { 0.18f, 0.34f, 0.30f }, { 0.95f, 0.78f, 0.30f } },
};
static const float GUIDE_COL[2][3] = { { 0.55f, 1.00f, 1.05f }, { 0.95f, 1.05f, 1.10f } };   /* hint guide: pale glowing cyan */

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

/* 물체 모양: 칸 가운데 바닥 기준 좌표 (x, y, z). mat에 재질 번호.
 * 위에서 내려다봐도 종류를 알 수 있게: 모든 물체는 둥근 받침 위에 있고(윗면에 종류별 색 고리),
 * 거울 판은 두껍고 윗면이 밝아서 위에서 보면 거울 방향이 밝은 막대로 보인다 */
enum { M_BASE, M_SILVER, M_FRAME, M_GLASSY, M_PRISM, M_FILTER, M_BODY, M_NOZZLE, M_GEM, M_EYE, M_RING, M_CORE, M_CRATE };

#define BASE_R 0.42f                    /* 받침 반지름 */
#define BASE_H 0.10f                    /* 받침 높이 */

static float obj_sdf(const Obj *o, float x, float y, float z, int *mat)
{
    float d = fmaxf(sqrtf(x * x + z * z) - BASE_R, fabsf(y - BASE_H / 2) - BASE_H / 2), e;
    *mat = M_BASE;
    switch (o->kind) {
    case O_MIRROR:
    case O_SPLIT: {
        float a = (float)o->rot * PI / 8, c = cosf(a), s = sinf(a);
        float u = x * c + z * s, w = -x * s + z * c;
        e = sd_box(u, y - 0.56f, w, 0.40f, 0.33f, 0.065f);       /* 거울 판: 양 끝 세로 틀만 구리색 */
        if (e < d) { d = e; *mat = fabsf(u) > 0.35f ? M_FRAME : o->kind == O_MIRROR ? M_SILVER : M_GLASSY; }
        e = sd_box(u, y - 0.17f, w, 0.05f, 0.08f, 0.05f);         /* 받침과 판을 잇는 기둥 */
        if (e < d) { d = e; *mat = M_FRAME; }
        break;
    }
    case O_PRISM: {                                               /* 세모기둥 (안쪽 반지름 0.22) */
        float t = -1e9f;
        for (int k = 0; k < 3; k++) {
            float a = PI / 2 + (float)k * 2 * PI / 3;
            t = fmaxf(t, x * cosf(a) + z * sinf(a) - 0.22f);
        }
        e = fmaxf(t, fabsf(y - 0.46f) - 0.36f);
        if (e < d) { d = e; *mat = M_PRISM; }
        break;
    }
    case O_FILTER:
        /* 색유리 상자: 어느 방향의 빛이든 지나가며 걸러진다 (광학과 같은 모양). 네 모서리 기둥과 위아래 띠는 틀 */
        e = sd_box(x, y - 0.50f, z, 0.28f, 0.32f, 0.28f);
        if (e < d) { d = e; *mat = ((fabsf(x) > 0.23f && fabsf(z) > 0.23f) || fabsf(y - 0.50f) > 0.27f) ? M_FRAME : M_FILTER; }
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
        e = (fabsf(x) + fabsf(y - 0.60f) * 0.7f + fabsf(z) - 0.38f) * 0.57f;
        if (e < d) { d = e; *mat = M_GEM; }
        break;
    case O_SENSOR:                                        /* 감지기: 짧은 기둥 위의 둥근 렌즈 (빛줄기 높이를 덮는다) */
        e = fmaxf(sqrtf(x * x + z * z) - 0.17f, fabsf(y - 0.23f) - 0.13f);
        if (e < d) { d = e; *mat = M_BODY; }
        e = sqrtf(x * x + (y - 0.50f) * (y - 0.50f) + z * z) - 0.19f;
        if (e < d) { d = e; *mat = M_EYE; }
        break;
    case O_PORTAL: {                                      /* 순간이동 고리: 가로 고리와 천천히 도는 세로 고리, 가운데 빛나는 구슬 */
        float yy = y - 0.55f, a = (float)g_fr.frame * 0.06f, c = cosf(a), s = sinf(a), u = x * c + z * s, w = -x * s + z * c;
        float q = sqrtf(x * x + z * z) - 0.27f;
        e = sqrtf(q * q + yy * yy) - 0.045f;
        if (e < d) { d = e; *mat = M_RING; }
        q = sqrtf(u * u + yy * yy) - 0.27f;
        e = sqrtf(q * q + w * w) - 0.035f;
        if (e < d) { d = e; *mat = M_RING; }
        e = sqrtf(x * x + yy * yy + z * z) - 0.11f;
        if (e < d) { d = e; *mat = M_CORE; }
        e = fmaxf(sqrtf(x * x + z * z) - 0.035f, fabsf(y - 0.20f) - 0.12f);
        if (e < d) { d = e; *mat = M_FRAME; }
        break;
    }
    case O_BOX:                                           /* 상자: 나무 상자, 모서리는 구리 띠 (광학의 BOX_HALF와 같은 크기) */
        e = sd_box(x, y - 0.42f, z, 0.30f, 0.32f, 0.30f);
        if (e < d) {
            d = e;
            *mat = (fabsf(x) > 0.25f) + (fabsf(z) > 0.25f) + (fabsf(y - 0.42f) > 0.27f) >= 2 ? M_FRAME : M_CRATE;
        }
        break;
    }
    return d;
}

/* 표면 한 점이 받는 빛: 은은한 위쪽 조명 + 빛줄기 주변의 색 + 켜진 수정 */
static void surface_light(float px, float py, float pz, float nx, float ny, float nz, float *lr, float *lg, float *lb)
{
    /* 기본 조명을 밝게 해서 물체 모양과 색이 빛줄기 없이도 보이게 하고,
     * 빛줄기·수정이 주변을 물들이는 범위는 좁게 해서 물체를 뿌옇게 덮지 않게 한다 */
    float ndl = fmaxf(0, nx * 0.35f + ny * 0.85f + nz * 0.4f);
    float base = 0.34f + 0.50f * ndl;
    *lr = base; *lg = base; *lb = base * 1.06f;
    for (int i = 0; i < s_nseg; i++) {
        const Seg *s = &s_seg[i];
        float ex = s->x1 - s->x0, ez = s->z1 - s->z0, l2 = ex * ex + ez * ez;
        float t = l2 > 0 ? clampf(((px - s->x0) * ex + (pz - s->z0) * ez) / l2, 0, 1) : 0;
        float dx = px - (s->x0 + ex * t), dy = py - BY, dz = pz - (s->z0 + ez * t);
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > 2) continue;
        float f = 1 / (1 + d2 / 0.08f);
        f *= f * 0.9f;
        *lr += s->r * f; *lg += s->g * f; *lb += s->b * f;
    }
    for (int i = 0; i < g_nobj; i++) {
        const Obj *o = &g_obj[i];
        if (o->kind != O_CRYSTAL && o->kind != O_EMIT) continue;
        float k = o->kind == O_EMIT ? 0.35f : o->lit ? 1.1f : 0.08f;
        float dx = px - ((float)o->x + 0.5f), dy = py - 1.6f, dz = pz - ((float)o->z + 0.5f);
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > 4) continue;
        float f = k / (1 + d2 / 0.3f);
        f *= f;
        *lr += o->col[0] * f; *lg += o->col[1] * f; *lb += o->col[2] * f;
    }
    for (int i = 0; i < s_nglow; i++) {                   /* 켜진 감지기, 순간이동 고리 */
        const float *gl = s_glow[i];
        float dx = px - gl[0], dy = py - 1.55f, dz = pz - gl[1];
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > 4) continue;
        float f = gl[2] / (1 + d2 / 0.3f);
        f *= f;
        *lr += gl[3] * f; *lg += gl[4] * f; *lb += gl[5] * f;
    }
}

/* 바닥의 빛 자국: 빛줄기는 바닥보다 BY - 1만큼 떠 있어서 비스듬히 보면 지나는 칸과 어긋나 보인다.
 * 빛줄기 바로 아래 바닥에 가는 줄을 더해 실제로 지나는 칸을 알 수 있게 한다 */
static void floor_trace(float px, float pz, float fw, float *lr, float *lg, float *lb)
{
    float w = fmaxf(0.05f, 0.6f * fw), lim = 9 * w * w, kw = 0.05f / w;   /* 픽셀보다 가늘지 않게, 넓힌 만큼 옅게 */
    for (int i = 0; i < s_nseg; i++) {
        const Seg *s = &s_seg[i];
        float ex = s->x1 - s->x0, ez = s->z1 - s->z0, l2 = ex * ex + ez * ez;
        float t = l2 > 0 ? clampf(((px - s->x0) * ex + (pz - s->z0) * ez) / l2, 0, 1) : 0;
        float dx = px - (s->x0 + ex * t), dz = pz - (s->z0 + ez * t), d2 = dx * dx + dz * dz;
        if (d2 > lim) continue;
        float f = 0.55f * kw * expf(-d2 / (w * w));
        *lr += s->r * f; *lg += s->g * f; *lb += s->b * f;
    }
}

/* ---------- ambient occlusion and contact shadows ---------- */

/* blocks at body height that darken the floor next to them */
static inline int ao_solid(int x, int z)
{
    if ((unsigned)x >= RW || (unsigned)z >= RD) return 1;
    int b = g_blk[1][z][x];
    return b == B_WALL || b == B_DOOR || b == B_GATE;
}

/* falloff from an occluding edge: 1 at the edge, 0 at distance r */
static inline float ao_fall(float d, float r)
{
    float k = 1 - d / r;
    return k > 0 ? k * k : 0;
}

/* How much the floor point (hx, hz) in cell (x, z) is occluded, 0..1:
 * walls on the four sides and inner corners, plus soft contact shadows
 * under objects and player bodies. Only reads the voxel grid and lists. */
static float floor_occlusion(float hx, float hz, int x, int z)
{
    const float R = 0.5f;
    float u = hx - (float)x, v = hz - (float)z, occ = 0;
    int l = ao_solid(x - 1, z), r = ao_solid(x + 1, z), n = ao_solid(x, z - 1), s = ao_solid(x, z + 1);
    if (l) occ += ao_fall(u, R);
    if (r) occ += ao_fall(1 - u, R);
    if (n) occ += ao_fall(v, R);
    if (s) occ += ao_fall(1 - v, R);
    if (!l && !n && ao_solid(x - 1, z - 1)) occ += ao_fall(sqrtf(u * u + v * v), R);
    if (!r && !n && ao_solid(x + 1, z - 1)) occ += ao_fall(sqrtf((1 - u) * (1 - u) + v * v), R);
    if (!l && !s && ao_solid(x - 1, z + 1)) occ += ao_fall(sqrtf(u * u + (1 - v) * (1 - v)), R);
    if (!r && !s && ao_solid(x + 1, z + 1)) occ += ao_fall(sqrtf((1 - u) * (1 - u) + (1 - v) * (1 - v)), R);
    for (int dz = -1; dz <= 1; dz++)                         /* objects stand on a base of radius ~0.4 */
        for (int dx = -1; dx <= 1; dx++) {
            int ox = x + dx, oz = z + dz;
            if ((unsigned)ox >= RW || (unsigned)oz >= RD || !g_objat[oz][ox]) continue;
            float ex = hx - ((float)ox + 0.5f), ez = hz - ((float)oz + 0.5f);
            occ += 0.7f * ao_fall(fmaxf(sqrtf(ex * ex + ez * ez) - 0.3f, 0), 0.32f);
        }
    for (int i = 0; i < s_nbody; i++) {                      /* player bodies, radius 0.2 */
        float ex = hx - s_body[i].x, ez = hz - s_body[i].z;
        float sh = 0.6f * ao_fall(fmaxf(sqrtf(ex * ex + ez * ez) - 0.12f, 0), 0.3f);
        if (s_body[i].y > 0) sh *= fmaxf(0, 1 - s_body[i].y * 1.2f);   /* shadow fades while jumping */
        occ += sh;
    }
    return fminf(occ, 1);
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

/* 화면 광선과 빛줄기 선분 사이의 가장 가까운 거리. *tc에 광선 위 가장 가까운 점까지의 거리 */
static float ray_seg_dist(float ox, float oy, float oz, float dx, float dy, float dz, float tmax, const Seg *s, float *tc)
{
    float ax = s->x0, ay = BY, az = s->z0, ex = s->x1 - s->x0, ez = s->z1 - s->z0;
    float rx = ox - ax, ry = oy - ay, rz = oz - az;
    float a = dx * dx + dy * dy + dz * dz, e = ex * ex + ez * ez, f = ex * rx + ez * rz;
    float c = dx * rx + dy * ry + dz * rz, b = dx * ex + dz * ez, den = a * e - b * b;
    float t = den > 1e-8f ? clampf((b * f - c * e) / den, 0, tmax) : 0;
    float u = e > 1e-8f ? clampf((b * t + f) / e, 0, 1) : 0;
    t = clampf((b * u - c) / a, 0, tmax);
    float qx = ox + dx * t - (ax + ex * u), qy = oy + dy * t - ay, qz = oz + dz * t - (az + ez * u);
    *tc = t;
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
    float tp = s_nbody ? player_hit(&s_body[0], ox, s_body_oy[0], oz, dx, dy, dz, &pnx, &pny, &pnz, &phead) : 1e9f;
    if (s_nbody) pwho = s_body[0].who;
    for (int i = 1; i < s_nbody; i++) {                    /* 다른 몸: 더 가까우면 바꾼다 */
        float nx2 = 0, ny2 = 0, nz2 = 0;
        int head2 = 0;
        float t2 = player_hit(&s_body[i], ox, s_body_oy[i], oz, dx, dy, dz, &nx2, &ny2, &nz2, &head2);
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
                const float *col = pwho == BODY_GUIDE ? GUIDE_COL[phead] : BODY_COL[pwho & 1][phead];
                if (pwho == BODY_GUIDE) {                                        /* the guide glows on its own */
                    float rim = 1 - fabsf(pnx * dx + pny * dy + pnz * dz);           /* brighter toward the silhouette */
                    layer(&acc, col[0] * lr * 150 + 50 + 120 * rim, col[1] * lg * 150 + 120 + 110 * rim, col[2] * lb * 150 + 130 + 110 * rim, 1);
                } else
                    layer(&acc, col[0] * lr * 255, col[1] * lg * 255, col[2] * lb * 255, 1);
                thit = tp;
                *zdep = tp * (dx * F->fx + dy * F->fy + dz * F->fz);
                *id = (uint16_t)(99 - pwho);
                break;
            }
            int b = g_blk[y][z][x];
            if (b == B_WALL && y >= 2 && !F->fp && near_wall(F, x, z)) b = B_AIR;          /* 카메라 쪽 벽은 잘라서 안이 보이게 */
            if (b == B_FLOOR || b == B_WALL || b == B_DOOR || b >= B_PLATE) {
                float hx = ox + dx * tcell, hy = oy + dy * tcell, hz = oz + dz * tcell;
                if (axis < 0) {                                                  /* 상자 경계에서 바로 맞음: 가장 가까운 면 */
                    float fx = hx - floorf(hx), fy = hy - floorf(hy), fz = hz - floorf(hz);
                    float m = fminf(fminf(fminf(fx, 1 - fx), fminf(fy, 1 - fy)), fminf(fz, 1 - fz));
                    axis = (m == fx || m == 1 - fx) ? 0 : (m == fy || m == 1 - fy) ? 1 : 2;
                }
                float nx = axis == 0 ? (float)-sx : 0, ny = axis == 1 ? (float)-sy : 0, nz = axis == 2 ? (float)-sz : 0;
                float u = axis == 0 ? hz - floorf(hz) : hx - floorf(hx), v = axis == 1 ? hz - floorf(hz) : hy - floorf(hy);
                /* 이 점에서 픽셀 하나가 표면 위에 덮는 길이 (비스듬할수록 길다) */
                float fws = tcell * F->pix / fmaxf(fabsf(dx * nx + dy * ny + dz * nz), 0.2f);
                float edge = fminf(fminf(u, 1 - u), fminf(v, 1 - v));    /* 칸 가장자리까지 거리 */
                float ar, ag, ab, hl = 0;
                if (b == B_FLOOR) {
                    float k = ((x + z) & 1) ? 1.0f : 0.86f;
                    k *= 1 - 0.4f * band(edge, -0.04f, 0.04f, fws);       /* 칸 경계선 */
                    ar = 0.26f * k; ag = 0.27f * k; ab = 0.32f * k;
                    if (x == F->tx && z == F->tz && axis == 1) hl = band(edge, 0, 0.09f, fws);
                } else if (b == B_DOOR) {
                    float c = band(fminf(fabsf(u - 0.5f), fabsf(v - 0.5f)), -0.04f, 0.04f, fws);
                    ar = mixf(0.42f, 0.9f, c); ag = mixf(0.30f, 0.75f, c); ab = mixf(0.18f, 0.35f, c);
                } else if (b == B_PLATE || b == B_PLATE_ON) {                    /* 발판: 구리색 판, 눌리면 초록 */
                    float c = axis != 1 ? 1 : band(edge, -0.12f, 0.12f, fws);    /* 테두리 */
                    int ch = g_levels[g_cur_level].chan[z][x] & 3;
                    if (b == B_PLATE_ON) { ar = 0.30f; ag = 0.95f; ab = 0.45f; }
                    else { ar = 0.80f; ag = 0.52f; ab = 0.24f; }
                    if (ch) {                                                    /* 연결 번호가 있으면 판과 테두리를 그 색으로 */
                        const float *cc = CHAN_COL[ch];
                        if (b == B_PLATE) { ar = cc[0] * 0.85f; ag = cc[1] * 0.85f; ab = cc[2] * 0.85f; }
                        ar = mixf(ar, cc[0] * 0.45f, c); ag = mixf(ag, cc[1] * 0.45f, c); ab = mixf(ab, cc[2] * 0.45f, c);
                    } else {
                        ar = mixf(ar, 0.22f, c); ag = mixf(ag, 0.22f, c); ab = mixf(ab, 0.27f, c);
                    }
                    if (x == F->tx && z == F->tz && axis == 1) hl = band(edge, 0, 0.09f, fws);
                } else if (b == B_GATE) {                                        /* 창살: 세로 막대 */
                    int ch = g_levels[g_cur_level].chan[z][x] & 3;
                    ar = 0.34f; ag = 0.50f; ab = 0.66f;
                    if (ch) { ar = CHAN_COL[ch][0] * 0.6f; ag = CHAN_COL[ch][1] * 0.6f; ab = CHAN_COL[ch][2] * 0.6f; }
                    if (ny == 0) {
                        float f = fmodf(u * 4 + 0.25f, 1);
                        float c = band(fminf(fabsf(f - 0.25f), fminf(fabsf(f - 1.25f), fabsf(f + 0.75f))) / 4, -0.0625f, 0.0625f, fws);
                        ar = mixf(ar, 0.12f, c); ag = mixf(ag, 0.17f, c); ab = mixf(ab, 0.24f, c);
                        float h = band(fabsf(v - 0.5f), -0.05f, 0.05f, fws);
                        if (ch) { ar = mixf(ar, CHAN_COL[ch][0], h); ag = mixf(ag, CHAN_COL[ch][1], h); ab = mixf(ab, CHAN_COL[ch][2], h); }
                        else { ar = mixf(ar, 0.55f, h); ag = mixf(ag, 0.72f, h); ab = mixf(ab, 0.90f, h); }
                    }
                } else if (b == B_HATCH) {                                       /* 전달 창구: 나무 판에 노란 줄무늬 테두리 */
                    float c = axis != 1 ? 1 : band(edge, -0.10f, 0.10f, fws);
                    float st = band(fmodf(hx + hz + 8, 0.5f), 0, 0.25f, fws);     /* 테두리의 빗금 */
                    ar = mixf(0.55f, mixf(0.20f, 0.95f, st), c); ag = mixf(0.38f, mixf(0.18f, 0.78f, st), c); ab = mixf(0.22f, mixf(0.15f, 0.20f, st), c);
                    if (x == F->tx && z == F->tz && axis == 1) hl = band(edge, 0, 0.09f, fws);
                } else {
                    float k = ny > 0 ? 1.15f : 1.0f;
                    if (ny == 0) {                                               /* 벽돌 줄눈 (가로 0.5칸, 세로 1칸마다, 줄마다 반 칸 어긋남) */
                        float vv = hy - floorf(hy), row = floorf(hy * 2);
                        float uu = (axis == 0 ? hz : hx) + ((int)row % 2) * 0.5f;
                        float e1 = fmodf(vv * 2, 1) * 0.5f, e2 = fmodf(uu, 1);
                        float c = fmaxf(band(e1, 0, 0.035f, fws) + band(e1, 0.5f, 0.535f, fws),
                                        band(e2, 0, 0.04f, fws) + band(e2, 1, 1.04f, fws));
                        k *= 1 - 0.3f * fminf(c, 1);
                        if (y == 1) k *= 1 - 0.35f * ao_fall(hy - 1, 0.45f);   /* darker where the wall meets the floor */
                    }
                    ar = 0.33f * k; ag = 0.33f * k; ab = 0.40f * k;
                }
                if (y == 0 && axis == 1 && ny > 0) {                             /* floor level: ambient occlusion */
                    float o = 1 - 0.5f * floor_occlusion(hx, hz, x, z);
                    ar *= o; ag *= o; ab *= o;
                }
                /* 표시할 칸(게임: 놓을 칸, 에디터: 커서)의 윗면 테두리. 바닥이 아닌 벽·문 윗면도 표시한다 */
                if (hl == 0 && x == F->tx && z == F->tz && axis == 1 && ny > 0) hl = band(edge, 0, 0.09f, fws);
                float lr, lg, lb;
                surface_light(hx + nx * 0.01f, hy + ny * 0.01f, hz + nz * 0.01f, nx, ny, nz, &lr, &lg, &lb);
                if (axis == 1 && ny > 0 && y == 0) floor_trace(hx, hz, fws, &lr, &lg, &lb);   /* 빛줄기 바로 아래 바닥 */
                layer(&acc, mixf(ar * lr * 255, 255, hl), mixf(ag * lg * 255, 205, hl), mixf(ab * lb * 255, 80, hl), 1);   /* hl: 물체를 놓을 칸 */
                thit = tcell;
                *zdep = tcell * (dx * F->fx + dy * F->fy + dz * F->fz);
                *id = (uint16_t)(b * 8 + axis * 2 + (nx + ny + nz > 0) + 1);
                break;
            }
            if (b == B_GLASS) {                                                  /* 유리 블록: 옅게 비치고 모서리는 밝게 */
                float hx = ox + dx * tcell, hy = oy + dy * tcell, hz = oz + dz * tcell, fw = tcell * F->pix;
                float fx = hx - floorf(hx), fy = hy - floorf(hy), fz = hz - floorf(hz);
                float c0 = band(fminf(fx, 1 - fx), -0.05f, 0.05f, fw), c1 = band(fminf(fy, 1 - fy), -0.05f, 0.05f, fw),
                      c2 = band(fminf(fz, 1 - fz), -0.05f, 0.05f, fw);
                /* 맞은 면 방향은 늘 1에 가깝다: 나머지 둘 중 큰 값이 모서리에 가까운 정도 */
                float e = c0 + c1 + c2 - fmaxf(c0, fmaxf(c1, c2)) - fminf(c0, fminf(c1, c2));
                layer(&acc, mixf(120, 190, e), mixf(190, 235, e), mixf(230, 255, e), mixf(0.14f, 0.55f, e));
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
                    case M_BASE: {                                            /* 움직일 수 있는 물체는 구리색 받침 */
                        if (o->movable) { ar = 0.58f; ag = 0.38f; ab = 0.22f; spec = 0.3f; }
                        else { ar = 0.24f; ag = 0.25f; ab = 0.30f; }
                        if (ny > 0.7f) {                                      /* 윗면의 종류별 색 고리 (스스로 조금 빛난다) */
                            float rc = sqrtf(px * px + pz * pz), c = band(rc, 0.30f, 0.40f, t * F->pix);
                            float qr, qg, qb;
                            switch (o->kind) {
                            case O_MIRROR: qr = 0.85f; qg = 0.92f; qb = 1.0f; break;
                            case O_SPLIT: qr = 0.35f; qg = 0.90f; qb = 1.0f; break;
                            case O_PRISM: qr = 1.0f; qg = 1.0f; qb = 1.0f; break;
                            case O_EMIT: qr = 1.0f; qg = 0.82f; qb = 0.35f; break;
                            default: {                                        /* 필터, 수정: 자기 색 (꺼진 수정은 옅게) */
                                float k = (o->kind == O_CRYSTAL || o->kind == O_SENSOR) && !o->lit ? 0.55f : 1.0f;
                                qr = o->col[0] * k + 0.08f; qg = o->col[1] * k + 0.08f; qb = o->col[2] * k + 0.08f;
                            }
                            }
                            ar = mixf(ar, qr, c); ag = mixf(ag, qg, c); ab = mixf(ab, qb, c);
                            er = qr * 70 * c; eg = qg * 70 * c; eb = qb * 70 * c;
                        }
                        break;
                    }
                    case M_FRAME: ar = 0.80f; ag = 0.58f; ab = 0.30f; spec = 0.5f; break;
                    case M_SILVER: ar = 0.88f; ag = 0.93f; ab = 1.0f; spec = 1; er = 45; eg = 48; eb = 55; break;       /* 거울: 밝은 은색 */
                    case M_GLASSY: ar = 0.40f; ag = 0.88f; ab = 1.0f; spec = 0.7f; er = 15; eg = 50; eb = 60; break;   /* 반투명 거울: 하늘색 */
                    case M_PRISM: {
                        float a = atan2f(nz, nx);
                        ar = 0.78f + 0.22f * cosf(a); ag = 0.78f + 0.22f * cosf(a + 2.1f); ab = 0.82f + 0.18f * cosf(a + 4.2f);
                        er = eg = eb = 40;
                        spec = 0.8f;
                        break;
                    }
                    case M_FILTER: ar = o->col[0] * 0.8f; ag = o->col[1] * 0.8f; ab = o->col[2] * 0.9f; er = o->col[0] * 90; eg = o->col[1] * 90; eb = o->col[2] * 110; break;
                    case M_BODY: ar = 0.30f; ag = 0.31f; ab = 0.38f; break;
                    case M_NOZZLE: er = 255; eg = 250; eb = 235; ar = ag = ab = 0; break;
                    case M_GEM: {
                        float k = o->lit ? 1.0f : 0.5f, flick = o->lit ? 0.85f + 0.15f * sinf((float)F->frame * 0.15f + (float)x) : 1;
                        ar = o->col[0] * k * 0.6f + 0.05f; ag = o->col[1] * k * 0.6f + 0.05f; ab = o->col[2] * k * 0.6f + 0.05f;
                        float em = o->lit ? 200 * flick : 45;
                        er = o->col[0] * em; eg = o->col[1] * em; eb = o->col[2] * em;
                        spec = 0.6f;
                        break;
                    }
                    case M_EYE:                                                  /* 감지기 렌즈: 꺼지면 어두운 유리, 켜지면 연결 번호의 색으로 밝게 */
                        if (o->lit) { ar = o->col[0] * 0.6f + 0.3f; ag = o->col[1] * 0.6f + 0.3f; ab = o->col[2] * 0.6f + 0.3f; er = o->col[0] * 235; eg = o->col[1] * 235; eb = o->col[2] * 235; }
                        else { ar = 0.10f; ag = 0.20f; ab = 0.22f; er = 10; eg = 28; eb = 30; }
                        spec = 0.9f;
                        break;
                    case M_RING: ar = 0.62f; ag = 0.30f; ab = 0.95f; er = 110; eg = 40; eb = 170; spec = 0.5f; break;
                    case M_CORE: {
                        float fl = 0.85f + 0.15f * sinf((float)F->frame * 0.2f + (float)z);
                        ar = ag = ab = 0; er = 235 * fl; eg = 175 * fl; eb = 255 * fl;
                        break;
                    }
                    case M_CRATE: {                                              /* 나뭇결: 높이에 따라 옅은 줄무늬 */
                        float k = 0.88f + 0.12f * sinf(py * 55 + (float)(x * 3 + z));
                        ar = 0.62f * k; ag = 0.42f * k; ab = 0.24f * k;
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
    if (F->fp && thit < 1e8f) {                              /* first person: faint distance haze for depth */
        float f = fminf(fmaxf((thit - 5) / 28, 0), 0.28f);
        acc.r = mixf(acc.r, bgr, f); acc.g = mixf(acc.g, bgg, f); acc.b = mixf(acc.b, bgb, f);
    }

    /* 빛줄기: 화면 광선과 가까울수록 밝게 (가려진 부분은 thit에서 끊김) */
    float tm = thit < 1e8f ? thit : 200;
    for (int i = 0; i < s_nseg; i++) {
        const Seg *s = &s_seg[i];
        float tc, d = ray_seg_dist(ox, oy, oz, dx, dy, dz, tm, s, &tc);
        /* 빛줄기 심은 픽셀보다 가늘어지지 않게 넓히고, 넓힌 만큼 옅게 한다 (멀리서 끊겨 보이지 않게) */
        /* 후광은 좁고 옅게: 넓게 번지면 빛줄기 근처의 물체가 뿌옇게 덮인다 */
        float fw = tc * F->pixl, sc = fmaxf(0.045f, 0.6f * fw), sh = fmaxf(0.10f, 1.3f * fw);
        if (d > fmaxf(0.32f, 3 * sh)) continue;
        float core = expf(-(d * d) / (sc * sc)), halo = expf(-(d * d) / (sh * sh));
        float k = core * 1.4f * sqrtf(0.045f / sc) + halo * 0.28f * (0.10f / sh);
        acc.r += (s->r * 255 * 0.85f + core * 120) * k;
        acc.g += (s->g * 255 * 0.85f + core * 120) * k;
        acc.b += (s->b * 255 * 0.85f + core * 120) * k;
    }
    return pack(acc.r, acc.g, acc.b);
}

void raycast_rows(int first, int step)
{
    const Frame *F = &g_fr;
    float aspect = F->aspect;
    for (int j = first; j < F->H; j += step) {
        float sy = (1 - 2 * ((float)j + 0.5f) / (float)F->H) * F->tanh * aspect;
        for (int i = 0; i < F->W; i++) {
            float sx = (2 * ((float)i + 0.5f) / (float)F->W - 1) * F->tanh;
            g_pix[j][i] = trace(F, F->fx + F->rx * sx + F->ux * sy, F->fy + F->ry * sx + F->uy * sy,
                                F->fz + F->rz * sx + F->uz * sy, &g_depth[j][i], &g_id[j][i]);
        }
    }
}

/* (fi, fj) 위치(픽셀 단위, 소수 가능)로 쏘는 광선 */
static uint32_t trace_at(const Frame *F, float fi, float fj)
{
    float aspect = F->aspect;
    float sy = (1 - 2 * fj / (float)F->H) * F->tanh * aspect, sx = (2 * fi / (float)F->W - 1) * F->tanh, zd;
    uint16_t id;
    return trace(F, F->fx + F->rx * sx + F->ux * sy, F->fy + F->ry * sx + F->uy * sy, F->fz + F->rz * sx + F->uz * sy, &zd, &id);
}

/* 경계 픽셀인가: 물체 번호가 이웃과 다르거나 깊이가 크게 다르다 */
static int is_edge(int i, int j, int W, int H)
{
    static const int D[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    uint16_t id = g_id[j][i];
    float z = g_depth[j][i];
    for (int k = 0; k < 4; k++) {
        int ii = i + D[k][0], jj = j + D[k][1];
        if (ii < 0 || jj < 0 || ii >= W || jj >= H) continue;
        float zn = g_depth[jj][ii];
        if (g_id[jj][ii] != id || zn > z * 1.04f || z > zn * 1.04f) return 1;
    }
    return 0;
}

void raycast_aa_rows(int first, int step)
{
    /* 회전 격자 4점: 가로·세로 경계 모두에서 서로 다른 위치를 본다 */
    static const float OFS[4][2] = { { 0.375f, 0.125f }, { 0.875f, 0.375f }, { 0.625f, 0.875f }, { 0.125f, 0.625f } };
    const Frame *F = &g_fr;
    uint32_t row[PIX_W];
    for (int j = first; j < F->H; j += step) {
        int any = 0;
        for (int i = 0; i < F->W; i++) {
            row[i] = g_pix[j][i];
            if (!is_edge(i, j, F->W, F->H)) continue;
            unsigned r = 0, g = 0, b = 0;
            for (int k = 0; k < 4; k++) {
                uint32_t c = trace_at(F, (float)i + OFS[k][0], (float)j + OFS[k][1]);
                r += c >> 16 & 255; g += c >> 8 & 255; b += c & 255;
            }
            row[i] = (r / 4) << 16 | (g / 4) << 8 | b / 4;
            any = 1;
        }
        if (any) memcpy(g_pix[j], row, sizeof row[0] * (size_t)F->W);   /* 다른 스레드가 읽는 g_id, g_depth는 그대로 */
    }
}
