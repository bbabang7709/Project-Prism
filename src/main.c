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
#include "optics/optics.h"
#include "renderer/renderer.h"
#include "platform/terminal.h"
#include "world/world.h"

/* 퍼즐 상태: 모든 수정이 켜졌는가 */
static int g_solved;

/* 플레이어: 방 안을 걸어 다니며 물체를 들고, 놓고, 돌린다. 몸이 빛을 가린다 */
static float g_px = 2.5f, g_pz = 8.5f, g_pyaw = PI / 2, g_ppitch = -0.35f;
static int g_fp = 1;           /* 1: 1인칭, 0: 내려다보기 */
static int g_carry = -1;       /* 들고 있는 물체 */
static int g_tobj = -1, g_tx = -1, g_tz = -1;   /* 겨누는 물체, 놓을 수 있는 칸 */

/* ---------- 빛 계산과 문 ---------- */

/* 빛을 다시 계산하고(플레이어 몸이 빛을 가림), 결과에 따라 문을 연다.
 * 문 블록은 빛을 막으므로 순서는 반드시 "빛 계산 → 문 갱신"이다. */
static void simulate(void)
{
    g_solved = optics_simulate(g_px, g_pz);
    world_set_door(g_solved);
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

/* ---------- 카메라 ---------- */

static float cam_yaw = 0.62f, cam_yaw_target = 0.62f, cam_pitch = -0.78f, cam_dist = 19;

/* 지금 게임 상태로 한 프레임을 그린다 */
static void render_pixels(int W, int H, int frame)
{
    View v = { g_fp, g_px, g_pz, g_pyaw, g_ppitch, cam_yaw, cam_pitch, cam_dist, g_tx, g_tz, g_tobj, g_seg, g_nseg };
    renderer_draw(&v, W, H, frame);
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
    int total, lit = optics_count_lit(&total);
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
    int nthreads = renderer_init();

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
            renderer_write_ppm(shot, W, H, 3);
            printf("wrote %s (%dx%d px, %.1f ms with %d threads)\n", shot, W, H, ms, nthreads);
        }
        renderer_shutdown();
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
    renderer_shutdown();
    close(tfd);
    return 0;
}
