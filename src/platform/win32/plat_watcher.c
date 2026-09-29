/* -*- coding: utf-8 -*- */
/**
 * @file plat_watcher.c
 * @brief Windows 专属：配置文件监视器（§4.5）。
 *
 * 本实现**不提供原生监视机制**，一律返回 NULL，由核心按契约回退到
 * `hp_watcher_poll_create()` 的轮询实现（mtime + size 比对）。
 *
 * 这是刻意的收敛选择：`ReadDirectoryChangesW` 需要额外的线程 / 重叠 I/O 与
 * 目录句柄管理，且其与"写临时文件 + rename 原子替换"的组合行为需要大量
 * 边界处理；轮询在配置热加载这一低频路径上成本可忽略，语义更简单可预期。
 *
 * 若后续需要原生机制，应在此文件内替换实现，**不得**改动契约或核心。
 */

#include "hplogc_platform.h"

#include <hplogc.h>

hp_watcher_t* hp_watcher_create(const char* path)
{
    (void)path;
    return NULL; /* 原生机制不可用 → 调用方回退到轮询 */
}

int hp_watcher_wait(hp_watcher_t* w, unsigned timeout_ms)
{
    (void)w;
    (void)timeout_ms;
    return -1; /* 本实现的句柄恒为 NULL，到达此处即调用方使用错误 */
}

void hp_watcher_destroy(hp_watcher_t* w)
{
    (void)w; /* 无资源可释放 */
}
