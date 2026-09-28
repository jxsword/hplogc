/* -*- coding: utf-8 -*- */
/**
 * @file atomic_stdatomic.h
 * @brief 原子后端：C11 `<stdatomic.h>`（Linux / macOS 的 C11 构建）。
 *
 * 与其它后端提供**完全相同的符号与语义**，由 `hplogc_atomic.h` 按构建选择。
 */

#ifndef HPLOGC_ATOMIC_STDATOMIC_H
#define HPLOGC_ATOMIC_STDATOMIC_H

#include <stdatomic.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 64 位无符号原子量。 */
typedef struct {
    atomic_ullong v; /*!< 底层原子对象 */
} hp_atomic_u64;

/** @brief 32 位无符号原子量。 */
typedef struct {
    atomic_uint v; /*!< 底层原子对象 */
} hp_atomic_u32;

/** @brief 32 位有符号原子量。 */
typedef struct {
    atomic_int v; /*!< 底层原子对象 */
} hp_atomic_i32;

static inline void hp_atomic_init_u64(hp_atomic_u64* a, unsigned long long v)
{
    atomic_init(&a->v, v);
}

static inline unsigned long long hp_atomic_load_u64(const hp_atomic_u64* a)
{
    return atomic_load_explicit((atomic_ullong*)(void*)&a->v,
                                memory_order_acquire);
}

static inline void hp_atomic_store_u64(hp_atomic_u64* a, unsigned long long v)
{
    atomic_store_explicit(&a->v, v, memory_order_release);
}

static inline unsigned long long hp_atomic_fetch_add_u64(hp_atomic_u64* a,
                                                         unsigned long long d)
{
    return atomic_fetch_add_explicit(&a->v, d, memory_order_acq_rel);
}

static inline unsigned long long hp_atomic_fetch_sub_u64(hp_atomic_u64* a,
                                                         unsigned long long d)
{
    return atomic_fetch_sub_explicit(&a->v, d, memory_order_acq_rel);
}

static inline int hp_atomic_cas_u64(hp_atomic_u64* a,
                                    unsigned long long* expected,
                                    unsigned long long desired)
{
    return atomic_compare_exchange_strong_explicit(&a->v, expected, desired,
                                                   memory_order_acq_rel,
                                                   memory_order_acquire);
}

static inline unsigned long long hp_atomic_exchange_u64(hp_atomic_u64* a,
                                                        unsigned long long v)
{
    return atomic_exchange_explicit(&a->v, v, memory_order_acq_rel);
}

static inline void hp_atomic_init_u32(hp_atomic_u32* a, unsigned v)
{
    atomic_init(&a->v, v);
}

static inline unsigned hp_atomic_load_u32(const hp_atomic_u32* a)
{
    return atomic_load_explicit((atomic_uint*)(void*)&a->v,
                                memory_order_acquire);
}

static inline void hp_atomic_store_u32(hp_atomic_u32* a, unsigned v)
{
    atomic_store_explicit(&a->v, v, memory_order_release);
}

static inline unsigned hp_atomic_fetch_add_u32(hp_atomic_u32* a, unsigned d)
{
    return atomic_fetch_add_explicit(&a->v, d, memory_order_acq_rel);
}

static inline int hp_atomic_cas_u32(hp_atomic_u32* a, unsigned* expected,
                                    unsigned desired)
{
    return atomic_compare_exchange_strong_explicit(&a->v, expected, desired,
                                                   memory_order_acq_rel,
                                                   memory_order_acquire);
}

static inline unsigned hp_atomic_exchange_u32(hp_atomic_u32* a, unsigned v)
{
    return atomic_exchange_explicit(&a->v, v, memory_order_acq_rel);
}

static inline void hp_atomic_init_i32(hp_atomic_i32* a, int v)
{
    atomic_init(&a->v, v);
}

static inline int hp_atomic_load_i32(const hp_atomic_i32* a)
{
    return atomic_load_explicit((atomic_int*)(void*)&a->v,
                                memory_order_acquire);
}

static inline void hp_atomic_store_i32(hp_atomic_i32* a, int v)
{
    atomic_store_explicit(&a->v, v, memory_order_release);
}

static inline int hp_atomic_cas_i32(hp_atomic_i32* a, int* expected,
                                    int desired)
{
    return atomic_compare_exchange_strong_explicit(&a->v, expected, desired,
                                                   memory_order_acq_rel,
                                                   memory_order_acquire);
}

static inline int hp_atomic_exchange_i32(hp_atomic_i32* a, int v)
{
    return atomic_exchange_explicit(&a->v, v, memory_order_acq_rel);
}

static inline void hp_atomic_fence_acquire(void)
{
    atomic_thread_fence(memory_order_acquire);
}

static inline void hp_atomic_fence_release(void)
{
    atomic_thread_fence(memory_order_release);
}

static inline void hp_atomic_fence_seq(void)
{
    atomic_thread_fence(memory_order_seq_cst);
}

#ifdef __cplusplus
}
#endif

#endif /* HPLOGC_ATOMIC_STDATOMIC_H */
