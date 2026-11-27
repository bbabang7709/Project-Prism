#include "game/menu.h"

#include <stdio.h>
#include <string.h>
#include "game/game.h"
#include "game/hud.h"
#include "game/progress.h"
#include "platform/input.h"
#include "platform/terminal.h"
#include "game/ui.h"
#include "renderer/renderer.h"
#include "world/world.h"

enum { IT_SOLO, IT_HOST, IT_JOIN, IT_EDIT, IT_HELP, IT_QUIT, NITEM };

static int s_sel;                       /* selected item */
static int s_solo[MAXLEVEL], s_nsolo;   /* rooms playable solo (g_levels indices) */
static int s_room_solo, s_room_host;    /* chosen start room: index into s_solo, g_levels index */
static int s_room_edit;                 /* room to edit: g_levels index, s_nall = new room */
static char s_names[MAXLEVEL][64];
static int s_coop[MAXLEVEL], s_nall;
static int s_best[MAXLEVEL];            /* fewest moves per room (-1 if unsolved) */
static int s_opened;                    /* first time the menu opens (start at the first unsolved room) */
static char s_addr[64];                 /* address to join */
static int s_editing;                   /* typing the address */
static int s_help;                      /* showing the controls */
static char s_msg[160];
static float s_yaw = 0.62f;             /* background camera */

void menu_message(const char *msg) { snprintf(s_msg, sizeof s_msg, "%s", msg ? msg : ""); }

void menu_open(const char *msg)
{
    s_nall = g_nlevels;
    s_nsolo = 0;
    for (int i = 0; i < g_nlevels; i++) {
        memcpy(s_names[i], g_levels[i].name, sizeof s_names[i]);
        s_names[i][sizeof s_names[i] - 1] = 0;
        s_coop[i] = g_levels[i].coop;
        s_best[i] = progress_best(g_levels[i].file);
        if (!g_levels[i].coop) s_solo[s_nsolo++] = i;
    }
    if (!s_opened) {                                      /* continue: solo starts at the first unsolved room */
        s_opened = 1;
        for (int k = 0; k < s_nsolo; k++)
            if (s_best[s_solo[k]] < 0) { s_room_solo = k; break; }
    }
    if (s_room_solo >= s_nsolo) s_room_solo = 0;
    if (s_room_host >= s_nall) s_room_host = 0;
    if (s_room_edit > s_nall) s_room_edit = s_nall;
    s_editing = s_help = 0;
    menu_message(msg);
}

MenuChoice menu_key(int vk)
{
    MenuChoice c = { MENU_NONE, 0, NULL };
    if (s_help) {                                         /* controls page: any key closes it */
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
        if (s_sel == IT_EDIT) s_room_edit = (s_room_edit + d + s_nall + 1) % (s_nall + 1);
        break;
    }
    case VK_ENTER:
        if (s_sel == IT_SOLO && s_nsolo) { c.what = MENU_SOLO; c.level = s_room_solo; }
        else if (s_sel == IT_SOLO) menu_message("혼자 할 수 있는 방이 없어요");
        else if (s_sel == IT_HOST) { c.what = MENU_HOST; c.level = s_room_host; }
        else if (s_sel == IT_EDIT) { c.what = MENU_EDIT; c.level = s_room_edit < s_nall ? s_room_edit : -1; }
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
    if (!s_editing || ch == ' ' || ch > 126) return;     /* addresses are ASCII only */
    size_t n = strlen(s_addr);
    if (n + 1 < sizeof s_addr) { s_addr[n] = (char)ch; s_addr[n + 1] = 0; }
}

/* ---------- drawing ---------- */

/* controls page: key caps and what they do */
static void draw_help(int x0, int y0, int w)
{
    static const char *ROWS[][2] = {
        { "W A S D", "걷기 (Shift를 누르고 있으면 달리기)" },
        { "Space", "점프" },
        { "방향키", "시선 (내려다보기: ←→ 카메라, ↑↓ 기울기)" },
        { "F / 클릭", "물체 들기, 빈 바닥에 내려놓기" },
        { "Q E / 휠", "겨눈 거울을 22.5도씩 돌리기" },
        { "Tab  + -", "1인칭 ↔ 내려다보기 / 확대·축소" },
        { "Z  R  I", "되돌리기 / 처음 상태로 / 힌트 (안내자가 길 안내)" },
        { "V", "마우스 시선 (Windows mouselock 도우미)" },
        { "H  G  P", "도움말 줄 / 화면 방식 / 성능 표시" },
        { "ESC", "메뉴로 (메뉴에서는 끝내기)" },
    };
    int n = (int)(sizeof ROWS / sizeof ROWS[0]);
    ui_frame(x0, y0, w, n + 5, 0x4A5470, UI_PANEL, "조작법");
    for (int i = 0; i < n; i++) {
        term_text(x0 + 3, y0 + 2 + i, 0xFFFFFF, UI_KEY, " %-8s ", ROWS[i][0]);
        term_text(x0 + 15, y0 + 2 + i, UI_TEXT, UI_PANEL, "%s", ROWS[i][1]);
    }
    term_text(x0 + 3, y0 + n + 3, UI_DIM, UI_PANEL, "모든 수정에 알맞은 색의 빛을 보내면 문이 열립니다.");
}

void menu_draw(int frame)
{
    if (g_cols < 80 || g_rows < 24) {
        for (int r = 0; r < g_rows; r++) term_bar(r, 0x101010);
        term_text(1, 1, 0xFFD060, 0x101010, "창이 작아요: 80x24 이상 (지금 %dx%d)", g_cols, g_rows);
        term_present();
        return;
    }
    /* background: the solved room seen from outside, slowly turning */
    int W, H;
    View v;
    game_view(&v);
    hud_frame_size(&W, &H, &v.paspect);
    v.fp = 0;
    v.nbody = 0;
    v.tx = v.tz = v.sel_obj = -1;
    s_yaw += 0.004f;
    v.cam_yaw = s_yaw;
    v.cam_pitch = -0.9f;
    v.cam_dist = 18;
    renderer_draw(&v, W, H, frame);
    hud_blit(110);                                       /* darker background */

    int bw = 66 < g_cols - 4 ? 66 : g_cols - 4, bx = g_cols / 2 - bw / 2, top = g_rows / 2 - 11;
    if (top < 1) top = 1;

    /* title card: the logo in spectrum colours, as a prism splits white light */
    ui_frame(bx, top, bw, 5, 0x4A5470, UI_BAR, NULL);
    ui_logo(g_cols / 2 - 15, top + 1, UI_BAR, frame);
    const char *sub = " 빛의 신전 · 터미널 3D 광학 퍼즐 ";
    term_text(g_cols / 2 - term_str_w(sub) / 2, top + 4, 0xA8B0C4, UI_BAR, "%s", sub);
    int solved = 0;
    for (int i = 0; i < s_nsolo; i++) solved += s_best[s_solo[i]] >= 0;
    if (s_nsolo) ui_chip(bx + bw - 15, top, solved == s_nsolo ? 0x101420 : UI_INFO, solved == s_nsolo ? UI_OK : UI_BAR, "푼 방 %d/%d", solved, s_nsolo);

    int iy = top + 5;
    if (s_help) draw_help(bx, iy, bw);
    else {
        static const char *LABEL[NITEM] = { "혼자 하기", "호스트로 시작", "참가하기", "방 만들기", "조작법", "끝내기" };
        static const char *DESC[NITEM] = {
            "혼자서 방을 차례로 풀어요. ←→로 시작 방을 고릅니다",
            "2인 협동: 다른 터미널이나 컴퓨터가 참가할 때까지 기다려요",
            "2인 협동: 호스트 주소를 입력하고 Enter",
            "레벨 에디터: 방을 고치거나 새로 만들어요",
            "모든 키와 규칙 보기",
            "게임을 끝내요",
        };
        ui_frame(bx, iy, bw, NITEM * 2 + 2, 0x4A5470, UI_PANEL, NULL);
        for (int i = 0; i < NITEM; i++) {
            int y = iy + 1 + i * 2, on = i == s_sel;
            uint32_t bg = on ? UI_PANEL_HI : UI_PANEL, fg = on ? 0xFFFFFF : 0xC8CCD8;
            ui_fill(bx + 1, y, bw - 2, 1, bg);
            if (on) term_text(bx + 1, y, UI_GOLD, bg, "▌");
            term_text(bx + 3, y, on ? UI_GOLD : 0x6A7088, bg, on ? "▶" : "·");
            term_text(bx + 6, y, fg, bg, "%s", LABEL[i]);
            if (i == IT_HOST || i == IT_JOIN) term_text(bx + 6 + term_str_w(LABEL[i]) + 1, y, 0xB8A8E8, bg, "2인");
            char val[96] = "";
            if (i == IT_SOLO && s_nsolo) {
                int lv = s_solo[s_room_solo];
                char done[32] = "";                       /* solved room: fewest moves */
                if (s_best[lv] >= 0) snprintf(done, sizeof done, "  ✓ %d번", s_best[lv]);
                snprintf(val, sizeof val, "◀ 방 %d  %.30s%s ▶", s_room_solo + 1, s_names[lv], done);
            } else if (i == IT_HOST && s_nall) {
                snprintf(val, sizeof val, "◀ 방 %d  %.34s%s ▶", s_room_host + 1, s_names[s_room_host], s_coop[s_room_host] ? " (2인)" : "");
            } else if (i == IT_EDIT) {
                if (s_room_edit < s_nall) snprintf(val, sizeof val, "◀ 방 %d  %.34s 고치기 ▶", s_room_edit + 1, s_names[s_room_edit]);
                else snprintf(val, sizeof val, "◀ 새 방 ▶");
            } else if (i == IT_JOIN) {
                snprintf(val, sizeof val, "[%-21.21s%s]", s_addr, s_editing && (frame / 8) % 2 ? "_" : " ");
            }
            if (val[0]) term_text(bx + bw - 2 - term_str_w(val), y, on ? UI_INFO : 0x8088A0, bg, "%s", val);
        }
        term_text(bx + 2, iy + NITEM * 2 + 1, UI_DIM, UI_PANEL, " %s ", DESC[s_sel]);   /* what the selected item does */
    }
    term_bar(g_rows - 1, UI_BAR);
    static const char *KE[] = { "글자", "입력", "Backspace", "지우기", "Enter", "접속", "ESC", "취소" };
    static const char *KH[] = { "Enter", "닫기", "ESC", "닫기" };
    static const char *KM[] = { "↑↓", "고르기", "←→", "시작 방 바꾸기", "Enter", "시작", "ESC", "끝내기" };
    if (s_editing) ui_keys(1, g_rows - 1, UI_BAR, KE, 4);
    else if (s_help) ui_keys(1, g_rows - 1, UI_BAR, KH, 2);
    else ui_keys(1, g_rows - 1, UI_BAR, KM, 4);
    if (s_msg[0]) ui_center(g_rows - 2, 0xFFE6A0, 0x22252F, s_msg);
    else if (g_cols < 160)                                /* screen resolution comes from the terminal's cell count */
        ui_center(g_rows - 2, UI_INFO, UI_BAR, " 더 선명하게 보려면 창을 최대화하거나 Ctrl + - 로 글자를 줄이세요 ");
    term_present();
}
