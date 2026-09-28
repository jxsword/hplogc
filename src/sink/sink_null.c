/* -*- coding: utf-8 -*- */
/**
 * @file sink_null.c
 * @brief null sink：丢弃一切（契约参考实现与测试替身，§4.7.1、§13.1）。
 *
 * 也是"审计 / 指标下线"的路由落点（§7.3）。
 */

#include "hplogc_internal.h"

#include <hplogc.h>

/** @brief null sink 无私有数据；保留 1 字节以便 `hplogc_sink_priv()` 非 NULL。 */
typedef struct {
    int unused; /*!< 占位 */
} hp_null_priv_t;

static int hp_null_configure(hplogc_sink_t* sink, const char* key,
                             const char* val)
{
    (void)sink;
    (void)key;
    (void)val;
    return -1; /* null sink 不识别任何私有键 */
}

static int hp_null_init(hplogc_sink_t* sink)
{
    hp_null_priv_t* p = (hp_null_priv_t*)hplogc_sink_priv(sink);
    if (p != NULL && p->unused == 0) {
        p->unused = 1; /* 默认值填充：仅条件赋值（§4.10.2） */
    }
    return 0;
}

static void hp_null_emit(hplogc_sink_t* sink, const hplogc_event_t* ev)
{
    (void)sink;
    (void)ev; /* 丢弃 */
}

static int hp_null_emit_batch(hplogc_sink_t* sink,
                              const hplogc_event_t* const* evs, size_t n)
{
    (void)sink;
    (void)evs;
    return (int)n; /* 全部"写出"成功（记账按 §12.4） */
}

const hplogc_sink_ops_t hp_sink_null_ops = {
    "null",
    HPLOGC_SINK_ABI_VERSION,
    HPLOGC_CAP_SYNC | HPLOGC_CAP_ASYNC,
    sizeof(hp_null_priv_t),
    hp_null_configure,
    hp_null_init,
    NULL, /* start */
    hp_null_emit,
    hp_null_emit_batch,
    NULL, /* flush：无缓冲语义 → hplogc_sink_flush 返回 UNSUPPORTED（§7.5） */
    NULL, /* destroy */
    { NULL, NULL, NULL, NULL }
};
