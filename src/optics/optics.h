/*
 * 빛 시뮬레이션: 광원에서 나온 빛줄기를 칸 단위로 따라가며
 * 거울 반사, 반투명 거울 분할, 프리즘 분광, 유리 굴절(스넬 법칙, 전반사), 색 필터를 처리한다.
 *
 * 입력  월드(g_blk, g_obj, g_objat)와 빛을 가리는 몸들의 위치 (반지름은 PR)
 * 출력  g_seg / g_nseg      빛줄기 선분 (렌더러가 그린다)
 *       g_obj[i].got, .lit  수정이 받은 빛과 켜졌는지
 *       반환값              모든 수정이 켜졌는가
 *
 * 월드의 블록은 바꾸지 않는다. 문 열기·닫기는 호출하는 쪽(게임)이 결과를 보고 한다.
 * 문 블록은 빛을 막으므로, 다음 계산 결과는 그 사이에 바뀐 문 상태에 따라 달라진다.
 */
#ifndef PRISM_OPTICS_H
#define PRISM_OPTICS_H

#include "common/config.h"
#include "common/types.h"

extern Seg g_seg[MAXSEG];                /* 읽기 전용 (optics만 쓴다) */
extern int g_nseg;

/* 빛을 처음부터 다시 계산한다. bodies는 빛을 가리는 플레이어 몸들 (최대 8개) */
int optics_simulate(const Body *bodies, int nbody);

/* 켜진 수정 수를 돌려주고 *total에 전체 수정 수를 넣는다 */
int optics_count_lit(int *total);

#endif
