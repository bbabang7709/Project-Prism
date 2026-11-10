/*
 * 렌더러 내부: 화면 광선 하나를 방에 쏘아 색, 깊이, 물체 번호를 구한다.
 * renderer.c만 include한다. 게임이나 광학 모듈은 이 헤더를 쓰지 않는다.
 *
 * id 버퍼 번호 규칙 (윤곽선과 선택 테두리가 쓴다)
 *   0                 배경
 *   b*8 + axis*2 + s + 1   블록 면 (b: B_*, axis: 0 x / 1 y / 2 z, s: 법선이 양수면 1) — 최대 38
 *   99 - 플레이어 번호  플레이어 몸 (99, 98)
 *   100 + g_objat     물체 (= 101 + 물체 번호)
 */
#ifndef PRISM_RENDERER_RAYCAST_H
#define PRISM_RENDERER_RAYCAST_H

#include <stdint.h>
#include "common/config.h"
#include "common/types.h"

/* 한 프레임 동안 바뀌지 않는 값. 메인 스레드가 채운 뒤 바리어를 지나 작업 스레드들이 읽기만 한다 */
typedef struct {
    float ox, oy, oz, fx, fy, fz, rx, ry, rz, ux, uy, uz, tanh;
    float nearx, nearz;       /* 카메라 쪽 방향 (앞 벽 잘라내기) */
    int W, H, frame, fp, tx, tz;
} Frame;

/* 이번 프레임의 빛줄기 선분과 그릴 플레이어 몸들을 복사해 둔다.
 * 메인 스레드가 작업 스레드를 깨우기(바리어) 전에 부른다. */
void raycast_prepare(const Seg *seg, int nseg, const Body *body, int nbody);

static inline uint32_t pack(float r, float g, float b)
{
    int R = (int)r, G = (int)g, B = (int)b;
    return (uint32_t)((R > 255 ? 255 : R < 0 ? 0 : R) << 16 | (G > 255 ? 255 : G < 0 ? 0 : G) << 8 | (B > 255 ? 255 : B < 0 ? 0 : B));
}

/* 렌더러 안에서만 쓰는 프레임 정보와 결과 버퍼 (g_pix는 renderer.h에 공개) */
extern Frame g_fr;
extern uint32_t g_pix[MAX_H * 2][MAX_W];
extern float g_depth[MAX_H * 2][MAX_W];
extern uint16_t g_id[MAX_H * 2][MAX_W];

/* g_fr로 first, first + step, first + 2*step, ... 번째 줄을 그려 g_pix, g_depth, g_id에 쓴다 */
void raycast_rows(int first, int step);

#endif
