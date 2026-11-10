#include "game/hud.h"

#include <stdio.h>
#include <string.h>
#include "game/game.h"
#include "optics/optics.h"
#include "platform/input.h"
#include "platform/terminal.h"
#include "renderer/renderer.h"
#include "world/world.h"

static int g_help = 1;                  /* 아래쪽 도움말 줄 보이기 */

void hud_toggle_help(void) { g_help = !g_help; }

void hud_draw(int frame, int fps)
{
    if (g_cols < 80 || g_rows < 24) {
        for (int r = 0; r < g_rows; r++) term_bar(r, 0x101010);
        term_text(1, 1, 0xFFD060, 0x101010, "창이 작아요: 80x24 이상 (지금 %dx%d)", g_cols, g_rows);
        term_present();
        return;
    }
    int W = g_cols, H = g_rows * 2;
    View view;
    game_view(&view);
    renderer_draw(&view, W, H, frame);
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
    term_text(g_cols - 34, 0, 0x7A8090, 0x161822, "키 %s  빛줄기 %d  %2d fps", input_win32() ? "누름/뗌" : "탭", g_nseg, fps);

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
