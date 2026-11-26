/*
 * Progress: solved rooms and the fewest moves for each, kept in a file in the home folder.
 *
 * File    $HOME/.prism_progress (nothing is saved without HOME; PRISM_PROGRESS overrides the path)
 * Format  one "room-file-name moves" per line (e.g. "03-meeting.lvl 7"); unknown lines are skipped
 * Saving  write a temp file, fsync, then rename over the old one (same as saving a level)
 *
 * Rooms are keyed by their file name; rooms without a file (the built-in room) are not recorded.
 */
#ifndef PRISM_GAME_PROGRESS_H
#define PRISM_GAME_PROGRESS_H

/* read the progress file (empty if missing); safe to call again */
void progress_load(void);

/* fewest moves that solved the room; -1 if not solved yet */
int progress_best(const char *file);

/* a room was solved: record and save it if it is new or took fewer moves.
 * 1 = new record, 0 = not, -1 = could not save */
int progress_record(const char *file, int moves);

#endif
