#include "game/game.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include "common/config.h"
#include "common/mathutil.h"
#include "optics/optics.h"
#include "platform/input.h"
#include "world/world.h"

float g_px = 2.5f, g_pz = 8.5f, g_pyaw = PI / 2, g_ppitch = -0.35f;
int g_fp = 1;
int g_carry = -1;
int g_tobj = -1, g_tx = -1, g_tz = -1;

float cam_yaw = 0.62f, cam_yaw_target = 0.62f, cam_pitch = -0.78f, cam_dist = 19;

int g_solved;
char g_msg[160];
int g_msg_t;
static int g_moves;                     /* 들기·놓기·돌리기 횟수 */

/* ---------- 빛 계산과 문 ---------- */

/* 빛을 다시 계산하고(플레이어 몸이 빛을 가림), 결과에 따라 문을 연다.
 * 문 블록은 빛을 막으므로 순서는 반드시 "빛 계산 → 문 갱신"이다. */
void game_simulate(void)
{
    g_solved = optics_simulate(g_px, g_pz);
    world_set_door(g_solved);
}

void game_init(void)
{
    load_level();
    game_simulate();
}

/* 회전 가능한 물체의 모든 각도를 시험해 푸는 방법을 찾는다 (시험과 스크린샷용) */
int game_solve(void)
{
    int idx[8], k = 0;
    for (int i = 0; i < g_nobj && k < 8; i++)
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

/* ---------- 플레이어 조작 ---------- */

void game_message(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_msg, sizeof g_msg, fmt, ap);
    va_end(ap);
    g_msg_t = 90;
}

static void after_change(void)
{
    int was = g_solved;
    game_simulate();
    g_moves++;
    if (g_solved && !was) game_message("모든 수정이 빛났어요! 문이 열렸습니다 (%d번 조작)", g_moves);
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
void game_find_target(void)
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

void game_act(void)
{
    if (g_carry >= 0) {
        if (g_tx < 0) { game_message("빈 바닥을 겨누고 F를 누르면 내려놓아요"); return; }
        Obj *o = &g_obj[g_carry];
        o->x = g_tx; o->z = g_tz; o->held = 0;
        g_objat[g_tz][g_tx] = g_carry + 1;
        game_message("%s을(를) 내려놓았어요", KIND_NAME[o->kind]);
        g_carry = -1;
        after_change();
        game_find_target();
        return;
    }
    if (g_tobj < 0) { game_message("물체를 겨누고 F를 누르면 들 수 있어요"); return; }
    Obj *o = &g_obj[g_tobj];
    if (!o->movable) { game_message("%s은(는) 고정되어 있어요", KIND_NAME[o->kind]); return; }
    g_objat[o->z][o->x] = 0;
    o->held = 1;
    g_carry = g_tobj;
    game_message("%s을(를) 들었어요. 빈 바닥을 겨누고 F로 내려놓으세요", KIND_NAME[o->kind]);
    after_change();
    game_find_target();
}

void game_rotate(int d)
{
    Obj *o = g_tobj >= 0 ? &g_obj[g_tobj] : g_carry >= 0 ? &g_obj[g_carry] : NULL;
    if (!o) { game_message("돌릴 거울을 겨누세요"); return; }
    if (o->kind != O_MIRROR && o->kind != O_SPLIT) { game_message("%s은(는) 돌릴 수 없어요", KIND_NAME[o->kind]); return; }
    o->rot = (o->rot + d + 8) % 8;
    after_change();
}

static void reset_room(void)
{
    load_level();
    g_carry = -1;
    g_px = 2.5f; g_pz = 8.5f; g_pyaw = PI / 2; g_ppitch = -0.35f;
    game_simulate();
    g_moves = 0;
    game_find_target();
    game_message("처음 상태로 되돌렸어요");
}

void game_press(int vk)
{
    switch (vk) {
    case VK_TAB: g_fp = !g_fp; game_message(g_fp ? "1인칭 시점" : "내려다보기 시점 (←→ 카메라 회전)"); break;
    case 'F': game_act(); break;
    case 'Q': game_rotate(-1); break;
    case 'E': game_rotate(1); break;
    case 'R': reset_room(); break;
    case VK_PLUS: cam_dist = fmaxf(10, cam_dist - 1.5f); break;
    case VK_MINUS: cam_dist = fminf(30, cam_dist + 1.5f); break;
    case VK_LEFT: if (!g_fp) cam_yaw_target -= PI / 4; break;
    case VK_RIGHT: if (!g_fp) cam_yaw_target += PI / 4; break;
    case VK_UP: if (!g_fp) cam_pitch = fmaxf(-1.35f, cam_pitch - 0.08f); break;
    case VK_DOWN: if (!g_fp) cam_pitch = fminf(-0.3f, cam_pitch + 0.08f); break;
    }
}

/* 걷기 (벽에 미끄러지며), 몸이 움직이면 빛 다시 계산, 겨누기 갱신 */
static void player_tick(float dt)
{
    float f = (float)(input_held('W') - input_held('S')), s = (float)(input_held('D') - input_held('A')), vx, vz;
    if (g_fp) {
        g_pyaw += (float)(input_held(VK_RIGHT) - input_held(VK_LEFT)) * 2.2f * dt;
        g_ppitch = clampf(g_ppitch + (float)(input_held(VK_UP) - input_held(VK_DOWN)) * 1.6f * dt, -1.3f, 0.9f);
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
            game_simulate();                              /* 몸이 빛줄기를 가릴 수 있다 */
            if (g_solved && !was) game_message("모든 수정이 빛났어요! 문이 열렸습니다");
        }
    }
    game_find_target();
}

void game_tick(float dt)
{
    player_tick(dt);
    cam_yaw += (cam_yaw_target - cam_yaw) * 0.25f;        /* 카메라는 부드럽게 돈다 */
    if (g_msg_t > 0) g_msg_t--;
}

/* ---------- 렌더러와 시험용 ---------- */

void game_view(View *v)
{
    *v = (View){ g_fp, g_px, g_pz, g_pyaw, g_ppitch, cam_yaw, cam_pitch, cam_dist, g_tx, g_tz, g_tobj, g_seg, g_nseg };
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
        else if (o->movable)
            printf("  %s (%d,%d) rot %d\n", o->kind == O_MIRROR ? "mirror" : o->kind == O_SPLIT ? "splitter" : o->kind == O_PRISM ? "prism" : "filter",
                   o->x, o->z, o->rot);
    }
}
