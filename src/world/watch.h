/*
 * Level folder watch: tells the game loop when the level files changed on disk.
 *
 * inotify reports writes made inside Linux. WSL's /mnt drives do not report writes
 * made by Windows programs (Notepad etc.), so once a second a fingerprint of the
 * .lvl files' names, sizes and modification times is compared as well.
 * Editors write in bursts, so a change is reported 150 ms after the last event.
 */
#ifndef PRISM_WORLD_WATCH_H
#define PRISM_WORLD_WATCH_H

#include <stdint.h>

int watch_start(void);                  /* watch g_level_dir; returns the inotify fd for poll (-1: fingerprint only) */
void watch_stop(void);
void watch_loaded(void);                /* the level files were just (re)read: remember their fingerprint */
void watch_events(int64_t now_ms);      /* the inotify fd is readable: drain it */
int watch_due(int64_t now_ms);          /* 1 once when the levels should be reloaded now */

#endif
