#include "game/hud.h"

#include <stdio.h>
#include <string.h>
#include "game/game.h"
#include "optics/optics.h"
#include "platform/bridge.h"
#include "platform/input.h"
#include "platform/terminal.h"
#include "game/ui.h"
#include "renderer/renderer.h"
#include "world/world.h"

static int g_help = 1;                  /* show the help line at the bottom */
static int g_quad = 1;                  /* 1: 2x2 pixels per cell (quadrant blocks), 0: 1x2 pixels (half block) */

void hud_toggle_help(void) { g_help = !g_help; }

/* ---------- performance overlay and dynamic resolution ---------- */

static int g_perf;                      /* P: show the performance overlay */
static int g_autores = 1;               /* lower the render resolution when frames get slow */
static const float RES_STEP[] = { 1.0f, 0.85f, 0.7f, 0.55f };
#define NRES ((int)(sizeof RES_STEP / sizeof RES_STEP[0]))
static int s_res = 0;                   /* index into RES_STEP */
static float s_ema = 0;                 /* smoothed render time (ms) */
static int s_cool = 0;                  /* frames to wait before changing the resolution again */
static uint32_t s_up[PIX_H][PIX_W];     /* upscale scratch */

void hud_toggle_perf(void) { g_perf = !g_perf; }
void hud_set_autores(int on) { g_autores = on; if (!on) s_res = 0; }

/*
 * Keep 30 fps on slow machines or huge windows: the frame budget is 33 ms and the
 * terminal output needs part of it, so aim for about 18 ms of rendering.
 * Too slow for a while -> render fewer pixels; plenty of headroom -> go back up.
 */
static void autores_update(float ms)
{
    s_ema = s_ema == 0 ? ms : s_ema * 0.9f + ms * 0.1f;
    if (!g_autores) return;
    if (s_cool > 0) { s_cool--; return; }
    float k = RES_STEP[s_res] * RES_STEP[s_res];          /* render time scales with the pixel count */
    if (s_ema > 22 && s_res < NRES - 1) { s_res++; s_ema *= RES_STEP[s_res] * RES_STEP[s_res] / k; s_cool = 15; }
    else if (s_res > 0 && s_ema * RES_STEP[s_res - 1] * RES_STEP[s_res - 1] / k < 12) { s_res--; s_ema *= RES_STEP[s_res] * RES_STEP[s_res] / k; s_cool = 60; }
}

/* bilinear upscale of g_pix (sw x sh) to W x H, in place through s_up */
static void upscale(int sw, int sh, int W, int H)
{
    for (int y = 0; y < H; y++) {
        float fy = ((float)y + 0.5f) * (float)sh / (float)H - 0.5f;
        int y0 = fy < 0 ? 0 : (int)fy, y1 = y0 + 1 < sh ? y0 + 1 : sh - 1;
        int wy = (int)((fy - (float)y0) * 256);
        wy = wy < 0 ? 0 : wy > 256 ? 256 : wy;
        int iwy = 256 - wy;
        for (int x = 0; x < W; x++) {
            float fx = ((float)x + 0.5f) * (float)sw / (float)W - 0.5f;
            int x0 = fx < 0 ? 0 : (int)fx, x1 = x0 + 1 < sw ? x0 + 1 : sw - 1;
            int wx = (int)((fx - (float)x0) * 256);
            wx = wx < 0 ? 0 : wx > 256 ? 256 : wx;
            int iwx = 256 - wx;
            uint32_t a = g_pix[y0][x0], b = g_pix[y0][x1], c = g_pix[y1][x0], d = g_pix[y1][x1], out = 0;
            for (int s = 0; s < 24; s += 8) {
                int v = ((int)(a >> s & 255) * iwx + (int)(b >> s & 255) * wx) * iwy + ((int)(c >> s & 255) * iwx + (int)(d >> s & 255) * wx) * wy;
                out |= (uint32_t)(v >> 16) << s;
            }
            s_up[y][x] = out;
        }
    }
    for (int y = 0; y < H; y++) memcpy(g_pix[y], s_up[y], sizeof(uint32_t) * (size_t)W);
}

/* the overlay: what the engine spends each frame on, drawn over the top-left of the view */
static void draw_perf(int fps)
{
    const RenderStats *rs = renderer_stats();
    int bytes, changed, cells;
    term_stats(&bytes, &changed, &cells);
    char line[160];
    int y = 3, n = rs->nthreads, rows = 6 + (n + 3) / 4;   /* row 2 is for messages */
    uint32_t bg = 0x14161E, fg = 0xD8DCE8, dim = 0x8A90A0;
    for (int r = y; r < y + rows && r < g_rows - 3; r++)
        for (int c = 0; c < 58 && c < g_cols; c++) g_back[r][c] = (VCell){ fg, bg, " ", 0 };
    term_text(1, y++, 0xFFD27A, bg, "성능  (P 닫기)   %d fps, 틱 33.3ms", fps);
    snprintf(line, sizeof line, "렌더 %.1fms  %d×%d 픽셀  해상도 %d%%%s", rs->frame_ms, rs->W, rs->H,
             (int)(RES_STEP[s_res] * 100 + 0.5f), g_autores ? " (자동)" : "");
    term_text(1, y++, fg, bg, "%s", line);
    term_text(1, y++, dim, bg, "스레드 %d개   작업 / 바리어 대기 (ms)", n);
    for (int i = 0; i < n; i += 4) {
        int x = 2;
        for (int k = i; k < i + 4 && k < n; k++)
            x += term_text(x, y, fg, bg, "#%d %4.1f/%-4.1f  ", k, rs->work_ms[k], rs->wait_ms[k]);
        y++;
    }
    term_text(1, y++, fg, bg, "빛 계산 %.2fms  (빛줄기 %d개)", g_sim_us / 1000.0f, g_nseg);
    int full = changed > 0 ? (int)((long long)bytes * cells / changed) : bytes;
    term_text(1, y++, fg, bg, "출력 %.1fKB  바뀐 칸 %d/%d (%d%%)", bytes / 1024.0f, changed, cells, cells ? changed * 100 / cells : 0);
    term_text(1, y++, dim, bg, "전체를 다시 그리면 약 %.0fKB → %d%% 아낌", full / 1024.0f, full ? 100 - bytes * 100 / full : 0);
}
int hud_quad(void) { return g_quad; }
void hud_set_quad(int on) { g_quad = on; }

/* ---------- pixels -> terminal cells ---------- */

/* quadrant block characters: bit 1 top left, 2 top right, 4 bottom left, 8 bottom right are the foreground colour */
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

/* pick the split of four pixels into two colours that fits best (smallest error against each group's mean) */
static void quad_cell(const uint32_t p[4], VCell *v)
{
    int best = 15;
    float best_err = 1e30f, fr = 0, fg = 0, fb = 0, br = 0, bgc = 0, bb = 0;
    for (int m = 1; m < 16; m++) {
        if (m != 15 && (m & 8)) continue;                 /* m and 15-m are the same split: only look at those with the bottom right in the background */
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
            err += dr * dr * 0.3f + dg * dg * 0.59f + db * db * 0.11f;   /* perceived brightness weights */
        }
        if (err < best_err - 1e-3f) {
            best_err = err; best = m;
            fr = s[1][0]; fg = s[1][1]; fb = s[1][2];
            br = s[0][0]; bgc = s[0][1]; bb = s[0][2];
        }
    }
    uint32_t fc = (uint32_t)(fr + 0.5f) << 16 | (uint32_t)(fg + 0.5f) << 8 | (uint32_t)(fb + 0.5f);
    uint32_t bc = best == 15 ? fc : (uint32_t)(br + 0.5f) << 16 | (uint32_t)(bgc + 0.5f) << 8 | (uint32_t)(bb + 0.5f);
    if (best == 15) put_cell(v, QUAD[3], fc, fc);        /* one colour: the same two colours on a half block (printed as a space) */
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
    *paspect = g_quad ? 2.0f : 1.0f;                     /* quadrant pixels are half as wide, so they are tall */
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
    int rw = (int)((float)W * RES_STEP[s_res]), rh = (int)((float)H * RES_STEP[s_res]);
    if (rw < 16 || rh < 16) { rw = W; rh = H; }
    renderer_draw(&view, rw, rh, frame);                  /* fewer pixels when dynamic resolution stepped down */
    autores_update(renderer_stats()->frame_ms);
    if (rw != W || rh != H) upscale(rw, rh, W, H);
    if (me->fp) {                                           /* crosshair (wider when pixels are narrow) */
        int cx = W / 2, cy = H / 2, kx = g_quad ? 4 : 2;
        for (int k = -kx; k <= kx; k++) g_pix[cy][cx + k] = k ? 0xE8E8E8 : 0xFFFFFF;
        for (int k = -2; k <= 2; k++) if (k) g_pix[cy + k][cx] = 0xE8E8E8;
    }
    /* carried-object window (bottom right): the held object rendered in 3D, seen from where you look */
    int iy = g_rows - (g_help ? 3 : 2), ww = 22, wh = 9, wx = g_cols - ww - 1, wy = iy - wh;
    int show_carry = me->carry >= 0 && g_cols >= 90 && wy >= 5;
    if (show_carry) {
        int cw = g_quad ? 2 : 1;
        renderer_icon(me->carry, (wx + 1) * cw, (wy + 1) * 2, (ww - 2) * cw, (wh - 2) * 2, view.paspect, me->fp ? me->yaw : cam_yaw);
    }
    hud_blit(256);

    /* top bar: room, crystals (one diamond each, in its colour, bright when lit), door, view */
    int total, lit = optics_count_lit(&total);
    term_bar(0, UI_BAR);
    int x = ui_chip(0, 0, 0x101420, UI_GOLD, "◆ PRISM") + 1;
    if (g_nlevels > 1) x += term_text(x, 0, UI_INFO, UI_BAR, " 방 %d/%d ", g_cur_level + 1, g_nlevels);
    x += term_text(x, 0, UI_TEXT, UI_BAR, "%s ", g_levels[g_cur_level].name);
    if (g_levels[g_cur_level].coop) x += ui_chip(x, 0, 0xE8DCFF, 0x3A2E58, "2인 협동") + 1;
    x += term_text(x, 0, UI_DIM, UI_BAR, "  수정 ");
    for (int i = 0; i < g_nobj; i++) {
        const Obj *o = &g_obj[i];
        if (o->kind != O_CRYSTAL) continue;
        float k = o->lit ? 255 : 90, a0 = o->lit ? 0 : 40;
        uint32_t c = (uint32_t)(a0 + o->col[0] * k) << 16 | (uint32_t)(a0 + o->col[1] * k) << 8 | (uint32_t)(a0 + o->col[2] * k);
        x += term_text(x, 0, c, UI_BAR, "◆");
    }
    x += term_text(x, 0, UI_DIM, UI_BAR, " %d/%d  ", lit, total);
    x += g_solved ? ui_chip(x, 0, 0x101420, UI_OK, "문 열림") : ui_chip(x, 0, UI_DIM, 0x252A38, "문 닫힘");
    x += term_text(x, 0, UI_DIM, UI_BAR, "  %s", me->fp ? "1인칭" : "내려다보기");
    char right[64];
    snprintf(right, sizeof right, "키 %s · 빛줄기 %d · %2d fps ", input_win32() ? "누름/뗌" : "탭", g_nseg, fps);
    if (x + term_str_w(right) + 2 < g_cols) term_text(g_cols - term_str_w(right), 0, 0x6A7088, UI_BAR, "%s", right);
    if (net) ui_chip(0, 1, UI_INFO, 0x1E2638, "%s", net);       /* network status (left of the second row) */

    /* above the help line: what you hold or aim at, with the keys that act on it */
    const Obj *ao = me->carry >= 0 ? &g_obj[me->carry] : me->tobj >= 0 ? &g_obj[me->tobj] : NULL;
    if (ao) {
        static const char *K1[] = { "F", "여기에 내려놓기", "Q E", "돌리기" }, *K2[] = { "F", "들기", "Q E", "돌리기" };
        int turn = ao->kind == O_MIRROR || ao->kind == O_SPLIT, nk;
        const char *const *keys;
        const char *note = NULL;
        if (me->carry >= 0) { keys = K1; nk = me->tx >= 0 ? (turn ? 2 : 1) : 0; if (me->tx < 0) note = "빈 바닥을 겨누세요"; }
        else if (!ao->movable) { keys = K2; nk = 0; note = "고정됨"; }
        else { keys = K2; nk = turn ? 2 : 1; }
        char name[64];
        snprintf(name, sizeof name, "%s%s", me->carry >= 0 ? "손에 든 것: " : "", KIND_NAME[ao->kind]);
        int w = term_str_w(name) + 3 + ui_keys_w(keys, nk) + (note ? term_str_w(note) + 1 : 0);
        int ix = g_cols / 2 - w / 2;
        ui_fill(ix - 1, iy, w + 1, 1, 0x22252F);
        ix += ui_chip(ix, iy, 0x101420, 0xFFE6A0, "%s", name) + 1;
        if (nk) ix += ui_keys(ix, iy, 0x22252F, keys, nk);
        if (note) term_text(ix, iy, UI_DIM, 0x22252F, "%s", note);
    }
    if (show_carry) {                                     /* frame around the 3D icon */
        const Obj *co = &g_obj[me->carry];
        ui_border(wx, wy, ww, wh, 0x5A6480, UI_PANEL, "손에 든 것");
        if (co->kind == O_MIRROR || co->kind == O_SPLIT)
            term_text(wx + 2, wy + wh - 1, UI_TEXT, UI_PANEL, " %s %.1f° ", KIND_NAME[co->kind], co->rot * 22.5);
        else
            term_text(wx + 2, wy + wh - 1, UI_TEXT, UI_PANEL, " %s ", KIND_NAME[co->kind]);
    }

    /* help line: key caps for the actions (movement keys are on the menu's controls page) */
    if (g_help) {
        term_bar(g_rows - 1, UI_BAR);
        static const char *FP[] = { "F", "들기/놓기", "Q E", "돌리기", "Tab", "내려다보기", "Z", "되돌리기", "I", "힌트", "R", "처음부터", "H", "도움말", "ESC", "메뉴" };
        static const char *FPV[] = { "F", "들기/놓기", "Q E", "돌리기", "Tab", "내려다보기", "V", "마우스", "Z", "되돌리기", "I", "힌트", "R", "처음부터", "ESC", "메뉴" };
        static const char *TOP[] = { "←→", "카메라", "↑↓", "기울기", "+ -", "확대", "F", "들기/놓기", "Q E", "돌리기", "Tab", "1인칭", "Z", "되돌리기", "I", "힌트", "ESC", "메뉴" };
        if (!me->fp) ui_keys(1, g_rows - 1, UI_BAR, TOP, 9);
        else if (bridge_connected()) ui_keys(1, g_rows - 1, UI_BAR, FPV, 8);   /* V only with the mouse helper */
        else ui_keys(1, g_rows - 1, UI_BAR, FP, 8);
    }
    if (g_perf) draw_perf(fps);

    /* message card at the top: green when the room is solved, gold for hints */
    if (me->msg_t > 0) {
        int mw = term_str_w(me->msg) + 6, mx = g_cols / 2 - mw / 2;
        uint32_t accent = g_solved ? UI_OK : me->hint_t > 0 ? UI_GOLD : UI_INFO;
        ui_frame(mx, 2, mw, 3, accent, UI_PANEL, NULL);
        term_text(mx + 1, 3, accent, UI_PANEL, "▌");
        term_text(mx + 3, 3, 0xFFFFFF, UI_PANEL, "%s", me->msg);
    }

    /* end screen: a results card */
    if (g_cleared) {
        int bw = 48, bh = 11, bx = g_cols / 2 - bw / 2, by = g_rows / 2 - bh / 2;
        ui_frame(bx, by, bw, bh, UI_OK, 0x16221C, "끝");
        const char *l1 = "모든 방을 풀었어요!";
        term_text(g_cols / 2 - term_str_w(l1) / 2, by + 2, UI_OK, 0x16221C, "%s", l1);
        int sec = g_ticks / 30;
        term_text(bx + 10, by + 4, UI_DIM, 0x16221C, "걸린 시간");
        term_text(bx + 26, by + 4, 0xFFFFFF, 0x16221C, "%d분 %02d초", sec / 60, sec % 60);
        term_text(bx + 10, by + 5, UI_DIM, 0x16221C, "조작 횟수");
        term_text(bx + 26, by + 5, 0xFFFFFF, 0x16221C, "%d번", g_total_moves);
        term_text(bx + 10, by + 6, UI_DIM, 0x16221C, "푼 방");
        term_text(bx + 26, by + 6, 0xFFFFFF, 0x16221C, "%d개", g_nlevels);
        static const char *K[] = { "Enter", "메뉴로" };
        ui_keys(g_cols / 2 - ui_keys_w(K, 1) / 2, by + 8, 0x16221C, K, 1);
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
