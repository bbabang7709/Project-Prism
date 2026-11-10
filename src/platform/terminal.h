/*
 * 터미널 출력: raw 모드, 화면 칸 버퍼(뒤/앞), 바뀐 칸만 ANSI 24비트 색으로 내보내기,
 * UTF-8과 두 칸 너비 글자(한글) 처리, 창 크기와 시그널.
 *
 * 화면 한 칸(VCell)은 반블록 문자(▀) 하나에 위(fg)·아래(bg) 픽셀 두 개를 담거나,
 * HUD 글자 하나를 담는다. wide: 0 보통, 1 두 칸 글자의 왼쪽, 2 그 오른쪽(출력 안 함).
 */
#ifndef PRISM_PLATFORM_TERMINAL_H
#define PRISM_PLATFORM_TERMINAL_H

#include <signal.h>
#include <stdint.h>
#include "common/config.h"

typedef struct { uint32_t fg, bg; char ch[5]; uint8_t wide; } VCell;

extern VCell g_back[MAX_H][MAX_W];       /* 이번 프레임에 그릴 칸 */
extern int g_cols, g_rows;                /* 지금 창 크기 (MAX_W, MAX_H로 자름) */
extern volatile sig_atomic_t g_quit, g_resize;

void term_setup(void);                    /* raw 모드, 대체 화면, 마우스·win32 입력 모드 켜기 */
void term_restore(void);
void term_install_signals(void);          /* SIGINT/TERM/HUP → g_quit, SIGWINCH → g_resize */
void term_get_size(void);                 /* 창 크기를 읽고 다음 출력은 전체 다시 그리기 */
void term_present(void);                  /* g_back을 화면에 내보낸다 (바뀐 칸만) */

/* (x, y) 칸부터 글자를 쓰고 쓴 너비(칸)를 돌려준다 */
int term_text(int x, int y, uint32_t fg, uint32_t bg, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));
void term_bar(int y, uint32_t bg);        /* y 줄 전체를 bg 색 빈칸으로 */
int term_str_w(const char *s);            /* 화면 너비 (두 칸 글자는 2) */

#endif
