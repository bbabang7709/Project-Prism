/* 단조 시계 (밀리초, 마이크로초) */
#ifndef PRISM_PLATFORM_CLOCK_H
#define PRISM_PLATFORM_CLOCK_H

#include <stdint.h>
#include <time.h>

static inline int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static inline int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

#endif
