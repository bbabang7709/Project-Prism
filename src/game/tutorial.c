/*
 * tutorial.c - step-by-step tutorial and first-appearance device introductions.
 */
#include "game/tutorial.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "game/game.h"
#include "world/world.h"

int g_room_intro;

static const char *STEP[TUT_STEPS] = {
    "W A S D로 걸어 보세요 (Shift: 달리기, Space: 점프)",
    "조준점(+)을 거울에 맞추고 F를 눌러 들어 보세요",
    "빈 바닥을 보고 F로 내려놓으세요",
    "거울을 겨누고 Q나 E로 돌려 보세요",
    "Tab을 눌러 위에서 내려다보세요 (다시 Tab: 1인칭)",
    "거울로 빛을 꺾어 흰 수정에 보내세요 (막히면 I: 힌트)",
    "문이 열렸어요. 열린 문으로 걸어 나가세요",
};

const char *tutorial_text(int step) { return step >= 1 && step <= TUT_STEPS ? STEP[step - 1] : ""; }

/* each player remembers where the step started (to see walking) and how many turns it had made */
static float s_x[MAXPLAYER], s_z[MAXPLAYER];
static int s_rot[MAXPLAYER];

static void begin(int p, int step)
{
    Player *pl = &g_pl[p];
    pl->tut = step;
    s_x[p] = pl->x; s_z[p] = pl->z;
    s_rot[p] = pl->nrot;
}

void tutorial_join(int p)
{
    if (g_levels[g_cur_level].tutorial) begin(p, 1);
}

void tutorial_tick(void)
{
    if (!g_levels[g_cur_level].tutorial) return;
    for (int p = 0; p < MAXPLAYER; p++) {
        Player *pl = &g_pl[p];
        if (!pl->on || pl->tut <= 0) continue;
        int done = 0;
        switch (pl->tut) {
        case 1: done = hypotf(pl->x - s_x[p], pl->z - s_z[p]) > 1.5f; break;
        case 2: done = pl->carry >= 0; break;
        case 3: done = pl->carry < 0; break;
        case 4: done = pl->nrot > s_rot[p]; break;
        case 5: done = !pl->fp; break;
        case 6: done = g_solved; break;
        default: break;                                   /* last step stays until the room changes */
        }
        if (g_solved && pl->tut < 7) begin(p, 7);         /* solved early: skip to the door */
        else if (done) begin(p, pl->tut + 1);
    }
}

/* ---------- introductions ---------- */

enum {
    F_PRISM, F_SPLIT, F_FILTER, F_GLASS, F_YELLOW, F_CYAN, F_MAGENTA, F_PLATE, F_SENSOR, F_PORTAL,
    F_BOX, F_COOP, F_LINK, F_HATCH, F_LOWWALL, F_LIFT, NFEAT
};

static const char *FEAT[NFEAT] = {
    "프리즘: 흰빛을 빨강·초록·파랑으로 나눠요",
    "반투명 거울: 빛을 반은 반사하고 반은 통과시켜요",
    "필터: 자기 색의 빛만 통과시켜요",
    "유리 블록: 비스듬히 들어온 빛을 굴절시켜요",
    "노랑 수정: 빨강과 초록이 함께 닿아야 켜져요",
    "청록 수정: 초록과 파랑이 함께 닿아야 켜져요",
    "자홍 수정: 빨강과 파랑이 함께 닿아야 켜져요",
    "발판과 창살: 사람이나 물체가 발판을 누르면 창살이 열려요",
    "감지기: 빛을 받으면 켜지고, 모두 켜지면 창살이 열려요",
    "순간이동 고리: 한쪽에 든 빛이 짝 고리에서 그대로 나와요",
    "상자: 들고 다니며 빛을 막거나 발판을 눌러요",
    "2인 협동: 두 사람의 몸이 모두 빛을 가려요",
    "연결 색: 같은 색의 발판·감지기·창살끼리 이어져 있어요",
    "전달 창구: 사람은 못 지나가지만 물체와 빛은 건너가요",
    "낮은 벽: 사람과 낮은 빛을 막아요. 높은 빛은 위로 지나가요",
    "빛 승강기: 지나는 빛을 높은 길로 올리거나 낮은 길로 내려요",
};

static unsigned features(const Level *lv)
{
    unsigned f = 0;
    for (int z = 0; z < RD; z++)
        for (int x = 0; x < RW; x++) {
            switch (lv->map[z][x]) {
            case 'P': f |= 1u << F_PRISM; break;
            case 'S': f |= 1u << F_SPLIT; break;
            case 'b': case 'r': f |= 1u << F_FILTER; break;
            case 'g': f |= 1u << F_GLASS; break;
            case 'Y': f |= 1u << F_YELLOW; break;
            case 'C': f |= 1u << F_CYAN; break;
            case 'V': f |= 1u << F_MAGENTA; break;
            case '_': case '=': f |= 1u << F_PLATE; break;
            case 'O': f |= 1u << F_SENSOR; break;
            case 'T': f |= 1u << F_PORTAL; break;
            case 'K': f |= 1u << F_BOX; break;
            case 'h': f |= 1u << F_HATCH; break;
            case 'w': f |= 1u << F_LOWWALL; break;
            case 'U': f |= 1u << F_LIFT; break;
            }
            if (lv->chan[z][x]) f |= 1u << F_LINK;
        }
    if (lv->coop) f |= 1u << F_COOP;
    return f;
}

void tutorial_room_start(void)
{
    const Level *lv = &g_levels[g_cur_level];
    for (int p = 0; p < MAXPLAYER; p++) {
        if (lv->tutorial && g_pl[p].on) begin(p, 1);
        else g_pl[p].tut = 0;
    }
    g_room_intro = 0;
    if (lv->tutorial) return;                             /* the tutorial explains itself */
    unsigned seen = 0;
    for (int i = 0; i < g_cur_level; i++) seen |= features(&g_levels[i]);
    unsigned fresh = features(lv) & ~seen;
    if (!fresh) return;
    char text[400];
    int n = snprintf(text, sizeof text, "이 방에서 처음 나오는 것"), shown = 0;
    for (int k = 0; k < NFEAT && shown < 3; k++)          /* at most three lines; the rest show up in later rooms' play */
        if (fresh & 1u << k) {
            n += snprintf(text + n, sizeof text - (size_t)n, "\n◆ %s", FEAT[k]);
            shown++;
        }
    game_message(-1, "%s", text);
    for (int p = 0; p < MAXPLAYER; p++) g_pl[p].msg_t = 30 * 10;   /* ten seconds */
    g_room_intro = 1;
}
