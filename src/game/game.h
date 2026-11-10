/*
 * 게임: 플레이어와 카메라 상태, 겨누기, 들기·놓기·돌리기, 처음 상태로, 퍼즐 판정.
 *
 * 빛은 물체가 바뀌거나 몸이 움직였을 때만 다시 계산한다 (game_simulate).
 * 상태는 이 모듈이 쓰고, HUD와 진입점(명령행 옵션)은 읽거나 처음 값을 정한다.
 */
#ifndef PRISM_GAME_H
#define PRISM_GAME_H

#include "renderer/renderer.h"

/* 플레이어: 방 안을 걸어 다니며 물체를 들고, 놓고, 돌린다. 몸이 빛을 가린다 */
extern float g_px, g_pz, g_pyaw, g_ppitch;
extern int g_fp;                        /* 1: 1인칭, 0: 내려다보기 */
extern int g_carry;                     /* 들고 있는 물체 (-1이면 없음) */
extern int g_tobj, g_tx, g_tz;          /* 겨누는 물체, 놓을 수 있는 칸 (-1이면 없음) */

/* 내려다보기 카메라 */
extern float cam_yaw, cam_yaw_target, cam_pitch, cam_dist;

/* 퍼즐 상태와 알림 */
extern int g_solved;                    /* 모든 수정이 켜졌는가 (문 열림) */
extern char g_msg[160];
extern int g_msg_t;                     /* 알림이 남은 틱 수 */

void game_init(int level);              /* level번 방(0부터)을 만들고 시작 칸에 서서 빛을 계산한다 */
void game_simulate(void);               /* 빛 계산 → 문 갱신 */
int game_solve(void);                   /* 거울 각도를 모두 시험해 푸는 방법을 찾는다 */
void game_find_target(void);            /* 겨누는 물체와 놓을 칸을 다시 구한다 */
void game_message(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void game_press(int vk);                /* 게임 키 (시점, F, Q/E, R, 카메라) */
void game_act(void);                    /* 들기 / 내려놓기 */
void game_rotate(int d);                /* 겨눈(또는 든) 거울을 22.5도 * d 돌리기 */
void game_tick(float dt);               /* 걷기, 카메라 따라가기, 알림 시간 */

void game_view(View *v);                /* 렌더러에 넘길 시점 정보 */
void game_print_state(const char *title);   /* --check 출력 */

#endif
