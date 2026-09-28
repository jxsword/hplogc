/* -*- coding: utf-8 -*- */
/**
 * @file plat_watcher.c
 * @brief Linux 专属：基于 inotify 的配置文件监视器（§4.5 的第一触发源）。
 *
 * 监视**配置文件所在目录**而非文件本身：编辑器与配置管理工具常以"写临时文件 +
 * rename"的方式原子替换配置，只监视原文件会漏掉这类变更。
 * 事件到达后再用 mtime / size 比对确认，避免同一保存动作重复触发。
 */

#include "hplogc_platform.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <hplogc.h>

/** @brief inotify 监视器。 */
struct hp_watcher {
    int      fd;                        /*!< inotify 实例 fd */
    int      wd;                        /*!< 目录监视描述符 */
    char     path[HPLOGC_MAX_PATH_LEN]; /*!< 配置文件完整路径 */
    char     dir[HPLOGC_MAX_PATH_LEN];  /*!< 配置文件所在目录 */
    char     name[256];                 /*!< 配置文件名（用于过滤事件） */
    uint64_t size;                      /*!< 基线大小 */
    uint64_t mtime_ns;                  /*!< 基线修改时间 */
};

hp_watcher_t* hp_watcher_create(const char* path)
{
    hp_watcher_t* w;
    const char* base;
    uint32_t mask;
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
    snprintf(w->path, sizeof(w->path), "%s", path);
    if (hp_path_dirname(path, w->dir, sizeof(w->dir)) != HPLOGC_OK) {
        free(w);
        return NULL;
    }
    base = hp_path_basename(path);
    if (base == NULL || strlen(base) >= sizeof(w->name)) {
        free(w);
        return NULL;
    }
    snprintf(w->name, sizeof(w->name), "%s", base);

    w->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (w->fd < 0) {
        free(w);
        return NULL;
    }
    mask = IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE
           | IN_MOVED_FROM | IN_ATTRIB;
    w->wd = inotify_add_watch(w->fd, w->dir, mask);
    if (w->wd < 0) {
        close(w->fd);
        free(w);
        return NULL;
    }
    w->size = size;
    w->mtime_ns = mtime;
    return w;
}

/** @brief 排空 inotify 事件队列，返回是否出现了与配置文件相关的事件。 */
static int hp_watcher_drain(hp_watcher_t* w)
{
    union {
        struct inotify_event ev; /*!< 仅用于保证缓冲区对齐 */
        char buf[4096];
    } u;
    int hit = 0;

    for (;;) {
        ssize_t n = read(w->fd, u.buf, sizeof(u.buf));
        ssize_t off = 0;
        if (n <= 0) {
            break; /* EAGAIN 或出错：本轮结束 */
        }
        while (off + (ssize_t)sizeof(struct inotify_event) <= n) {
            const struct inotify_event* ev =
                (const struct inotify_event*)(void*)(u.buf + off);
            off += (ssize_t)sizeof(*ev) + (ssize_t)ev->len;
            if (ev->len > 0 && ev->name[0] != '\0'
                && strcmp(ev->name, w->name) == 0) {
                hit = 1;
            }
        }
    }
    return hit;
}

int hp_watcher_wait(hp_watcher_t* w, unsigned timeout_ms)
{
    struct pollfd pfd;
    int rc;

    if (w == NULL) {
        return -1;
    }
    pfd.fd = w->fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    rc = poll(&pfd, 1, (int)timeout_ms);
    if (rc < 0) {
        return (errno == EINTR) ? 0 : -1;
    }
    if (rc == 0) {
        return 0; /* 超时 */
    }
    if (!hp_watcher_drain(w)) {
        return 0; /* 与本配置文件无关的事件 */
    }
    {
        uint64_t size = 0;
        uint64_t mtime = 0;
        if (hp_file_stat(w->path, &size, &mtime) != HPLOGC_OK) {
            /* 文件正在被替换：交给下一轮 */
            return 0;
        }
        if (size == w->size && mtime == w->mtime_ns) {
            return 0; /* 内容未变的触碰事件 */
        }
        w->size = size;
        w->mtime_ns = mtime;
    }
    return 1;
}

void hp_watcher_destroy(hp_watcher_t* w)
{
    if (w == NULL) {
        return;
    }
    if (w->fd >= 0) {
        inotify_rm_watch(w->fd, w->wd);
        close(w->fd);
    }
    free(w);
}
