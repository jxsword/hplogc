/* -*- coding: utf-8 -*- */
/**
 * @file sink_registry.c
 * @brief sink 类型注册表：type → ops（init 后只读、免锁访问，§4.10.1）。
 */

#include "hplogc_internal.h"

#include <stdlib.h>

#include <hplogc.h>

/** @brief 注册表条目。 */
typedef struct {
    const hplogc_sink_ops_t* ops; /*!< 操作表指针（核心不拷贝内容） */
} hp_reg_entry_t;

static hp_reg_entry_t g_reg[HPLOGC_MAX_SINK_TYPES]; /*!< 注册表 */
static int            g_reg_count;                  /*!< 已注册类型数 */
static int            g_reg_ready;                  /*!< 内置类型是否已注册 */

const hplogc_sink_ops_t* hp_sink_lookup(const char* type)
{
    int i;

    if (type == NULL) {
        return NULL;
    }
    /* "file" 是 "rollingfile" 的配置别名（§4.7.1） */
    if (strcmp(type, "file") == 0) {
        type = "rollingfile";
    }
    for (i = 0; i < g_reg_count; i++) {
        if (g_reg[i].ops != NULL && strcmp(g_reg[i].ops->type, type) == 0) {
            return g_reg[i].ops;
        }
    }
    return NULL;
}

int hp_sink_register_ops(const hplogc_sink_ops_t* ops)
{
    int i;

    if (ops == NULL || ops->type == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    for (i = 0; i < g_reg_count; i++) {
        if (g_reg[i].ops != NULL && strcmp(g_reg[i].ops->type, ops->type) == 0) {
            return HPLOGC_ERR_INVALID_ARG; /* 重复注册（§7.3） */
        }
    }
    if (g_reg_count >= HPLOGC_MAX_SINK_TYPES) {
        return HPLOGC_ERR_UNSUPPORTED; /* 类型表已满 */
    }
    g_reg[g_reg_count].ops = ops;
    g_reg_count++;
    return HPLOGC_OK;
}

int hp_sink_unregister_ops(const char* type)
{
    int i;
    int j;

    if (type == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    /* 存在活跃实例时不允许注销（§7.3） */
    hp_mutex_lock(&g_rt.inst_mu);
    for (i = 0; i < HPLOGC_MAX_SINKS; i++) {
        if (g_rt.insts[i] != NULL && strcmp(g_rt.insts[i]->type, type) == 0) {
            hp_mutex_unlock(&g_rt.inst_mu);
            return HPLOGC_ERR_STATE;
        }
    }
    hp_mutex_unlock(&g_rt.inst_mu);

    for (i = 0; i < g_reg_count; i++) {
        if (g_reg[i].ops != NULL && strcmp(g_reg[i].ops->type, type) == 0) {
            for (j = i; j + 1 < g_reg_count; j++) {
                g_reg[j].ops = g_reg[j + 1].ops;
            }
            g_reg[g_reg_count - 1].ops = NULL;
            g_reg_count--;
            return HPLOGC_OK;
        }
    }
    return HPLOGC_ERR_UNSUPPORTED;
}

void hp_sink_register_builtins(void)
{
    if (g_reg_ready) {
        return;
    }
#ifdef HPLOGC_SINK_HAS_CONSOLE
    hp_sink_register_ops(&hp_sink_console_ops);
#endif
#ifdef HPLOGC_SINK_HAS_ROLLINGFILE
    hp_sink_register_ops(&hp_sink_rollingfile_ops);
#endif
#ifdef HPLOGC_SINK_HAS_NULL
    hp_sink_register_ops(&hp_sink_null_ops);
#endif
#ifdef HPLOGC_SINK_HAS_SYSLOG
    hp_sink_register_ops(&hp_sink_syslog_ops);
#endif
#ifdef HPLOGC_SINK_HAS_SOCKET
    hp_sink_register_ops(&hp_sink_socket_ops);
#endif
    g_reg_ready = 1;
}

/**
 * @brief 内置类型的 fsync 内部钩子（§18-C8）。
 *
 * v0.2 的 `hplogc_sink_ops_t` 字段顺序已冻结且无 fsync 槽位，因此核心通过
 * 本内部钩子驱动 fsync：内置类型在此登记实现，自定义类型返回 UNSUPPORTED。
 */
int hp_sink_internal_fsync(hplogc_sink_t* sink)
{
    if (sink == NULL || !(sink->caps & HPLOGC_CAP_FSYNC)) {
        return HPLOGC_ERR_UNSUPPORTED;
    }
#ifdef HPLOGC_SINK_HAS_ROLLINGFILE
    if (strcmp(sink->type, "rollingfile") == 0) {
        return hp_rollingfile_fsync(sink);
    }
#endif
    return HPLOGC_ERR_UNSUPPORTED;
}
