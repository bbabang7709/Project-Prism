/*
 * 월드: 방 파일(레벨), 복셀 블록, 물체 목록, 칸별 물체 번호.
 *
 * 소유 규칙
 *   g_levels 방 파일에서 읽은 방들. world_load_levels / world_use_builtin이 채운다.
 *   g_blk    load_level, world_set_door가 쓴다. 나머지 모듈은 읽기만 한다.
 *   g_obj    위치와 각도(x, z, rot, held)는 게임이, got와 lit는 빛 시뮬레이션이 쓴다.
 *   g_objat  물체 번호 + 1 (0은 빈 칸). load_level과 게임(들기/놓기)이 쓴다.
 *
 * 방 파일 형식 (levels 폴더의 .lvl 파일, 이름 순서대로 방 1, 2, ...)
 *   # 주석               지도 앞에서 #로 시작하는 줄
 *   name 빛의 신전        방 이름
 *   start 2 8 E          시작 칸 (x z)과 바라보는 방향 E S W N
 *   rot 6 2 2            (x, z) 칸 물체의 처음 각도 0~7 (거울은 22.5도, 광원은 45도 단위)
 *   coop 11 9            2인용 방. 한 사람이 (x, z) 칸에 서서 빛을 막아야 풀린다 (혼자 할 때는 건너뜀)
 *   stand 4 8            풀이 확인(--solve)에서 누군가 이 칸에 서 있다고 본다 (발판 방)
 *   map                  다음 12줄이 16칸짜리 지도
 *   지도 글자: . 바닥  # 벽  | 기둥  D 문  g 유리  E 광원   P 프리즘  M 거울  S 반투명 거울
 *             b 파랑 필터  r 빨강 필터  R G B Y 수정 (빨강, 초록, 파랑, 노랑)
 *             _ 발판  = 창살 (발판이 모두 눌려 있는 동안 열린다. 발판은 사람이나 물체가 누른다)
 */
#ifndef PRISM_WORLD_H
#define PRISM_WORLD_H

#include <stddef.h>
#include <stdint.h>
#include "common/config.h"

#define MAXLEVEL 16

enum { B_AIR, B_FLOOR, B_WALL, B_GLASS, B_DOOR, B_PLATE, B_PLATE_ON, B_GATE };   /* 발판(안 눌림/눌림), 창살 */
enum { O_EMIT = 1, O_MIRROR, O_SPLIT, O_PRISM, O_FILTER, O_CRYSTAL };

typedef struct {
    int kind, x, z, rot, movable, held;
    float col[3];             /* 광원 색, 필터 색, 수정이 원하는 색 */
    float got[3];             /* 수정에 들어온 빛 */
    int lit;
} Obj;

typedef struct {
    char name[64];
    char map[RD][RW + 1];
    signed char rot[RD][RW];  /* 물체의 처음 각도 */
    int sx, sz;               /* 시작 칸 */
    char face;                /* 처음 바라보는 방향: 'E' 'S' 'W' 'N' */
    int coop;                 /* 2인용 방 (혼자 할 때는 건너뛴다) */
    int has_stand;            /* 풀이 확인용: 누군가 (stand_x, stand_z)에 서 있어야 풀린다 */
    int stand_x, stand_z;
} Level;

extern const char *KIND_NAME[];          /* O_* 번호로 찾는 물체 이름 */

extern Level g_levels[MAXLEVEL];
extern int g_nlevels, g_cur_level;       /* 방 수, 지금 방 (0부터) */

extern uint8_t g_blk[RH][RD][RW];
extern Obj g_obj[MAXOBJ];
extern int g_nobj, g_objat[RD][RW];

/* dir 폴더의 *.lvl 파일을 이름 순서로 읽는다.
 * 읽은 방 수를 돌려준다. 폴더가 없으면 0, 파일이 잘못되었으면 -1과 err에 "파일:줄: 이유" */
int world_load_levels(const char *dir, char *err, size_t errlen);

/* 2인용 방(coop)을 목록에서 뺀다 (혼자 할 때) */
void world_drop_coop(void);

/* 방 파일 없이 기본 방 하나(원래 프로토타입의 방)를 쓴다 */
void world_use_builtin(void);

/* 지금 방(g_cur_level)을 처음 상태로 만든다 */
void load_level(void);

/* 지금 방 지도의 문(D) 칸을 열거나(공기) 닫는다(문 블록) */
void world_set_door(int open);

/* 발판 칸의 눌림 표시, 창살 칸 열고 닫기 (언제 누르고 여는지는 게임이 정한다) */
void world_set_plate(int x, int z, int pressed);
void world_set_gate(int x, int z, int open);

/* (x, z)가 지금 방의 문 칸인가 */
int world_is_door(int x, int z);

#endif
