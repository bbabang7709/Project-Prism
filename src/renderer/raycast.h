/*
 * Renderer internals: shoot one screen ray into the room and find its colour, depth and object number.
 * Only renderer.c includes this; the game and optics modules do not.
 *
 * id buffer numbers (used by outlines and the selection border)
 *   0                 background
 *   b*8 + axis*2 + s + 1   block face (b: B_*, axis: 0 x / 1 y / 2 z, s: 1 if the normal is positive), at most 70
 *   99 - who          player bodies (99, 98) and the hint guide (97)
 *   100 + g_objat     objects (= 101 + object number)
 */
#ifndef PRISM_RENDERER_RAYCAST_H
#define PRISM_RENDERER_RAYCAST_H

#include <stdint.h>
#include "common/config.h"
#include "common/types.h"

/* values fixed for one frame: the main thread fills them, then the workers only read them after the barrier */
typedef struct {
    float ox, oy, oz, fx, fy, fz, rx, ry, rz, ux, uy, uz, tanh;
    float nearx, nearz;       /* direction toward the camera (cutting away the near walls) */
    int W, H, frame, fp, tx, tz;
    float pix;                /* size of one pixel at distance 1; at distance t it is t * pix (line widths follow the pixel size) */
    float aspect;             /* screen height/width ratio = H / W x pixel height/width ratio */
    float pixl;               /* pixel size for beam width: the larger of width and height (so beams stay solid on tall pixels) */
} Frame;

/* copy this frame's beam segments and the player bodies to draw.
 * Called by the main thread before it wakes the workers (barrier). */
void raycast_prepare(const Seg *seg, int nseg, const Body *body, int nbody, float cam_y);

static inline uint32_t pack(float r, float g, float b)
{
    int R = (int)r, G = (int)g, B = (int)b;
    return (uint32_t)((R > 255 ? 255 : R < 0 ? 0 : R) << 16 | (G > 255 ? 255 : G < 0 ? 0 : G) << 8 | (B > 255 ? 255 : B < 0 ? 0 : B));
}

/* frame data and result buffers used only inside the renderer (g_pix is public in renderer.h) */
extern Frame g_fr;
extern uint32_t g_pix[PIX_H][PIX_W];
extern float g_depth[PIX_H][PIX_W];
extern uint16_t g_id[PIX_H][PIX_W];

/* draw rows first, first + step, first + 2*step, ... of g_fr into g_pix, g_depth and g_id */
void raycast_rows(int first, int step);

/* on the same rows, re-shoot only edge pixels (object number or depth differs from a neighbour) four more times
 * and average them (anti-aliasing). Call after raycast_rows finished on every row. Changes g_pix only */
void raycast_aa_rows(int first, int step);

#endif
