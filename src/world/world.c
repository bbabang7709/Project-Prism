#define _GNU_SOURCE
#include "world/world.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const char *KIND_NAME[] = { "", "광원", "거울", "반투명 거울", "프리즘", "필터", "수정" };

Level g_levels[MAXLEVEL];
int g_nlevels, g_cur_level;

uint8_t g_blk[RH][RD][RW];
Obj g_obj[MAXOBJ];
int g_nobj, g_objat[RD][RW];

/* ---------- 기본 방 (방 파일이 없을 때) ---------- */

static const char *BUILTIN_MAP[RD] = {
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

void world_use_builtin(void)
{
    Level *lv = &g_levels[0];
    memset(lv, 0, sizeof *lv);
    snprintf(lv->name, sizeof lv->name, "빛의 신전");
    for (int z = 0; z < RD; z++) memcpy(lv->map[z], BUILTIN_MAP[z], RW + 1);
    lv->rot[2][6] = 2;                                    /* 풀리지 않은 처음 각도 */
    lv->rot[8][6] = 4;
    lv->rot[8][10] = 0;
    lv->sx = 2; lv->sz = 8; lv->face = 'E';
    g_nlevels = 1;
    g_cur_level = 0;
}

/* ---------- 방 파일 읽기 ---------- */

static const char MAP_CHARS[] = ".#|DgEPMSbrRGBY";
static const char OBJ_CHARS[] = "EPMSbrRGBY";

static int fail(char *err, size_t errlen, const char *file, int line, const char *fmt, ...)
{
    char msg[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (line > 0) snprintf(err, errlen, "%s:%d: %s", file, line, msg);
    else snprintf(err, errlen, "%s: %s", file, msg);
    return -1;
}

/* 파일 전체를 읽는다 (64KB까지) */
static char *slurp(const char *path, char *err, size_t errlen)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { fail(err, errlen, path, 0, "열 수 없습니다 (%s)", strerror(errno)); return NULL; }
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size > 65536) {
        close(fd);
        fail(err, errlen, path, 0, "파일이 너무 큽니다");
        return NULL;
    }
    char *buf = malloc((size_t)st.st_size + 1);
    size_t got = 0;
    while (buf && got < (size_t)st.st_size) {
        ssize_t n = read(fd, buf + got, (size_t)st.st_size - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (buf) buf[got] = 0;
    return buf;
}

static int parse_level(const char *path, Level *lv, char *err, size_t errlen)
{
    char *text = slurp(path, err, errlen);
    if (!text) return -1;
    memset(lv, 0, sizeof *lv);
    lv->sx = -1;
    int rows = -1, rc = 0, lineno = 0, nobj = 0, nemit = 0, ncrystal = 0;
    struct { int x, z, r, line; } rots[MAXOBJ * 2];
    int nrots = 0;
    for (char *line = text, *next; line && rc == 0; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        lineno++;
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\r' || (rows < 0 && (line[len - 1] == ' ' || line[len - 1] == '\t')))) line[--len] = 0;
        if (rows >= 0 && rows < RD) {                     /* 지도 줄 */
            if ((int)len != RW) { rc = fail(err, errlen, path, lineno, "지도 줄은 %d칸이어야 합니다 (지금 %zu칸)", RW, len); break; }
            for (int x = 0; x < RW; x++) {
                if (!strchr(MAP_CHARS, line[x])) { rc = fail(err, errlen, path, lineno, "알 수 없는 지도 글자 '%c' (%d번째 칸)", line[x], x); break; }
                if (strchr(OBJ_CHARS, line[x])) {
                    nobj++;
                    nemit += line[x] == 'E';
                    ncrystal += line[x] == 'R' || line[x] == 'G' || line[x] == 'B' || line[x] == 'Y';
                }
            }
            memcpy(lv->map[rows++], line, RW + 1);
            continue;
        }
        if (len == 0 || line[0] == '#') continue;
        if (rows >= RD) { rc = fail(err, errlen, path, lineno, "지도 뒤에 내용이 더 있습니다"); break; }
        char word[16];
        int n = 0;
        if (sscanf(line, "%15s %n", word, &n) != 1) continue;
        const char *arg = line + n;
        if (!strcmp(word, "name")) snprintf(lv->name, sizeof lv->name, "%.60s", arg);
        else if (!strcmp(word, "start")) {
            char f = 0;
            if (sscanf(arg, "%d %d %c", &lv->sx, &lv->sz, &f) != 3 || !strchr("ESWN", f) || !f)
                rc = fail(err, errlen, path, lineno, "start는 \"start x z 방향(E S W N)\" 형식입니다");
            lv->face = f;
        } else if (!strcmp(word, "rot")) {
            if (nrots >= (int)(sizeof rots / sizeof rots[0])) rc = fail(err, errlen, path, lineno, "rot 줄이 너무 많습니다");
            else if (sscanf(arg, "%d %d %d", &rots[nrots].x, &rots[nrots].z, &rots[nrots].r) != 3 || rots[nrots].r < 0 || rots[nrots].r > 7)
                rc = fail(err, errlen, path, lineno, "rot는 \"rot x z 각도(0~7)\" 형식입니다");
            else rots[nrots++].line = lineno;
        } else if (!strcmp(word, "coop")) {
            if (sscanf(arg, "%d %d", &lv->stand_x, &lv->stand_z) != 2) rc = fail(err, errlen, path, lineno, "coop은 \"coop x z\" 형식입니다");
            lv->coop = 1;
        } else if (!strcmp(word, "map")) rows = 0;
        else rc = fail(err, errlen, path, lineno, "알 수 없는 항목 '%s'", word);
    }
    free(text);
    if (rc) return rc;
    if (rows < RD) return fail(err, errlen, path, 0, "지도가 %d줄이어야 합니다 (map 다음에 %d줄)", RD, rows < 0 ? 0 : rows);
    if (nobj > MAXOBJ) return fail(err, errlen, path, 0, "물체가 너무 많습니다 (%d개, 최대 %d개)", nobj, MAXOBJ);
    if (!nemit) return fail(err, errlen, path, 0, "광원(E)이 없습니다");
    if (!ncrystal) return fail(err, errlen, path, 0, "수정(R G B Y)이 없습니다");
    if (lv->sx < 0) return fail(err, errlen, path, 0, "start 줄이 없습니다");
    if (lv->sx <= 0 || lv->sz <= 0 || lv->sx >= RW - 1 || lv->sz >= RD - 1 || lv->map[lv->sz][lv->sx] != '.')
        return fail(err, errlen, path, 0, "시작 칸 (%d, %d)이 빈 바닥이 아닙니다", lv->sx, lv->sz);
    if (lv->coop && (lv->stand_x <= 0 || lv->stand_z <= 0 || lv->stand_x >= RW - 1 || lv->stand_z >= RD - 1 || lv->map[lv->stand_z][lv->stand_x] != '.'))
        return fail(err, errlen, path, 0, "coop 칸 (%d, %d)이 빈 바닥이 아닙니다", lv->stand_x, lv->stand_z);
    for (int i = 0; i < nrots; i++) {
        int x = rots[i].x, z = rots[i].z;
        if (x < 0 || z < 0 || x >= RW || z >= RD || !strchr(OBJ_CHARS, lv->map[z][x]))
            return fail(err, errlen, path, rots[i].line, "rot의 칸 (%d, %d)에 물체가 없습니다", x, z);
        lv->rot[z][x] = (signed char)rots[i].r;
    }
    if (!lv->name[0]) {
        const char *base = strrchr(path, '/');
        snprintf(lv->name, sizeof lv->name, "%.60s", base ? base + 1 : path);
    }
    return 0;
}

static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int world_load_levels(const char *dir, char *err, size_t errlen)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    char *names[MAXLEVEL + 1];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len < 5 || strcmp(e->d_name + len - 4, ".lvl")) continue;
        if (n == MAXLEVEL) {
            closedir(d);
            for (int i = 0; i < n; i++) free(names[i]);
            return fail(err, errlen, dir, 0, "방 파일이 너무 많습니다 (최대 %d개)", MAXLEVEL);
        }
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof names[0], by_name);
    int rc = 0;
    for (int i = 0; i < n; i++) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        if (rc == 0 && parse_level(path, &g_levels[i], err, errlen) < 0) rc = -1;
        free(names[i]);
    }
    if (rc < 0) return -1;
    g_nlevels = n;
    g_cur_level = 0;
    return n;
}

void world_drop_coop(void)
{
    int n = 0;
    for (int i = 0; i < g_nlevels; i++)
        if (!g_levels[i].coop) g_levels[n++] = g_levels[i];
    if (n) g_nlevels = n;                               /* 2인용 방뿐이면 그대로 둔다 */
}

/* ---------- 방 만들기 ---------- */

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
    const Level *lv = &g_levels[g_cur_level];
    memset(g_blk, 0, sizeof g_blk);
    memset(g_objat, 0, sizeof g_objat);
    g_nobj = 0;
    for (int z = 0; z < RD; z++)
        for (int x = 0; x < RW; x++) {
            char c = lv->map[z][x];
            int rot = lv->rot[z][x];
            g_blk[0][z][x] = B_FLOOR;
            switch (c) {
            case '#': for (int y = 1; y < RH; y++) g_blk[y][z][x] = B_WALL; break;
            case '|': g_blk[1][z][x] = g_blk[2][z][x] = B_WALL; break;
            case 'D': g_blk[1][z][x] = g_blk[2][z][x] = B_DOOR; g_blk[3][z][x] = B_WALL; break;
            case 'g': g_blk[1][z][x] = B_GLASS; break;
            case 'E': add_obj(O_EMIT, x, z, rot, 0, 1, 1, 1); break;
            case 'P': add_obj(O_PRISM, x, z, rot, 1, 0, 0, 0); break;
            case 'M': add_obj(O_MIRROR, x, z, rot, 1, 0, 0, 0); break;
            case 'S': add_obj(O_SPLIT, x, z, rot, 1, 0, 0, 0); break;
            case 'b': add_obj(O_FILTER, x, z, rot, 1, 0.2f, 0.35f, 1); break;
            case 'r': add_obj(O_FILTER, x, z, rot, 1, 1, 0.1f, 0.1f); break;
            case 'R': add_obj(O_CRYSTAL, x, z, rot, 0, 1, 0, 0); break;
            case 'G': add_obj(O_CRYSTAL, x, z, rot, 0, 0, 1, 0); break;
            case 'B': add_obj(O_CRYSTAL, x, z, rot, 0, 0, 0, 1); break;
            case 'Y': add_obj(O_CRYSTAL, x, z, rot, 0, 1, 1, 0); break;
            }
        }
}

void world_set_door(int open)
{
    const Level *lv = &g_levels[g_cur_level];
    for (int z = 0; z < RD; z++)
        for (int x = 0; x < RW; x++)
            if (lv->map[z][x] == 'D') g_blk[1][z][x] = g_blk[2][z][x] = open ? B_AIR : B_DOOR;
}

int world_is_door(int x, int z)
{
    return x >= 0 && z >= 0 && x < RW && z < RD && g_levels[g_cur_level].map[z][x] == 'D';
}
