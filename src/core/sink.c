/* -*- coding: utf-8 -*- */
/**
 * @file sink.c
 * @brief sink 实例生命周期与 per-sink 统计（§4.10、§7.3、§7.5）。
 *
 * 核心只认识 `hplogc_sink_ops_t` 指针，不认识任何具体 sink 类型。
 */

#include "hplogc_internal.h"

#include <stdlib.h>

#include <hplogc.h>

/* ============================ 全局实例表（观测用） ============================ */

/** @brief 把实例登记进全局表（供 `hplogc_sink_find()` 观测）。 */
static void hp_inst_add(hplogc_sink_t* s)
{
    int i;
    hp_mutex_lock(&g_rt.inst_mu);
    for (i = 0; i < HPLOGC_MAX_SINKS; i++) {
        if (g_rt.insts[i] == NULL) {
            g_rt.insts[i] = s;
            if (i + 1 > g_rt.inst_count) {
                g_rt.inst_count = i + 1;
            }
            hp_mutex_unlock(&g_rt.inst_mu);
            return;
        }
    }
    hp_mutex_unlock(&g_rt.inst_mu);
}

/** @brief 从全局表移除实例。 */
static void hp_inst_remove(hplogc_sink_t* s)
{
    int i;
    hp_mutex_lock(&g_rt.inst_mu);
    for (i = 0; i < HPLOGC_MAX_SINKS; i++) {
        if (g_rt.insts[i] == s) {
            g_rt.insts[i] = NULL;
            break;
        }
    }
    while (g_rt.inst_count > 0 && g_rt.insts[g_rt.inst_count - 1] == NULL) {
        g_rt.inst_count--;
    }
    hp_mutex_unlock(&g_rt.inst_mu);
}

/* ============================ 创建与配置 ============================ */

/** @brief 分配并清零一个 sink 实例（不含 start）。 */
static int hp_sink_alloc(const hplogc_sink_ops_t* ops, const char* name,
                         hplogc_sink_t** out)
{
    hplogc_sink_t* s;

    if (ops == NULL || name == NULL || out == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    if (name[0] == '\0' || strlen(name) >= HPLOGC_MAX_NAME_LEN) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    s = (hplogc_sink_t*)calloc(1, sizeof(*s));
    if (s == NULL) {
        return HPLOGC_ERR_NO_MEM;
    }
    s->ops = ops;
    s->caps = ops->caps;
    s->async_mode = -1;
    s->fsync_level = 0;
    snprintf(s->name, sizeof(s->name), "%s", name);
    snprintf(s->type, sizeof(s->type), "%s", ops->type);
    if (ops->priv_size > 0) {
        s->priv = (unsigned char*)calloc(1, ops->priv_size);
        if (s->priv == NULL) {
            free(s);
            return HPLOGC_ERR_NO_MEM;
        }
    }
    hp_mutex_init(&s->io_mu);
    hp_atomic_init_u64(&s->written, 0ull);
    hp_atomic_init_u64(&s->dropped, 0ull);
    hp_atomic_init_u64(&s->failed, 0ull);
    hp_atomic_init_u64(&s->fields_dropped, 0ull);
    hp_atomic_init_u64(&s->bytes_written, 0ull);
    *out = s;
    return HPLOGC_OK;
}

int hplogc_sink_create(const char* type, const char* name,
                       hplogc_sink_t** out)
{
    const hplogc_sink_ops_t* ops;
    hplogc_sink_t* s;
    int rc;

    if (type == NULL || name == NULL || out == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    ops = hp_sink_lookup(type);
    if (ops == NULL) {
        return HPLOGC_ERR_UNSUPPORTED; /* 未注册 / 被裁剪（§4.8） */
    }
    rc = hp_sink_alloc(ops, name, &s);
    if (rc != HPLOGC_OK) {
        return rc;
    }
    hp_inst_add(s);
    *out = s;
    return HPLOGC_OK;
}

int hplogc_sink_configure(hplogc_sink_t* sink, const char* key,
                          const char* val)
{
    int rc;

    if (sink == NULL || key == NULL || val == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    if (sink->started) {
        return HPLOGC_ERR_STATE; /* 必须在 start 之前（§4.10.2） */
    }
    /* 通用键由核心消费（§10.3） */
    if (strcmp(key, "enabled") == 0) {
        return HPLOGC_OK;
    }
    if (strcmp(key, "async") == 0) {
        if (strcmp(val, "true") == 0 || strcmp(val, "1") == 0) {
            sink->async_mode = 1;
        } else if (strcmp(val, "false") == 0 || strcmp(val, "0") == 0) {
            sink->async_mode = 0;
        } else {
            return HPLOGC_ERR_CONFIG;
        }
        return HPLOGC_OK;
    }
    if (sink->ops->configure == NULL) {
        return HPLOGC_ERR_UNSUPPORTED;
    }
    rc = sink->ops->configure(sink, key, val);
    if (rc != 0) {
        return HPLOGC_ERR_UNSUPPORTED; /* 键不被该 sink 识别（§7.3） */
    }
    if (sink->opt_count < HPLOGC_MAX_OPTIONS) {
        snprintf(sink->opts[sink->opt_count].key,
                 sizeof(sink->opts[sink->opt_count].key), "%s", key);
        snprintf(sink->opts[sink->opt_count].val,
                 sizeof(sink->opts[sink->opt_count].val), "%s", val);
        sink->opt_count++;
    }
    return HPLOGC_OK;
}

int hplogc_sink_start(hplogc_sink_t* sink)
{
    int rc;
    int want_async;

    if (sink == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    if (sink->started) {
        return HPLOGC_ERR_STATE;
    }
    /* per-sink async 键的作用域：仅在能力位矩阵内显式选择（§4.10.3） */
    want_async = sink->async_mode;
    if (want_async == 1 && !(sink->caps & HPLOGC_CAP_ASYNC)) {
        hp_err_printf("hplogc: sink '%s' 不具备 CAP_ASYNC，忽略 async=true",
                      sink->name);
        want_async = -1;
    }
    if (want_async == 0 && !(sink->caps & HPLOGC_CAP_SYNC)) {
        hp_err_printf("hplogc: sink '%s' 不具备 CAP_SYNC，忽略 async=false",
                      sink->name);
        want_async = -1;
    }
    if (want_async < 0) {
#ifndef HPLOGC_HAS_ASYNC
        /* 同步构建：sink 必须具备 CAP_SYNC，否则 init 失败（§4.10.3） */
        if (!(sink->caps & HPLOGC_CAP_SYNC)) {
            hp_err_printf(
                "hplogc: sink 类型 '%s' 在同步构建下不可用（缺 CAP_SYNC）",
                sink->type);
            return HPLOGC_ERR_UNSUPPORTED;
        }
        want_async = 0;
#else
        want_async = (sink->caps & HPLOGC_CAP_ASYNC) ? 1 : 0;
#endif
    }
    sink->async_mode = want_async;

    /* init 只做默认值填充（必须晚于全部 configure，§4.10.2） */
    if (sink->ops->init != NULL) {
        rc = sink->ops->init(sink);
        if (rc != 0) {
            return HPLOGC_ERR_CONFIG;
        }
    }
    if (sink->ops->start != NULL) {
        rc = sink->ops->start(sink);
        if (rc != 0) {
            return HPLOGC_ERR_IO;
        }
    }
    sink->started = 1;
    return HPLOGC_OK;
}

void* hplogc_sink_priv(hplogc_sink_t* sink)
{
    if (sink == NULL) {
        return NULL;
    }
    return sink->priv;
}

int hplogc_sink_flush(hplogc_sink_t* sink)
{
    if (sink == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    if (sink->ops == NULL || sink->ops->flush == NULL) {
        return HPLOGC_ERR_UNSUPPORTED; /* 未声明缓冲语义（§7.5） */
    }
    return (sink->ops->flush(sink) == 0) ? HPLOGC_OK : HPLOGC_ERR_IO;
}

int hplogc_sink_get_stats(hplogc_sink_t* sink, hplogc_sink_stats_t* stats)
{
    if (sink == NULL || stats == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    if (hp_atomic_load_i32(&g_rt.init_state) != 1) {
        return HPLOGC_ERR_STATE; /* 未初始化（§7.5） */
    }
    stats->written = hp_atomic_load_u64(&sink->written);
    stats->dropped = hp_atomic_load_u64(&sink->dropped);
    stats->failed = hp_atomic_load_u64(&sink->failed);
    stats->fields_dropped = hp_atomic_load_u64(&sink->fields_dropped);
    stats->bytes_written = hp_atomic_load_u64(&sink->bytes_written);
    return HPLOGC_OK;
}

hplogc_sink_t* hplogc_sink_find(const char* name)
{
    int i;
    hplogc_sink_t* found = NULL;

    if (name == NULL || hp_atomic_load_i32(&g_rt.init_state) != 1) {
        return NULL;
    }
    hp_mutex_lock(&g_rt.inst_mu);
    for (i = 0; i < HPLOGC_MAX_SINKS; i++) {
        if (g_rt.insts[i] != NULL && strcmp(g_rt.insts[i]->name, name) == 0) {
            found = g_rt.insts[i];
            break;
        }
    }
    hp_mutex_unlock(&g_rt.inst_mu);
    return found;
}

void hplogc_sink_destroy(hplogc_sink_t* sink)
{
    if (sink == NULL) {
        return;
    }
    hp_inst_remove(sink);
    if (sink->ops != NULL && sink->ops->flush != NULL) {
        sink->ops->flush(sink);
    }
    if (sink->ops != NULL && sink->ops->destroy != NULL) {
        sink->ops->destroy(sink);
    }
    hp_mutex_destroy(&sink->io_mu);
    free(sink->priv);
    free(sink);
}

/* ============================ 注册 / 注销 / 查询 ============================ */

int hplogc_sink_register(const hplogc_sink_ops_t* ops)
{
    int i;

    if (ops == NULL || ops->type == NULL || ops->type[0] == '\0') {
        return HPLOGC_ERR_INVALID_ARG;
    }
    if (strlen(ops->type) >= HPLOGC_MAX_NAME_LEN) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    if (HPLOGC_SINK_ABI_MAJOR_OF(ops->abi_version)
        != HPLOGC_SINK_ABI_VERSION_MAJOR) {
        return HPLOGC_ERR_UNSUPPORTED; /* ABI 主版本必须相等（§4.10.5） */
    }
    if (ops->emit == NULL && ops->emit_batch == NULL) {
        return HPLOGC_ERR_INVALID_ARG; /* 必须至少实现其一（§4.10.3） */
    }
    for (i = 0; i < 4; i++) {
        if (ops->reserved[i] != NULL) {
            return HPLOGC_ERR_UNSUPPORTED; /* 扩展槽必须全为 NULL */
        }
    }
    if (hp_atomic_load_i32(&g_rt.init_state) == 1) {
        return HPLOGC_ERR_STATE; /* 已初始化（§7.3） */
    }
    return hp_sink_register_ops(ops);
}

int hplogc_sink_unregister(const char* type)
{
    if (type == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    return hp_sink_unregister_ops(type);
}

int hplogc_sink_is_registered(const char* type)
{
    if (type == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    return (hp_sink_lookup(type) != NULL) ? HPLOGC_OK : HPLOGC_ERR_UNSUPPORTED;
}

/* ============================ 配置装配期使用 ============================ */

int hp_sink_create_from_def(const hp_sink_def_t* def, hplogc_sink_t** out)
{
    const hplogc_sink_ops_t* ops;
    hplogc_sink_t* s;
    int rc;
    size_t i;
    int strict;

    if (def == NULL || out == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    ops = hp_sink_lookup(def->type);
    if (ops == NULL) {
        return HPLOGC_ERR_UNSUPPORTED;
    }
    rc = hp_sink_alloc(ops, def->name, &s);
    if (rc != HPLOGC_OK) {
        return rc;
    }
    {
        hp_runtime_t* cur = hp_rt_active();
        strict = (cur != NULL) ? cur->cfg.strict_init : 1;
    }
    for (i = 0; i < def->opt_count; i++) {
        rc = hplogc_sink_configure(s, def->opts[i].key, def->opts[i].val);
        if (rc != HPLOGC_OK) {
            if (strict) {
                hp_err_printf("hplogc: sink '%s' 的私有键 '%s' 无法识别",
                              def->name, def->opts[i].key);
                hplogc_sink_destroy(s);
                return HPLOGC_ERR_CONFIG;
            }
            hp_err_printf("hplogc: sink '%s' 的私有键 '%s' 无法识别，已忽略",
                          def->name, def->opts[i].key);
        }
    }
    /* async 键（代码内配置为整型，此处按 -1/0/1 语义设置） */
    if (def->async == 0 || def->async == 1) {
        s->async_mode = def->async;
    }
    rc = hplogc_sink_start(s);
    if (rc != HPLOGC_OK) {
        hplogc_sink_destroy(s);
        return rc;
    }
    *out = s;
    return HPLOGC_OK;
}

int hp_sink_def_equal(const hp_sink_def_t* a, const hp_sink_def_t* b)
{
    size_t i;
    size_t j;

    if (a == NULL || b == NULL) {
        return 0;
    }
    if (strcmp(a->type, b->type) != 0 || a->enabled != b->enabled
        || a->opt_count != b->opt_count) {
        return 0;
    }
    /* 私有键值按集合比较（顺序无关） */
    for (i = 0; i < a->opt_count; i++) {
        int found = 0;
        for (j = 0; j < b->opt_count; j++) {
            if (strcmp(a->opts[i].key, b->opts[j].key) == 0
                && strcmp(a->opts[i].val, b->opts[j].val) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            return 0;
        }
    }
    return 1;
}
