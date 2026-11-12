/*
 * 터미널 입력: 표준 입력 바이트를 키·마우스 사건으로 바꾼다.
 *
 * - Windows Terminal의 win32 입력 모드(ESC [ Vk;Sc;Uc;Kd;Cs;Rc _)에서는 누름과 뗌을 그대로 안다.
 * - 다른 터미널은 키 반복으로 "누르고 있음"을 흉내 낸다 (처음 200ms, 반복 중 90ms 유지).
 *   걷기(WASD)만 누르고 있는 동안 한 번 실행하고, 나머지 키는 누를 때마다 실행한다.
 *
 * 게임 함수를 직접 부르지 않고, input_init에 넘긴 콜백으로 알린다.
 * 키 번호는 Windows 가상 키 코드를 쓴다 (글자는 대문자 'A'..'Z').
 */
#ifndef PRISM_PLATFORM_INPUT_H
#define PRISM_PLATFORM_INPUT_H

#include <stdint.h>

enum {
    VK_BACK = 0x08, VK_TAB = 0x09, VK_ENTER = 0x0D, VK_ESC = 0x1B,
    VK_LEFT = 0x25, VK_UP = 0x26, VK_RIGHT = 0x27, VK_DOWN = 0x28,
    VK_PLUS = 0xBB, VK_MINUS = 0xBD,
};

typedef struct {
    void (*press)(int vk);    /* 키를 눌렀다 */
    void (*click)(void);      /* 마우스 왼쪽 클릭 */
    void (*wheel)(int dir);   /* 마우스 휠: 위 -1, 아래 +1 */
    void (*quit)(void);       /* Ctrl+C */
    void (*text)(int ch);     /* 글자 하나 (ASCII 32~126, 입력란용, NULL이면 안 씀) */
} InputHandlers;

void input_init(const InputHandlers *h);
void input_set_time(int64_t now_ms);   /* 이번 틱의 시각. 읽기와 input_held 전에 부른다 */
void input_read(void);                 /* 표준 입력에서 읽을 수 있는 만큼 읽고 사건을 알린다 */
int input_held(int vk);                /* 지금 누르고 있는가 */
int input_esc_expired(void);           /* ESC 한 바이트만 60ms 넘게 남았으면 그 바이트를 쓰고 1 (ESC 키 단독) */
int input_win32(void);                 /* win32 입력 모드를 받았는가 */

#endif
