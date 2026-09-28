/* -*- coding: utf-8 -*- */
/**
 * @file core.c
 * @brief 初始化 / 关闭 / 运行时状态 / 统计 / 级别控制（§7.2、§7.3、§7.5）。
 */

#include "hplogc_internal.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>

#include <hplogc.h>

/** @brief 全局运行时实例（唯一）。 */
hplogc_rt_t g_rt;

/** @brief 旧运行时集合的回收宽限（毫秒）。 */
#define HP_RT_RETIRE_MS 5000u

/** @brief stderr 告警的限频间隔（毫秒，§9：每 5 秒至多 1 条）。 */
#define HP_WARN_INTERVAL_MS 5000u

/* ============================ 基础工具 ============================ */

int hp_ieq_str(const char* a, const char* b)
{
    if (a == NULL || b == NULL) {
        return (a == b) ? 0 : 1;
    }
    while (*a != '\0' && *b != '\0') {
        int ca = *a;
        int cb = *b;
        if (ca >= 'A' && ca <= 'Z') {
            ca += 32;
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb += 32;
        }
        if (ca != cb) {
            return 1;
        }
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0') ? 0 : 1;
}

int hp_parse_bool_str(const char* v)
{
    if (v == NULL) {
        return -1;
    }
    if (hp_ieq_str(v, "true") == 0 || hp_ieq_str(v, "1") == 0
        || hp_ieq_str(v, "on") == 0 || hp_ieq_str(v, "yes") == 0) {
        return 1;
    }
    if (hp_ieq_str(v, "false") == 0 || hp_ieq_str(v, "0") == 0
        || hp_ieq_str(v, "off") == 0 || hp_ieq_str(v, "no") == 0) {
        return 0;
    }
    return -1;
}

int hp_parse_size_str(const char* v, unsigned long long* out)
{
    const char* end = NULL;
    unsigned long long n = 0;
    int digits = 0;

    if (v == NULL || out == NULL || v[0] == '\0') {
        return -1;
    }
    while (*v >= '0' && *v <= '9') {
        if (n > (~0ull - 9ull) / 10ull) {
            return -1; /* 溢出 */
        }
        n = n * 10ull + (unsigned long long)(*v - '0');
        v++;
        digits++;
    }
    if (digits == 0) {
        return -1;
    }
    while (*v == ' ' || *v == '\t') {
        v++;
    }
    end = v;
    if (*end == '\0') {
        *out = n;
        return 0;
    }
    if (hp_ieq_str(end, "kb") == 0) {
        *out = n * 1024ull;
    } else if (hp_ieq_str(end, "k") == 0) {
        *out = n * 1000ull;
    } else if (hp_ieq_str(end, "mb") == 0) {
        *out = n * 1024ull * 1024ull;
    } else if (hp_ieq_str(end, "m") == 0) {
        *out = n * 1000000ull;
    } else if (hp_ieq_str(end, "gb") == 0) {
        *out = n * 1024ull * 1024ull * 1024ull;
    } else if (hp_ieq_str(end, "g") == 0) {
        *out = n * 1000000000ull;
    } else {
        return -1;
    }
    return 0;
}

int hp_global_utc(void)
{
    hp_runtime_t* rt = hp_rt_active();
    return (rt != NULL) ? rt->cfg.utc : 0;
}

void hp_sink_note_failed(hplogc_sink_t* sink)
{
    if (sink != NULL) {
        hp_atomic_fetch_add_u64(&sink->failed, 1ull);
    }
}

void hp_sink_note_dropped(hplogc_sink_t* sink)
{
    if (sink != NULL) {
        hp_atomic_fetch_add_u64(&sink->dropped, 1ull);
    }
}

/* ============================ stderr 诊断 ============================ */

void hp_err_printf(const char* fmt, ...)
{
    va_list ap;
    char buf[512];
    int n;

    if (fmt == NULL) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if ((size_t)n >= sizeof(buf)) {
        n = (int)sizeof(buf) - 1;
    }
    hp_console_write(2, buf, (size_t)n);
    hp_console_write(2, "\n", 1);
}

void hp_warn_throttled(const char* fmt, ...)
{
    uint64_t now = hp_now_monotonic_ns() / 1000000ull;
    unsigned long long last;
    va_list ap;
    char buf[512];
    int n;

    if (fmt == NULL) {
        return;
    }
    last = hp_atomic_load_u64(&g_rt.last_warn_ms);
    if (last != 0 && now - last < HP_WARN_INTERVAL_MS) {
        return; /* 限频：每 5 秒至多 1 条（§9） */
    }
    if (!hp_atomic_cas_u64(&g_rt.last_warn_ms, &last, now)) {
        return; /* 其它线程刚发过 */
    }
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if ((size_t)n >= sizeof(buf)) {
        n = (int)sizeof(buf) - 1;
    }
    hp_console_write(2, buf, (size_t)n);
    hp_console_write(2, "\n", 1);
}

hp_runtime_t* hp_rt_active(void)
{
    return (hp_runtime_t*)(uintptr_t)hp_atomic_load_u64(&g_rt.active_ptr);
}

/** @brief 原子发布新的运行时集合。 */
static void hp_rt_publish(hp_runtime_t* rt)
{
    hp_runtime_t* old = hp_rt_active();
    hp_atomic_store_u64(&g_rt.active_ptr, (unsigned long long)(uintptr_t)rt);
    if (old != NULL) {
        old->retire_at_ms = hp_now_monotonic_ns() / 1000000ull
                            + HP_RT_RETIRE_MS;
        old->next_retired = g_rt.retired;
        g_rt.retired = old;
    }
}

void hp_runtime_destroy(hp_runtime_t* rt)
{
    int i;

    if (rt == NULL) {
        return;
    }
    for (i = 0; i < rt->sink_count; i++) {
        if (rt->sinks[i] != NULL && rt->owned_sinks[i]) {
            hplogc_sink_destroy(rt->sinks[i]);
            rt->sinks[i] = NULL;
        }
    }
    free(rt);
}

/** @brief 回收宽限期已过的运行时集合。 */
static void hp_rt_reap(int force)
{
    uint64_t now = hp_now_monotonic_ns() / 1000000ull;
    hp_runtime_t* cur = g_rt.retired;
    hp_runtime_t* keep = NULL;
    hp_runtime_t* next;

    while (cur != NULL) {
        next = cur->next_retired;
        if (force || cur->retire_at_ms <= now) {
            hp_runtime_destroy(cur);
        } else {
            cur->next_retired = keep;
            keep = cur;
        }
        cur = next;
    }
    g_rt.retired = keep;
}

/* ============================ 运行时集合装配 ============================ */

int hp_runtime_apply(hp_config_t* cfg, hp_runtime_t** out)
{
    hp_runtime_t* rt;
    int i;
    int j;

    if (cfg == NULL || out == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    rt = (hp_runtime_t*)calloc(1, sizeof(*rt));
    if (rt == NULL) {
        return HPLOGC_ERR_NO_MEM;
    }
    memcpy(&rt->cfg, cfg, sizeof(rt->cfg));

    /* ---- 格式：内置名始终可用，同名条目覆盖内置模板（§7.2） ---- */
    for (i = 0; i < hp_builtin_format_count(); i++) {
        const char* bname = hp_builtin_format_name(i);
        const char* tmpl = hp_builtin_format_tmpl(bname);
        for (j = 0; j < (int)cfg->format_count; j++) {
            if (strcmp(cfg->formats[j].name, bname) == 0) {
                tmpl = cfg->formats[j].tmpl; /* 显式配置优先 */
                break;
            }
        }
        if (hp_format_compile(bname, tmpl, &rt->fmts[rt->fmt_count])
            != HPLOGC_OK) {
            hp_err_printf("hplogc: 格式 '%s' 的模板非法（未知占位符？）", bname);
            free(rt);
            return HPLOGC_ERR_CONFIG;
        }
        rt->fmt_count++;
    }
    for (i = 0; i < (int)cfg->format_count; i++) {
        int is_builtin = 0;
        for (j = 0; j < hp_builtin_format_count(); j++) {
            if (strcmp(cfg->formats[i].name, hp_builtin_format_name(j)) == 0) {
                is_builtin = 1;
                break;
            }
        }
        if (is_builtin) {
            continue;
        }
        if (rt->fmt_count >= HPLOGC_MAX_FORMATS) {
            hp_err_printf("hplogc: 格式条目数超过上限 %d", HPLOGC_MAX_FORMATS);
            free(rt);
            return HPLOGC_ERR_CONFIG;
        }
        if (hp_format_compile(cfg->formats[i].name, cfg->formats[i].tmpl,
                              &rt->fmts[rt->fmt_count]) != HPLOGC_OK) {
            hp_err_printf("hplogc: 格式 '%s' 的模板非法（未知占位符？）",
                          cfg->formats[i].name);
            free(rt);
            return HPLOGC_ERR_CONFIG;
        }
        rt->fmt_count++;
    }
    rt->default_fmt_idx = 0;
    for (i = 0; i < rt->fmt_count; i++) {
        if (strcmp(rt->fmts[i].name, cfg->default_format) == 0) {
            rt->default_fmt_idx = i;
            break;
        }
    }

    /* ---- sink 实例 ---- */
    for (i = 0; i < (int)cfg->sink_count; i++) {
        hplogc_sink_t* s = NULL;
        int rc;
        if (!cfg->sinks[i].enabled) {
            continue; /* enabled=false：条目保留但不创建实例（§10.4） */
        }
        rc = hp_sink_create_from_def(&cfg->sinks[i], &s);
        if (rc != HPLOGC_OK) {
            hp_err_printf("hplogc: sink '%s'（类型 '%s'）创建失败：%d",
                          cfg->sinks[i].name, cfg->sinks[i].type, rc);
            hp_runtime_destroy(rt);
            return rc;
        }
        rt->sinks[rt->sink_count] = s;
        rt->owned_sinks[rt->sink_count] = 1;
        rt->sink_count++;
    }

    /* ---- 规则 ---- */
    for (i = 0; i < (int)cfg->sink_count; i++) {
        (void)i;
    }
    for (i = 0; i < (int)cfg->sink_count; i++) {
        /* 占位：保持编译器对 cfg 的使用清晰 */
        break;
    }
    rt->rule_count = 0;
    for (i = 0; i < (int)cfg->sink_count; i++) {
        (void)i;
    }

    /* ---- 兜底 sink ---- */
    for (i = 0; i < (int)cfg->default_sink_count; i++) {
        for (j = 0; j < rt->sink_count; j++) {
            if (strcmp(rt->sinks[j]->name, cfg->default_sinks[i]) == 0) {
                rt->default_sinks[rt->default_sink_count] = j;
                rt->default_sink_count++;
                break;
            }
        }
    }

    *out = rt;
    return HPLOGC_OK;
}

int hp_runtime_reload_apply(hp_config_t* cfg)
{
    hp_runtime_t* rt = NULL;
    int          rc;

    if (cfg == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    rc = hp_runtime_apply(cfg, &rt);
    if (rc != HPLOGC_OK) {
        return rc; /* 装配失败：旧配置保持（整体回滚，§10.5） */
    }
    hp_rt_publish(rt); /* 原子替换并退休旧集合（宽限后回收） */
    hp_rt_reap(0);
    memcpy(&g_rt.saved_cfg, cfg, sizeof(g_rt.saved_cfg));
    g_rt.has_saved_cfg = 1;
    return HPLOGC_OK;
}

/* ============================ 初始化 ============================ */

/** @brief 一次性初始化全局互斥量等（init 期单线程调用）。 */
static void hp_rt_init_once(void)
{
    static int done = 0;
    if (done) {
        return;
    }
    memset(&g_rt, 0, sizeof(g_rt));
    hp_atomic_init_i32(&g_rt.init_state, 0);
    hp_atomic_init_u64(&g_rt.active_ptr, 0ull);
    hp_mutex_init(&g_rt.mgr_mu);
    hp_mutex_init(&g_rt.inst_mu);
    hp_mutex_init(&g_rt.cat_mu);
    hp_cond_init(&g_rt.consumer_cv);
    hp_mutex_init(&g_rt.consumer_mu);
    hp_atomic_init_i32(&g_rt.consumer_stop, 0);
    hp_atomic_init_i32(&g_rt.reload_stop, 0);
    hp_atomic_init_i32(&g_rt.reload_signal, 0);
    hp_atomic_init_i32(&g_rt.signal_safe, 0);
    hp_atomic_init_i32(&g_rt.forked_dirty, 0);
    hp_atomic_init_i32(&g_rt.child_disabled, 0);
    hp_atomic_init_u64(&g_rt.accepted, 0ull);
    hp_atomic_init_u64(&g_rt.dropped, 0ull);
    hp_atomic_init_u64(&g_rt.throttled, 0ull);
    hp_atomic_init_u64(&g_rt.written, 0ull);
    hp_atomic_init_u64(&g_rt.fields_dropped, 0ull);
    hp_atomic_init_u64(&g_rt.sample_counter, 0ull);
    hp_atomic_init_u64(&g_rt.g_tokens, 0ull);
    hp_atomic_init_u64(&g_rt.g_last_ms, 0ull);
    hp_atomic_init_u64(&g_rt.last_warn_ms, 0ull);
    done = 1;
}

/** @brief 按配置启动运行时（供三种 init 路径共用）。 */
static int hp_core_start(hp_config_t* cfg, const char* path)
{
    hp_runtime_t* rt = NULL;
    int rc;
    size_t bufsz;

    /* 溢出策略可用性（§4.3 / §10.4 fail-fast） */
    if (cfg->overflow_policy == HPLOGC_OVERFLOW_WAIT) {
#ifdef HPLOGC_USE_LOCKFREE
        hp_err_printf("hplogc: 无锁构建不支持 overflow policy = wait");
        return HPLOGC_ERR_UNSUPPORTED;
#endif
    }
    if (cfg->overflow_policy == HPLOGC_OVERFLOW_OVERWRITE) {
#ifdef HPLOGC_USE_LOCKFREE
#ifndef HPLOGC_CONCURRENCY_SPSC
        hp_err_printf("hplogc: 无锁 MPSC 构建不支持 overflow policy = overwrite");
        return HPLOGC_ERR_UNSUPPORTED;
#endif
#endif
    }
    /* 缓冲区下限（§4.3） */
    if (cfg->buffer_size < 2u * cfg->max_log_length) {
        size_t want = 2u * cfg->max_log_length;
        if (want < HPLOGC_MIN_BUFFER_SIZE) {
            want = HPLOGC_MIN_BUFFER_SIZE;
        }
        hp_err_printf("hplogc: buffer size %lu 小于 2 x max log length，"
                      "自动提升至 %lu",
                      (unsigned long)cfg->buffer_size, (unsigned long)want);
        cfg->buffer_size = want;
    }

    g_rt.scratch_size = cfg->max_log_length + HPLOGC_REC_EXTRA;
    if (!g_rt.tls_ready) {
        if (hp_tls_create(&g_rt.scratch_tls, free) == HPLOGC_OK) {
            g_rt.tls_ready = 1;
        }
    }
    bufsz = cfg->buffer_size;

#ifdef HPLOGC_HAS_ASYNC
    if (hp_ring_init(&g_rt.ring, bufsz, cfg->overflow_policy) < 0) {
        hp_err_printf("hplogc: 环形缓冲初始化失败");
        return HPLOGC_ERR_NO_MEM;
    }
    g_rt.has_ring = 1;
#else
    (void)bufsz;
    g_rt.has_ring = 0; /* 同步构建不创建队列（§18-C1） */
#endif

    rc = hp_runtime_apply(cfg, &rt);
    if (rc != HPLOGC_OK) {
        if (g_rt.has_ring) {
            hp_ring_destroy(&g_rt.ring);
            g_rt.has_ring = 0;
        }
        return rc;
    }
    hp_rt_publish(rt);
    g_rt.init_mono_ns = hp_now_monotonic_ns();

    if (path != NULL) {
        snprintf(g_rt.config_path, sizeof(g_rt.config_path), "%s", path);
        g_rt.has_config_path = 1;
    } else {
        g_rt.has_config_path = 0;
    }
    memcpy(&g_rt.saved_cfg, cfg, sizeof(g_rt.saved_cfg));
    g_rt.has_saved_cfg = 1;
    hp_atomic_store_i32(&g_rt.signal_safe, cfg->signal_safe ? 1 : 0);

#ifdef HPLOGC_HAS_ASYNC
    if (hp_async_start() != HPLOGC_OK) {
        hp_err_printf("hplogc: 消费者线程启动失败");
        hplogc_shutdown();
        return HPLOGC_ERR_NO_MEM;
    }
#endif
#ifdef HPLOGC_HAS_HOT_RELOAD
    if (hp_reload_start() != HPLOGC_OK) {
        hp_err_printf("hplogc: 热加载监视线程启动失败");
    }
#endif
    hp_signal_init();
    hp_atomic_store_i32(&g_rt.init_state, 1);
    return HPLOGC_OK;
}

int hplogc_init(const hplogc_config_t* cfg)
{
    hp_config_t c;
    int rc;

    hp_rt_init_once();
    hp_sink_register_builtins();
    if (hp_atomic_load_i32(&g_rt.init_state) == 1) {
        return HPLOGC_ERR_STATE; /* 重复 init（§7.5） */
    }
    if (cfg == NULL) {
        return hplogc_init_default();
    }
    if (cfg->struct_size != (uint32_t)sizeof(hplogc_config_t)) {
        hp_err_printf("hplogc: config struct_size=%u 与库内 %lu 不一致（ABI 自检）",
                      cfg->struct_size, (unsigned long)sizeof(hplogc_config_t));
        return HPLOGC_ERR_CONFIG;
    }
    rc = hp_conf_from_code(cfg, &c);
    if (rc != HPLOGC_OK) {
        return rc;
    }
    return hp_core_start(&c, NULL);
}

int hplogc_init_from_file(const char* config_path)
{
    hp_ini_t ini;
    hp_config_t c;
    int rc;

    if (config_path == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    hp_rt_init_once();
    hp_sink_register_builtins();
    if (hp_atomic_load_i32(&g_rt.init_state) == 1) {
        return HPLOGC_ERR_STATE;
    }
#ifndef HPLOGC_HAS_INI
    hp_err_printf("hplogc: 本构建已裁剪 INI 解析器（HPLOGC_ENABLE_INI=OFF）");
    return HPLOGC_ERR_UNSUPPORTED;
#else
    memset(&ini, 0, sizeof(ini));
    rc = hp_ini_parse(config_path, &ini);
    if (rc != HPLOGC_OK) {
        return rc;
    }
    rc = hp_conf_build(&ini, config_path, &c);
    hp_ini_free(&ini);
    if (rc != HPLOGC_OK) {
        return rc;
    }
    return hp_core_start(&c, config_path);
#endif
}

int hplogc_init_default(void)
{
    hp_config_t c;

    hp_rt_init_once();
    hp_sink_register_builtins();
    if (hp_atomic_load_i32(&g_rt.init_state) == 1) {
        return HPLOGC_ERR_STATE;
    }
    hp_conf_default(&c);
    /* 默认配置：level=INFO、standard 格式、内置 stderr console sink（§7.2） */
    c.level = HPLOGC_LEVEL_INFO;
    c.sink_count = 1;
    snprintf(c.sinks[0].name, sizeof(c.sinks[0].name), "stderr");
    snprintf(c.sinks[0].type, sizeof(c.sinks[0].type), "console");
    c.sinks[0].enabled = 1;
    c.sinks[0].async = -1;
    c.sinks[0].opt_count = 1;
    snprintf(c.sinks[0].opts[0].key, sizeof(c.sinks[0].opts[0].key), "stream");
    snprintf(c.sinks[0].opts[0].val, sizeof(c.sinks[0].opts[0].val), "stderr");
    c.default_sink_count = 1;
    snprintf(c.default_sinks[0], sizeof(c.default_sinks[0]), "stderr");
    return hp_core_start(&c, NULL);
}

void hplogc_config_default(hplogc_config_t* cfg)
{
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->struct_size = (uint32_t)sizeof(hplogc_config_t);
    cfg->level = HPLOGC_LEVEL_INFO;
    cfg->default_format = NULL;
    cfg->default_sinks = NULL;
    cfg->default_sink_count = 0;
    cfg->sinks = NULL;
    cfg->sink_count = 0;
    cfg->rules = NULL;
    cfg->rule_count = 0;
    cfg->global_options = NULL;
    cfg->global_option_count = 0;
    cfg->buffer_size = HPLOGC_DEFAULT_BUFFER_SIZE;
    cfg->overflow_policy = HPLOGC_OVERFLOW_DISCARD;
    cfg->batch_size = 64;
    cfg->flush_interval_ms = 100;
    cfg->shutdown_timeout_ms = 5000;
    cfg->escape_injection = 1;
    cfg->max_log_length = HPLOGC_DEFAULT_MAX_LOG_LEN;
    cfg->truncation_marker = NULL;
    cfg->crash_safety = HPLOGC_CRASH_SHUTDOWN;
    cfg->signal_safe = 0;
}

void hplogc_shutdown(void)
{
    if (hp_atomic_load_i32(&g_rt.init_state) == 0) {
        return; /* 未初始化：幂等 */
    }
    hp_atomic_store_i32(&g_rt.init_state, 2);

    /* ① 先停止热加载监视线程（§7.5） */
#ifdef HPLOGC_HAS_HOT_RELOAD
    hp_reload_stop();
#endif
    /* ② flush 并排空队列 */
    hplogc_flush();
#ifdef HPLOGC_HAS_ASYNC
    hp_async_stop();
#endif
    /* ③ 关闭输出、释放资源 */
    hp_signal_fini();
    {
        hp_runtime_t* rt = hp_rt_active();
        if (rt != NULL) {
            int i;
            /* shutdown 时按 crash_safety 对 CAP_FSYNC 实例 fsync（§9） */
            if (rt->cfg.crash_safety != HPLOGC_CRASH_NONE) {
                for (i = 0; i < rt->sink_count; i++) {
                    if (rt->sinks[i] != NULL) {
                        hp_sink_internal_fsync(rt->sinks[i]);
                    }
                }
            }
            hp_atomic_store_u64(&g_rt.active_ptr, 0ull);
            hp_runtime_destroy(rt);
        }
        hp_rt_reap(1);
    }
    if (g_rt.has_ring) {
        hp_ring_destroy(&g_rt.ring);
        g_rt.has_ring = 0;
    }
    hp_atomic_store_i32(&g_rt.init_state, 0);
}

/* ============================ 运行时控制 ============================ */

int hplogc_set_level(hplogc_level_t level)
{
    hp_runtime_t* rt;

    if (hp_atomic_load_i32(&g_rt.init_state) != 1) {
        return HPLOGC_ERR_STATE;
    }
    if ((int)level < 0 || (int)level > (int)HPLOGC_LEVEL_OFF) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    rt = hp_rt_active();
    if (rt == NULL) {
        return HPLOGC_ERR_STATE;
    }
    rt->cfg.level = (int)level;
    return HPLOGC_OK;
}

int hplogc_set_level_for_category(const char* category, hplogc_level_t level)
{
    int idx;

    if (category == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
#ifndef HPLOGC_HAS_CATEGORY
    return HPLOGC_ERR_CONFIG; /* 裁剪后为 stub（§4.8） */
#else
    if (hp_atomic_load_i32(&g_rt.init_state) != 1) {
        return HPLOGC_ERR_STATE;
    }
    if ((int)level < 0 || (int)level > (int)HPLOGC_LEVEL_OFF) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    idx = hp_cat_intern(category);
    if (idx < 0) {
        return HPLOGC_ERR_NO_MEM; /* 表满 */
    }
    hp_mutex_lock(&g_rt.cat_mu);
    g_rt.cats[idx].level_set = 1;
    g_rt.cats[idx].level = (int)level;
    hp_mutex_unlock(&g_rt.cat_mu);
    return HPLOGC_OK;
#endif
}

int hplogc_clear_level_for_category(const char* category)
{
    int idx;

    if (category == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
#ifndef HPLOGC_HAS_CATEGORY
    return HPLOGC_ERR_CONFIG;
#else
    if (hp_atomic_load_i32(&g_rt.init_state) != 1) {
        return HPLOGC_ERR_STATE;
    }
    idx = hp_cat_intern(category);
    if (idx < 0) {
        return HPLOGC_OK; /* 未设置过：幂等（§7.3） */
    }
    hp_mutex_lock(&g_rt.cat_mu);
    g_rt.cats[idx].level_set = 0;
    g_rt.cats[idx].level = 0;
    hp_mutex_unlock(&g_rt.cat_mu);
    return HPLOGC_OK;
#endif
}

int hplogc_flush(void)
{
    hp_runtime_t* rt;
    int i;

    if (hp_atomic_load_i32(&g_rt.init_state) != 1) {
        return HPLOGC_ERR_STATE;
    }
#ifdef HPLOGC_HAS_ASYNC
    hp_async_kick();      /* 通知消费者提交已入队日志 */
    hp_async_flush_wait();/* 阻塞等待排空（§7.5） */
#endif
    rt = hp_rt_active();
    if (rt != NULL) {
        for (i = 0; i < rt->sink_count; i++) {
            if (rt->sinks[i] != NULL && rt->sinks[i]->ops != NULL
                && rt->sinks[i]->ops->flush != NULL) {
                rt->sinks[i]->ops->flush(rt->sinks[i]);
            }
        }
    }
    return HPLOGC_OK;
}

int hplogc_sync(void)
{
    hp_runtime_t* rt;
    int i;
    int rc;

    rc = hplogc_flush();
    if (rc != HPLOGC_OK) {
        return rc;
    }
    rt = hp_rt_active();
    if (rt == NULL) {
        return HPLOGC_ERR_STATE;
    }
    for (i = 0; i < rt->sink_count; i++) {
        if (rt->sinks[i] != NULL) {
            hp_sink_internal_fsync(rt->sinks[i]); /* 仅 CAP_FSYNC 实例生效 */
        }
    }
    return HPLOGC_OK;
}

int hplogc_get_stats(hplogc_stats_t* stats)
{
    if (stats == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    if (hp_atomic_load_i32(&g_rt.init_state) != 1) {
        return HPLOGC_ERR_STATE;
    }
    stats->accepted = hp_atomic_load_u64(&g_rt.accepted);
    stats->dropped = hp_atomic_load_u64(&g_rt.dropped);
    stats->overwritten = g_rt.has_ring
                              ? (unsigned long long)hp_ring_overwritten(&g_rt.ring)
                              : 0ull;
    stats->throttled = hp_atomic_load_u64(&g_rt.throttled);
    stats->written = hp_atomic_load_u64(&g_rt.written);
    stats->fields_dropped = hp_atomic_load_u64(&g_rt.fields_dropped);
    if (g_rt.has_ring) {
        stats->buffer_used = hp_ring_used(&g_rt.ring);
        {
            long long cap = (long long)g_rt.ring.cap;
            stats->buffer_size = (cap > 0) ? (size_t)cap : 0u;
        }
    } else {
        stats->buffer_used = 0u;
        stats->buffer_size = 0u; /* 同步构建无队列（§18-C1） */
    }
    return HPLOGC_OK;
}

/** @brief 取环形缓冲容量（供统计使用）。 */
size_t hp_ring_capacity(const hp_ring_t* rb);

size_t hp_ring_capacity(const hp_ring_t* rb)
{
    return (rb != NULL) ? rb->cap : 0u;
}
