/* -*- coding: utf-8 -*- */
/**
 * @file plat_signal.c
 * @brief Windows 专属：信号与 fork 语义。
 *
 * Windows **没有 POSIX 信号**，也没有 `fork()`。按契约的"不支持即安全降级"：
 * - `hp_install_sighup()` 返回 `HPLOGC_ERR_UNSUPPORTED`，由核心输出警告后
 *   仅依赖 watcher / 轮询触发热加载（§4.5 的三种触发源退化为两种）；
 * - `hp_atfork_child()` 返回 `HPLOGC_ERR_UNSUPPORTED`（无 fork 即无子进程语义）。
 *
 * 这样处理**不会**让调用方崩溃，也不会引入假的 SIGHUP 行为。
 */

#include "hplogc_platform.h"

#include <hplogc.h>

int hp_install_sighup(void (*handler)(int))
{
    (void)handler;
    return HPLOGC_ERR_UNSUPPORTED;
}

void hp_uninstall_sighup(void)
{
    /* 从未安装过处理器：无需动作 */
}

int hp_atfork_child(void (*child)(void))
{
    (void)child;
    return HPLOGC_ERR_UNSUPPORTED;
}
