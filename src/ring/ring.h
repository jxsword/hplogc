/* -*- coding: utf-8 -*- */
/**
 * @file ring.h
 * @brief 环形缓冲契约：有锁与无锁两套实现使用**同一组符号**（§3.3 / §4.3）。
 *
 * 有锁（`ringbuf_locked.c`）与无锁（`ringbuf_lockfree.c`）二选一编译：CMake 根据
 * `HPLOGC_LOCKFREE` 选择其一，核心层（`log.c` / `async.c`）不感知具体实现。
 *
 * 为避免头文件对具体实现细节的依赖，结构体**完整定义**放在此处；两套实现共用全部
 * 字段（互斥/条件变量与原子计数字段并存，各用所需）。
 *
 * 环形缓冲单条记录布局：
 *   [4 字节小端长度（含头）][ 载荷 ... ]，长度 `== HP_RING_PAD` 表示一段尾部填充。
 */

#ifndef HPLOGC_RING_H
#define HPLOGC_RING_H

#include "hplogc_atomic.h"
#include "hplogc_platform.h"

/** @brief 槽位头：4 字节小端长度（含头）。 */
#define HP_RING_SLOT_HDR 4
/** @brief 对齐填充占位标记。 */
#define HP_RING_PAD ((uint32_t)0xFFFFFFFFu)

/** @brief `hp_ring_reserve` 返回值。 */
enum {
    HP_RING_OK = 0,       /*!< 预留成功 */
    HP_RING_DISCARD = 1,   /*!< 按 discard 策略丢弃（队列已满） */
    HP_RING_ERR = -1       /*!< 参数错误或单条超过容量 */
};

/** @brief 环形缓冲（完整结构，供 g_rt 内嵌）。 */
struct hp_ring {
    unsigned char* buf;      /*!< 数据存储 */
    size_t         cap;      /*!< 容量（8 的倍数） */
    hp_atomic_u64  head;     /*!< 写指针（已用起点） */
    hp_atomic_u64  tail;     /*!< 读指针 */
    hp_atomic_u64  used;     /*!< 已占用字节 */
    size_t         res_idx;  /*!< 本次预留的起始位置 */
    size_t         res_slot; /*!< 本次预留的 slot 长度 */
    size_t         res_pad;  /*!< 本次预留所需的尾部填充字节数 */
    hp_atomic_u64  overwritten; /*!< 被覆盖条目计数（overwrite 策略） */
    hp_mutex_t     mu;       /*!< 有锁实现：互斥 */
    hp_cond_t      cnd;      /*!< 有锁实现：wait 策略 / 生产者唤醒 */
    int            overflow; /*!< 溢出策略（HPLOGC_OVERFLOW_*） */
    hp_atomic_u32  drop_seq; /*!< 无锁实现：丢弃计数（顺序锁版本） */
};

typedef struct hp_ring hp_ring_t;

/**
 * @brief 初始化环形缓冲（分配内部缓冲区，容量向上取 8 的倍数）。
 *
 * @param rb      环形缓冲。
 * @param capacity 期望容量（字节）。
 * @param policy 溢出策略（HPLOGC_OVERFLOW_DISCARD/OVERWRITE/WAIT）。
 * @return 0 成功；-1 失败（内存不足或参数错误）。
 */
int hp_ring_init(hp_ring_t* rb, size_t capacity, int policy);

/** @brief 销毁环形缓冲并释放内部缓冲区。 */
void hp_ring_destroy(hp_ring_t* rb);

/**
 * @brief 预留一条记录的空间。
 *
 * @param rb      环形缓冲。
 * @param len     payload 字节数（不含 4 字节长度头）。
 * @param out     成功时接收写入地址（指向缓冲区内部，commit 前有效）。
 * @param reserved_capacity 接收实际可写容量（恒 >= len）。
 * @return HP_RING_OK 成功；HP_RING_DISCARD 队列已满且策略 discard；
 *         HP_RING_ERR 参数或单条超过容量。
 */
int hp_ring_reserve(hp_ring_t* rb, size_t len, unsigned char** out,
                    size_t* reserved_capacity);

/**
 * @brief 提交已预留的记录（写入长度头并推进写指针）。
 *
 * @param rb  环形缓冲。
 * @param len payload 字节数（与 reserve 时一致）。
 */
void hp_ring_commit(hp_ring_t* rb, size_t len);

/**
 * @brief 放弃一次预留（未提交时无需回滚，仅作显式语义）。
 */
void hp_ring_abort(hp_ring_t* rb);

/**
 * @brief 消费者：取出队首记录并拷贝到调用方缓冲，同时推进队首。
 *
 * @return 1 取到记录；0 队列为空；-1 参数或容量错误。
 */
int hp_ring_pop(hp_ring_t* rb, unsigned char* dst, size_t cap, size_t* len);

/** @brief 当前已占用字节。 */
size_t hp_ring_used(const hp_ring_t* rb);

/** @brief 累计被覆盖（overwrite 策略）的记录数。 */
unsigned long long hp_ring_overwritten(const hp_ring_t* rb);

/** @brief 唤醒 wait 策略下阻塞的生产者（丢弃/释放空间后调用）。 */
void hp_ring_wake_producers(hp_ring_t* rb);

#endif /* HPLOGC_RING_H */
