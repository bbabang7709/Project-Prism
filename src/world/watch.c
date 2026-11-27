/*
 * watch.c - inotify on the level folder, with a modification-time fingerprint as a fallback.
 */
#define _GNU_SOURCE
#include "world/watch.h"

#include <dirent.h>
#include <fcntl.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>
#include "world/world.h"

static int s_ifd = -1;                  /* inotify descriptor (-1: not watching) */
static int64_t s_reload_at;             /* when to report a change (ms), 0 = nothing pending */
static int64_t s_sig_check;             /* last fingerprint check (ms) */
static uint64_t s_sig;                  /* fingerprint of the level folder at the last load */

/* order-independent fingerprint of the .lvl files: FNV-1a of each name mixed with its size and mtime */
static uint64_t signature(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    uint64_t sum = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        struct stat st;
        if (l < 5 || strcmp(e->d_name + l - 4, ".lvl") || fstatat(dirfd(d), e->d_name, &st, 0) != 0) continue;
        uint64_t h = 1469598103934665603ULL;
        for (size_t i = 0; i < l; i++) h = (h ^ (unsigned char)e->d_name[i]) * 1099511628211ULL;
        h ^= (uint64_t)st.st_size * 0x9E3779B97F4A7C15ULL;
        h ^= ((uint64_t)st.st_mtim.tv_sec * 1000000000ULL + (uint64_t)st.st_mtim.tv_nsec) * 0xC2B2AE3D27D4EB4FULL;
        sum += h;
    }
    closedir(d);
    return sum;
}

void watch_loaded(void) { s_sig = signature(g_level_dir); }

int watch_start(void)
{
    watch_loaded();
    if (s_ifd >= 0) return s_ifd;
    s_ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (s_ifd >= 0 && inotify_add_watch(s_ifd, g_level_dir, IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE | IN_CREATE) < 0) {
        close(s_ifd);                                        /* no level folder (built-in room): fingerprint only */
        s_ifd = -1;
    }
    return s_ifd;
}

void watch_stop(void)
{
    if (s_ifd >= 0) close(s_ifd);
    s_ifd = -1;
}

void watch_events(int64_t now)
{
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t n;
    if (s_ifd < 0) return;
    while ((n = read(s_ifd, buf, sizeof buf)) > 0)
        for (char *p = buf; p < buf + n; p += sizeof(struct inotify_event) + ((struct inotify_event *)p)->len) {
            const struct inotify_event *ev = (const struct inotify_event *)p;
            size_t l = ev->len ? strlen(ev->name) : 0;
            if (l >= 5 && !strcmp(ev->name + l - 4, ".lvl")) s_reload_at = now + 150;
        }
}

int watch_due(int64_t now)
{
    if (now - s_sig_check >= 1000) {                         /* fallback for edits inotify cannot see */
        s_sig_check = now;
        if (!s_reload_at && signature(g_level_dir) != s_sig) s_reload_at = now;
    }
    if (!s_reload_at || now < s_reload_at) return 0;
    s_reload_at = 0;
    return 1;
}
