/* -*- coding: utf-8 -*- */
/**
 * @file plat_watcher_poll.c
 * @brief POSIX 共享层：轮询式配置文件监视器（平台原生机制不可用时的回退）。
 *
 * 以 mtime + size 比对判定变更（§4.5 的第二种触发源）。
 */

#include "hplogc_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hplogc.h>

/** @brief 轮询监视器。 */
struct hp_watcher {
    char path[HPLOGC_MAX_PATH_LEN]; /*!< 被监视的配置文件路径 */
    uint64_t size;                  /*!< 基线大小 */
    uint64_t mtime_ns;              /*!< 基线修改时间 */
};

hp_watcher_t* hp_watcher_poll_create(const char* path)
{
    hp_watcher_t* w;
    uint64_t size = 0;
    uint64_t mtime = 0;

    if (path == NULL) {
        return NULL;
    }
    if (hp_file_stat(path, &size, &mtime) != HPLOGC_OK) {
        return NULL;
    }
    w = (hp_watcher_t*)malloc(sizeof(*w));
    if (w == NULL) {
        return NULL;
    }
    snprintf(w->path, sizeof(w->path), "%s", path);
    w->size = size;
    w->mtime_ns = mtime;
    return w;
}

int hp_watcher_wait(hp_watcher_t* w, unsigned timeout_ms)
{
    uint64_t size = 0;
    uint64_t mtime = 0;
    unsigned waited = 0;

    if (w == NULL) {
        return -1;
    }
    /* 以 100ms 为步进轮询，保证 shutdown 能及时打断 */
    while (waited < timeout_ms) {
        unsigned step = (timeout_ms - waited < 100u) ? (timeout_ms - waited)
                                                     : 100u;
        hp_sleep_ms(step);
        waited += step;
        if (hp_file_stat(w->path, &size, &mtime) != HPLOGC_OK) {
            /* 文件暂时不可访问（如被原子替换的瞬间）：下一轮继续 */
            continue;
        }
        if (size != w->size || mtime != w->mtime_ns) {
            w->size = size;
            w->mtime_ns = mtime;
            return 1;
        }
    }
    return 0;
}

void hp_watcher_destroy(hp_watcher_t* w)
{
    free(w);
}
