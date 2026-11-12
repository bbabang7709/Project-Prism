#include "game/menu.h"

#include <stdio.h>
#include <string.h>
#include "game/game.h"
#include "platform/input.h"
#include "platform/terminal.h"
#include "renderer/renderer.h"
#include "world/world.h"

enum { IT_SOLO, IT_HOST, IT_JOIN, IT_HELP, IT_QUIT, NITEM };

static int s_sel;                       /* 고른 항목 */
static int s_solo[MAXLEVEL], s_nsolo;   /* 혼자 할 수 있는 방 (g_levels 번호) */
static int s_room_solo, s_room_host;    /* 고른 시작 방: s_solo 안의 번호, g_levels 번호 */
static char s_names[MAXLEVEL][64];
static int s_coop[MAXLEVEL], s_nall;
static char s_addr[64];                 /* 참가할 주소 */
static int s_editing;                   /* 주소를 입력하는 중 */
static int s_help;                      /* 조작법 보기 */
static char s_msg[160];
static float s_yaw = 0.62f;             /* 배경 카메라 */

void menu_message(const char *msg) { snprintf(s_msg, sizeof s_msg, "%s", msg ? msg : ""); }

void menu_open(const char *msg)
{
    s_nall = g_nlevels;
    s_nsolo = 0;
    for (int i = 0; i < g_nlevels; i++) {
        memcpy(s_names[i], g_levels[i].name, sizeof s_names[i]);
        s_names[i][sizeof s_names[i] - 1] = 0;
        s_coop[i] = g_levels[i].coop;
        if (!g_levels[i].coop) s_solo[s_nsolo++] = i;
    }
    if (s_room_solo >= s_nsolo) s_room_solo = 0;
    if (s_room_host >= s_nall) s_room_host = 0;
    s_editing = s_help = 0;
    menu_message(msg);
}

MenuChoice menu_key(int vk)
{
    MenuChoice c = { MENU_NONE, 0, NULL };
    if (s_help) {                                         /* 조작법 화면: 아무 키나 닫는다 */
        if (vk == VK_ENTER || vk == VK_ESC || vk == VK_BACK) s_help = 0;
        return c;
    }
    if (s_editing) {
        if (vk == VK_BACK) { size_t n = strlen(s_addr); if (n) s_addr[n - 1] = 0; }
        else if (vk == VK_ESC) s_editing = 0;
        else if (vk == VK_ENTER) {
            s_editing = 0;
            if (s_addr[0]) { c.what = MENU_JOIN; c.addr = s_addr; }
        }
        return c;
    }
    switch (vk) {
    case VK_UP: s_sel = (s_sel + NITEM - 1) % NITEM; break;
    case VK_DOWN: s_sel = (s_sel + 1) % NITEM; break;
    case VK_LEFT:
    case VK_RIGHT: {
        int d = vk == VK_LEFT ? -1 : 1;
        if (s_sel == IT_SOLO && s_nsolo) s_room_solo = (s_room_solo + d + s_nsolo) % s_nsolo;
        if (s_sel == IT_HOST && s_nall) s_room_host = (s_room_host + d + s_nall) % s_nall;
        break;
    }
    case VK_ENTER:
        if (s_sel == IT_SOLO && s_nsolo) { c.what = MENU_SOLO; c.level = s_room_solo; }
        else if (s_sel == IT_SOLO) menu_message("혼자 할 수 있는 방이 없어요");
        else if (s_sel == IT_HOST) { c.what = MENU_HOST; c.level = s_room_host; }
        else if (s_sel == IT_JOIN) { s_editing = 1; menu_message("호스트 주소를 입력하고 Enter (예: 192.168.0.10 또는 127.0.0.1:5462)"); }
        else if (s_sel == IT_HELP) s_help = 1;
        else c.what = MENU_QUIT;
        break;
    case VK_ESC: c.what = MENU_QUIT; break;
    }
    return c;
}

void menu_text(int ch)
{
    if (!s_editing || ch == ' ') return;
    size_t n = strlen(s_addr);
    if (n + 1 < sizeof s_addr) { s_addr[n] = (char)ch; s_addr[n + 1] = 0; }
}

/* ---------- 그리기 ---------- */

static uint32_t dim(uint32_t c, int k)    /* k/256 밝기 */
{
    unsigned r = (c >> 16 & 255) * (unsigned)k >> 8, g = (c >> 8 & 255) * (unsigned)k >> 8, b = (c & 255) * (unsigned)k >> 8;
    return r << 16 | g << 8 | b;
}

/* 가운데 맞춰 글자를 쓴다 */
static void center(int y, uint32_t fg, uint32_t bg, const char *s)
{
    term_text(g_cols / 2 - term_str_w(s) / 2, y, fg, bg, "%s", s);
}

/* 칸 버퍼에 상자를 칠한다 */
static void box(int x0, int y0, int w, int h, uint32_t bg)
{
    for (int y = y0; y < y0 + h && y < g_rows; y++)
        for (int x = x0; x < x0 + w && x < g_cols; x++)
            if (x >= 0 && y >= 0) g_back[y][x] = (VCell){ 0xFFFFFF, bg, " ", 0 };
}

static void draw_help(int x0, int y0, int w)
{
    static const char *LINES[] = {
        "W A S D        걷기",
        "방향키          시선 (내려다보기: ←→ 카메라, ↑↓ 기울기)",
        "F / 클릭       물체 들기, 빈 바닥에 내려놓기",
        "Q E / 휠       겨눈 거울을 22.5도씩 돌리기",
        "Tab            1인칭 ↔ 내려다보기",
        "+ -            내려다보기 확대·축소",
        "R              방을 처음 상태로",
        "H              도움말 줄 숨기기",
        "ESC            메뉴로 (메뉴에서는 끝내기)",
        "",
        "모든 수정에 맞는 색의 빛을 보내면 문이 열립니다.",
        "발판을 누르면 창살이 열리고, 몸은 빛을 가립니다.",
    };
    int n = (int)(sizeof LINES / sizeof LINES[0]);
    box(x0, y0, w, n + 3, 0x1A1E2A);
    term_text(x0 + 2, y0, 0xFFD27A, 0x1A1E2A, "조작법");
    for (int i = 0; i < n; i++) term_text(x0 + 3, y0 + 2 + i, 0xD8DCE8, 0x1A1E2A, "%s", LINES[i]);
}

void menu_draw(int frame)
{
    if (g_cols < 80 || g_rows < 24) {
        for (int r = 0; r < g_rows; r++) term_bar(r, 0x101010);
        term_text(1, 1, 0xFFD060, 0x101010, "창이 작아요: 80x24 이상 (지금 %dx%d)", g_cols, g_rows);
        term_present();
        return;
    }
    /* 배경: 풀린 방을 바깥에서 내려다보며 천천히 돈다 */
    int W = g_cols, H = g_rows * 2;
    View v;
    game_view(&v);
    v.fp = 0;
    v.nbody = 0;
    v.tx = v.tz = v.sel_obj = -1;
    s_yaw += 0.004f;
    v.cam_yaw = s_yaw;
    v.cam_pitch = -0.72f;
    v.cam_dist = 21;
    renderer_draw(&v, W, H, frame);
    static const char HALF[4] = "\xE2\x96\x80";
    for (int r = 0; r < g_rows; r++)
        for (int c = 0; c < W; c++) {
            VCell *cell = &g_back[r][c];
            cell->fg = dim(g_pix[2 * r][c], 120);
            cell->bg = dim(g_pix[2 * r + 1][c], 120);
            memcpy(cell->ch, HALF, 4);
            cell->ch[4] = 0;
            cell->wide = 0;
        }

    int bw = 64 < g_cols - 4 ? 64 : g_cols - 4, bx = g_cols / 2 - bw / 2, top = g_rows / 2 - 10;
    if (top < 1) top = 1;
    box(bx, top, bw, 5, 0x161822);
    center(top + 1, 0xFFD27A, 0x161822, "P  R  I  S  M");
    center(top + 3, 0xA8B0C4, 0x161822, "빛의 신전  ·  터미널 3D 광학 퍼즐");

    int iy = top + 6;
    if (s_help) draw_help(bx, iy, bw);
    else {
        box(bx, iy, bw, NITEM * 2 + 1, 0x1A1E2A);
        for (int i = 0; i < NITEM; i++) {
            int y = iy + 1 + i * 2, on = i == s_sel;
            uint32_t bg = on ? 0x2E3550 : 0x1A1E2A, fg = on ? 0xFFFFFF : 0xC8CCD8;
            for (int x = bx + 1; x < bx + bw - 1; x++) g_back[y][x] = (VCell){ fg, bg, " ", 0 };
            static const char *LABEL[NITEM] = { "혼자 하기", "호스트로 시작 (2인 협동)", "참가하기 (2인 협동)", "조작법", "끝내기" };
            term_text(bx + 3, y, on ? 0xFFD27A : 0x6A7088, bg, on ? "▶" : " ");
            term_text(bx + 6, y, fg, bg, "%s", LABEL[i]);
            char val[96] = "";
            if (i == IT_SOLO && s_nsolo) {
                int lv = s_solo[s_room_solo];
                snprintf(val, sizeof val, "◀ 방 %d  %.40s ▶", s_room_solo + 1, s_names[lv]);
            } else if (i == IT_HOST && s_nall) {
                snprintf(val, sizeof val, "◀ 방 %d  %.34s%s ▶", s_room_host + 1, s_names[s_room_host], s_coop[s_room_host] ? " (2인)" : "");
            } else if (i == IT_JOIN) {
                snprintf(val, sizeof val, "[%-21.21s%s]", s_addr, s_editing && (frame / 8) % 2 ? "_" : " ");
            }
            if (val[0]) term_text(bx + bw - 2 - term_str_w(val), y, on ? 0x9FD4FF : 0x8088A0, bg, "%s", val);
        }
    }
    term_bar(g_rows - 1, 0x161822);
    term_text(1, g_rows - 1, 0x9AA0B0, 0x161822, "%s",
              s_editing ? "글자 입력   Backspace 지우기   Enter 접속   ESC 취소"
              : s_help ? "Enter / ESC 닫기"
                       : "↑↓ 고르기   ←→ 시작 방 바꾸기   Enter 시작   ESC 끝내기");
    if (s_msg[0]) center(g_rows - 3, 0xFFE6A0, 0x22252F, s_msg);
    term_present();
}
