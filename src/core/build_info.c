/* -*- coding: utf-8 -*- */
/**
 * @file build_info.c
 * @brief 构建信息查询（§7.3）。
 */

#include "hplogc_internal.h"

#include <hplogc.h>

#ifndef HPLOGC_SINK_LIST
/** @brief 未在 CMake 中注入时的兜底（正常构建不会出现）。 */
#define HPLOGC_SINK_LIST ""
#endif

void hplogc_get_build_info(hplogc_build_info_t* info)
{
    if (info == NULL) {
        return;
    }
    info->build_version = HPLOGC_PRESET_NAME;
#ifdef HPLOGC_HAS_ASYNC
    info->has_async = 1;
#else
    info->has_async = 0;
#endif
#ifdef HPLOGC_HAS_COLOR
    info->has_color = 1;
#else
    info->has_color = 0;
#endif
#ifdef HPLOGC_HAS_ROTATE
    info->has_rotate = 1;
#else
    info->has_rotate = 0;
#endif
#ifdef HPLOGC_HAS_HOT_RELOAD
    info->has_hot_reload = 1;
#else
    info->has_hot_reload = 0;
#endif
#ifdef HPLOGC_HAS_CATEGORY
    info->has_category = 1;
#else
    info->has_category = 0;
#endif
#ifdef HPLOGC_HAS_THROTTLE
    info->has_throttle = 1;
#else
    info->has_throttle = 0;
#endif
#ifdef HPLOGC_HAS_INI
    info->has_ini = 1;
#else
    info->has_ini = 0;
#endif
#ifdef HPLOGC_USE_LOCKFREE
    info->lockfree = 1;
#else
    info->lockfree = 0;
#endif
#ifdef HPLOGC_CONCURRENCY_SPSC
    info->concurrency = "spsc";
#else
    info->concurrency = "mpsc";
#endif
    info->has_fields = 1; /* v0.2 起恒为 1（§7.6） */
    info->sinks = HPLOGC_SINK_LIST;
}
