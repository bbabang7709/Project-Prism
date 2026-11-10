#include "world/world.h"

#include <string.h>

const char *KIND_NAME[] = { "", "광원", "거울", "반투명 거울", "프리즘", "필터", "수정" };

uint8_t g_blk[RH][RD][RW];
Obj g_obj[MAXOBJ];
int g_nobj, g_objat[RD][RW];

/* 문자 지도: E 광원(동쪽), P 프리즘, M 거울, S 반투명 거울, b 파랑 필터, R G B Y 수정, g 유리, # 벽, | 기둥, D 문 */
static const char *LEVEL[RD] = {
    "################",
    "#..............#",
    "#.....M...b..B.#",
    "#....g.........#",
    "#..............#",
    "#E.P......Y....D",
    "#...........|..D",
    "#..............#",
    "#.....M.g.S..R.#",
    "#..............#",
    "#..............#",
    "################",
};
static const int START_ROT[3] = { 2, 4, 0 };   /* 거울, 거울, 반투명 거울의 처음 각도 (풀리지 않은 상태) */

static void add_obj(int kind, int x, int z, int rot, int movable, float r, float g, float b)
{
    Obj *o = &g_obj[g_nobj++];
    memset(o, 0, sizeof *o);
    o->kind = kind; o->x = x; o->z = z; o->rot = rot; o->movable = movable;
    o->col[0] = r; o->col[1] = g; o->col[2] = b;
    g_objat[z][x] = g_nobj;
}

void load_level(void)
{
    memset(g_blk, 0, sizeof g_blk);
    memset(g_objat, 0, sizeof g_objat);
    g_nobj = 0;
    int nrot = 0;
    for (int z = 0; z < RD; z++)
        for (int x = 0; x < RW; x++) {
            char c = LEVEL[z][x];
            g_blk[0][z][x] = B_FLOOR;
            switch (c) {
            case '#': for (int y = 1; y < RH; y++) g_blk[y][z][x] = B_WALL; break;
            case '|': g_blk[1][z][x] = g_blk[2][z][x] = B_WALL; break;
            case 'D': g_blk[1][z][x] = g_blk[2][z][x] = B_DOOR; g_blk[3][z][x] = B_WALL; break;
            case 'g': g_blk[1][z][x] = B_GLASS; break;
            case 'E': add_obj(O_EMIT, x, z, 0, 0, 1, 1, 1); break;
            case 'P': add_obj(O_PRISM, x, z, 0, 1, 0, 0, 0); break;
            case 'M': add_obj(O_MIRROR, x, z, START_ROT[nrot++ % 3], 1, 0, 0, 0); break;
            case 'S': add_obj(O_SPLIT, x, z, START_ROT[nrot++ % 3], 1, 0, 0, 0); break;
            case 'b': add_obj(O_FILTER, x, z, 0, 1, 0.2f, 0.35f, 1); break;
            case 'R': add_obj(O_CRYSTAL, x, z, 0, 0, 1, 0, 0); break;
            case 'G': add_obj(O_CRYSTAL, x, z, 0, 0, 0, 1, 0); break;
            case 'B': add_obj(O_CRYSTAL, x, z, 0, 0, 0, 0, 1); break;
            case 'Y': add_obj(O_CRYSTAL, x, z, 0, 0, 1, 1, 0); break;
            }
        }
}

void world_set_door(int open)
{
    for (int z = 0; z < RD; z++)
        for (int x = 0; x < RW; x++)
            if (LEVEL[z][x] == 'D') g_blk[1][z][x] = g_blk[2][z][x] = open ? B_AIR : B_DOOR;
}
