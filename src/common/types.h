/*
 * Data passed between modules.
 *
 * Seg:  a light beam segment, made by the light simulation (optics) and drawn by the renderer.
 *       Every segment is at height BY and runs (x0, z0) -> (x1, z1) with colour (r, g, b).
 *       The renderer needs only this struct, not the optics internals.
 * Body: a player body (a cylinder of radius PR). Blocks light; drawn by the renderer. who = player number (colour).
 *       y is the jump height above the floor; only the renderer uses it
 *       (a jumping body still blocks the beam height, so optics ignore it).
 */
#ifndef PRISM_COMMON_TYPES_H
#define PRISM_COMMON_TYPES_H

typedef struct { float x0, z0, x1, z1, r, g, b; } Seg;
typedef struct { float x, z; int who; float y; } Body;

#define BODY_GUIDE 2                    /* Body.who of the hint guide (drawn only, never blocks light) */

#endif
