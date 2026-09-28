/* -*- coding: utf-8 -*- */
/**
 * @file async.c
 * @brief 异步消费者线程：批量取出、路由、整行组装与输出（§4.4 / §4.9 ⑤⑥⑦⑧）。
 *
 * 仅当 `HPLOGC_ENABLE_ASYNC=ON` 编译参与；同步构建下本文件不进入编译单元。
 */

#include "hplogc_internal.h"

#include <stdlib.h>

#include <hplogc.h>

#ifndef HPLOGC_HAS_ASYNC
/* 同步构建：消费者相关函数为空实现（调用方仍声明存在以统一链接） */
int hp_async_start(void)
{
    return HPLOGC_OK;
}
void hp_async_stop(void)
{
}
void hp_async_kick(void)
{
}
#else

/** @brief 消费者主循环。 */
static void hp_async_main(void* arg)
{
    hp_runtime_t* rt = hp_rt_active();
    size_t rec_stride;
    size_t line_stride;
    char*  recs = NULL;
    char*  lines = NULL;
    size_t* lens = NULL;
    size_t batch;

    (void)arg;
    if (rt == NULL) {
        return;
    }
    batch = (rt->cfg.batch_size > 0) ? rt->cfg.batch_size : 64;
    rec_stride = rt->cfg.max_log_length + HPLOGC_REC_EXTRA;
    line_stride = rt->cfg.max_log_length + 2;

    recs = (char*)malloc(rec_stride * batch);
    lines = (char*)malloc(line_stride * batch);
    lens = (size_t*)malloc(sizeof(size_t) * batch);
    if (recs == NULL || lines == NULL || lens == NULL) {
        free(recs);
        free(lines);
        free(lens);
        return;
    }

    hp_mutex_lock(&g_rt.consumer_mu);
    for (;;) {
        int n = 0;
        int stop = hp_atomic_load_i32(&g_rt.consumer_stop);

        /* 批量取出 */
        while (n < (int)batch) {
            size_t l = 0;
            int rc = hp_ring_pop(&g_rt.ring, (unsigned char*)(recs + n * rec_stride),
                                 rec_stride, &l);
            if (rc == 1) {
                lens[n] = l;
                n++;
                continue;
            }
            /* 读到“空”时短暂让步，等待生产者 head 释放存储可见，
               避免陈旧读误判为空而漏取记录（§4.3 SPSC 可见性）。 */
            hp_yield();
            rc = hp_ring_pop(&g_rt.ring, (unsigned char*)(recs + n * rec_stride),
                             rec_stride, &l);
            if (rc == 1) {
                lens[n] = l;
                n++;
                continue;
            }
            break;
        }
        if (n > 0) {
            hp_mutex_unlock(&g_rt.consumer_mu);
            hp_process_batch((unsigned char*)recs, rec_stride, lens, (size_t)n,
                             lines, line_stride);
            hp_mutex_lock(&g_rt.consumer_mu);
        }

        if (stop) {
            /* 排空阶段：生产者线程已停止入队（main 在 stop 置位前已完成
               全部入队），head 不会再增长，故自旋直至环形缓冲真正为空，
               确保即便 head 释放存储曾暂不可见也不会漏处理。 */
            size_t guard = 0;
            while (hp_ring_used(&g_rt.ring) > 0 && guard < 10000000u) {
                size_t l = 0;
                int rc = hp_ring_pop(&g_rt.ring,
                                     (unsigned char*)(recs),
                                     rec_stride, &l);
                if (rc == 1) {
                    lens[0] = l;
                    hp_mutex_unlock(&g_rt.consumer_mu);
                    hp_process_batch((unsigned char*)recs, rec_stride, lens,
                                     1u, lines, line_stride);
                    hp_mutex_lock(&g_rt.consumer_mu);
                } else {
                    hp_yield();
                }
                guard++;
            }
            break;
        }
        if (n < (int)batch) {
            unsigned fi = (rt->cfg.flush_interval_ms > 0)
                              ? rt->cfg.flush_interval_ms
                              : 100u;
            hp_cond_timedwait(&g_rt.consumer_cv, &g_rt.consumer_mu, fi);
            if (g_rt.flush_gen > g_rt.flush_done_gen
                && hp_ring_used(&g_rt.ring) == 0 && n == 0) {
                g_rt.flush_done_gen = g_rt.flush_gen;
                hp_cond_broadcast(&g_rt.consumer_cv);
            }
        }
    }
    hp_mutex_unlock(&g_rt.consumer_mu);

    free(recs);
    free(lines);
    free(lens);
}

int hp_async_start(void)
{
    if (hp_thread_create(&g_rt.consumer_th, hp_async_main, NULL) != HPLOGC_OK) {
        return HPLOGC_ERR_NO_MEM;
    }
    g_rt.consumer_started = 1;
    return HPLOGC_OK;
}

void hp_async_stop(void)
{
    if (!g_rt.consumer_started) {
        return;
    }
    hp_atomic_store_i32(&g_rt.consumer_stop, 1);
    hp_mutex_lock(&g_rt.consumer_mu);
    hp_cond_broadcast(&g_rt.consumer_cv);
    hp_mutex_unlock(&g_rt.consumer_mu);
    hp_thread_join(&g_rt.consumer_th);
    g_rt.consumer_started = 0;
}

void hp_async_kick(void)
{
    hp_mutex_lock(&g_rt.consumer_mu);
    g_rt.flush_gen++;
    hp_cond_broadcast(&g_rt.consumer_cv);
    hp_mutex_unlock(&g_rt.consumer_mu);
}

/** @brief 阻塞等待已入队日志被消费者排空（flush 用）。 */
void hp_async_flush_wait(void)
{
    size_t want;
    hp_mutex_lock(&g_rt.consumer_mu);
    want = g_rt.flush_gen;
    while (g_rt.flush_done_gen < want) {
        if (hp_cond_timedwait(&g_rt.consumer_cv, &g_rt.consumer_mu, 1000u)
            == 0) {
            break;
        }
    }
    hp_mutex_unlock(&g_rt.consumer_mu);
}

#endif /* HPLOGC_HAS_ASYNC */
