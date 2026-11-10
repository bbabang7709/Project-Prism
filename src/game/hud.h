/*
 * 화면 한 장 만들기: 렌더러로 그린 픽셀을 반블록 칸으로 옮기고,
 * 위쪽 상태 줄, 손에 든 것·겨누는 것, 도움말 줄, 알림을 얹은 뒤 터미널에 내보낸다.
 */
#ifndef PRISM_GAME_HUD_H
#define PRISM_GAME_HUD_H

void hud_draw(int frame, int fps);
void hud_toggle_help(void);

#endif
