#include "game/hud.h"

#include <stdio.h>
#include <string.h>
#include "game/game.h"
#include "optics/optics.h"
#include "platform/bridge.h"
#include "platform/input.h"
#include "platform/terminal.h"
#include "renderer/renderer.h"
#include "world/world.h"

static int g_help = 1;                  /* 아래쪽 도움말 줄 보이기 */
static int g_quad = 1;                  /* 1: 한 칸에 2×2 픽셀(사분 블록), 0: 1×2 픽셀(반블록) */

void hud_toggle_help(void) { g_help = !g_help; }
int hud_quad(void) { return g_quad; }
void hud_set_quad(int on) { g_quad = on; }

/* ---------- 픽셀 → 터미널 칸 ---------- */

/* 사분 블록 문자: 비트 1 왼쪽 위, 2 오른쪽 위, 4 왼쪽 아래, 8 오른쪽 아래가 글자색 */
static const char *QUAD[16] = {
    " ", "\xE2\x96\x98", "\xE2\x96\x9D", "\xE2\x96\x80", "\xE2\x96\x96", "\xE2\x96\x8C", "\xE2\x96\x9E", "\xE2\x96\x9B",
    "\xE2\x96\x97", "\xE2\x96\x9A", "\xE2\x96\x90", "\xE2\x96\x9C", "\xE2\x96\x84", "\xE2\x96\x99", "\xE2\x96\x9F", "\xE2\x96\x88",
};

static uint32_t scale_k(uint32_t c, int k)
{
    if (k >= 256) return c;
    unsigned r = (c >> 16 & 255) * (unsigned)k >> 8, g = (c >> 8 & 255) * (unsigned)k >> 8, b = (c & 255) * (unsigned)k >> 8;
    return r << 16 | g << 8 | b;
}

static void put_cell(VCell *v, const char *ch, uint32_t fg, uint32_t bg)
{
    size_t n = strlen(ch);
    memcpy(v->ch, ch, n);
    v->ch[n] = 0;
    v->fg = fg;
    v->bg = bg;
    v->wide = 0;
}

/* 네 픽셀을 두 색으로 가장 잘 나누는 모양을 고른다 (나눈 두 무리의 평균색과의 차이가 가장 작은 것) */
static void quad_cell(const uint32_t p[4], VCell *v)
{
    int best = 15;
    float best_err = 1e30f, fr = 0, fg = 0, fb = 0, br = 0, bgc = 0, bb = 0;
    for (int m = 1; m < 16; m++) {
        if (m != 15 && (m & 8)) continue;                 /* m과 15-m은 같은 나눔: 오른쪽 아래가 배경인 쪽만 본다 */
        float s[2][3] = { { 0 } }, n[2] = { 0 };
        for (int i = 0; i < 4; i++) {
            int gi = (m >> i) & 1;
            s[gi][0] += (float)(p[i] >> 16 & 255); s[gi][1] += (float)(p[i] >> 8 & 255); s[gi][2] += (float)(p[i] & 255);
            n[gi]++;
        }
        for (int gi = 0; gi < 2; gi++)
            if (n[gi]) { s[gi][0] /= n[gi]; s[gi][1] /= n[gi]; s[gi][2] /= n[gi]; }
        float err = 0;
        for (int i = 0; i < 4; i++) {
            int gi = (m >> i) & 1;
            float dr = (float)(p[i] >> 16 & 255) - s[gi][0], dg = (float)(p[i] >> 8 & 255) - s[gi][1], db = (float)(p[i] & 255) - s[gi][2];
            err += dr * dr * 0.3f + dg * dg * 0.59f + db * db * 0.11f;   /* 사람 눈 밝기 가중치 */
        }
        if (err < best_err - 1e-3f) {
            best_err = err; best = m;
            fr = s[1][0]; fg = s[1][1]; fb = s[1][2];
            br = s[0][0]; bgc = s[0][1]; bb = s[0][2];
        }
    }
    uint32_t fc = (uint32_t)(fr + 0.5f) << 16 | (uint32_t)(fg + 0.5f) << 8 | (uint32_t)(fb + 0.5f);
    uint32_t bc = best == 15 ? fc : (uint32_t)(br + 0.5f) << 16 | (uint32_t)(bgc + 0.5f) << 8 | (uint32_t)(bb + 0.5f);
    if (best == 15) put_cell(v, QUAD[3], fc, fc);        /* 한 색: ▀에 같은 두 색 (출력할 때 공백으로 줄어든다) */
    else put_cell(v, QUAD[best], fc, bc);
}

void hud_blit(int k)
{
    for (int r = 0; r < g_rows; r++)
        for (int c = 0; c < g_cols; c++) {
            VCell *v = &g_back[r][c];
            if (g_quad) {
                uint32_t p[4] = { scale_k(g_pix[2 * r][2 * c], k), scale_k(g_pix[2 * r][2 * c + 1], k),
                                  scale_k(g_pix[2 * r + 1][2 * c], k), scale_k(g_pix[2 * r + 1][2 * c + 1], k) };
                quad_cell(p, v);
            } else {
                put_cell(v, QUAD[3], scale_k(g_pix[2 * r][c], k), scale_k(g_pix[2 * r + 1][c], k));
            }
        }
}

void hud_frame_size(int *W, int *H, float *paspect)
{
    *W = g_cols * (g_quad ? 2 : 1);
    *H = g_rows * 2;
    *paspect = g_quad ? 2.0f : 1.0f;                     /* 사분 블록 픽셀은 가로가 반이라 세로로 길다 */
}

void hud_draw(int frame, int fps, const char *net)
{
    if (g_cols < 80 || g_rows < 24) {
        for (int r = 0; r < g_rows; r++) term_bar(r, 0x101010);
        term_text(1, 1, 0xFFD060, 0x101010, "창이 작아요: 80x24 이상 (지금 %dx%d)", g_cols, g_rows);
        term_present();
        return;
    }
    const Player *me = &g_pl[g_me];
    int W, H;
    View view;
    game_view(&view);
    hud_frame_size(&W, &H, &view.paspect);
    renderer_draw(&view, W, H, frame);
    if (me->fp) {                                           /* 조준점 (가로 픽셀이 좁으면 가로로 더 길게) */
        int cx = W / 2, cy = H / 2, kx = g_quad ? 4 : 2;
        for (int k = -kx; k <= kx; k++) g_pix[cy][cx + k] = k ? 0xE8E8E8 : 0xFFFFFF;
        for (int k = -2; k <= 2; k++) if (k) g_pix[cy + k][cx] = 0xE8E8E8;
    }
    hud_blit(256);
    int total, lit = optics_count_lit(&total);
    term_bar(0, 0x161822);
    int x = term_text(1, 0, 0xFFD27A, 0x161822, " PRISM ") + 2;
    if (g_nlevels > 1) x += term_text(x, 0, 0xC8CCD8, 0x161822, "방 %d/%d ", g_cur_level + 1, g_nlevels);
    x += term_text(x, 0, 0xC8CCD8, 0x161822, "%s%s  |  수정 %d/%d", g_levels[g_cur_level].name, g_levels[g_cur_level].coop ? " (2인 협동)" : "", lit, total);
    x += term_text(x, 0, g_solved ? 0x7CE07C : 0xC8CCD8, 0x161822, g_solved ? "  |  문 열림" : "  |  문 닫힘");
    x += term_text(x, 0, 0xC8CCD8, 0x161822, "  |  %s", me->fp ? "1인칭" : "내려다보기");
    if (net) term_text(1, 1, 0x9FD4FF, 0x101010, " %s ", net);    /* 네트워크 상태 (둘째 줄 왼쪽) */
    term_text(g_cols - 34, 0, 0x7A8090, 0x161822, "키 %s  빛줄기 %d  %2d fps", input_win32() ? "누름/뗌" : "탭", g_nseg, fps);

    /* 아래쪽: 손에 든 것, 겨누는 것 */
    int iy = g_rows - (g_help ? 3 : 2);
    char info[160];
    if (me->carry >= 0)
        snprintf(info, sizeof info, " 손에 든 것: %s  —  %s ", KIND_NAME[g_obj[me->carry].kind], me->tx >= 0 ? "F: 여기에 내려놓기" : "빈 바닥을 겨누세요");
    else if (me->tobj >= 0)
        snprintf(info, sizeof info, " %s  —  %s ", KIND_NAME[g_obj[me->tobj].kind],
                 !g_obj[me->tobj].movable ? "고정됨" : (g_obj[me->tobj].kind == O_MIRROR || g_obj[me->tobj].kind == O_SPLIT) ? "F: 들기   Q/E: 돌리기" : "F: 들기");
    else
        info[0] = 0;
    if (info[0]) term_text(g_cols / 2 - term_str_w(info) / 2, iy, 0xFFE6A0, 0x22252F, "%s", info);
    if (g_help) {
        term_bar(g_rows - 1, 0x161822);
        term_text(1, g_rows - 1, 0x9AA0B0, 0x161822,
             /* actions only: movement keys are listed on the menu's controls page; V only with the mouse helper */
             me->fp ? (bridge_connected() ? "F 들기/놓기   Q/E·휠 돌리기   Tab 내려다보기   V 마우스   Z 되돌리기   I 힌트   R 처음부터   H 도움말   G 화면   ESC 메뉴"
                                          : "F 들기/놓기   Q/E·휠 돌리기   Tab 내려다보기   Z 되돌리기   I 힌트   R 처음부터   H 도움말   G 화면   ESC 메뉴")
                    : "←→ 카메라   ↑↓ 기울기   +/- 확대   F 들기/놓기   Q/E 돌리기   Tab 1인칭   Z 되돌리기   I 힌트   R 처음부터   ESC 메뉴");
    }
    if (me->msg_t > 0) term_text(g_cols / 2 - term_str_w(me->msg) / 2 - 1, 2, g_solved ? 0x101010 : 0xFFFFFF, g_solved ? 0x7CE07C : 0x2A2F3C, " %s ", me->msg);
    if (g_cleared) {                                      /* 끝 화면: 기록과 안내 */
        int bw = 46, bx = g_cols / 2 - bw / 2, by = g_rows / 2 - 4;
        for (int y = by; y < by + 7; y++)
            for (int x = bx; x < bx + bw; x++) g_back[y][x] = (VCell){ 0xFFFFFF, 0x1C2A22, " ", 0 };
        char line[96];
        int sec = g_ticks / 30;
        const char *l1 = "모든 방을 풀었어요!";
        term_text(g_cols / 2 - term_str_w(l1) / 2, by + 1, 0x7CE07C, 0x1C2A22, "%s", l1);
        snprintf(line, sizeof line, "걸린 시간 %d:%02d   ·   조작 %d번   ·   방 %d개", sec / 60, sec % 60, g_total_moves, g_nlevels);
        term_text(g_cols / 2 - term_str_w(line) / 2, by + 3, 0xE8ECF4, 0x1C2A22, "%s", line);
        const char *l3 = "Enter 또는 ESC: 메뉴로";
        term_text(g_cols / 2 - term_str_w(l3) / 2, by + 5, 0x9AA0B0, 0x1C2A22, "%s", l3);
    }
    term_present();
}

void hud_rasterize(int cw, int ch)
{
    for (int r = 0; r < g_rows; r++)
        for (int c = 0; c < g_cols; c++) {
            const VCell *v = &g_back[r][c];
            int mask = 0;
            for (int m = 0; m < 16; m++)
                if (!strcmp(v->ch, QUAD[m])) { mask = m; break; }
            for (int y = 0; y < ch; y++)
                for (int x = 0; x < cw; x++) {
                    int bit = (y * 2 >= ch ? 4 : 1) << (x * 2 >= cw ? 1 : 0);
                    g_pix[r * ch + y][c * cw + x] = (mask & bit) ? v->fg : v->bg;
                }
        }
}
