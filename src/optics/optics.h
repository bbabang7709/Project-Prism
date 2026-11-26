/*
 * Light simulation: follows each beam from its emitter cell by cell and handles
 * mirror reflection, half-mirror splitting, prism dispersion, glass refraction (Snell's law, total internal
 * reflection), colour filters, portals (continue from the paired portal), boxes (stop) and sensors (record light).
 *
 * Input   the world (g_blk, g_obj, g_objat) and the bodies that block light (radius PR)
 * Output  g_seg / g_nseg      beam segments (drawn by the renderer)
 *         g_obj[i].got, .lit  light received by crystals and sensors, and whether they are lit
 *         return value        whether every crystal is lit
 *
 * It never changes world blocks. Opening and closing doors is up to the caller (the game).
 * Door blocks stop light, so the next result depends on the doors changed in between.
 */
#ifndef PRISM_OPTICS_H
#define PRISM_OPTICS_H

#include "common/config.h"
#include "common/types.h"

extern Seg g_seg[MAXSEG];                /* read-only (only optics writes them) */
extern int g_nseg;

/* recompute all light. bodies are the player bodies that block light (at most 8) */
int optics_simulate(const Body *bodies, int nbody);

/* returns the number of lit crystals and stores the total number in *total */
int optics_count_lit(int *total);

#endif
