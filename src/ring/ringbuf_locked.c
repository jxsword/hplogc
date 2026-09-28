/* -*- coding: utf-8 -*- */
/**
 * @file ringbuf_locked.c
 * @brief 有锁环形缓冲（`HPLOGC_LOCKFREE=OFF`，默认）。
 *
 * 互斥与条件变量经平台契约封装（Linux / macOS 为 `pthread`，Windows 为
 * `SRWLOCK` / `CONDITION_VARIABLE`），因此本文件**不含任何平台分支**。
 * 支持 discard / overwrite / wait 三种溢出策略（§4.3）。
 */

#include "ring.h"

#include <stdlib.h>
#include <string.h>

#include "hplogc_platform.h"

#include <hplogc.h>

/** @brief 计算一条记录所需的槽位（含 4 字节头，向上取 4 的倍数）。 */
static size_t hp_ring_slot_of(size_t payload)
{
    size_t slot = payload + HP_RING_SLOT_HDR;
    return (slot + 3u) & ~(size_t)3u;
}

int hp_ring_init(hp_ring_t* rb, size_t capacity, int policy)
{
    size_t cap;

    if (rb == NULL) {
        return HP_RING_ERR;
    }
    memset(rb, 0, sizeof(*rb));
    cap = capacity & ~(size_t)7u;
    if (cap < 64u) {
        cap = 64u;
    }
    rb->buf = (unsigned char*)malloc(cap);
    if (rb->buf == NULL) {
        return HP_RING_ERR;
    }
    rb->cap = cap;
    rb->overflow = policy;
    if (hp_mutex_init(&rb->mu) != HPLOGC_OK) {
        free(rb->buf);
        rb->buf = NULL;
        return HP_RING_ERR;
    }
    if (hp_cond_init(&rb->cnd) != HPLOGC_OK) {
        hp_mutex_destroy(&rb->mu);
        free(rb->buf);
        rb->buf = NULL;
        return HP_RING_ERR;
    }
    hp_atomic_init_u64(&rb->overwritten, 0ull);
    hp_atomic_init_u64(&rb->head, 0ull);
    hp_atomic_init_u64(&rb->tail, 0ull);
    hp_atomic_init_u64(&rb->used, 0ull);
    return HP_RING_OK;
}

void hp_ring_destroy(hp_ring_t* rb)
{
    if (rb == NULL) {
        return;
    }
    if (rb->buf != NULL) {
        hp_mutex_destroy(&rb->mu);
        hp_cond_destroy(&rb->cnd);
        free(rb->buf);
        rb->buf = NULL;
    }
}

/**
 * @brief 丢弃队首的一个 slot（持锁调用）。
 *
 * @return 1 丢弃了一条记录（计入 overwritten）；0 仅跳过尾部填充。
 */
static int hp_ring_drop_oldest_locked(hp_ring_t* rb)
{
    size_t t = (size_t)hp_atomic_load_u64(&rb->tail);
    uint32_t sl = 0;

    memcpy(&sl, rb->buf + t, sizeof(sl));
    if (sl == HP_RING_PAD) {
        size_t pad = rb->cap - t;
        hp_atomic_store_u64(&rb->tail, 0ull);
        hp_atomic_fetch_sub_u64(&rb->used, (unsigned long long)pad);
        return 0;
    }
    hp_atomic_store_u64(&rb->tail, (unsigned long long)((t + sl) % rb->cap));
    hp_atomic_fetch_sub_u64(&rb->used, (unsigned long long)sl);
    hp_atomic_fetch_add_u64(&rb->overwritten, 1ull);
    return 1;
}

int hp_ring_reserve(hp_ring_t* rb, size_t len, unsigned char** out,
                    size_t* reserved_capacity)
{
    size_t slot;
    size_t idx;
    size_t avail_end;
    size_t pad;
    size_t need;

    if (rb == NULL || out == NULL || reserved_capacity == NULL) {
        return HP_RING_ERR;
    }
    slot = hp_ring_slot_of(len);
    if (slot + HP_RING_SLOT_HDR > rb->cap) {
        return HP_RING_ERR; /* 单条记录超过缓冲区容量 */
    }

    hp_mutex_lock(&rb->mu);
    for (;;) {
        idx = (size_t)hp_atomic_load_u64(&rb->head);
        avail_end = rb->cap - idx;
        pad = (avail_end < slot) ? avail_end : 0u;
        need = slot + pad;
        if (need > rb->cap) {
            hp_mutex_unlock(&rb->mu);
            return HP_RING_ERR;
        }
        if (hp_atomic_load_u64(&rb->used) + need <= rb->cap) {
            break;
        }
        if (rb->overflow == HPLOGC_OVERFLOW_WAIT) {
            hp_cond_wait(&rb->cnd, &rb->mu);
            continue;
        }
        if (rb->overflow != HPLOGC_OVERFLOW_OVERWRITE) {
            hp_mutex_unlock(&rb->mu);
            return HP_RING_DISCARD;
        }
        /* overwrite：消费者持锁拷贝，故不存在在途记录（§18-C5 天然满足） */
        if (hp_atomic_load_u64(&rb->used) == 0ull) {
            hp_mutex_unlock(&rb->mu);
            return HP_RING_DISCARD;
        }
        hp_ring_drop_oldest_locked(rb);
    }

    rb->res_idx = idx;
    rb->res_slot = slot;
    rb->res_pad = pad;
    *out = rb->buf + ((pad > 0) ? 0u : idx) + HP_RING_SLOT_HDR;
    *reserved_capacity = slot - HP_RING_SLOT_HDR;
    hp_mutex_unlock(&rb->mu);
    return HP_RING_OK;
}

void hp_ring_commit(hp_ring_t* rb, size_t len)
{
    size_t rec_idx;
    uint32_t sl;

    (void)len; /* 长度由预留时的 res_slot 决定 */
    if (rb == NULL) {
        return;
    }
    hp_mutex_lock(&rb->mu);
    rec_idx = (rb->res_pad > 0) ? 0u : rb->res_idx;
    if (rb->res_pad > 0) {
        uint32_t pad_mark = HP_RING_PAD;
        memcpy(rb->buf + rb->res_idx, &pad_mark, sizeof(pad_mark));
    }
    sl = (uint32_t)rb->res_slot;
    memcpy(rb->buf + rec_idx, &sl, sizeof(sl));
    hp_atomic_store_u64(&rb->head,
                        (unsigned long long)((rec_idx + rb->res_slot) % rb->cap));
    hp_atomic_fetch_add_u64(&rb->used,
                            (unsigned long long)(rb->res_pad + rb->res_slot));
    hp_mutex_unlock(&rb->mu);
}

void hp_ring_abort(hp_ring_t* rb)
{
    (void)rb; /* 预留期间未计入 used，无需回滚 */
}

int hp_ring_pop(hp_ring_t* rb, unsigned char* dst, size_t cap, size_t* len)
{
    uint32_t sl = 0;
    size_t payload;

    if (rb == NULL || dst == NULL || len == NULL) {
        return -1;
    }
    hp_mutex_lock(&rb->mu);
    for (;;) {
        if (hp_atomic_load_u64(&rb->used) == 0ull) {
            hp_mutex_unlock(&rb->mu);
            return 0;
        }
        memcpy(&sl, rb->buf + (size_t)hp_atomic_load_u64(&rb->tail), sizeof(sl));
        if (sl == HP_RING_PAD) {
            size_t pad = rb->cap - (size_t)hp_atomic_load_u64(&rb->tail);
            hp_atomic_store_u64(&rb->tail, 0ull);
            hp_atomic_fetch_sub_u64(&rb->used, (unsigned long long)pad);
            continue;
        }
        break;
    }
    payload = (size_t)sl - HP_RING_SLOT_HDR;
    if (payload > cap) {
        hp_mutex_unlock(&rb->mu);
        return -1;
    }
    /* 持锁拷贝：与生产者互斥，因此不存在"边读边被覆盖"的窗口 */
    memcpy(dst, rb->buf + (size_t)hp_atomic_load_u64(&rb->tail) + HP_RING_SLOT_HDR,
           payload);
    hp_atomic_store_u64(&rb->tail,
                        (unsigned long long)(((size_t)hp_atomic_load_u64(&rb->tail)
                                             + (size_t)sl) % rb->cap));
    hp_atomic_fetch_sub_u64(&rb->used, (unsigned long long)sl);
    *len = payload;
    hp_cond_broadcast(&rb->cnd);
    hp_mutex_unlock(&rb->mu);
    return 1;
}

size_t hp_ring_used(const hp_ring_t* rb)
{
    if (rb == NULL) {
        return 0;
    }
    return (size_t)hp_atomic_load_u64(&rb->used);
}

unsigned long long hp_ring_overwritten(const hp_ring_t* rb)
{
    if (rb == NULL) {
        return 0;
    }
    return hp_atomic_load_u64(&rb->overwritten);
}

void hp_ring_wake_producers(hp_ring_t* rb)
{
    if (rb == NULL) {
        return;
    }
    hp_mutex_lock(&rb->mu);
    hp_cond_broadcast(&rb->cnd);
    hp_mutex_unlock(&rb->mu);
}
