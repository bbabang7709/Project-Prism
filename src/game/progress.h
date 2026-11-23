/*
 * 진행 기록: 푼 방과 방마다 가장 적은 조작 횟수를 홈 폴더의 파일에 남긴다.
 *
 * 파일  $HOME/.prism_progress (HOME이 없으면 기록하지 않는다. 환경 변수 PRISM_PROGRESS로 바꿀 수 있다)
 * 형식  한 줄에 "방파일이름 조작횟수" (예: "03-meeting.lvl 7"). 모르는 줄은 건너뛴다
 * 저장  임시 파일에 쓰고 fsync한 뒤 rename으로 바꿔 넣는다 (방 파일 저장과 같은 방식)
 *
 * 방은 방 파일 이름으로 구분한다. 기본 방처럼 파일이 없는 방은 기록하지 않는다.
 */
#ifndef PRISM_GAME_PROGRESS_H
#define PRISM_GAME_PROGRESS_H

/* 기록 파일을 읽는다 (없으면 빈 기록). 여러 번 불러도 된다 */
void progress_load(void);

/* 그 방을 푼 가장 적은 조작 횟수. 아직 못 풀었으면 -1 */
int progress_best(const char *file);

/* 방을 풀었다. 처음 풀었거나 조작 횟수가 줄었으면 기록하고 저장한다.
 * 새 기록이면 1, 아니면 0, 저장하지 못했으면 -1 */
int progress_record(const char *file, int moves);

#endif
