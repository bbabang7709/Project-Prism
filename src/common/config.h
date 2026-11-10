/*
 * 공용 상수: 방 크기, 버퍼 크기, 스레드 수, 플레이어 몸 크기.
 * 배열 크기와 인덱스 규칙은 원본(prism.c)과 같다.
 */
#ifndef PRISM_COMMON_CONFIG_H
#define PRISM_COMMON_CONFIG_H

#define RW 16                 /* 방 크기 (칸) */
#define RD 12
#define RH 4
#define BY 1.55f              /* 빛줄기 높이 */
#define MAXOBJ 32
#define MAXSEG 512
#define MAX_W 320             /* 터미널 칸 수 상한 (픽셀 버퍼는 MAX_H * 2 줄) */
#define MAX_H 100
#define MAX_THREADS 8
#define PI 3.14159265f

/* 플레이어 */
#define PR     0.22f          /* 몸 반지름 */
#define EYE_Y  2.35f          /* 눈 높이 (바닥 윗면이 1) */

#endif
