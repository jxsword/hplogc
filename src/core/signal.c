/* -*- coding: utf-8 -*- */
/**
 * @file signal.c
 * @brief signal safe 通道、SIGHUP 热加载信号与 fork 行为（§4.5 / §7.3 / §10.5）。
 */

#include "hplogc_internal.h"

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
    hp_runtime_t* rt = hp_rt_active();

    if (rt != NULL && rt->cfg.signal_reload) {
        /* 信号安装一律走平台契约（§15）：核心层不得直接调用平台 API。
           不支持 SIGHUP 的平台（Windows）返回 HPLOGC_ERR_UNSUPPORTED，
           此时热加载仅依赖 watcher / 轮询两种触发源（§4.5）；
           返回 1 表示宿主已安装处理器，按契约不覆盖。 */
        (void)hp_install_sighup(hp_sighup_handler);
    }
    /* atfork 仅注册一次（重复注册无害）；child 回调按当前配置决定行为 */
    hp_atfork_child(hp_signal_atfork_child);
}

void hp_signal_fini(void)
{
    hp_runtime_t* rt = hp_rt_active();

    if (rt != NULL && rt->cfg.signal_reload) {
        hp_uninstall_sighup();
    }
}
