#include "game/game.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include "common/config.h"
#include "common/mathutil.h"
#include "optics/optics.h"
#include "platform/input.h"
#include "world/world.h"

Player g_pl[MAXPLAYER] = {
    { .on = 1, .x = 2.5f, .z = 8.5f, .yaw = PI / 2, .pitch = -0.35f, .fp = 1, .walk_yaw = 0.62f,
      .carry = -1, .tobj = -1, .tx = -1, .tz = -1 },
    { .carry = -1, .tobj = -1, .tx = -1, .tz = -1 },
};
int g_me;

float cam_yaw = 0.62f, cam_yaw_target = 0.62f, cam_pitch = -0.78f, cam_dist = 19;

int g_solved;
int g_moves;
int g_cleared;

/* ---------- 빛 계산과 문 ---------- */

/* 빛을 다시 계산하고(모든 플레이어의 몸이 빛을 가림), 결과에 따라 문을 연다.
 * 문 블록은 빛을 막으므로 순서는 반드시 "빛 계산 → 문 갱신"이다. */
void game_simulate(void)
{
    Body b[MAXPLAYER];
    int n = 0;
    for (int i = 0; i < MAXPLAYER; i++)
        if (g_pl[i].on) b[n++] = (Body){ g_pl[i].x, g_pl[i].z, i };
    g_solved = optics_simulate(b, n);
    world_set_door(g_solved);
}

/* 지금 방의 시작 칸 가운데에, 정해진 방향을 보고 선다.
 * 두 번째 플레이어부터는 시작 칸 둘레의 빈 바닥에 선다 */
static void place_player(int p)
{
    static const int NB[9][2] = { { 0, 0 }, { 0, 1 }, { 0, -1 }, { -1, 0 }, { 1, 0 }, { -1, 1 }, { 1, 1 }, { -1, -1 }, { 1, -1 } };
    const Level *lv = &g_levels[g_cur_level];
    int x = lv->sx, z = lv->sz;
    for (int k = p ? 1 : 0; k < 9; k++) {
        int cx = lv->sx + NB[k][0], cz = lv->sz + NB[k][1];
        if (lv->map[cz][cx] == '.') { x = cx; z = cz; break; }
    }
    Player *pl = &g_pl[p];
    pl->x = (float)x + 0.5f;
    pl->z = (float)z + 0.5f;
    pl->yaw = lv->face == 'E' ? PI / 2 : lv->face == 'W' ? -PI / 2 : lv->face == 'N' ? PI : 0;
    pl->pitch = -0.35f;
    pl->carry = -1;
}

void game_init(int level)
{
    g_cur_level = level;
    load_level();
    for (int i = 0; i < MAXPLAYER; i++)
        if (g_pl[i].on) place_player(i);
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

/* ---------- 알림 ---------- */

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

/* ---------- 플레이어 조작 ---------- */

static void after_change(void)
{
    int was = g_solved;
    game_simulate();
    g_moves++;
    if (g_solved && !was) game_message(-1, "모든 수정이 빛났어요! 문이 열렸습니다 (%d번 조작)", g_moves);
}

static int cell_free(int x, int z)
{
    return x > 0 && z > 0 && x < RW - 1 && z < RD - 1 && g_blk[1][z][x] == B_AIR && !g_objat[z][x];
}

/* 플레이어 몸(원)이 (x, z) 칸과 겹치는가 */
static int body_on_cell(const Player *p, int x, int z)
{
    float qx = clampf(p->x, (float)x, (float)x + 1), qz = clampf(p->z, (float)z, (float)z + 1);
    return (qx - p->x) * (qx - p->x) + (qz - p->z) * (qz - p->z) < PR * PR;
}

/* self의 몸(원)을 (x, z)에 두면 벽, 유리, 물체 칸, 다른 플레이어와 겹치는가 */
static int circle_hits(int self, float x, float z)
{
    for (int cz = (int)floorf(z - PR); cz <= (int)floorf(z + PR); cz++)
        for (int cx = (int)floorf(x - PR); cx <= (int)floorf(x + PR); cx++) {
            int solid = cx < 0 || cz < 0 || cx >= RW || cz >= RD || g_blk[1][cz][cx] != B_AIR || g_objat[cz][cx];
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

/* 겨누기: 1인칭은 화면 가운데 광선(4.5칸까지), 내려다보기는 플레이어 바로 앞 칸 */
void game_find_target(int p)
{
    Player *pl = &g_pl[p];
    pl->tobj = -1;
    pl->tx = pl->tz = -1;
    if (pl->fp) {
        float cp = cosf(pl->pitch), dx = sinf(pl->yaw) * cp, dy = sinf(pl->pitch), dz = cosf(pl->yaw) * cp;
        float ox = pl->x, oy = EYE_Y, oz = pl->z;
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
            if (y == 1 && g_objat[z][x]) { pl->tobj = g_objat[z][x] - 1; return; }
            int b = g_blk[y][z][x];
            if (b != B_AIR) {
                if (b == B_FLOOR && y == 0 && axis == 1 && cell_free(x, z)) { pl->tx = x; pl->tz = z; }
                break;
            }
        }
    } else {
        int x = (int)floorf(pl->x + sinf(pl->yaw) * 0.85f), z = (int)floorf(pl->z + cosf(pl->yaw) * 0.85f);
        if (x < 0 || z < 0 || x >= RW || z >= RD) return;
        if (g_objat[z][x]) pl->tobj = g_objat[z][x] - 1;
        else if (cell_free(x, z)) { pl->tx = x; pl->tz = z; }
    }
    if (pl->tx >= 0)                                      /* 누군가 서 있는 칸에는 못 놓는다 */
        for (int i = 0; i < MAXPLAYER; i++)
            if (g_pl[i].on && body_on_cell(&g_pl[i], pl->tx, pl->tz)) { pl->tx = pl->tz = -1; break; }
}

static void take_or_place(int p)
{
    Player *pl = &g_pl[p];
    if (pl->carry >= 0) {
        if (pl->tx < 0) { game_message(p, "빈 바닥을 겨누고 F를 누르면 내려놓아요"); return; }
        Obj *o = &g_obj[pl->carry];
        o->x = pl->tx; o->z = pl->tz; o->held = 0;
        g_objat[pl->tz][pl->tx] = pl->carry + 1;
        game_message(p, "%s을(를) 내려놓았어요", KIND_NAME[o->kind]);
        pl->carry = -1;
        after_change();
        for (int i = 0; i < MAXPLAYER; i++) if (g_pl[i].on) game_find_target(i);
        return;
    }
    if (pl->tobj < 0) { game_message(p, "물체를 겨누고 F를 누르면 들 수 있어요"); return; }
    Obj *o = &g_obj[pl->tobj];
    if (!o->movable) { game_message(p, "%s은(는) 고정되어 있어요", KIND_NAME[o->kind]); return; }
    g_objat[o->z][o->x] = 0;
    o->held = 1;
    pl->carry = pl->tobj;
    game_message(p, "%s을(를) 들었어요. 빈 바닥을 겨누고 F로 내려놓으세요", KIND_NAME[o->kind]);
    after_change();
    for (int i = 0; i < MAXPLAYER; i++) if (g_pl[i].on) game_find_target(i);
}

static void rotate(int p, int d)
{
    Player *pl = &g_pl[p];
    Obj *o = pl->tobj >= 0 ? &g_obj[pl->tobj] : pl->carry >= 0 ? &g_obj[pl->carry] : NULL;
    if (!o) { game_message(p, "돌릴 거울을 겨누세요"); return; }
    if (o->kind != O_MIRROR && o->kind != O_SPLIT) { game_message(p, "%s은(는) 돌릴 수 없어요", KIND_NAME[o->kind]); return; }
    o->rot = (o->rot + d + 8) % 8;
    after_change();
}

/* 지금 방을 처음 상태로 만들고 모두 시작 칸에 선다 */
static void restart_room(void)
{
    load_level();
    for (int i = 0; i < MAXPLAYER; i++)
        if (g_pl[i].on) place_player(i);
    game_simulate();
    g_moves = 0;
    for (int i = 0; i < MAXPLAYER; i++)
        if (g_pl[i].on) game_find_target(i);
}

/* 열린 문을 지났다: 다음 방으로, 마지막 방이면 끝 */
static void through_door(void)
{
    if (g_cur_level + 1 < g_nlevels) {
        g_cur_level++;
        restart_room();
        game_message(-1, "방 %d/%d: %s", g_cur_level + 1, g_nlevels, g_levels[g_cur_level].name);
    } else if (!g_cleared) {
        g_cleared = 1;
        game_message(-1, "모든 방을 풀었어요! 빛의 신전을 빠져나왔습니다");
    }
}

void game_action(int p, int act)
{
    if (p < 0 || p >= MAXPLAYER || !g_pl[p].on) return;
    switch (act) {
    case ACT_TAKE: take_or_place(p); break;
    case ACT_ROT_LEFT: rotate(p, -1); break;
    case ACT_ROT_RIGHT: rotate(p, 1); break;
    case ACT_RESET: restart_room(); game_message(-1, "처음 상태로 되돌렸어요"); break;
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
    if (pl->carry >= 0) {                                 /* 든 물체는 발밑이나 둘레의 빈 칸에 내려놓는다 */
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

/* ---------- 이 터미널의 키 ---------- */

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

unsigned game_local_keys(void)
{
    return (input_held('W') ? K_W : 0) | (input_held('A') ? K_A : 0) | (input_held('S') ? K_S : 0) | (input_held('D') ? K_D : 0) |
           (input_held(VK_LEFT) ? K_LEFT : 0) | (input_held(VK_RIGHT) ? K_RIGHT : 0) |
           (input_held(VK_UP) ? K_UP : 0) | (input_held(VK_DOWN) ? K_DOWN : 0);
}

/* ---------- 틱 ---------- */

/* 걷기 (벽에 미끄러지며), 몸이 움직이면 빛 다시 계산, 열린 문에 들어가면 다음 방 */
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
    float l = sqrtf(vx * vx + vz * vz);
    if (l > 0) {
        int moved = 0;
        vx = vx / l * 3.0f * dt;
        vz = vz / l * 3.0f * dt;
        if (!circle_hits(p, pl->x + vx, pl->z)) { pl->x += vx; moved = 1; }
        if (!circle_hits(p, pl->x, pl->z + vz)) { pl->z += vz; moved = 1; }
        if (moved) {
            int was = g_solved;
            game_simulate();                              /* 몸이 빛줄기를 가릴 수 있다 */
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
    cam_yaw += (cam_yaw_target - cam_yaw) * 0.25f;        /* 카메라는 부드럽게 돈다 */
    for (int i = 0; i < MAXPLAYER; i++)
        if (g_pl[i].msg_t > 0) g_pl[i].msg_t--;
}

void game_local_tick(void)
{
    cam_yaw += (cam_yaw_target - cam_yaw) * 0.25f;
    if (g_pl[g_me].msg_t > 0) g_pl[g_me].msg_t--;
}

/* ---------- 렌더러와 시험용 ---------- */

void game_view(View *v)
{
    static Body bodies[MAXPLAYER];
    const Player *me = &g_pl[g_me];
    int n = 0;
    for (int i = 0; i < MAXPLAYER; i++)                   /* 1인칭에서는 내 몸은 그리지 않는다 */
        if (g_pl[i].on && !(i == g_me && me->fp)) bodies[n++] = (Body){ g_pl[i].x, g_pl[i].z, i };
    *v = (View){ me->fp, me->x, me->z, me->yaw, me->pitch, cam_yaw, cam_pitch, cam_dist, me->tx, me->tz, me->tobj,
                 g_seg, g_nseg, bodies, n };
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
