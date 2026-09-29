/* -*- coding: utf-8 -*- */
/**
 * @file atomic_msvc.h
 * @brief 原子后端：MSVC / MinGW `Interlocked*`（Windows 全标准，§2.2）。
 *
 * 与其它后端提供**完全相同的符号与语义**，由 `hplogc_atomic.h` 按构建选择。
 *
 * 实现说明：
 * - Windows 的 `Interlocked*` 系列在 x86 / x64 / ARM64 上均具备完整的屏障语义，
 *   因此本后端的 acquire / release / seq_cst 三类栅栏统一以 `MemoryBarrier()`（MSVC）
 *   或 `__sync_synchronize()`（MinGW）实现——保守但语义正确。
 * - 64 位原子量显式按 8 字节对齐：`Interlocked*64` 要求目标 8 字节对齐，
 *   否则在 x86 上可能不是原子操作。
 * - 读操作以"与 0 的比较交换"实现（返回旧值即当前值），避免引入数据竞争。
 */

#ifndef HPLOGC_ATOMIC_MSVC_H
#define HPLOGC_ATOMIC_MSVC_H

#include <stdint.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_MSC_VER)
/** @brief 原子量对齐声明（MSVC）。 */
#define HP_ATOMIC_ALIGN(n) __declspec(align(n))
#else
/** @brief 原子量对齐声明（MinGW / GCC）。 */
#define HP_ATOMIC_ALIGN(n) __attribute__((aligned(n)))
#endif

/** @brief 64 位无符号原子量。 */
typedef struct {
    HP_ATOMIC_ALIGN(8) volatile unsigned long long v; /*!< 底层原子对象 */
} hp_atomic_u64;

/** @brief 32 位无符号原子量。 */
typedef struct {
    volatile unsigned long v; /*!< 底层原子对象 */
} hp_atomic_u32;

/** @brief 32 位有符号原子量。 */
typedef struct {
    volatile long v; /*!< 底层原子对象 */
} hp_atomic_i32;

/** @brief 取原子量底层地址的整数形式（避免 const 限定符告警）。 */
#define HP_ATOMIC_PTR(a) ((volatile unsigned long long*)(uintptr_t) \
                              &(a)->v)
#define HP_ATOMIC_PTR32(a) ((volatile unsigned long*)(uintptr_t) &(a)->v)

static inline void hp_atomic_init_u64(hp_atomic_u64* a, unsigned long long v)
{
    a->v = v;
}

static inline unsigned long long hp_atomic_load_u64(const hp_atomic_u64* a)
{
    return (unsigned long long)InterlockedCompareExchange64(
        (volatile LONGLONG*)HP_ATOMIC_PTR(a), 0, 0);
}

static inline void hp_atomic_store_u64(hp_atomic_u64* a, unsigned long long v)
{
    (void)InterlockedExchange64((volatile LONGLONG*)HP_ATOMIC_PTR(a),
                                (LONGLONG)v);
}

static inline unsigned long long hp_atomic_fetch_add_u64(hp_atomic_u64* a,
                                                         unsigned long long d)
{
    return (unsigned long long)InterlockedExchangeAdd64(
        (volatile LONGLONG*)HP_ATOMIC_PTR(a), (LONGLONG)d);
}

static inline unsigned long long hp_atomic_fetch_sub_u64(hp_atomic_u64* a,
                                                         unsigned long long d)
{
    return (unsigned long long)InterlockedExchangeAdd64(
        (volatile LONGLONG*)HP_ATOMIC_PTR(a), -(LONGLONG)d);
}

static inline int hp_atomic_cas_u64(hp_atomic_u64* a,
                                    unsigned long long* expected,
                                    unsigned long long desired)
{
    LONGLONG prev = InterlockedCompareExchange64(
        (volatile LONGLONG*)HP_ATOMIC_PTR(a), (LONGLONG)desired,
        (LONGLONG)*expected);
    if (prev == (LONGLONG)*expected) {
        return 1;
    }
    *expected = (unsigned long long)prev;
    return 0;
}

static inline unsigned long long hp_atomic_exchange_u64(hp_atomic_u64* a,
                                                        unsigned long long v)
{
    return (unsigned long long)InterlockedExchange64(
        (volatile LONGLONG*)HP_ATOMIC_PTR(a), (LONGLONG)v);
}

static inline void hp_atomic_init_u32(hp_atomic_u32* a, unsigned v)
{
    a->v = (unsigned long)v;
}

static inline unsigned hp_atomic_load_u32(const hp_atomic_u32* a)
{
    return (unsigned)InterlockedCompareExchange(
        (volatile LONG*)HP_ATOMIC_PTR32(a), 0, 0);
}

static inline void hp_atomic_store_u32(hp_atomic_u32* a, unsigned v)
{
    (void)InterlockedExchange((volatile LONG*)HP_ATOMIC_PTR32(a), (LONG)v);
}

static inline unsigned hp_atomic_fetch_add_u32(hp_atomic_u32* a, unsigned d)
{
    return (unsigned)InterlockedExchangeAdd((volatile LONG*)HP_ATOMIC_PTR32(a),
                                            (LONG)d);
}

static inline int hp_atomic_cas_u32(hp_atomic_u32* a, unsigned* expected,
                                    unsigned desired)
{
    LONG prev = InterlockedCompareExchange((volatile LONG*)HP_ATOMIC_PTR32(a),
                                           (LONG)desired, (LONG)*expected);
    if (prev == (LONG)*expected) {
        return 1;
    }
    *expected = (unsigned)prev;
    return 0;
}

static inline unsigned hp_atomic_exchange_u32(hp_atomic_u32* a, unsigned v)
{
    return (unsigned)InterlockedExchange((volatile LONG*)HP_ATOMIC_PTR32(a),
                                         (LONG)v);
}

static inline void hp_atomic_init_i32(hp_atomic_i32* a, int v)
{
    a->v = (long)v;
}

static inline int hp_atomic_load_i32(const hp_atomic_i32* a)
{
    return (int)InterlockedCompareExchange((volatile LONG*)HP_ATOMIC_PTR32(a),
                                           0, 0);
}

static inline void hp_atomic_store_i32(hp_atomic_i32* a, int v)
{
    (void)InterlockedExchange((volatile LONG*)HP_ATOMIC_PTR32(a), (LONG)v);
}

static inline int hp_atomic_cas_i32(hp_atomic_i32* a, int* expected,
                                    int desired)
{
    LONG prev = InterlockedCompareExchange((volatile LONG*)HP_ATOMIC_PTR32(a),
                                           (LONG)desired, (LONG)*expected);
    if (prev == (LONG)*expected) {
        return 1;
    }
    *expected = (int)prev;
    return 0;
}

static inline int hp_atomic_exchange_i32(hp_atomic_i32* a, int v)
{
    return (int)InterlockedExchange((volatile LONG*)HP_ATOMIC_PTR32(a),
                                    (LONG)v);
}

static inline void hp_atomic_fence_acquire(void)
{
#if defined(_MSC_VER)
    MemoryBarrier();
#else
    __sync_synchronize();
#endif
}

static inline void hp_atomic_fence_release(void)
{
#if defined(_MSC_VER)
    MemoryBarrier();
#else
    __sync_synchronize();
#endif
}

static inline void hp_atomic_fence_seq(void)
{
#if defined(_MSC_VER)
    MemoryBarrier();
#else
    __sync_synchronize();
#endif
}

#ifdef __cplusplus
}
#endif

#endif /* HPLOGC_ATOMIC_MSVC_H */
