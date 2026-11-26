/*
 * Renderer: draws the room as seen from the camera into the pixel buffer (g_pix).
 *
 * It never reads game state; it takes a View filled in by the caller.
 * Beams also come in the View, as Seg segments, not from the optics module.
 * The world (blocks, objects) is read only.
 *
 * Threads: renderer_init creates the workers, renderer_draw splits one frame between them with barriers.
 * No other thread may change the world or the segments until renderer_draw returns.
 */
#ifndef PRISM_RENDERER_H
#define PRISM_RENDERER_H

#include <stdint.h>
#include "common/config.h"
#include "common/types.h"

typedef struct {
    int fp;                           /* 1: first person (player's eye), 0: top-down */
    float px, pz, pyaw, ppitch;       /* player position and view direction */
    float cam_yaw, cam_pitch, cam_dist;   /* top-down camera */
    int tx, tz;                       /* floor cell marked as a place to put an object (-1 = none) */
    int sel_obj;                      /* object to outline in yellow (-1 = none) */
    const Seg *seg;                   /* beam segments */
    int nseg;
    const Body *bodies;               /* player bodies to draw (in first person, without your own) */
    int nbody;
    float paspect;                    /* pixel height/width ratio (0 means 1: square). Terminal quadrant blocks: 2 */
    float py;                         /* first person: jump height added to the eye height */
} View;

/* the result: H rows from the top, W columns from the left (0xRRGGBB) */
extern uint32_t g_pix[PIX_H][PIX_W];

/* timings of the last renderer_draw, for the performance overlay */
typedef struct {
    int nthreads, W, H;                       /* threads and the pixel size that was drawn */
    float frame_ms;                           /* whole renderer_draw */
    float work_ms[MAX_THREADS];               /* per thread: tracing its rows + edge resampling */
    float wait_ms[MAX_THREADS];               /* per thread: idle at the middle barrier */
} RenderStats;
const RenderStats *renderer_stats(void);

int renderer_init(void);              /* creates the worker threads; returns how many threads render */
void renderer_shutdown(void);         /* stops the worker threads and waits for them */

/* draw one W x H frame. frame drives crystal flicker and the selection pulse */
void renderer_draw(const View *v, int W, int H, int frame);

/* save the W x H area of g_pix, scaled up scale times, as PPM (P6) */
void renderer_write_ppm(const char *path, int W, int H, int scale);

/* save the same area as PNG (stored deflate blocks, no external library). -1 on failure */
int renderer_write_png(const char *path, int W, int H, int scale);

#endif
