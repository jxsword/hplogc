/* -*- coding: utf-8 -*- */
/**
 * @file ringbuf_lockfree.c
 * @brief 无锁环形缓冲（`HPLOGC_LOCKFREE=ON`）。
 *
 * 仅支持 SPSC（单生产者 / 单消费者，§4.3）；MPSC 必须走有锁实现。
 * 设计要点：
 *   - `head` / `tail` 为单调递增的绝对计数器（不回绕），缓冲下标 = counter % cap；
 *   - `used = head - tail`，因此无需存储 used；
 *   - 生产者写 `head`（release），消费者读 `head`（acquire）；
 *   - `tail` 由消费者（pop）与生产者（overwrite 丢弃最旧）通过 CAS 推进，避免更新丢失；
 *   - overwrite 策略：生产者 CAS 推进 `tail` 丢弃最旧记录，直至腾出空间（§18-C5）。
 *
 * 全部经原子接口与平台契约，本文件不含平台分支。
 */

#include "ring.h"

#include <stdlib.h>
#include <string.h>

#include "hplogc_platform.h"

#include <hplogc.h>

/** @brief 计算一条记录所需的槽位（含 4 字节头，向上取 4 的倍数）。 */
static size_t hp_lf_slot_of(size_t payload)
{
    size_t slot = payload + HP_RING_SLOT_HDR;
    return (slot + 3u) & ~(size_t)3u;
}

int hp_ring_init(hp_ring_t* rb, size_t capacity, int policy)
{
    size_t cap = capacity & ~(size_t)7u;
    if (rb == NULL) {
        return HP_RING_ERR;
    }
    if (cap < 64u) {
        cap = 64u;
    }
    memset(rb, 0, sizeof(*rb));
    rb->buf = (unsigned char*)malloc(cap);
    if (rb->buf == NULL) {
        return HP_RING_ERR;
    }
    rb->cap = cap;
    rb->overflow = policy;
    hp_atomic_init_u64(&rb->head, 0ull);
    hp_atomic_init_u64(&rb->tail, 0ull);
    hp_atomic_init_u64(&rb->overwritten, 0ull);
    hp_atomic_init_u32(&rb->drop_seq, 0u);
    return HP_RING_OK;
}

void hp_ring_destroy(hp_ring_t* rb)
{
    if (rb == NULL) {
        return;
    }
    free(rb->buf);
    rb->buf = NULL;
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
    slot = hp_lf_slot_of(len);
    if (slot > rb->cap) {
        return HP_RING_ERR; /* 单条记录超过缓冲区容量 */
    }
    for (;;) {
        unsigned long long h = hp_atomic_load_u64(&rb->head);
        unsigned long long t = hp_atomic_load_u64(&rb->tail);
        idx = (size_t)(h % rb->cap);
        avail_end = rb->cap - idx;
        pad = (avail_end < slot) ? avail_end : 0u;
        need = slot + pad;
        if (h - t + need > rb->cap) {
            if (rb->overflow == HPLOGC_OVERFLOW_DISCARD) {
                /* 重读 tail：给消费者推进 tail 的释放存储一点可见时间，
                   避免读到陈旧 tail 误判为满而丢弃本可容纳的记录
                   （§4.3 SPSC 可见性）。 */
                int spins = 0;
                while (spins < 16) {
                    hp_yield();
                    t = hp_atomic_load_u64(&rb->tail);
                    if (h - t + need <= rb->cap) {
                        break;
                    }
                    spins++;
                }
                if (h - t + need > rb->cap) {
                    return HP_RING_DISCARD;
                }
                /* 落空：视为有空间，继续正常 reserve */
            }
            if (rb->overflow == HPLOGC_OVERFLOW_WAIT) {
                hp_yield();
                continue;
            }
            /* OVERWRITE：丢弃最旧记录（CAS 推进 tail） */
            {
                unsigned long long old_t = t;
                size_t ti = (size_t)(t % rb->cap);
                uint32_t sl = 0;
                memcpy(&sl, rb->buf + ti, sizeof(sl));
                {
                    unsigned long long new_t;
                    if (sl == HP_RING_PAD) {
                        new_t = t + (uint64_t)(rb->cap - ti);
                    } else {
                        new_t = t + (uint64_t)sl;
                    }
                    if (!hp_atomic_cas_u64(&rb->tail, &old_t, new_t)) {
                        continue; /* 重试 */
                    }
                }
                hp_atomic_fetch_add_u64(&rb->overwritten, 1ull);
                continue;
            }
        }
        rb->res_idx = idx;
        rb->res_pad = pad;
        rb->res_slot = slot;
        *out = rb->buf + ((pad > 0) ? 0u : idx) + HP_RING_SLOT_HDR;
        *reserved_capacity = slot - HP_RING_SLOT_HDR;
        return HP_RING_OK;
    }
}

void hp_ring_commit(hp_ring_t* rb, size_t len)
{
    unsigned long long h;
    size_t  idx;
    uint32_t sl;

    (void)len;
    if (rb == NULL) {
        return;
    }
    idx = (rb->res_pad > 0) ? 0u : rb->res_idx;
    if (rb->res_pad > 0) {
        uint32_t pad_mark = HP_RING_PAD;
        memcpy(rb->buf + rb->res_idx, &pad_mark, sizeof(pad_mark));
    }
    sl = (uint32_t)rb->res_slot;
    memcpy(rb->buf + idx, &sl, sizeof(sl));
    h = hp_atomic_load_u64(&rb->head);
    hp_atomic_store_u64(&rb->head,
                        h + (uint64_t)(rb->res_slot + rb->res_pad));
}

void hp_ring_abort(hp_ring_t* rb)
{
    (void)rb;
}

int hp_ring_pop(hp_ring_t* rb, unsigned char* dst, size_t cap, size_t* len)
{
    uint32_t sl = 0;
    size_t  payload;

    if (rb == NULL || dst == NULL || len == NULL) {
        return -1;
    }
    for (;;) {
        unsigned long long h = hp_atomic_load_u64(&rb->head);
        unsigned long long t = hp_atomic_load_u64(&rb->tail);
        size_t  idx;
        unsigned long long old_t;
        unsigned long long new_t;
        if (h == t) {
            return 0; /* 空 */
        }
        idx = (size_t)(t % rb->cap);
        memcpy(&sl, rb->buf + idx, sizeof(sl));
        if (sl == HP_RING_PAD) {
            old_t = t;
            new_t = t + (uint64_t)(rb->cap - idx);
            if (!hp_atomic_cas_u64(&rb->tail, &old_t, new_t)) {
                continue;
            }
            continue;
        }
        payload = (size_t)sl - HP_RING_SLOT_HDR;
        if (payload > cap) {
            return -1;
        }
        memcpy(dst, rb->buf + idx + HP_RING_SLOT_HDR, payload);
        old_t = t;
        new_t = t + (uint64_t)sl;
        if (!hp_atomic_cas_u64(&rb->tail, &old_t, new_t)) {
            continue;
        }
        *len = payload;
        return 1;
    }
}

size_t hp_ring_used(const hp_ring_t* rb)
{
    if (rb == NULL) {
        return 0;
    }
    return (size_t)(hp_atomic_load_u64(&rb->head)
                    - hp_atomic_load_u64(&rb->tail));
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
    (void)rb; /* 无锁实现无需唤醒 */
}
