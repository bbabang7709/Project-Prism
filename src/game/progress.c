#include "game/progress.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAXREC 64

static struct { char file[64]; int moves; } s_rec[MAXREC];
static int s_nrec;

/* path of the progress file; 0 if there is nowhere to write */
static int path_of(char *out, size_t n)
{
    const char *p = getenv("PRISM_PROGRESS");
    if (p && p[0]) { snprintf(out, n, "%s", p); return 1; }
    const char *home = getenv("HOME");
    if (!home || !home[0]) return 0;
    snprintf(out, n, "%s/.prism_progress", home);
    return 1;
}

void progress_load(void)
{
    char path[512], buf[8192];
    s_nrec = 0;
    if (!path_of(path, sizeof path)) return;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;                                   /* a first-time player: empty progress */
    size_t got = 0;
    while (got < sizeof buf - 1) {
        ssize_t k = read(fd, buf + got, sizeof buf - 1 - got);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        got += (size_t)k;
    }
    close(fd);
    buf[got] = 0;
    for (char *line = buf, *next; line && *line && s_nrec < MAXREC; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        char file[64];
        int moves;
        if (sscanf(line, "%63s %d", file, &moves) != 2 || moves < 0) continue;   /* drop broken lines */
        snprintf(s_rec[s_nrec].file, sizeof s_rec[s_nrec].file, "%s", file);
        s_rec[s_nrec++].moves = moves;
    }
}

static int find(const char *file)
{
    for (int i = 0; i < s_nrec; i++)
        if (!strcmp(s_rec[i].file, file)) return i;
    return -1;
}

int progress_best(const char *file)
{
    int i = file && file[0] ? find(file) : -1;
    return i < 0 ? -1 : s_rec[i].moves;
}

static int save(void)
{
    char path[512], tmp[520], text[MAXREC * 80];
    if (!path_of(path, sizeof path)) return -1;
    size_t n = 0;
    for (int i = 0; i < s_nrec; i++) n += (size_t)snprintf(text + n, sizeof text - n, "%s %d\n", s_rec[i].file, s_rec[i].moves);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    const char *p = text;
    while (n > 0) {
        ssize_t k = write(fd, p, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) { close(fd); unlink(tmp); return -1; }
        p += k;
        n -= (size_t)k;
    }
    if (fsync(fd) < 0) { close(fd); unlink(tmp); return -1; }
    close(fd);
    if (rename(tmp, path) < 0) { unlink(tmp); return -1; }
    return 0;
}

int progress_record(const char *file, int moves)
{
    if (!file || !file[0] || strchr(file, ' ') || moves < 0) return 0;
    int i = find(file);
    if (i >= 0 && s_rec[i].moves <= moves) return 0;
    if (i < 0) {
        if (s_nrec >= MAXREC) return 0;
        i = s_nrec++;
        snprintf(s_rec[i].file, sizeof s_rec[i].file, "%s", file);
    }
    s_rec[i].moves = moves;
    return save() < 0 ? -1 : 1;
}
