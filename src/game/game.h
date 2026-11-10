/*
 * 게임: 플레이어(최대 2명)와 카메라, 겨누기, 들기·놓기·돌리기, 처음 상태로, 퍼즐 판정, 방 이동.
 *
 * 빛은 물체가 바뀌거나 몸이 움직였을 때만 다시 계산한다 (game_simulate).
 * 모든 플레이어의 몸이 빛을 가린다.
 *
 * 혼자 할 때와 호스트는 이 모듈이 모든 플레이어를 움직인다(game_tick).
 * 참가자(클라이언트)는 호스트가 보낸 상태를 받아 그리기만 하고, 키와 동작은 호스트로 보낸다.
 */
#ifndef PRISM_GAME_H
#define PRISM_GAME_H

#include "renderer/renderer.h"

#define MAXPLAYER 2

/* 누르고 있는 키 (Player.keys) */
enum { K_W = 1, K_A = 2, K_S = 4, K_D = 8, K_LEFT = 16, K_RIGHT = 32, K_UP = 64, K_DOWN = 128 };

/* 조작 (네트워크로 보내는 단위이기도 하다) */
enum { ACT_TAKE = 1, ACT_ROT_LEFT, ACT_ROT_RIGHT, ACT_RESET };

typedef struct {
    int on;                             /* 이 자리에 플레이어가 있다 */
    float x, z, yaw, pitch;             /* 위치와 시선 */
    int fp;                             /* 1: 1인칭, 0: 내려다보기 (겨누기 방식이 다르다) */
    float walk_yaw;                     /* 내려다보기에서 걷는 방향의 기준 (그 사람의 카메라 각도) */
    unsigned keys;                      /* 누르고 있는 키 K_* */
    int carry;                          /* 들고 있는 물체 (-1이면 없음) */
    int tobj, tx, tz;                   /* 겨누는 물체, 놓을 수 있는 칸 (-1이면 없음) */
    char msg[160];                      /* 이 사람에게 보여 줄 알림 */
    int msg_t, msg_seq;                 /* 알림이 남은 틱 수, 알림 번호 (새 알림마다 1씩) */
} Player;

extern Player g_pl[MAXPLAYER];
extern int g_me;                        /* 이 터미널의 플레이어 번호 */

/* 이 터미널의 내려다보기 카메라 */
extern float cam_yaw, cam_yaw_target, cam_pitch, cam_dist;

/* 퍼즐 상태 */
extern int g_solved;                    /* 모든 수정이 켜졌는가 (문 열림) */
extern int g_moves;                     /* 지금 방에서 조작한 횟수 */
extern int g_cleared;                   /* 마지막 방의 문까지 지났다 */

void game_init(int level);              /* level번 방(0부터)을 만들고 플레이어 0을 시작 칸에 세워 빛을 계산한다 */
void game_simulate(void);               /* 빛 계산 → 문 갱신 */
int game_solve(void);                   /* 거울 각도를 모두 시험해 푸는 방법을 찾는다 */
void game_find_target(int p);           /* p번 플레이어가 겨누는 물체와 놓을 칸을 다시 구한다 */
void game_message(int p, const char *fmt, ...) __attribute__((format(printf, 2, 3)));   /* p < 0이면 모두에게 */

void game_join(int p);                  /* p번 자리에 플레이어가 들어왔다 (시작 칸 근처에 선다) */
void game_leave(int p);                 /* p번 플레이어가 나갔다 (든 물체는 그 자리에 내려놓는다) */

void game_action(int p, int act);       /* p번 플레이어의 조작 ACT_* */
void game_view_key(int vk);             /* 이 터미널의 시점·카메라 키 (Tab, 방향키, +/-) */
unsigned game_local_keys(void);         /* 이 터미널에서 지금 누르고 있는 키 → K_* */
void game_tick(float dt);               /* 모든 플레이어 걷기, 겨누기, 알림 시간 (혼자 / 호스트) */
void game_local_tick(void);             /* 이 터미널의 카메라 따라가기와 알림 시간 (참가자) */

void game_view(View *v);                /* 렌더러에 넘길 시점 정보 (이 터미널의 플레이어 기준) */
void game_print_state(const char *title);   /* --check 출력 */

#endif
