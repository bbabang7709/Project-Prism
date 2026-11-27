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

const char *KIND_NAME[] = { "", "광원", "거울", "반투명 거울", "프리즘", "필터", "수정", "감지기", "순간이동 고리", "상자", "빛 승강기" };

const float CHAN_COL[4][3] = { { 0.45f, 1, 0.8f }, { 1, 0.55f, 0.15f }, { 0.95f, 0.35f, 0.85f }, { 0.35f, 0.65f, 1 } };

Level g_levels[MAXLEVEL];
int g_nlevels, g_cur_level;

uint8_t g_blk[RH][RD][RW];
Obj g_obj[MAXOBJ];
int g_nobj, g_objat[RD][RW];

/* ---------- built-in room (when there are no level files) ---------- */

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
    lv->rot[2][6] = 2;                                    /* starting angles that leave it unsolved */
    lv->rot[8][6] = 4;
    lv->rot[8][10] = 0;
    lv->sx = 2; lv->sz = 8; lv->face = 'E';
    g_nlevels = 1;
    g_cur_level = 0;
}

/* ---------- reading level files ---------- */

static const char MAP_CHARS[] = ".#|Dg_=EPMSbrRGBYCVWOTKhwU";
static const char OBJ_CHARS[] = "EPMSbrRGBYCVWOTKU";

/* crystals: R G B Y, C (cyan = green + blue), V (violet/magenta = red + blue), W (white = all three) */
static int is_crystal(char c) { return c && strchr("RGBYCVW", c) != NULL; }

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

/* read a whole file (up to 64 KB) */
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
    int rows = -1, rc = 0, lineno = 0, nobj = 0, nemit = 0, ncrystal = 0, nportal = 0, seen_item = 0;
    struct { int x, z, r, line; } rots[MAXOBJ * 2], links[RW * RD];
    int nrots = 0, nlinks = 0;
    char *start = text;
    /* skip a UTF-8 BOM (Windows Notepad adds one); strncmp so short files are never read past their end */
    if (!strncmp(start, "\xEF\xBB\xBF", 3)) start += 3;
    for (char *line = start, *next; line && rc == 0; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        lineno++;
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\r' || (rows < 0 && (line[len - 1] == ' ' || line[len - 1] == '\t')))) line[--len] = 0;
        if (rows >= 0 && rows < RD) {                     /* map row */
            if ((int)len != RW) { rc = fail(err, errlen, path, lineno, "지도 줄은 %d칸이어야 합니다 (지금 %zu칸)", RW, len); break; }
            for (int x = 0; x < RW; x++) {
                if (!strchr(MAP_CHARS, line[x])) { rc = fail(err, errlen, path, lineno, "알 수 없는 지도 글자 '%c' (%d번째 칸)", line[x], x); break; }
                if (strchr(OBJ_CHARS, line[x])) {
                    nobj++;
                    nemit += line[x] == 'E';
                    ncrystal += is_crystal(line[x]);
                    nportal += line[x] == 'T';
                }
            }
            memcpy(lv->map[rows++], line, RW + 1);
            continue;
        }
        if (line[0] == '#' && rows < 0 && !seen_item) {   /* keep the description comment at the top (the editor writes it back) */
            size_t used = strlen(lv->note);
            if (used + len + 2 < sizeof lv->note) { memcpy(lv->note + used, line, len); lv->note[used + len] = '\n'; lv->note[used + len + 1] = 0; }
            continue;
        }
        if (len == 0 || line[0] == '#') continue;
        seen_item = 1;
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
        } else if (!strcmp(word, "stand")) {
            int *px = lv->has_stand ? &lv->stand2_x : &lv->stand_x, *pz = lv->has_stand ? &lv->stand2_z : &lv->stand_z;
            if (lv->has_stand2) rc = fail(err, errlen, path, lineno, "stand는 두 줄까지입니다");
            else if (sscanf(arg, "%d %d", px, pz) != 2) rc = fail(err, errlen, path, lineno, "stand는 \"stand x z\" 형식입니다");
            else if (lv->has_stand) lv->has_stand2 = 1;
            else lv->has_stand = 1;
        } else if (!strcmp(word, "tutorial")) {
            lv->tutorial = 1;
        } else if (!strcmp(word, "coop")) {
            lv->coop = 1;
            if (!arg[0]) continue;                        /* "coop" alone: a two-player room without a fixed cell */
            if (sscanf(arg, "%d %d", &lv->stand_x, &lv->stand_z) != 2) rc = fail(err, errlen, path, lineno, "coop은 \"coop\"이나 \"coop x z\" 형식입니다");
            lv->has_stand = 1;
        } else if (!strcmp(word, "start2")) {
            char f = 0;
            if (sscanf(arg, "%d %d %c", &lv->sx2, &lv->sz2, &f) != 3 || !f || !strchr("ESWN", f))
                rc = fail(err, errlen, path, lineno, "start2는 \"start2 x z 방향(E S W N)\" 형식입니다");
            lv->face2 = f;
            lv->has_start2 = 1;
        } else if (!strcmp(word, "link")) {
            if (nlinks >= (int)(sizeof links / sizeof links[0])) rc = fail(err, errlen, path, lineno, "link 줄이 너무 많습니다");
            else if (sscanf(arg, "%d %d %d", &links[nlinks].x, &links[nlinks].z, &links[nlinks].r) != 3 || links[nlinks].r < 0 || links[nlinks].r > 3)
                rc = fail(err, errlen, path, lineno, "link는 \"link x z 번호(0~3)\" 형식입니다");
            else links[nlinks++].line = lineno;
        } else if (!strcmp(word, "move")) {
            int m[4];
            if (lv->nmove >= 4) rc = fail(err, errlen, path, lineno, "move 줄은 4개까지입니다");
            else if (sscanf(arg, "%d %d %d %d", &m[0], &m[1], &m[2], &m[3]) != 4) rc = fail(err, errlen, path, lineno, "move는 \"move x1 z1 x2 z2\" 형식입니다");
            else if (m[0] <= 0 || m[1] <= 0 || m[2] <= 0 || m[3] <= 0 || m[0] >= RW - 1 || m[2] >= RW - 1 || m[1] >= RD - 1 || m[3] >= RD - 1)
                rc = fail(err, errlen, path, lineno, "move의 칸이 방 안쪽이 아닙니다");
            else for (int k = 0; k < 4; k++) lv->move[lv->nmove][k] = (signed char)m[k];
            if (rc == 0) lv->nmove++;
        } else if (!strcmp(word, "map")) rows = 0;
        else rc = fail(err, errlen, path, lineno, "알 수 없는 항목 '%s'", word);
    }
    free(text);
    if (rc) return rc;
    if (rows < RD) return fail(err, errlen, path, 0, "지도가 %d줄이어야 합니다 (map 다음에 %d줄)", RD, rows < 0 ? 0 : rows);
    if (nobj > MAXOBJ) return fail(err, errlen, path, 0, "물체가 너무 많습니다 (%d개, 최대 %d개)", nobj, MAXOBJ);
    if (!nemit) return fail(err, errlen, path, 0, "광원(E)이 없습니다");
    if (!ncrystal) return fail(err, errlen, path, 0, "수정(R G B Y)이 없습니다");
    if (nportal != 0 && nportal != 2) return fail(err, errlen, path, 0, "순간이동 고리(T)는 0개나 2개여야 합니다 (지금 %d개)", nportal);
    if (lv->sx < 0) return fail(err, errlen, path, 0, "start 줄이 없습니다");
    if (lv->sx <= 0 || lv->sz <= 0 || lv->sx >= RW - 1 || lv->sz >= RD - 1 || lv->map[lv->sz][lv->sx] != '.')
        return fail(err, errlen, path, 0, "시작 칸 (%d, %d)이 빈 바닥이 아닙니다", lv->sx, lv->sz);
    if (lv->has_stand2 && (lv->stand2_x <= 0 || lv->stand2_z <= 0 || lv->stand2_x >= RW - 1 || lv->stand2_z >= RD - 1 ||
                           !strchr("._", lv->map[lv->stand2_z][lv->stand2_x])))
        return fail(err, errlen, path, 0, "두 번째 stand 칸 (%d, %d)이 바닥이나 발판이 아닙니다", lv->stand2_x, lv->stand2_z);
    if (lv->has_start2 && (lv->sx2 <= 0 || lv->sz2 <= 0 || lv->sx2 >= RW - 1 || lv->sz2 >= RD - 1 || lv->map[lv->sz2][lv->sx2] != '.'))
        return fail(err, errlen, path, 0, "start2 칸 (%d, %d)이 빈 바닥이 아닙니다", lv->sx2, lv->sz2);
    for (int i = 0; i < nlinks; i++) {
        int x = links[i].x, z = links[i].z;
        if (x < 0 || z < 0 || x >= RW || z >= RD || !strchr("_=O", lv->map[z][x]))
            return fail(err, errlen, path, links[i].line, "link의 칸 (%d, %d)이 발판·창살·감지기가 아닙니다", x, z);
        lv->chan[z][x] = (signed char)links[i].r;
    }
    if (lv->has_stand && (lv->stand_x <= 0 || lv->stand_z <= 0 || lv->stand_x >= RW - 1 || lv->stand_z >= RD - 1 ||
                          !strchr("._", lv->map[lv->stand_z][lv->stand_x])))
        return fail(err, errlen, path, 0, "coop/stand 칸 (%d, %d)이 바닥이나 발판이 아닙니다", lv->stand_x, lv->stand_z);
    for (int i = 0; i < nrots; i++) {
        int x = rots[i].x, z = rots[i].z;
        if (x < 0 || z < 0 || x >= RW || z >= RD || !strchr(OBJ_CHARS, lv->map[z][x]))
            return fail(err, errlen, path, rots[i].line, "rot의 칸 (%d, %d)에 물체가 없습니다", x, z);
        lv->rot[z][x] = (signed char)rots[i].r;
    }
    for (int i = 0; i < lv->nmove; i++) {
        const signed char *m = lv->move[i];
        if (!strchr("PMSbrKU", lv->map[m[1]][m[0]]) || !strchr("._", lv->map[m[3]][m[2]]))
            return fail(err, errlen, path, 0, "move %d %d %d %d: 움직일 수 있는 물체를 빈 바닥이나 발판으로 옮겨야 합니다", m[0], m[1], m[2], m[3]);
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
        if (rc == 0) snprintf(g_levels[i].file, sizeof g_levels[i].file, "%.60s", names[i]);
        free(names[i]);
    }
    if (rc < 0) return -1;
    g_nlevels = n;
    g_cur_level = 0;
    if (n) snprintf(g_level_dir, sizeof g_level_dir, "%.250s", dir);
    return n;
}

/* ---------- checking and saving rooms (editor) ---------- */

char g_level_dir[256] = "levels";

int world_check_level(const Level *lv, char *err, size_t errlen)
{
    int nobj = 0, nemit = 0, ncrystal = 0, nportal = 0;
    for (int z = 0; z < RD; z++)
        for (int x = 0; x < RW; x++) {
            char c = lv->map[z][x];
            int border = x == 0 || z == 0 || x == RW - 1 || z == RD - 1;
            if (!strchr(MAP_CHARS, c)) { snprintf(err, errlen, "(%d, %d)에 알 수 없는 글자 '%c'", x, z, c); return -1; }
            if (border && c != '#' && c != 'D') { snprintf(err, errlen, "테두리 (%d, %d)는 벽이나 문이어야 해요", x, z); return -1; }
            if (strchr(OBJ_CHARS, c)) {
                nobj++;
                nemit += c == 'E';
                ncrystal += is_crystal(c);
                nportal += c == 'T';
            }
        }
    if (nobj > MAXOBJ) { snprintf(err, errlen, "물체가 너무 많아요 (%d개, 최대 %d개)", nobj, MAXOBJ); return -1; }
    if (!nemit) { snprintf(err, errlen, "광원이 없어요"); return -1; }
    if (!ncrystal) { snprintf(err, errlen, "수정이 없어요"); return -1; }
    if (nportal != 0 && nportal != 2) { snprintf(err, errlen, "순간이동 고리는 0개나 2개여야 해요 (지금 %d개)", nportal); return -1; }
    if (lv->map[lv->sz][lv->sx] != '.') { snprintf(err, errlen, "시작 칸 (%d, %d)이 빈 바닥이 아니에요", lv->sx, lv->sz); return -1; }
    if (lv->has_start2 && (lv->sx2 <= 0 || lv->sz2 <= 0 || lv->sx2 >= RW - 1 || lv->sz2 >= RD - 1 || lv->map[lv->sz2][lv->sx2] != '.' ||
                           (lv->sx2 == lv->sx && lv->sz2 == lv->sz))) {
        snprintf(err, errlen, "2번 시작 칸 (%d, %d)이 빈 바닥이 아니에요", lv->sx2, lv->sz2);
        return -1;
    }
    for (int z = 0; z < RD; z++)
        for (int x = 0; x < RW; x++)
            if (lv->chan[z][x] < 0 || lv->chan[z][x] > 3) { snprintf(err, errlen, "(%d, %d)의 연결 번호가 0~3이 아니에요", x, z); return -1; }
    return 0;
}

/* write the whole buffer (partial writes, EINTR) */
static int write_all_fd(int fd, const char *p, size_t n)
{
    while (n > 0) {
        ssize_t k = write(fd, p, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        p += k;
        n -= (size_t)k;
    }
    return 0;
}

int world_save_level(const Level *lv, const char *file, char *err, size_t errlen)
{
    if (world_check_level(lv, err, errlen) < 0) return -1;
    char text[4096];
    size_t n = 0;
    /* write the original file's description comment back; a short header for a new room */
    n += (size_t)snprintf(text + n, sizeof text - n, "%s", lv->note[0] ? lv->note : "# PRISM 방 파일 (레벨 에디터로 만듦)\n");
    n += (size_t)snprintf(text + n, sizeof text - n, "name %s\nstart %d %d %c\n",
                          lv->name[0] ? lv->name : "이름 없는 방", lv->sx, lv->sz, lv->face ? lv->face : 'E');
    if (lv->has_start2) n += (size_t)snprintf(text + n, sizeof text - n, "start2 %d %d %c\n", lv->sx2, lv->sz2, lv->face2 ? lv->face2 : 'W');
    if (lv->tutorial) n += (size_t)snprintf(text + n, sizeof text - n, "tutorial\n");
    if (lv->coop && lv->has_stand) n += (size_t)snprintf(text + n, sizeof text - n, "coop %d %d\n", lv->stand_x, lv->stand_z);
    else if (lv->coop) n += (size_t)snprintf(text + n, sizeof text - n, "coop\n");
    else if (lv->has_stand) n += (size_t)snprintf(text + n, sizeof text - n, "stand %d %d\n", lv->stand_x, lv->stand_z);
    if (lv->has_stand2) n += (size_t)snprintf(text + n, sizeof text - n, "stand %d %d\n", lv->stand2_x, lv->stand2_z);
    for (int z = 0; z < RD; z++)                          /* link numbers (only those still on plate, gate or sensor cells) */
        for (int x = 0; x < RW; x++)
            if (lv->chan[z][x] && strchr("_=O", lv->map[z][x])) n += (size_t)snprintf(text + n, sizeof text - n, "link %d %d %d\n", x, z, lv->chan[z][x]);
    for (int i = 0; i < lv->nmove && i < 4; i++) {        /* drop move lines that no longer fit the edited map */
        const signed char *m = lv->move[i];
        if (strchr("PMSbrKU", lv->map[m[1]][m[0]]) && strchr("._", lv->map[m[3]][m[2]]))
            n += (size_t)snprintf(text + n, sizeof text - n, "move %d %d %d %d\n", m[0], m[1], m[2], m[3]);
    }
    /* mirrors and half mirrors get a rot line even at angle 0 (like the hand-written files; shows the puzzle's start) */
    for (int z = 0; z < RD; z++)
        for (int x = 0; x < RW; x++) {
            char c = lv->map[z][x];
            if (c == 'M' || c == 'S' || (lv->rot[z][x] && strchr(OBJ_CHARS, c)))
                n += (size_t)snprintf(text + n, sizeof text - n, "rot %d %d %d\n", x, z, lv->rot[z][x]);
        }
    n += (size_t)snprintf(text + n, sizeof text - n, "map\n");
    for (int z = 0; z < RD; z++) n += (size_t)snprintf(text + n, sizeof text - n, "%.*s\n", RW, lv->map[z]);
    if (n >= sizeof text) { snprintf(err, errlen, "방 파일이 너무 커요"); return -1; }

    mkdir(g_level_dir, 0755);                             /* the folder may not exist if only the built-in room was used */
    char path[512], tmp[512];
    snprintf(path, sizeof path, "%s/%s", g_level_dir, file);
    snprintf(tmp, sizeof tmp, "%s/.%s.tmp", g_level_dir, file);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) { snprintf(err, errlen, "%s: %s", tmp, strerror(errno)); return -1; }
    if (write_all_fd(fd, text, n) < 0 || fsync(fd) < 0) {
        snprintf(err, errlen, "%s: %s", tmp, strerror(errno));
        close(fd);
        unlink(tmp);
        return -1;
    }
    close(fd);
    if (rename(tmp, path) < 0) {                          /* swap it in at once */
        snprintf(err, errlen, "%s: %s", path, strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

void world_new_file_name(char *out, size_t n)
{
    int best = 0;
    DIR *d = opendir(g_level_dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            int k = atoi(e->d_name);
            size_t len = strlen(e->d_name);
            if (len > 4 && !strcmp(e->d_name + len - 4, ".lvl") && k > best) best = k;
        }
        closedir(d);
    }
    snprintf(out, n, "%02d-custom.lvl", best + 1);
}

void world_drop_coop(void)
{
    int n = 0;
    for (int i = 0; i < g_nlevels; i++)
        if (!g_levels[i].coop) g_levels[n++] = g_levels[i];
    if (n) g_nlevels = n;                               /* keep the list if it has only two-player rooms */
}

/* ---------- building a room ---------- */

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
            case '_': g_blk[0][z][x] = B_PLATE; break;
            case '=': g_blk[1][z][x] = g_blk[2][z][x] = B_GATE; break;
            case 'h': g_blk[0][z][x] = B_HATCH; g_blk[2][z][x] = g_blk[3][z][x] = B_WALL; break;   /* a low opening in a wall */
            case 'w': g_blk[1][z][x] = B_WALL; break;                                   /* low wall: knee high */
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
            case 'C': add_obj(O_CRYSTAL, x, z, rot, 0, 0, 1, 1); break;   /* cyan: green + blue */
            case 'V': add_obj(O_CRYSTAL, x, z, rot, 0, 1, 0, 1); break;   /* magenta: red + blue */
            case 'W': add_obj(O_CRYSTAL, x, z, rot, 0, 1, 1, 1); break;   /* white: all three */
            case 'U': add_obj(O_LIFT, x, z, rot, 1, 0.75f, 0.95f, 1); break;   /* light lift (movable) */
            case 'O': {                                   /* col: colour shown when lit (differs per link number) */
                const float *c = CHAN_COL[lv->chan[z][x] & 3];
                add_obj(O_SENSOR, x, z, rot, 0, c[0], c[1], c[2]);
                break;
            }
            case 'T': add_obj(O_PORTAL, x, z, rot, 0, 0.8f, 0.35f, 1); break;
            case 'K': add_obj(O_BOX, x, z, rot, 1, 0.75f, 0.5f, 0.28f); break;
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

void world_set_plate(int x, int z, int pressed)
{
    g_blk[0][z][x] = pressed ? B_PLATE_ON : B_PLATE;
}

void world_set_gate(int x, int z, int open)
{
    g_blk[1][z][x] = g_blk[2][z][x] = open ? B_AIR : B_GATE;
}
