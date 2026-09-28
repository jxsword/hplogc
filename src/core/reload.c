/* -*- coding: utf-8 -*- */
/**
 * @file reload.c
 * @brief 热加载监视线程：inotify / mtime 轮询 / SIGHUP 三种触发源（§4.5 / §10.5）。
 *
 * 触发优先级：SIGHUP 原子标志 > 平台 watcher（inotify）> mtime 轮询（按
 * `hot reload interval`）。三种触发源均不可用时仍以 500ms 节奏空转检查停止 / 信号
 * 标志（§18-C3）。
 */

#include "hplogc_internal.h"

#include <stdlib.h>

#include <hplogc.h>

/** @brief 上次记录的配置文件 mtime（避免轮询误触发）。 */
static uint64_t g_last_mtime;

/** @brief 执行一次热加载（解析 + 装配 + 原子替换）。 */
int hp_reload_do(void)
{
    hp_ini_t  ini;
    hp_config_t c;
    int       rc;

    if (!g_rt.has_config_path) {
        return HPLOGC_ERR_CONFIG;
    }
    rc = hp_ini_parse(g_rt.config_path, &ini);
    if (rc != HPLOGC_OK) {
        hp_warn_throttled("hplogc: 热加载解析失败，旧配置保持（整体回滚）");
        return rc;
    }
    rc = hp_conf_build(&ini, g_rt.config_path, &c);
    hp_ini_free(&ini);
    if (rc != HPLOGC_OK) {
        hp_warn_throttled("hplogc: 热加载装配失败，旧配置保持（整体回滚）");
        return rc;
    }
    rc = hp_runtime_reload_apply(&c);
    if (rc != HPLOGC_OK) {
        hp_warn_throttled("hplogc: 热加载运行时装配失败，旧配置保持（整体回滚）");
        return rc;
    }
    hp_warn_throttled("hplogc: 热加载成功，配置已切换");
    return HPLOGC_OK;
}

/** @brief 监视线程主循环。 */
static void hp_reload_main(void* arg)
{
    (void)arg;
    for (;;) {
        int interval;
        int stop = hp_atomic_load_i32(&g_rt.reload_stop);

        if (stop) {
            break;
        }
        hp_mutex_lock(&g_rt.reload_mu);
        hp_cond_timedwait(&g_rt.reload_cv, &g_rt.reload_mu, 500);
        hp_mutex_unlock(&g_rt.reload_mu);
        if (hp_atomic_load_i32(&g_rt.reload_stop)) {
            break;
        }

        {
            int do_reload = 0;
            int sig = hp_atomic_load_i32(&g_rt.reload_signal);
            if (sig) {
                hp_atomic_store_i32(&g_rt.reload_signal, 0);
                do_reload = 1;
            } else if (g_rt.has_config_path && g_rt.saved_cfg.hot_reload_interval > 0) {
                uint64_t mt = 0;
                if (hp_file_stat(g_rt.config_path, NULL, &mt) == HPLOGC_OK) {
                    if (g_last_mtime != 0 && mt != g_last_mtime) {
                        do_reload = 1;
                    }
                    g_last_mtime = mt;
                }
            }
            if (!do_reload && g_rt.has_config_path) {
                /** @brief inotify 优先：尝试创建 watcher。 */
                hp_watcher_t* w = hp_watcher_create(g_rt.config_path);
                if (w != NULL) {
                    int r = hp_watcher_wait(
                        w, g_rt.saved_cfg.hot_reload_interval > 0 ? 200 : 400);
                    hp_watcher_destroy(w);
                    if (r == 1) {
                        do_reload = 1;
                    }
                }
            }
            if (do_reload) {
                hp_reload_do();
            }
        }
        (void)interval;
    }
    return;
}

int hp_reload_start(void)
{
    uint64_t mt = 0;
    if (hp_file_stat(g_rt.config_path, NULL, &mt) == HPLOGC_OK) {
        g_last_mtime = mt;
    }
    if (hp_thread_create(&g_rt.reload_th, hp_reload_main, NULL) != HPLOGC_OK) {
        return HPLOGC_ERR_NO_MEM;
    }
    g_rt.reload_started = 1;
    return HPLOGC_OK;
}

void hp_reload_stop(void)
{
    if (!g_rt.reload_started) {
        return;
    }
    hp_atomic_store_i32(&g_rt.reload_stop, 1);
    hp_mutex_lock(&g_rt.reload_mu);
    hp_cond_broadcast(&g_rt.reload_cv);
    hp_mutex_unlock(&g_rt.reload_mu);
    hp_thread_join(&g_rt.reload_th);
    g_rt.reload_started = 0;
}
