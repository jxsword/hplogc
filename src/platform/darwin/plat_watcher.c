/* -*- coding: utf-8 -*- */
/**
 * @file plat_watcher.c
 * @brief macOS 专属：基于 kqueue（EVFILT_VNODE）的配置文件监视器（§4.5 第一触发源）。
 *
 * 与 Linux 版（监视所在目录）不同，kqueue 的 vnode 过滤器**只能监视已打开的
 * 文件描述符**，无法像 inotify 那样监视目录内子项的创建 / 重命名。因此本实现
 * 直接监视配置文件本身，并在每次事件后**重新打开并重注册**，以覆盖编辑器与配置
 * 管理工具"写临时文件 + rename 原子替换"的场景（替换后原 vnode 失效）。
 *
 * 事件到达后再用 mtime / size 比对确认，避免同一保存动作重复触发（与 Linux 版
 * 一致的去重语义）。
 */

#include "hplogc_platform.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>

#include <hplogc.h>

#ifndef O_EVTONLY
/** @brief 非 macOS 环境下的兜底（本文件仅在 darwin 分支编译，此处仅为安全网）。 */
#define O_EVTONLY O_RDONLY
#endif

/** @brief kqueue 监视器。 */
struct hp_watcher {
    int      kq;                        /*!< kqueue 实例 */
    int      fd;                        /*!< 被监视文件的描述符（O_EVTONLY） */
    char     path[HPLOGC_MAX_PATH_LEN]; /*!< 配置文件完整路径 */
    uint64_t size;                      /*!< 基线大小 */
    uint64_t mtime_ns;                  /*!< 基线修改时间 */
};

/**
 * @brief 注册（或重新注册）配置文件的 vnode 监视。
 *
 * 若此前已有监视，先注销并关闭旧描述符——文件被原子替换后旧描述符对应的
 * vnode 不再随路径变化。
 *
 * @return 0 成功；负值表示失败（调用方应回退到轮询）。
 */
static int hp_watcher_arm(hp_watcher_t* w)
{
    struct kevent ev;

    if (w->fd >= 0) {
        EV_SET(&ev, w->fd, EVFILT_VNODE, EV_DELETE, 0, 0, NULL);
        (void)kevent(w->kq, &ev, 1, NULL, 0, NULL);
        close(w->fd);
        w->fd = -1;
    }
    w->fd = open(w->path, O_EVTONLY);
    if (w->fd < 0) {
        return HPLOGC_ERR_IO;
    }
    EV_SET(&ev, w->fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
           NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_ATTRIB, 0, NULL);
    if (kevent(w->kq, &ev, 1, NULL, 0, NULL) < 0) {
        close(w->fd);
        w->fd = -1;
        return HPLOGC_ERR_IO;
    }
    return HPLOGC_OK;
}

hp_watcher_t* hp_watcher_create(const char* path)
{
    hp_watcher_t* w;
    uint64_t size = 0;
    uint64_t mtime = 0;

    if (path == NULL) {
        return NULL;
    }
    if (hp_file_stat(path, &size, &mtime) != HPLOGC_OK) {
        return NULL; /* 非普通文件或不存在：由核心按 §10.4 处理 */
    }
    w = (hp_watcher_t*)malloc(sizeof(*w));
    if (w == NULL) {
        return NULL;
    }
    memset(w, 0, sizeof(*w));
    w->kq = -1;
    w->fd = -1;
    snprintf(w->path, sizeof(w->path), "%s", path);
    w->size = size;
    w->mtime_ns = mtime;

    w->kq = kqueue();
    if (w->kq < 0) {
        free(w);
        return NULL;
    }
    if (hp_watcher_arm(w) != HPLOGC_OK) {
        close(w->kq);
        free(w);
        return NULL;
    }
    return w;
}

int hp_watcher_wait(hp_watcher_t* w, unsigned timeout_ms)
{
    struct kevent ev;
    struct timespec ts;
    int n;
    uint64_t size = 0;
    uint64_t mtime = 0;

    if (w == NULL || w->kq < 0 || w->fd < 0) {
        return -1;
    }
    ts.tv_sec = (time_t)(timeout_ms / 1000u);
    ts.tv_nsec = (long)((timeout_ms % 1000u) * 1000000u);

    n = kevent(w->kq, NULL, 0, &ev, 1, &ts);
    if (n < 0) {
        return (errno == EINTR) ? 0 : -1;
    }
    if (n == 0) {
        return 0; /* 超时 */
    }
    /* 事件到达：以 mtime / size 比对确认确为内容变更 */
    if (hp_file_stat(w->path, &size, &mtime) != HPLOGC_OK) {
        /* 文件正被替换：重新挂接后交给下一轮 */
        (void)hp_watcher_arm(w);
        return 0;
    }
    if (size == w->size && mtime == w->mtime_ns) {
        return 0; /* 内容未变的触碰事件 */
    }
    w->size = size;
    w->mtime_ns = mtime;
    /* 原子替换后旧 vnode 失效：重新挂接；挂接失败则让调用方回退到轮询 */
    if (hp_watcher_arm(w) != HPLOGC_OK) {
        return -1;
    }
    return 1;
}

void hp_watcher_destroy(hp_watcher_t* w)
{
    if (w == NULL) {
        return;
    }
    if (w->fd >= 0) {
        struct kevent ev;
        EV_SET(&ev, w->fd, EVFILT_VNODE, EV_DELETE, 0, 0, NULL);
        (void)kevent(w->kq, &ev, 1, NULL, 0, NULL);
        close(w->fd);
    }
    if (w->kq >= 0) {
        close(w->kq);
    }
    free(w);
}
