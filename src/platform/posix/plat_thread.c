/* -*- coding: utf-8 -*- */
/**
 * @file plat_thread.c
 * @brief POSIX 共享层：线程、TLS、睡眠与线程标识。
 */

#include "hplogc_platform.h"

#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <hplogc.h>

/** @brief 线程入口的适配结构：把 `void (*)(void*)` 适配到 pthread 签名。 */
typedef struct {
    void (*fn)(void*); /*!< 真正的入口 */
    void* arg;         /*!< 入口参数 */
} hp_thread_boot_t;

static void* hp_thread_boot(void* p)
{
    hp_thread_boot_t boot;
    boot = *(hp_thread_boot_t*)p;
    free(p);
    boot.fn(boot.arg);
    return NULL;
}

int hp_thread_create(hp_thread_t* t, void (*fn)(void*), void* arg)
{
    pthread_t* pt;
    hp_thread_boot_t* boot;

    if (t == NULL || fn == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    /* 启动参数需在堆上分配：pthread_create 不保证新线程先于返回读到栈上参数 */
    boot = (hp_thread_boot_t*)malloc(sizeof(*boot));
    if (boot == NULL) {
        return HPLOGC_ERR_NO_MEM;
    }
    boot->fn = fn;
    boot->arg = arg;

    pt = (pthread_t*)(void*)t->bytes;
    if (pthread_create(pt, NULL, hp_thread_boot, boot) != 0) {
        free(boot);
        return HPLOGC_ERR_NO_MEM;
    }
    return HPLOGC_OK;
}

int hp_thread_join(hp_thread_t* t)
{
    if (t == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    return (pthread_join(*(pthread_t*)(void*)t->bytes, NULL) == 0)
               ? HPLOGC_OK
               : HPLOGC_ERR_STATE;
}

void hp_sleep_ms(unsigned ms)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)((ms % 1000u) * 1000000u);
    nanosleep(&ts, NULL);
}

void hp_yield(void)
{
    sched_yield();
}

int hp_tls_create(hp_tls_t* key, void (*dtor)(void*))
{
    pthread_key_t* pk;

    if (key == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    pk = (pthread_key_t*)(void*)key->bytes;
    return (pthread_key_create(pk, dtor) == 0) ? HPLOGC_OK : HPLOGC_ERR_NO_MEM;
}

void hp_tls_destroy(hp_tls_t* key)
{
    if (key == NULL) {
        return;
    }
    pthread_key_delete(*(pthread_key_t*)(void*)key->bytes);
}

void hp_tls_set(hp_tls_t* key, void* val)
{
    if (key == NULL) {
        return;
    }
    pthread_setspecific(*(pthread_key_t*)(void*)key->bytes, val);
}

void* hp_tls_get(hp_tls_t* key)
{
    if (key == NULL) {
        return NULL;
    }
    return pthread_getspecific(*(pthread_key_t*)(void*)key->bytes);
}

uint32_t hp_pid(void)
{
    return (uint32_t)getpid();
}

uint64_t hp_tid(void)
{
    pthread_t self = pthread_self();
    uint64_t tid = 0;
    /* pthread_t 可能是整型也可能是指针，按最小宽度安全折叠为 64 位 */
    if (sizeof(pthread_t) >= sizeof(uint64_t)) {
        memcpy(&tid, &self, sizeof(uint64_t));
    } else {
        memcpy(&tid, &self, sizeof(pthread_t));
    }
    return tid;
}
