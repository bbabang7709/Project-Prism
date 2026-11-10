/*
 * 모듈 사이에서 주고받는 공용 자료.
 *
 * Seg:  빛 시뮬레이션(optics)이 만들고 렌더러가 그리는 빛줄기 선분.
 *       높이는 모두 BY이고, (x0, z0) → (x1, z1) 구간에 색 (r, g, b)를 가진다.
 *       렌더러는 이 구조체만 알면 되고 광학 내부 구현에는 기대지 않는다.
 * Body: 플레이어 몸 (반지름 PR인 원기둥). 빛을 가리고, 렌더러가 그린다. who는 플레이어 번호(색).
 */
#ifndef PRISM_COMMON_TYPES_H
#define PRISM_COMMON_TYPES_H

typedef struct { float x0, z0, x1, z1, r, g, b; } Seg;
typedef struct { float x, z; int who; } Body;

#endif
