/*
 * 월드: 복셀 블록, 물체 목록, 칸별 물체 번호.
 *
 * 소유 규칙
 *   g_blk   load_level, world_set_door가 쓴다. 나머지 모듈은 읽기만 한다.
 *   g_obj   위치와 각도(x, z, rot, held)는 게임이, got와 lit는 빛 시뮬레이션이 쓴다.
 *   g_objat 물체 번호 + 1 (0은 빈 칸). load_level과 게임(들기/놓기)이 쓴다.
 */
#ifndef PRISM_WORLD_H
#define PRISM_WORLD_H

#include <stdint.h>
#include "common/config.h"

enum { B_AIR, B_FLOOR, B_WALL, B_GLASS, B_DOOR };
enum { O_EMIT = 1, O_MIRROR, O_SPLIT, O_PRISM, O_FILTER, O_CRYSTAL };

typedef struct {
    int kind, x, z, rot, movable, held;
    float col[3];             /* 광원 색, 필터 색, 수정이 원하는 색 */
    float got[3];             /* 수정에 들어온 빛 */
    int lit;
} Obj;

extern const char *KIND_NAME[];          /* O_* 번호로 찾는 물체 이름 */

extern uint8_t g_blk[RH][RD][RW];
extern Obj g_obj[MAXOBJ];
extern int g_nobj, g_objat[RD][RW];

/* 문자 지도로 방을 처음 상태로 만든다 */
void load_level(void);

/* 지도의 문(D) 칸을 열거나(공기) 닫는다(문 블록) */
void world_set_door(int open);

#endif
