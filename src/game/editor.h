/*
 * 레벨 에디터: 내려다보기 화면에서 커서로 칸을 골라 블록과 물체를 놓고, 지우고, 돌린다.
 * 바꿀 때마다 빛을 다시 계산해 바로 보여 준다.
 *
 *   방향키      커서 (화면 기준 위아래·좌우)      Tab / ]  다음 도구,  [  이전 도구
 *   Enter / F   놓기          Backspace / X  지우기 (바닥으로)
 *   Q / E       돌리기 (거울 22.5도, 광원 45도)
 *   , / .       카메라 돌리기      + / -  확대·축소
 *   N           이름 바꾸기 (한글 입력 가능)     V  풀이 확인     T  시험 플레이
 *   S           저장 (levels 폴더, 임시 파일 → fsync → rename)
 *   ESC         나가기 (저장하지 않았으면 한 번 더)
 *
 * 편집 중인 방은 g_levels[0]에 두고 load_level로 월드를 만든다. 메뉴로 돌아가면 main이 방 파일을 다시 읽는다.
 */
#ifndef PRISM_GAME_EDITOR_H
#define PRISM_GAME_EDITOR_H

#include "world/world.h"

enum { EDIT_NONE, EDIT_QUIT, EDIT_TEST };

void editor_open(const Level *src);     /* src를 고친다. NULL이면 새 방 */
void editor_resume(void);               /* 시험 플레이에서 돌아왔을 때 */
const Level *editor_level(void);        /* 편집 중인 방 (시험 플레이용) */

int editor_key(int vk);                 /* EDIT_* */
void editor_text(int ch);               /* [, ], 쉼표, 마침표, 이름 입력 */
void editor_draw(int frame);

#endif
