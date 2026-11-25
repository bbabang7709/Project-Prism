/*
 * 화면 한 장 만들기: 렌더러로 그린 픽셀을 반블록 칸으로 옮기고,
 * 위쪽 상태 줄, 손에 든 것·겨누는 것, 도움말 줄, 알림을 얹은 뒤 터미널에 내보낸다.
 */
#ifndef PRISM_GAME_HUD_H
#define PRISM_GAME_HUD_H

void hud_draw(int frame, int fps, const char *net);   /* net: 네트워크 상태 글 (혼자면 NULL) */
void hud_toggle_help(void);
void hud_toggle_perf(void);             /* P: performance overlay (render threads, light, terminal output) */
void hud_set_autores(int on);           /* dynamic resolution on/off (--full-res turns it off) */

/* 화면 방식: 1이면 터미널 한 칸에 2×2 픽셀(사분 블록 ▘▝▖▗ 등), 0이면 1×2 픽셀(반블록 ▀) */
int hud_quad(void);
void hud_set_quad(int on);

/* 지금 화면 방식에서 그릴 픽셀 크기와 픽셀 세로/가로 비 */
void hud_frame_size(int *W, int *H, float *paspect);

/* g_pix를 터미널 칸(g_back)으로 옮긴다. k/256 밝기 (256이면 그대로) */
void hud_blit(int k);

/* 개발용: 터미널 칸(g_back)의 블록 문자를 칸 하나 cw×ch 픽셀로 g_pix에 다시 그린다.
 * 실제 터미널에서 보일 모습을 이미지로 확인할 때 쓴다 */
void hud_rasterize(int cw, int ch);

#endif
