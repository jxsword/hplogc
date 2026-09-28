/* -*- coding: utf-8 -*- */
/**
 * @file atomic_gcc.h
 * @brief 原子后端：GCC / Clang 内建（Linux / macOS 的 C99 构建）。
 *
 * 默认使用 `__atomic_*` 内建；当构建探测到其不可用（或由
 * `HPLOGC_ATOMIC_BACKEND=gcc-sync` 强制指定）时回退到 `__sync_*` 内建。
 * 两条路径的符号与语义与 `atomic_stdatomic.h` / `atomic_msvc.h` 完全一致。
 */

#ifndef HPLOGC_ATOMIC_GCC_H
#define HPLOGC_ATOMIC_GCC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 64 位无符号原子量。 */
typedef struct {
    volatile unsigned long long v; /*!< 底层存储 */
} hp_atomic_u64;

/** @brief 32 位无符号原子量。 */
typedef struct {
    volatile unsigned v; /*!< 底层存储 */
} hp_atomic_u32;

/** @brief 32 位有符号原子量。 */
typedef struct {
    volatile int v; /*!< 底层存储 */
} hp_atomic_i32;

#if defined(HPLOGC_ATOMIC_BACKEND_GCC_SYNC)
/* ---------------------------- __sync_* 回退路径 ---------------------------- */

static inline void hp_atomic_init_u64(hp_atomic_u64* a, unsigned long long v)
{
    a->v = v;
}

static inline unsigned long long hp_atomic_load_u64(const hp_atomic_u64* a)
{
    return __sync_fetch_and_add((unsigned long long*)(void*)&a->v, 0ull);
}

static inline void hp_atomic_store_u64(hp_atomic_u64* a, unsigned long long v)
{
    __sync_lock_test_and_set(&a->v, v);
}

static inline unsigned long long hp_atomic_fetch_add_u64(hp_atomic_u64* a,
                                                         unsigned long long d)
{
    return __sync_fetch_and_add(&a->v, d);
}

static inline unsigned long long hp_atomic_fetch_sub_u64(hp_atomic_u64* a,
                                                         unsigned long long d)
{
    return __sync_fetch_and_sub(&a->v, d);
}

static inline int hp_atomic_cas_u64(hp_atomic_u64* a,
                                    unsigned long long* expected,
                                    unsigned long long desired)
{
    unsigned long long old = *expected;
    unsigned long long cur = __sync_val_compare_and_swap(&a->v, old, desired);
    if (cur == old) {
        return 1;
    }
    *expected = cur;
    return 0;
}

static inline unsigned long long hp_atomic_exchange_u64(hp_atomic_u64* a,
                                                        unsigned long long v)
{
    return __sync_lock_test_and_set(&a->v, v);
}

static inline void hp_atomic_init_u32(hp_atomic_u32* a, unsigned v)
{
    a->v = v;
}

static inline unsigned hp_atomic_load_u32(const hp_atomic_u32* a)
{
    return __sync_fetch_and_add((unsigned*)(void*)&a->v, 0u);
}

static inline void hp_atomic_store_u32(hp_atomic_u32* a, unsigned v)
{
    __sync_lock_test_and_set(&a->v, v);
}

static inline unsigned hp_atomic_fetch_add_u32(hp_atomic_u32* a, unsigned d)
{
    return __sync_fetch_and_add(&a->v, d);
}

static inline int hp_atomic_cas_u32(hp_atomic_u32* a, unsigned* expected,
                                    unsigned desired)
{
    unsigned old = *expected;
    unsigned cur = __sync_val_compare_and_swap(&a->v, old, desired);
    if (cur == old) {
        return 1;
    }
    *expected = cur;
    return 0;
}

static inline unsigned hp_atomic_exchange_u32(hp_atomic_u32* a, unsigned v)
{
    return __sync_lock_test_and_set(&a->v, v);
}

static inline void hp_atomic_init_i32(hp_atomic_i32* a, int v)
{
    a->v = v;
}

static inline int hp_atomic_load_i32(const hp_atomic_i32* a)
{
    return __sync_fetch_and_add((int*)(void*)&a->v, 0);
}

static inline void hp_atomic_store_i32(hp_atomic_i32* a, int v)
{
    __sync_lock_test_and_set(&a->v, v);
}

static inline int hp_atomic_cas_i32(hp_atomic_i32* a, int* expected,
                                    int desired)
{
    int old = *expected;
    int cur = __sync_val_compare_and_swap(&a->v, old, desired);
    if (cur == old) {
        return 1;
    }
    *expected = cur;
    return 0;
}

static inline int hp_atomic_exchange_i32(hp_atomic_i32* a, int v)
{
    return __sync_lock_test_and_set(&a->v, v);
}

static inline void hp_atomic_fence_acquire(void)
{
    __sync_synchronize();
}

static inline void hp_atomic_fence_release(void)
{
    __sync_synchronize();
}

static inline void hp_atomic_fence_seq(void)
{
    __sync_synchronize();
}

#else
/* ------------------------------ __atomic_* 路径 ----------------------------- */

static inline void hp_atomic_init_u64(hp_atomic_u64* a, unsigned long long v)
{
    __atomic_store_n(&a->v, v, __ATOMIC_RELAXED);
}

static inline unsigned long long hp_atomic_load_u64(const hp_atomic_u64* a)
{
    return __atomic_load_n(&a->v, __ATOMIC_ACQUIRE);
}

static inline void hp_atomic_store_u64(hp_atomic_u64* a, unsigned long long v)
{
    __atomic_store_n(&a->v, v, __ATOMIC_RELEASE);
}

static inline unsigned long long hp_atomic_fetch_add_u64(hp_atomic_u64* a,
                                                         unsigned long long d)
{
    return __atomic_fetch_add(&a->v, d, __ATOMIC_ACQ_REL);
}

static inline unsigned long long hp_atomic_fetch_sub_u64(hp_atomic_u64* a,
                                                         unsigned long long d)
{
    return __atomic_fetch_sub(&a->v, d, __ATOMIC_ACQ_REL);
}

static inline int hp_atomic_cas_u64(hp_atomic_u64* a,
                                    unsigned long long* expected,
                                    unsigned long long desired)
{
    return __atomic_compare_exchange_n(&a->v, expected, desired, 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static inline unsigned long long hp_atomic_exchange_u64(hp_atomic_u64* a,
                                                        unsigned long long v)
{
    return __atomic_exchange_n(&a->v, v, __ATOMIC_ACQ_REL);
}

static inline void hp_atomic_init_u32(hp_atomic_u32* a, unsigned v)
{
    __atomic_store_n(&a->v, v, __ATOMIC_RELAXED);
}

static inline unsigned hp_atomic_load_u32(const hp_atomic_u32* a)
{
    return __atomic_load_n(&a->v, __ATOMIC_ACQUIRE);
}

static inline void hp_atomic_store_u32(hp_atomic_u32* a, unsigned v)
{
    __atomic_store_n(&a->v, v, __ATOMIC_RELEASE);
}

static inline unsigned hp_atomic_fetch_add_u32(hp_atomic_u32* a, unsigned d)
{
    return __atomic_fetch_add(&a->v, d, __ATOMIC_ACQ_REL);
}

static inline int hp_atomic_cas_u32(hp_atomic_u32* a, unsigned* expected,
                                    unsigned desired)
{
    return __atomic_compare_exchange_n(&a->v, expected, desired, 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static inline unsigned hp_atomic_exchange_u32(hp_atomic_u32* a, unsigned v)
{
    return __atomic_exchange_n(&a->v, v, __ATOMIC_ACQ_REL);
}

static inline void hp_atomic_init_i32(hp_atomic_i32* a, int v)
{
    __atomic_store_n(&a->v, v, __ATOMIC_RELAXED);
}

static inline int hp_atomic_load_i32(const hp_atomic_i32* a)
{
    return __atomic_load_n(&a->v, __ATOMIC_ACQUIRE);
}

static inline void hp_atomic_store_i32(hp_atomic_i32* a, int v)
{
    __atomic_store_n(&a->v, v, __ATOMIC_RELEASE);
}

static inline int hp_atomic_cas_i32(hp_atomic_i32* a, int* expected,
                                    int desired)
{
    return __atomic_compare_exchange_n(&a->v, expected, desired, 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static inline int hp_atomic_exchange_i32(hp_atomic_i32* a, int v)
{
    return __atomic_exchange_n(&a->v, v, __ATOMIC_ACQ_REL);
}

static inline void hp_atomic_fence_acquire(void)
{
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
}

static inline void hp_atomic_fence_release(void)
{
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

static inline void hp_atomic_fence_seq(void)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

#endif /* HPLOGC_ATOMIC_BACKEND_GCC_SYNC */

#ifdef __cplusplus
}
#endif

#endif /* HPLOGC_ATOMIC_GCC_H */
