/*
 * 렌더러: 카메라에서 본 방을 픽셀 버퍼(g_pix)로 그린다.
 *
 * 게임 상태는 직접 읽지 않고, 호출하는 쪽이 채운 View를 받는다.
 * 빛줄기도 광학 모듈이 아니라 View의 선분 배열(Seg)로 받는다.
 * 월드(블록, 물체)는 읽기만 한다.
 *
 * 스레드: renderer_init이 작업 스레드를 만들고, renderer_draw가 바리어로 한 프레임을 나눠 그린다.
 * renderer_draw가 돌아올 때까지 다른 스레드에서 월드나 선분을 바꾸면 안 된다.
 */
#ifndef PRISM_RENDERER_H
#define PRISM_RENDERER_H

#include <stdint.h>
#include "common/config.h"
#include "common/types.h"

typedef struct {
    int fp;                           /* 1: 1인칭 (플레이어 눈), 0: 내려다보기 */
    float px, pz, pyaw, ppitch;       /* 플레이어 위치와 시선 */
    float cam_yaw, cam_pitch, cam_dist;   /* 내려다보기 카메라 */
    int tx, tz;                       /* 물체를 놓을 수 있다고 표시할 바닥 칸 (-1이면 없음) */
    int sel_obj;                      /* 노란 테두리를 그릴 물체 번호 (-1이면 없음) */
    const Seg *seg;                   /* 빛줄기 선분 */
    int nseg;
} View;

/* 그린 결과. 위에서부터 H줄, 왼쪽부터 W칸을 쓴다 (0xRRGGBB) */
extern uint32_t g_pix[MAX_H * 2][MAX_W];

int renderer_init(void);              /* 작업 스레드를 만들고 쓰는 스레드 수를 돌려준다 */
void renderer_shutdown(void);         /* 작업 스레드를 끝내고 기다린다 */

/* W×H 픽셀로 한 프레임을 그린다. frame은 수정 깜빡임, 선택 테두리 맥박에 쓴다 */
void renderer_draw(const View *v, int W, int H, int frame);

/* g_pix의 W×H 영역을 scale배 키워 PPM(P6)으로 저장한다 */
void renderer_write_ppm(const char *path, int W, int H, int scale);

#endif
