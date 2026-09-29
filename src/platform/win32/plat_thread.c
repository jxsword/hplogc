/* -*- coding: utf-8 -*- */
/**
 * @file plat_thread.c
 * @brief Windows 专属：线程、TLS、睡眠、让出与进程 / 线程标识。
 *
 * 线程创建在 MSVC 下走 `_beginthreadex`（保证 CRT 每线程数据正确初始化），
 * 其余工具链走 `CreateThread`。TLS 使用 **FLS**（`FlsAlloc` 系列）：与 POSIX 的
 * `pthread_key_create` 一样支持"线程退出时调用析构函数"，而 `TlsAlloc` 不支持。
 */

#include "hplogc_platform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdlib.h>

#include <hplogc.h>

#ifdef _MSC_VER
#include <process.h> /* _beginthreadex */
#endif

/** @brief 线程入口的适配结构：把 `void (*)(void*)` 适配到 Windows 线程签名。 */
typedef struct {
    void (*fn)(void*); /*!< 真正的入口 */
    void* arg;         /*!< 入口参数 */
} hp_thread_boot_t;

#ifdef _MSC_VER
static unsigned __stdcall hp_thread_boot(void* p)
#else
static DWORD WINAPI hp_thread_boot(LPVOID p)
#endif
{
    hp_thread_boot_t boot;
    boot = *(hp_thread_boot_t*)p;
    free(p);
    boot.fn(boot.arg);
    return 0;
}

int hp_thread_create(hp_thread_t* t, void (*fn)(void*), void* arg)
{
    hp_thread_boot_t* boot;
    HANDLE h;

    if (t == NULL || fn == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    /* 启动参数在堆上分配：不保证新线程先于 CreateThread 返回读到栈上参数 */
    boot = (hp_thread_boot_t*)malloc(sizeof(*boot));
    if (boot == NULL) {
        return HPLOGC_ERR_NO_MEM;
    }
    boot->fn = fn;
    boot->arg = arg;

#ifdef _MSC_VER
    h = (HANDLE)_beginthreadex(NULL, 0, hp_thread_boot, boot, 0, NULL);
#else
    h = CreateThread(NULL, 0, hp_thread_boot, boot, 0, NULL);
#endif
    if (h == NULL) {
        free(boot);
        return HPLOGC_ERR_NO_MEM;
    }
    *(HANDLE*)(void*)t->bytes = h;
    return HPLOGC_OK;
}

int hp_thread_join(hp_thread_t* t)
{
    HANDLE h;

    if (t == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    h = *(HANDLE*)(void*)t->bytes;
    if (h == NULL) {
        return HPLOGC_ERR_STATE;
    }
    if (WaitForSingleObject(h, INFINITE) != WAIT_OBJECT_0) {
        return HPLOGC_ERR_STATE;
    }
    CloseHandle(h);
    *(HANDLE*)(void*)t->bytes = NULL;
    return HPLOGC_OK;
}

void hp_sleep_ms(unsigned ms)
{
    Sleep((DWORD)ms);
}

void hp_yield(void)
{
    /* SwitchToThread：让出当前时间片；不可用（极旧系统）时退化为 Sleep(0) */
    SwitchToThread();
}

int hp_tls_create(hp_tls_t* key, void (*dtor)(void*))
{
    DWORD idx;

    if (key == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    idx = FlsAlloc((PFLS_CALLBACK_FUNCTION)(void*)dtor);
    if (idx == FLS_OUT_OF_INDEXES) {
        return HPLOGC_ERR_NO_MEM;
    }
    *(DWORD*)(void*)key->bytes = idx;
    return HPLOGC_OK;
}

void hp_tls_destroy(hp_tls_t* key)
{
    if (key == NULL) {
        return;
    }
    (void)FlsFree(*(DWORD*)(void*)key->bytes);
}

void hp_tls_set(hp_tls_t* key, void* val)
{
    if (key == NULL) {
        return;
    }
    (void)FlsSetValue(*(DWORD*)(void*)key->bytes, val);
}

void* hp_tls_get(hp_tls_t* key)
{
    if (key == NULL) {
        return NULL;
    }
    return FlsGetValue(*(DWORD*)(void*)key->bytes);
}

uint32_t hp_pid(void)
{
    return (uint32_t)GetCurrentProcessId();
}

uint64_t hp_tid(void)
{
    return (uint64_t)GetCurrentThreadId();
}
