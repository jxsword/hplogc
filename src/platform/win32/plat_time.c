/* -*- coding: utf-8 -*- */
/**
 * @file plat_time.c
 * @brief Windows 专属：高精度时钟、本地时间转换与扩展时间格式化。
 *
 * 时间转换一律走 Win32 `FILETIME` / `SYSTEMTIME`，**不使用** CRT 的
 * `localtime` / `gmtime`：二者依赖静态 `struct tm`，在多线程下不安全，
 * 而 MSVC 与 MinGW 对 `localtime_s` / `localtime_r` 的可用性并不一致。
 */

#include "hplogc_platform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <hplogc.h>

/** @brief FILETIME 起点（1601-01-01）与 Unix 起点（1970-01-01）之差，单位 100ns。 */
#define HP_FT_EPOCH_DIFF 116444736000000000ULL

/** @brief 把 FILETIME 转换为"Unix epoch 起算的纳秒"。 */
static uint64_t hp_filetime_to_unix_ns(const FILETIME* ft)
{
    ULARGE_INTEGER ul;

    ul.LowPart = ft->dwLowDateTime;
    ul.HighPart = ft->dwHighDateTime;
    if (ul.QuadPart < HP_FT_EPOCH_DIFF) {
        return 0; /* 1970 年之前：无有效表示 */
    }
    return (uint64_t)((ul.QuadPart - HP_FT_EPOCH_DIFF) * 100ULL);
}

uint64_t hp_now_realtime_ns(void)
{
    FILETIME ft;

#if defined(_WIN32_WINNT) && (_WIN32_WINNT >= 0x0602)
    GetSystemTimePreciseAsFileTime(&ft); /* Win8+：亚微秒精度 */
#else
    GetSystemTimeAsFileTime(&ft);
#endif
    return hp_filetime_to_unix_ns(&ft);
}

uint64_t hp_now_monotonic_ns(void)
{
    LARGE_INTEGER counter;
    LARGE_INTEGER freq;

    if (!QueryPerformanceCounter(&counter) || !QueryPerformanceFrequency(&freq)
        || freq.QuadPart == 0) {
        return 0;
    }
    /* 先乘后除：counter * 1e9 在 64 位下可能溢出（counter 通常远小于 1e9/s * 时长），
       故按"秒 + 余数"拆分计算，避免精度与溢出问题。 */
    {
        uint64_t whole = (uint64_t)(counter.QuadPart / freq.QuadPart);
        uint64_t rem = (uint64_t)(counter.QuadPart % freq.QuadPart);
        return whole * 1000000000ULL
               + (uint64_t)((rem * 1000000000ULL) / (uint64_t)freq.QuadPart);
    }
}

/** @brief 判断是否为闰年（公历）。 */
static int hp_is_leap(int year)
{
    return ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
}

/** @brief 平年各月之前的累计天数。 */
static const int k_days_before_month[12] = {
    0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
};

int hp_localtime(int64_t sec, int utc, struct tm* out)
{
    FILETIME ft;
    FILETIME local;
    SYSTEMTIME st;
    ULARGE_INTEGER ul;
    int year;

    if (out == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    /* Unix 秒 → FILETIME（100ns 单位） */
    if (sec < -11644473600LL) {
        return HPLOGC_ERR_INVALID_ARG; /* 早于 1601 年，无法表示 */
    }
    ul.QuadPart = (ULONGLONG)((sec + 11644473600LL) * 10000000LL);
    ft.dwLowDateTime = ul.LowPart;
    ft.dwHighDateTime = ul.HighPart;

    if (!utc) {
        if (!FileTimeToLocalFileTime(&ft, &local)) {
            return HPLOGC_ERR_INVALID_ARG;
        }
        ft = local;
    }
    if (!FileTimeToSystemTime(&ft, &st)) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    year = (int)st.wYear;
    out->tm_year = year - 1900;
    out->tm_mon = (int)st.wMonth - 1;
    out->tm_mday = (int)st.wDay;
    out->tm_hour = (int)st.wHour;
    out->tm_min = (int)st.wMinute;
    out->tm_sec = (int)st.wSecond;
    out->tm_wday = (int)st.wDayOfWeek; /* SYSTEMTIME 与 struct tm 同为 0=周日 */
    out->tm_yday = k_days_before_month[out->tm_mon] + (out->tm_mday - 1)
                   + ((hp_is_leap(year) && out->tm_mon > 1) ? 1 : 0);
    out->tm_isdst = -1; /* 由 Win32 时区信息推导，不另行判定 */
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
