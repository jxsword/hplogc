/* -*- coding: utf-8 -*- */
/**
 * @file time.c
 * @brief 时间戳渲染（§12、§12.2、§18-C4）。
 *
 * `hp_strftime_ext` 的实现位于平台契约层（`plat_time.c`），本文件只调用它。
 */

#include "hplogc_internal.h"

#include <stdio.h>

#include <hplogc.h>

size_t hp_time_render(char* buf, size_t cap, uint64_t ts_ns,
                      const hp_config_t* cfg)
{
    if (buf == NULL || cap == 0 || cfg == NULL) {
        return 0;
    }
    buf[0] = '\0';

    if (cfg->ts_monotonic) {
        /* monotonic：固定输出"秒.6 位微秒"（相对 init，§18-C4） */
        uint64_t rel_ns = ts_ns;
        unsigned long long sec;
        unsigned long long usec;
        if (rel_ns >= g_rt.init_mono_ns) {
            rel_ns -= g_rt.init_mono_ns;
        }
        sec = rel_ns / 1000000000ull;
        usec = (rel_ns % 1000000000ull) / 1000ull;
        return (size_t)snprintf(buf, cap, "%llu.%06llu", sec, usec);
    }

    {
        int64_t sec = (int64_t)(ts_ns / 1000000000ull);
        long usec = (long)((ts_ns % 1000000000ull) / 1000ull);
        struct tm tm;
        if (hp_localtime(sec, cfg->utc, &tm) != HPLOGC_OK) {
            return 0;
        }
        return hp_strftime_ext(buf, cap, cfg->time_format, &tm, usec);
    }
}
