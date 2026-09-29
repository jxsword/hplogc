/* -*- coding: utf-8 -*- */
/**
 * @file plat_sync.c
 * @brief Windows 专属：互斥量与条件变量（契约见 hplogc_platform.h）。
 *
 * 以 `CRITICAL_SECTION` 实现非递归互斥量，以 `CONDITION_VARIABLE` 实现条件变量；
 * 两者均为用户态优先的原语，且在 Vista 及以后所有 Windows 版本可用。
 */

#include "hplogc_platform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <hplogc.h>

int hp_mutex_init(hp_mutex_t* m)
{
    CRITICAL_SECTION* cs;

    if (m == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    cs = (CRITICAL_SECTION*)(void*)m->bytes;
    /* 自旋计数：短临界区（统计计数 / 环形缓冲判空）下避免立即陷入内核 */
    InitializeCriticalSectionAndSpinCount(cs, 1024);
    return HPLOGC_OK;
}

void hp_mutex_destroy(hp_mutex_t* m)
{
    if (m == NULL) {
        return;
    }
    DeleteCriticalSection((CRITICAL_SECTION*)(void*)m->bytes);
}

void hp_mutex_lock(hp_mutex_t* m)
{
    if (m == NULL) {
        return;
    }
    EnterCriticalSection((CRITICAL_SECTION*)(void*)m->bytes);
}

void hp_mutex_unlock(hp_mutex_t* m)
{
    if (m == NULL) {
        return;
    }
    LeaveCriticalSection((CRITICAL_SECTION*)(void*)m->bytes);
}

int hp_cond_init(hp_cond_t* c)
{
    CONDITION_VARIABLE* cv;

    if (c == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    cv = (CONDITION_VARIABLE*)(void*)c->bytes;
    InitializeConditionVariable(cv);
    return HPLOGC_OK;
}

void hp_cond_destroy(hp_cond_t* c)
{
    /* CONDITION_VARIABLE 无需显式销毁 */
    (void)c;
}

void hp_cond_wait(hp_cond_t* c, hp_mutex_t* m)
{
    if (c == NULL || m == NULL) {
        return;
    }
    (void)SleepConditionVariableCS((CONDITION_VARIABLE*)(void*)c->bytes,
                                   (CRITICAL_SECTION*)(void*)m->bytes,
                                   INFINITE);
}

int hp_cond_timedwait(hp_cond_t* c, hp_mutex_t* m, unsigned timeout_ms)
{
    BOOL ok;

    if (c == NULL || m == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    ok = SleepConditionVariableCS((CONDITION_VARIABLE*)(void*)c->bytes,
                                  (CRITICAL_SECTION*)(void*)m->bytes,
                                  (DWORD)timeout_ms);
    if (ok) {
        return 1; /* 被唤醒 */
    }
    return (GetLastError() == ERROR_TIMEOUT) ? 0 : HPLOGC_ERR_STATE;
}

void hp_cond_signal(hp_cond_t* c)
{
    if (c == NULL) {
        return;
    }
    WakeConditionVariable((CONDITION_VARIABLE*)(void*)c->bytes);
}

void hp_cond_broadcast(hp_cond_t* c)
{
    if (c == NULL) {
        return;
    }
    WakeAllConditionVariable((CONDITION_VARIABLE*)(void*)c->bytes);
}
