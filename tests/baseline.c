/*
 * 기준 커밋과 결과 비교, 방 풀이 확인 (make test)
 *
 * 기준 커밋(make test REF=..., 기본은 마지막 커밋)을 같은 플래그로 빌드한 것과
 * 지금 빌드(./prism)를 같은 인자로 실행해 결과를 비교한다.
 *   - 글자 출력(--check 등): 같아야 한다. 실행 시간(ms)과 출력 폴더 이름은 비교에서 뺀다.
 *   - --shot PPM 이미지: 바이트 단위로 같아야 한다. 다르면 다른 바이트 수와 최대 차이를 알려 준다.
 * 그다음 새 빌드로 방 파일을 하나씩(--level 1, 2, ...) 확인한다.
 *   - 처음에는 문이 닫혀 있어야 하고(풀리지 않은 상태), --solve로 풀 수 있어야 한다.
 *
 * 사용: tests/out/baseline [기준 실행 파일] [새 실행 파일]   (프로젝트 루트에서)
 *       기본값은 tests/out/prism_ref, ./prism
 *       기준 실행 파일은 그 파일이 있는 폴더에서 실행한다 (그 커밋의 levels 폴더를 쓰도록)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define OUT "tests/out"
#define MAXARG 16

static char g_root[PATH_MAX];            /* 프로젝트 루트 (결과 파일을 절대 경로로 넘긴다) */

/* 이름과 인자. 인자 안의 @는 결과 파일 경로(tests/out/<ref|new>/<이름>)로 바뀐다 */
static const struct { const char *name, *args; } CASES[] = {
    { "check",           "--check" },
    { "check_solve",     "--check --solve" },
    { "check_block",     "--check --solve --fp 5.5 5.5 0 0" },
    { "shot_top",        "--shot @.ppm" },
    { "shot_angle",      "--shot @.ppm 1.2 -0.6 15" },
    { "shot_solve",      "--shot @.ppm --solve" },
    { "shot_fp",         "--shot @.ppm --fp 2.5 8.5 1.5708 -0.35" },
    { "shot_fp_solve",   "--shot @.ppm --fp 9.5 4.5 3.1416 -0.45 --solve" },
    { "shot_fp_block",   "--shot @.ppm --fp 5.5 5.5 0 -0.3 --solve" },
    { "shot_select",     "--shot @.ppm --select 2" },
    { "shot_top_select", "--shot @.ppm 2.4 -1.0 12 --select 4 --solve" },
};
#define NCASES ((int)(sizeof CASES / sizeof CASES[0]))

/* ---------- 파일 ---------- */

/* 파일 전체를 읽는다. 없으면 NULL */
static char *read_file(const char *path, size_t *len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return NULL; }
    char *buf = malloc((size_t)st.st_size + 1);
    if (!buf) { close(fd); return NULL; }
    size_t got = 0;
    while (got < (size_t)st.st_size) {
        ssize_t n = read(fd, buf + got, (size_t)st.st_size - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    buf[got] = 0;
    *len = got;
    return buf;
}

static int exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static void make_dir(const char *path)
{
    if (mkdir(path, 0755) < 0 && errno != EEXIST) { perror(path); exit(2); }
}

/* ---------- 실행 ---------- */

/* bin을 인자 args로 (dir이 있으면 그 폴더에서) 실행하고 표준 출력과 오류를 out 파일에 담는다. 종료 코드를 돌려준다 */
static int run(const char *bin, const char *dir, const char *args, const char *side, const char *name, const char *out)
{
    char buf[512], *argv[MAXARG + 2];
    int argc = 0;
    argv[argc++] = (char *)bin;
    /* @를 결과 경로로 바꾸고 공백으로 나눈다 */
    size_t k = 0;
    for (const char *s = args; *s && k < sizeof buf - 64; s++) {
        if (*s == '@') k += (size_t)snprintf(buf + k, sizeof buf - k, "%s/" OUT "/%s/%s", g_root, side, name);
        else buf[k++] = *s;
    }
    buf[k] = 0;
    for (char *t = strtok(buf, " "); t && argc < MAXARG + 1; t = strtok(NULL, " ")) argv[argc++] = t;
    argv[argc] = NULL;

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); exit(2); }
    if (pid == 0) {
        int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { perror(out); _exit(127); }
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
        if (dir && chdir(dir) < 0) { perror(dir); _exit(127); }
        execv(bin, argv);
        perror(bin);
        _exit(127);
    }
    int status;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR) { perror("waitpid"); exit(2); }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

/* ---------- 비교 ---------- */

/* 매번 달라지는 부분을 지운다: ", 1.2 ms with" → ", X ms with", "/ref/"·"/new/" → "/X/" (경로 앞부분은 같다) */
static void normalize(char *s)
{
    char *w = s;
    for (const char *r = s; *r;) {
        if (r[0] == ',' && r[1] == ' ') {
            const char *p = r + 2;
            while ((*p >= '0' && *p <= '9') || *p == '.') p++;
            if (p > r + 2 && !strncmp(p, " ms with", 8)) {
                memcpy(w, ", X", 3); w += 3;
                r = p;
                continue;
            }
        }
        if (!strncmp(r, "/ref/", 5) || !strncmp(r, "/new/", 5)) {
            memcpy(w, "/X/", 3); w += 3;
            r += 5;
            continue;
        }
        *w++ = *r++;
    }
    *w = 0;
}

static int same_text(const char *a_path, const char *b_path)
{
    size_t la, lb;
    char *a = read_file(a_path, &la), *b = read_file(b_path, &lb);
    int same = a && b;
    if (same) {
        normalize(a);
        normalize(b);
        same = strcmp(a, b) == 0;
    }
    free(a);
    free(b);
    return same;
}

/* 이미지를 바이트 단위로 비교한다. 다르면 다른 바이트 수와 최대 차이를 detail에 적는다 */
static int same_image(const char *a_path, const char *b_path, char *detail, size_t dlen)
{
    size_t la, lb;
    unsigned char *a = (unsigned char *)read_file(a_path, &la), *b = (unsigned char *)read_file(b_path, &lb);
    int same = 0;
    if (!a || !b) snprintf(detail, dlen, "이미지 없음");
    else if (la != lb) snprintf(detail, dlen, "크기 다름 (%zu / %zu 바이트)", la, lb);
    else {
        size_t diff = 0;
        int maxd = 0;
        for (size_t i = 0; i < la; i++)
            if (a[i] != b[i]) {
                int d = abs((int)a[i] - (int)b[i]);
                diff++;
                if (d > maxd) maxd = d;
            }
        same = diff == 0;
        if (!same) snprintf(detail, dlen, "%zu바이트 다름, 최대 차이 %d", diff, maxd);
    }
    free(a);
    free(b);
    return same;
}

int main(int argc, char **argv)
{
    const char *ref = argc > 1 ? argv[1] : OUT "/prism_ref";
    const char *new = argc > 2 ? argv[2] : "./prism";
    if (!exists(ref)) { fprintf(stderr, "기준 실행 파일이 없습니다: %s (make test로 실행하세요)\n", ref); return 2; }
    if (!exists(new)) { fprintf(stderr, "먼저 make 하세요: %s 없음\n", new); return 2; }
    if (!getcwd(g_root, sizeof g_root)) { perror("getcwd"); return 2; }
    char ref_abs[PATH_MAX], ref_dir[PATH_MAX];            /* 기준 실행 파일은 자기 폴더에서 실행한다 */
    if (!realpath(ref, ref_abs)) { perror(ref); return 2; }
    snprintf(ref_dir, sizeof ref_dir, "%s", ref_abs);
    *strrchr(ref_dir, '/') = 0;
    ref = ref_abs;
    make_dir(OUT);
    make_dir(OUT "/ref");
    make_dir(OUT "/new");

    int fail = 0;
    for (int i = 0; i < NCASES; i++) {
        const char *name = CASES[i].name;
        char txt[2][256], img[2][256], detail[128] = "";
        const char *side[2] = { "ref", "new" }, *bin[2] = { ref, new };
        int rc[2];
        for (int s = 0; s < 2; s++) {
            snprintf(txt[s], sizeof txt[s], OUT "/%s/%s.txt", side[s], name);
            snprintf(img[s], sizeof img[s], OUT "/%s/%s.ppm", side[s], name);
            unlink(img[s]);                               /* 지난번 이미지가 남아 비교되지 않게 */
            rc[s] = run(bin[s], s == 0 ? ref_dir : NULL, CASES[i].args, side[s], name, txt[s]);
        }
        int ok = rc[0] == rc[1];
        if (!ok) snprintf(detail, sizeof detail, "종료 코드 다름 (%d / %d)", rc[0], rc[1]);
        else if (!same_text(txt[0], txt[1])) { ok = 0; snprintf(detail, sizeof detail, "글자 출력 다름"); }
        else if (exists(img[0]) || exists(img[1])) ok = same_image(img[0], img[1], detail, sizeof detail);
        printf("  %s   %-16s %s\n", ok ? "같음" : "다름", name, detail);
        fail += !ok;
    }
    if (fail) printf("실패: %d개 경우가 기준 커밋과 다릅니다 (" OUT "/ref, " OUT "/new 비교)\n", fail);
    else printf("통과: 모든 경우가 기준 커밋과 같습니다\n");

    /* 방 파일: 모든 방이 풀리지 않은 상태로 시작하고, 거울 각도를 돌려서 풀 수 있어야 한다 */
    int bad = 0, nlv = 0;
    for (int n = 1; n <= 64; n++) {
        char args[32], name[32], txt[256];
        snprintf(args, sizeof args, "--check --solve --level %d", n);
        snprintf(name, sizeof name, "level_%d", n);
        snprintf(txt, sizeof txt, OUT "/new/%s.txt", name);
        int rc = run(new, NULL, args, "new", name, txt);
        size_t len;
        char *out = read_file(txt, &len);
        if (rc != 0 && out && strstr(out, "방 번호는")) { free(out); break; }   /* 방이 더 없다 */
        nlv++;
        const char *why = rc != 0 ? "실행 실패" : !out ? "출력 없음"
                        : !strstr(out, "door closed\n") ? "처음부터 문이 열려 있음"
                        : !strstr(out, "\nsolved:") ? "풀 수 없음" : NULL;
        printf("  %s   방 %d %s\n", why ? "실패" : "풀림", n, why ? why : "");
        bad += why != NULL;
        free(out);
    }
    if (bad) printf("실패: %d/%d개 방에 문제가 있습니다 (" OUT "/new/level_*.txt)\n", bad, nlv);
    else printf("통과: 방 %d개 모두 풀 수 있습니다\n", nlv);
    return fail || bad ? 1 : 0;
}
