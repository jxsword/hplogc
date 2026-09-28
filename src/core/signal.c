/* -*- coding: utf-8 -*- */
/**
 * @file signal.c
 * @brief signal safe 通道、SIGHUP 热加载信号与 fork 行为（§4.5 / §7.3 / §10.5）。
 */

#include "hplogc_internal.h"

#include <signal.h>
#include <stdlib.h>

#include <hplogc.h>

/** @brief SIGHUP 处理：仅置原子标志（async-signal-safe）。 */
static void hp_sighup_handler(int sig)
{
    (void)sig;
    if (hp_atomic_load_i32(&g_rt.init_state) == 1) {
        hp_atomic_store_i32(&g_rt.reload_signal, 1);
    }
}

/** @brief 子进程 fork 后置回调：按 fork behavior 调整日志状态（§10.5）。 */
static void hp_signal_atfork_child(void)
{
    int beh = HPLOGC_FORK_REINIT;
    hp_runtime_t* rt = hp_rt_active();

    if (rt != NULL) {
        beh = rt->cfg.fork_behavior;
    }
    if (beh == HPLOGC_FORK_DISABLE) {
        hp_atomic_store_i32(&g_rt.child_disabled, 1);
    } else if (beh == HPLOGC_FORK_REINIT) {
        hp_atomic_store_i32(&g_rt.need_reinit, 1);
    }
    /* inherit：沿用父进程状态（不做处理） */
}

void hp_signal_init(void)
{
    struct sigaction sa;
    hp_runtime_t* rt = hp_rt_active();

    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = hp_sighup_handler;
    if (rt != NULL && rt->cfg.signal_reload) {
        sigaction(SIGHUP, &sa, NULL);
    }
    /* atfork 仅注册一次（重复注册无害）；child 回调按当前配置决定行为 */
    hp_atfork_child(hp_signal_atfork_child);
}

void hp_signal_fini(void)
{
    struct sigaction sa;
    hp_runtime_t* rt = hp_rt_active();

    if (rt != NULL && rt->cfg.signal_reload) {
        memset(&sa, 0, sizeof(sa));
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sa.sa_handler = SIG_DFL;
        sigaction(SIGHUP, &sa, NULL);
    }
}
