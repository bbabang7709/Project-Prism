/* 여러 모듈이 함께 쓰는 작은 수학 함수 */
#ifndef PRISM_COMMON_MATHUTIL_H
#define PRISM_COMMON_MATHUTIL_H

static inline float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

#endif
