/* -*- coding: utf-8 -*- */
/**
 * @file plat_time.c
 * @brief POSIX 共享层：高精度时钟、本地时间转换与扩展时间格式化。
 */

#include "hplogc_platform.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <hplogc.h>

uint64_t hp_now_realtime_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t hp_now_monotonic_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int hp_localtime(int64_t sec, int utc, struct tm* out)
{
    time_t t;
    if (out == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    t = (time_t)sec;
    if (utc) {
        if (gmtime_r(&t, out) == NULL) {
            return HPLOGC_ERR_INVALID_ARG;
        }
    } else {
        if (localtime_r(&t, out) == NULL) {
            return HPLOGC_ERR_INVALID_ARG;
        }
    }
    return HPLOGC_OK;
}

/** @brief 追加固定宽度的小数部分（取秒的小数前 @p n 位，零填充）。 */
static size_t hp_append_frac(char* buf, size_t cap, size_t used, long usec,
                             int n)
{
    char digits[8];
    int i;

    /* usec 固定 6 位，按需截断（不做四舍五入：截断语义简单且可预期） */
    snprintf(digits, sizeof(digits), "%06ld", (usec >= 0 && usec < 1000000L)
                                                  ? usec
                                                  : (usec % 1000000L));
    for (i = 0; i < n && used < cap - 1; i++) {
        buf[used++] = digits[i];
    }
    return used;
}

size_t hp_strftime_ext(char* buf, size_t cap, const char* fmt,
                       const struct tm* tm, long usec)
{
    size_t used = 0;
    const char* p = fmt;

    if (buf == NULL || cap == 0 || fmt == NULL || tm == NULL) {
        return 0;
    }
    buf[0] = '\0';
    while (*p != '\0' && used < cap - 1) {
        if (*p != '%') {
            buf[used++] = *p++;
            continue;
        }
        if (p[1] == '%') {
            buf[used++] = '%';
            p += 2;
            continue;
        }
        if (p[1] == 'f') {
            used = hp_append_frac(buf, cap, used, usec, 6);
            p += 2;
            continue;
        }
        if (p[1] == 'F' && p[2] >= '1' && p[2] <= '6') {
            used = hp_append_frac(buf, cap, used, usec, p[2] - '0');
            p += 3;
            continue;
        }
        /* 其余说明符（含 strftime 修饰符与宽度）整体交给系统 strftime */
        {
            char spec[16];
            size_t k = 0;
            spec[k++] = *p++;
            while (k + 1 < sizeof(spec) && *p != '\0'
                   && (strchr("-_0^#EO", *p) != NULL
                       || (*p >= '0' && *p <= '9'))) {
                spec[k++] = *p++;
            }
            if (k + 1 < sizeof(spec) && *p != '\0') {
                spec[k++] = *p++;
            }
            spec[k] = '\0';
            used += strftime(buf + used, cap - used, spec, tm);
        }
    }
    buf[used < cap ? used : cap - 1] = '\0';
    return used;
}
