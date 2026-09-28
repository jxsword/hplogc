/* -*- coding: utf-8 -*- */
/**
 * @file plat_sync.c
 * @brief POSIX 共享层：互斥量与条件变量（Linux 与 macOS 共用）。
 *
 * 实现 `hplogc_platform.h` 的同步原语契约，底层为 `pthread_mutex_t` /
 * `pthread_cond_t`。存储块尺寸由 `HP_PLAT_STORAGE` 保证足够。
 */

#include "hplogc_platform.h"

#include <errno.h>
#include <pthread.h>
#include <sys/time.h>
#include <time.h>

#include <hplogc.h>

int hp_mutex_init(hp_mutex_t* m)
{
    pthread_mutex_t* pm;
    pthread_mutexattr_t attr;
    int rc;

    if (m == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    pm = (pthread_mutex_t*)(void*)m->bytes;
    rc = pthread_mutexattr_init(&attr);
    if (rc != 0) {
        return HPLOGC_ERR_NO_MEM;
    }
    /* 非递归、非健壮：日志库内部使用，避免健壮互斥量的额外开销 */
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_NORMAL);
    rc = pthread_mutex_init(pm, &attr);
    pthread_mutexattr_destroy(&attr);
    return (rc == 0) ? HPLOGC_OK : HPLOGC_ERR_NO_MEM;
}

void hp_mutex_destroy(hp_mutex_t* m)
{
    if (m == NULL) {
        return;
    }
    pthread_mutex_destroy((pthread_mutex_t*)(void*)m->bytes);
}

void hp_mutex_lock(hp_mutex_t* m)
{
    pthread_mutex_lock((pthread_mutex_t*)(void*)m->bytes);
}

void hp_mutex_unlock(hp_mutex_t* m)
{
    pthread_mutex_unlock((pthread_mutex_t*)(void*)m->bytes);
}

int hp_cond_init(hp_cond_t* c)
{
    pthread_cond_t* pc;

    if (c == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    pc = (pthread_cond_t*)(void*)c->bytes;
    return (pthread_cond_init(pc, NULL) == 0) ? HPLOGC_OK : HPLOGC_ERR_NO_MEM;
}

void hp_cond_destroy(hp_cond_t* c)
{
    if (c == NULL) {
        return;
    }
    pthread_cond_destroy((pthread_cond_t*)(void*)c->bytes);
}

void hp_cond_wait(hp_cond_t* c, hp_mutex_t* m)
{
    pthread_cond_wait((pthread_cond_t*)(void*)c->bytes,
                      (pthread_mutex_t*)(void*)m->bytes);
}

int hp_cond_timedwait(hp_cond_t* c, hp_mutex_t* m, unsigned timeout_ms)
{
    struct timespec ts;
    struct timeval tv;
    long long nsec;
    int rc;

    /* 用墙上时钟做绝对超时（CLOCK_REALTIME 语义，与默认 cond 时钟一致） */
    if (gettimeofday(&tv, NULL) != 0) {
        return HPLOGC_ERR_IO;
    }
    nsec = (long long)tv.tv_sec * 1000000000LL
           + (long long)tv.tv_usec * 1000LL
           + (long long)timeout_ms * 1000000LL;
    ts.tv_sec = (time_t)(nsec / 1000000000LL);
    ts.tv_nsec = (long)(nsec % 1000000000LL);

    rc = pthread_cond_timedwait((pthread_cond_t*)(void*)c->bytes,
                                (pthread_mutex_t*)(void*)m->bytes, &ts);
    if (rc == 0) {
        return 1;
    }
    if (rc == ETIMEDOUT) {
        return 0;
    }
    return HPLOGC_ERR_STATE;
}

void hp_cond_signal(hp_cond_t* c)
{
    pthread_cond_signal((pthread_cond_t*)(void*)c->bytes);
}

void hp_cond_broadcast(hp_cond_t* c)
{
    pthread_cond_broadcast((pthread_cond_t*)(void*)c->bytes);
}
