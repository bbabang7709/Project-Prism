/*
 * 시작 메뉴: 천천히 도는 방(풀린 상태) 위에 메뉴를 그린다.
 *
 *   혼자 하기       ◀ 시작 방 ▶   (2인용 방은 빼고 고른다)
 *   호스트로 시작   ◀ 시작 방 ▶   (모든 방)
 *   참가하기        주소 입력
 *   조작법
 *   끝내기
 *
 * 키를 받아 고른 것을 MenuChoice로 돌려줄 뿐, 게임을 시작하는 일은 main이 한다.
 */
#ifndef PRISM_GAME_MENU_H
#define PRISM_GAME_MENU_H

enum { MENU_NONE, MENU_SOLO, MENU_HOST, MENU_JOIN, MENU_QUIT };

typedef struct {
    int what;                           /* MENU_* */
    int level;                          /* 시작 방 (0부터, 혼자 하기는 2인용 방을 뺀 목록 기준) */
    const char *addr;                   /* 참가할 주소 */
} MenuChoice;

/* 메뉴를 연다. 방 목록(g_levels)은 모든 방을 읽은 상태여야 한다. msg는 아래쪽에 보여 줄 알림 (NULL 가능) */
void menu_open(const char *msg);
void menu_message(const char *msg);

MenuChoice menu_key(int vk);            /* 방향키, Enter, ESC, Backspace */
void menu_text(int ch);                 /* 주소 입력란 글자 */
void menu_draw(int frame);

#endif
