/* small math helpers shared by several modules */
#ifndef PRISM_COMMON_MATHUTIL_H
#define PRISM_COMMON_MATHUTIL_H

static inline float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

#endif
