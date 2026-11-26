/*
 * Shared constants: room size, buffer sizes, thread count, player body size.
 * Array sizes and index rules are the same as in the original prototype (prism.c).
 */
#ifndef PRISM_COMMON_CONFIG_H
#define PRISM_COMMON_CONFIG_H

#define RW 16                 /* room size (cells) */
#define RD 12
#define RH 4
#define BY 1.55f              /* height of the light beams */
#define MAXOBJ 32
#define MAXSEG 512
#define MAX_W 320             /* upper limit of terminal cells */
#define MAX_H 100
#define PIX_W 720             /* pixel buffer: holds both the terminal (MAX_W x MAX_H*2) and screenshots (720x420) */
#define PIX_H 420
#define MAX_THREADS 8
#define PI 3.14159265f

/* player */
#define PR     0.22f          /* body radius */
#define EYE_Y  2.35f          /* eye height (the top of the floor is 1) */

#endif
