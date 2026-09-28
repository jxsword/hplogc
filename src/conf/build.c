/* -*- coding: utf-8 -*- */
/**
 * @file build.c
 * @brief 由 INI 解析结果或代码内配置装配并校验运行时配置快照（§10.4 / §7.2）。
 */

#include "hplogc_internal.h"

#include <stdio.h>

#include <hplogc.h>

/** @brief 节编号（与 ini.c 保持一致）。 */
enum {
    SEC_NONE = -1,
    SEC_BUILD = 0,
    SEC_GLOBAL = 1,
    SEC_FORMATS = 2,
    SEC_OUTPUTS = 3,
    SEC_BUFFER = 4,
    SEC_ASYNC = 5,
    SEC_THROTTLE = 6,
    SEC_RULES = 7,
    SEC_ADVANCED = 8
};

/* ============================ 默认配置 ============================ */

void hp_conf_default(hp_config_t* out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->strict_init = 0;
    out->level = HPLOGC_LEVEL_INFO;
    snprintf(out->default_format, sizeof(out->default_format), "standard");
    out->default_sink_count = 0;
    out->utc = 0;
    out->ts_monotonic = 0;
    snprintf(out->time_format, sizeof(out->time_format), "%s",
             "%Y-%m-%d %H:%M:%S.%f");
    out->capture_source_loc = 1;
    out->newline = HPLOGC_NEWLINE_AUTO;
    out->pid_fmt = HPLOGC_IDF_DECIMAL;
    out->tid_fmt = HPLOGC_IDF_DECIMAL;
    out->hot_reload_interval = 0;
    out->signal_reload = 0;
    out->format_count = 0;
    out->sink_count = 0;
    out->rule_count = 0;
    out->buffer_size = HPLOGC_DEFAULT_BUFFER_SIZE;
    out->overflow_policy = HPLOGC_OVERFLOW_DISCARD;
    out->batch_size = 64;
    out->flush_interval_ms = 100;
    out->shutdown_timeout_ms = 5000;
    out->global_rate = 0;
    out->per_cat_rate = 0;
    out->burst = 100;
    out->sampling_rate = 1.0;
    out->escape_injection = 1;
    out->max_log_length = HPLOGC_DEFAULT_MAX_LOG_LEN;
    snprintf(out->trunc_marker, sizeof(out->trunc_marker), "...[TRUNCATED]");
    out->fork_behavior = HPLOGC_FORK_REINIT;
    out->signal_safe = 0;
    out->crash_safety = HPLOGC_CRASH_SHUTDOWN;
    out->stats_interval = 0;
    out->stats_output = HPLOGC_STATS_OUT_STDERR;
    out->stats_file[0] = '\0';
}

/* ============================ 值解析辅助 ============================ */

/** @brief 字符串转无符号整数（十进制）。 */
static unsigned hp_to_uint(const char* v, unsigned def)
{
    if (v == NULL || v[0] == '\0') {
        return def;
    }
    return (unsigned)strtoul(v, NULL, 10);
}

/** @brief 字符串转 double。 */
static double hp_to_double(const char* v, double def)
{
    if (v == NULL || v[0] == '\0') {
        return def;
    }
    return strtod(v, NULL);
}

/** @brief 解析换行符枚举（auto/lf/crlf）。 */
static int hp_parse_newline(const char* v, int def)
{
    if (hp_ieq_str(v, "auto") == 0) {
        return HPLOGC_NEWLINE_AUTO;
    }
    if (hp_ieq_str(v, "lf") == 0) {
        return HPLOGC_NEWLINE_LF;
    }
    if (hp_ieq_str(v, "crlf") == 0) {
        return HPLOGC_NEWLINE_CRLF;
    }
    return def;
}

/** @brief 解析 id format（decimal/hex/none）。 */
static int hp_parse_idf(const char* v, int def)
{
    if (hp_ieq_str(v, "decimal") == 0) {
        return HPLOGC_IDF_DECIMAL;
    }
    if (hp_ieq_str(v, "hex") == 0) {
        return HPLOGC_IDF_HEX;
    }
    if (hp_ieq_str(v, "none") == 0) {
        return HPLOGC_IDF_NONE;
    }
    return def;
}

/** @brief 解析崩溃安全策略（none/periodic/entry/shutdown）。 */
static int hp_parse_crash(const char* v, int def)
{
    if (hp_ieq_str(v, "none") == 0) {
        return HPLOGC_CRASH_NONE;
    }
    if (hp_ieq_str(v, "periodic") == 0) {
        return HPLOGC_CRASH_PERIODIC;
    }
    if (hp_ieq_str(v, "entry") == 0) {
        return HPLOGC_CRASH_ENTRY;
    }
    if (hp_ieq_str(v, "shutdown") == 0) {
        return HPLOGC_CRASH_SHUTDOWN;
    }
    return def;
}

/** @brief 解析 fork 行为（reinit/disable/inherit）。 */
static int hp_parse_fork(const char* v, int def)
{
    if (hp_ieq_str(v, "reinit") == 0) {
        return HPLOGC_FORK_REINIT;
    }
    if (hp_ieq_str(v, "disable") == 0) {
        return HPLOGC_FORK_DISABLE;
    }
    if (hp_ieq_str(v, "inherit") == 0) {
        return HPLOGC_FORK_INHERIT;
    }
    return def;
}

/** @brief 解析溢出策略（discard/overwrite/wait）。 */
static int hp_parse_overflow(const char* v, int def)
{
    if (hp_ieq_str(v, "discard") == 0) {
        return HPLOGC_OVERFLOW_DISCARD;
    }
    if (hp_ieq_str(v, "overwrite") == 0) {
        return HPLOGC_OVERFLOW_OVERWRITE;
    }
    if (hp_ieq_str(v, "wait") == 0) {
        return HPLOGC_OVERFLOW_WAIT;
    }
    return def;
}

/** @brief 把逗号分隔的实例名列表追加到 default_sinks。 */
static int hp_parse_sink_list(hp_config_t* c, const char* v)
{
    const char* p = v;
    c->default_sink_count = 0;
    if (v == NULL || v[0] == '\0') {
        return 0;
    }
    for (;;) {
        const char* comma = strchr(p, ',');
        size_t n;
        char tmp[HPLOGC_MAX_NAME_LEN];
        if (comma != NULL) {
            n = (size_t)(comma - p);
        } else {
            n = strlen(p);
        }
        while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t')) {
            n--;
        }
        if (n >= sizeof(tmp)) {
            n = sizeof(tmp) - 1;
        }
        memcpy(tmp, p, n);
        tmp[n] = '\0';
        if (c->default_sink_count < HPLOGC_MAX_SINKS) {
            snprintf(c->default_sinks[c->default_sink_count], HPLOGC_MAX_NAME_LEN,
                     "%s", tmp);
            c->default_sink_count++;
        }
        if (comma == NULL) {
            break;
        }
        p = comma + 1;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
    }
    return 0;
}

/* ============================ 单个键值应用 ============================ */

/**
 * @brief 应用一个键值到对应节。
 *
 * @return 0 成功；1 未知键（调用方据 strict 决定成败）；-1 值非法。
 */
static int hp_apply_kv(hp_config_t* c, int section, const char* key,
                       const char* val)
{
    if (key == NULL || val == NULL) {
        return -1;
    }
    switch (section) {
    case SEC_GLOBAL:
        if (strcmp(key, "strict init") == 0) {
            c->strict_init = hp_parse_bool_str(val);
            if (c->strict_init < 0) {
                return -1;
            }
            return 0;
        }
        if (strcmp(key, "level") == 0) {
            hplogc_level_t lv;
            if (hplogc_level_parse(val, &lv) != HPLOGC_OK) {
                return -1;
            }
            c->level = (int)lv;
            return 0;
        }
        if (strcmp(key, "default format") == 0) {
            snprintf(c->default_format, sizeof(c->default_format), "%s", val);
            return 0;
        }
        if (strcmp(key, "default outputs") == 0
            || strcmp(key, "default sinks") == 0) {
            hp_parse_sink_list(c, val);
            return 0;
        }
        if (strcmp(key, "timezone") == 0) {
            if (hp_ieq_str(val, "utc") == 0) {
                c->utc = 1;
            } else if (hp_ieq_str(val, "local") == 0) {
                c->utc = 0;
            } else {
                return -1;
            }
            return 0;
        }
        if (strcmp(key, "timestamp source") == 0) {
            if (hp_ieq_str(val, "monotonic") == 0) {
                c->ts_monotonic = 1;
            } else if (hp_ieq_str(val, "realtime") == 0) {
                c->ts_monotonic = 0;
            } else {
                return -1;
            }
            return 0;
        }
        if (strcmp(key, "time format") == 0) {
            snprintf(c->time_format, sizeof(c->time_format), "%s", val);
            return 0;
        }
        if (strcmp(key, "encoding") == 0) {
            return 0; /* 仅 utf-8，静默忽略（已知键） */
        }
        if (strcmp(key, "capture source loc") == 0) {
            c->capture_source_loc = hp_parse_bool_str(val);
            if (c->capture_source_loc < 0) {
                return -1;
            }
            return 0;
        }
        if (strcmp(key, "newline") == 0) {
            c->newline = hp_parse_newline(val, c->newline);
            return 0;
        }
        if (strcmp(key, "pid format") == 0) {
            c->pid_fmt = hp_parse_idf(val, c->pid_fmt);
            return 0;
        }
        if (strcmp(key, "tid format") == 0) {
            c->tid_fmt = hp_parse_idf(val, c->tid_fmt);
            return 0;
        }
        if (strcmp(key, "hot reload interval") == 0) {
            c->hot_reload_interval = hp_to_uint(val, 0);
            return 0;
        }
        if (strcmp(key, "signal reload") == 0) {
            c->signal_reload = hp_parse_bool_str(val);
            if (c->signal_reload < 0) {
                return -1;
            }
            return 0;
        }
        return 1; /* 未知键 */

    case SEC_BUFFER:
        if (strcmp(key, "buffer size") == 0) {
            unsigned long long sz = 0;
            if (hp_parse_size_str(val, &sz) != 0) {
                return -1;
            }
            if (sz < HPLOGC_MIN_BUFFER_SIZE) {
                sz = HPLOGC_MIN_BUFFER_SIZE; /* 裁剪 + 警告（§10.4） */
            }
            if (sz > HPLOGC_MAX_BUFFER_SIZE) {
                sz = HPLOGC_MAX_BUFFER_SIZE;
            }
            c->buffer_size = (size_t)sz;
            return 0;
        }
        if (strcmp(key, "overflow policy") == 0) {
            c->overflow_policy = hp_parse_overflow(val, c->overflow_policy);
            return 0;
        }
        return 1;

    case SEC_ASYNC:
        if (strcmp(key, "batch size") == 0) {
            unsigned b = hp_to_uint(val, c->batch_size);
            if (b < 1) {
                b = 1;
            }
            if (b > 65535) {
                b = 65535;
            }
            c->batch_size = b;
            return 0;
        }
        if (strcmp(key, "flush interval") == 0) {
            unsigned f = hp_to_uint(val, c->flush_interval_ms);
            if (f < 1) {
                f = 1;
            }
            if (f > 60000) {
                f = 60000;
            }
            c->flush_interval_ms = f;
            return 0;
        }
        if (strcmp(key, "shutdown timeout") == 0) {
            c->shutdown_timeout_ms = hp_to_uint(val, c->shutdown_timeout_ms);
            return 0;
        }
        return 1;

    case SEC_THROTTLE:
        if (strcmp(key, "global rate limit") == 0) {
            c->global_rate = hp_to_uint(val, 0);
            return 0;
        }
        if (strcmp(key, "per category rate limit") == 0) {
            c->per_cat_rate = hp_to_uint(val, 0);
            return 0;
        }
        if (strcmp(key, "sampling rate") == 0) {
            double s = hp_to_double(val, 1.0);
            if (s < 0.0) {
                s = 0.0;
            }
            if (s > 1.0) {
                s = 1.0;
            }
            c->sampling_rate = s;
            return 0;
        }
        if (strcmp(key, "burst size") == 0) {
            c->burst = hp_to_uint(val, c->burst);
            return 0;
        }
        return 1;

    case SEC_ADVANCED:
        if (strcmp(key, "escape injection") == 0) {
            c->escape_injection = hp_parse_bool_str(val);
            if (c->escape_injection < 0) {
                return -1;
            }
            return 0;
        }
        if (strcmp(key, "max log length") == 0) {
            unsigned long long m = hp_to_uint(val, 0);
            if (m < HPLOGC_MIN_MAX_LOG_LEN) {
                m = HPLOGC_MIN_MAX_LOG_LEN;
            }
            if (m > HPLOGC_MAX_MAX_LOG_LEN) {
                m = HPLOGC_MAX_MAX_LOG_LEN;
            }
            c->max_log_length = (size_t)m;
            return 0;
        }
        if (strcmp(key, "truncation marker") == 0) {
            snprintf(c->trunc_marker, sizeof(c->trunc_marker), "%s", val);
            return 0;
        }
        if (strcmp(key, "fork behavior") == 0) {
            c->fork_behavior = hp_parse_fork(val, c->fork_behavior);
            return 0;
        }
        if (strcmp(key, "signal safe") == 0) {
            c->signal_safe = hp_parse_bool_str(val);
            if (c->signal_safe < 0) {
                return -1;
            }
            return 0;
        }
        if (strcmp(key, "crash safety") == 0) {
            c->crash_safety = hp_parse_crash(val, c->crash_safety);
            return 0;
        }
        if (strcmp(key, "stats interval") == 0) {
            c->stats_interval = hp_to_uint(val, 0);
            return 0;
        }
        if (strcmp(key, "stats output") == 0) {
            if (hp_ieq_str(val, "file") == 0) {
                c->stats_output = HPLOGC_STATS_OUT_FILE;
            } else if (hp_ieq_str(val, "stderr") == 0) {
                c->stats_output = HPLOGC_STATS_OUT_STDERR;
            } else {
                return -1;
            }
            return 0;
        }
        if (strcmp(key, "stats file") == 0) {
            snprintf(c->stats_file, sizeof(c->stats_file), "%s", val);
            return 0;
        }
        return 1;

    default:
        return 1;
    }
}

/* ============================ INI 装配 ============================ */

/** @brief 按节聚合应用 INI 条目。 */
int hp_conf_build(const hp_ini_t* ini, const char* path, hp_config_t* out)
{
    size_t i;
    int    rc = HPLOGC_OK;
    int    found_fmt[HPLOGC_MAX_FORMATS];
    int    found_out[HPLOGC_MAX_SINKS];

    (void)path;
    if (ini == NULL || out == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    hp_conf_default(out);
    memset(found_fmt, 0, sizeof(found_fmt));
    memset(found_out, 0, sizeof(found_out));

    for (i = 0; i < ini->count; i++) {
        const hp_ini_item_t* it = &ini->items[i];
        const char* k = it->key;
        const char* v = it->val;

        switch (it->section) {
        case SEC_FORMATS: {
            int dup = 0;
            size_t j;
            if (out->format_count >= HPLOGC_MAX_FORMATS) {
                hp_err_printf("hplogc: [formats] 条目超过上限 %d",
                              HPLOGC_MAX_FORMATS);
                return HPLOGC_ERR_CONFIG;
            }
            for (j = 0; j < out->format_count; j++) {
                if (strcmp(out->formats[j].name, k) == 0) {
                    dup = 1;
                    break;
                }
            }
            if (dup) {
                hp_err_printf("%s:%d: [formats] 重复定义 '%s'", path, it->line,
                              k);
                return HPLOGC_ERR_CONFIG;
            }
            snprintf(out->formats[out->format_count].name, HPLOGC_MAX_NAME_LEN,
                     "%s", k);
            snprintf(out->formats[out->format_count].tmpl, HPLOGC_MAX_FMT_LEN,
                     "%s", v);
            out->format_count++;
            break;
        }
        case SEC_OUTPUTS: {
            int dup = 0;
            size_t j;
            const char* comma;
            char type[HPLOGC_MAX_NAME_LEN];
            hp_sink_def_t* def;
            if (out->sink_count >= HPLOGC_MAX_SINKS) {
                hp_err_printf("hplogc: [outputs] 实例超过上限 %d",
                              HPLOGC_MAX_SINKS);
                return HPLOGC_ERR_CONFIG;
            }
            for (j = 0; j < out->sink_count; j++) {
                if (strcmp(out->sinks[j].name, k) == 0) {
                    dup = 1;
                    break;
                }
            }
            if (dup) {
                hp_err_printf("%s:%d: [outputs] 重复定义 '%s'", path, it->line,
                              k);
                return HPLOGC_ERR_CONFIG;
            }
            def = &out->sinks[out->sink_count];
            memset(def, 0, sizeof(*def));
            snprintf(def->name, HPLOGC_MAX_NAME_LEN, "%s", k);
            def->enabled = 1;
            def->async = -1;
            /* 首个逗号前为 type（或 type=...） */
            comma = strchr(v, ',');
            {
                size_t tn;
                const char* tstart = v;
                if (comma != NULL) {
                    tn = (size_t)(comma - v);
                } else {
                    tn = strlen(v);
                }
                while (tn > 0 && (v[tn - 1] == ' ' || v[tn - 1] == '\t')) {
                    tn--;
                }
                if (tn >= sizeof(type)) {
                    tn = sizeof(type) - 1;
                }
                memcpy(type, tstart, tn);
                type[tn] = '\0';
            }
            {
                char* teq = strchr(type, '=');
                if (teq != NULL) {
                    *teq = '\0';
                    if (strcmp(type, "type") != 0) {
                        hp_err_printf("%s:%d: [outputs] 非法的类型字段", path,
                                      it->line);
                        return HPLOGC_ERR_CONFIG;
                    }
                    snprintf(def->type, HPLOGC_MAX_NAME_LEN, "%s", teq + 1);
                } else {
                    snprintf(def->type, HPLOGC_MAX_NAME_LEN, "%s", type);
                }
            }
            /* 其余逗号分隔的 key=value */
            if (comma != NULL) {
                const char* p = comma + 1;
                while (*p != '\0') {
                    const char* c2 = strchr(p, ',');
                    size_t seglen;
                    char seg[HPLOGC_MAX_FMT_LEN];
                    if (c2 != NULL) {
                        seglen = (size_t)(c2 - p);
                    } else {
                        seglen = strlen(p);
                    }
                    while (seglen > 0 && (p[seglen - 1] == ' '
                                          || p[seglen - 1] == '\t')) {
                        seglen--;
                    }
                    if (seglen >= sizeof(seg)) {
                        seglen = sizeof(seg) - 1;
                    }
                    memcpy(seg, p, seglen);
                    seg[seglen] = '\0';
                    {
                        char* eq = strchr(seg, '=');
                        char kbuf[HPLOGC_MAX_PATH_LEN];
                        char* vp;
                        if (eq == NULL) {
                            hp_err_printf("%s:%d: [outputs] '%s' 缺少 '='",
                                          path, it->line, seg);
                            return HPLOGC_ERR_CONFIG;
                        }
                        *eq = '\0';
                        snprintf(kbuf, sizeof(kbuf), "%s", seg);
                        vp = eq + 1;
                        if (strcmp(kbuf, "enabled") == 0) {
                            def->enabled = hp_parse_bool_str(vp);
                            if (def->enabled < 0) {
                                return HPLOGC_ERR_CONFIG;
                            }
                        } else if (strcmp(kbuf, "async") == 0) {
                            def->async = hp_parse_bool_str(vp);
                            if (def->async < 0) {
                                return HPLOGC_ERR_CONFIG;
                            }
                        } else {
                            if (def->opt_count >= HPLOGC_MAX_OPTIONS) {
                                hp_err_printf(
                                    "%s:%d: sink '%s' 私有参数超过上限 %d", path,
                                    it->line, def->name, HPLOGC_MAX_OPTIONS);
                                return HPLOGC_ERR_CONFIG;
                            }
                            snprintf(def->opts[def->opt_count].key,
                                     HPLOGC_KV_KEY_LEN, "%s", kbuf);
                            snprintf(def->opts[def->opt_count].val,
                                     HPLOGC_MAX_FMT_LEN, "%s", vp);
                            def->opt_count++;
                        }
                    }
                    if (c2 == NULL) {
                        break;
                    }
                    p = c2 + 1;
                }
            }
            out->sink_count++;
            break;
        }
        case SEC_RULES: {
            char sel[HPLOGC_MAX_NAME_LEN];
            char* dot;
            char fmt[HPLOGC_MAX_NAME_LEN];
            hp_rule_def_t* r;
            if (out->rule_count >= HPLOGC_MAX_RULES) {
                hp_err_printf("hplogc: [rules] 超过上限 %d", HPLOGC_MAX_RULES);
                return HPLOGC_ERR_CONFIG;
            }
            /* selector：最后一个 '.' 分割 category 与 level */
            snprintf(sel, sizeof(sel), "%s", k);
            dot = strrchr(sel, '.');
            r = &out->rules[out->rule_count];
            memset(r, 0, sizeof(*r));
            if (dot == NULL) {
                snprintf(r->category, sizeof(r->category), "%s", sel);
            } else {
                *dot = '\0';
                snprintf(r->category, sizeof(r->category), "%s", sel);
                {
                    const char* lv = dot + 1;
                    if (strcmp(lv, "*") == 0) {
                        r->min_level = 0;
                        r->max_level = (int)HPLOGC_LEVEL_OFF;
                    } else {
                        const char* tilde = strchr(lv, '~');
                        if (tilde == NULL) {
                            hplogc_level_t x;
                            if (hplogc_level_parse(lv, &x) != HPLOGC_OK) {
                                hp_err_printf("%s:%d: [rules] 非法级别 '%s'",
                                              path, it->line, lv);
                                return HPLOGC_ERR_CONFIG;
                            }
                            r->min_level = (int)x;
                            r->max_level = (int)x;
                        } else {
                            char a[32], b[32];
                            hplogc_level_t la, lb;
                            size_t al = (size_t)(tilde - lv);
                            if (al >= sizeof(a)) {
                                al = sizeof(a) - 1;
                            }
                            memcpy(a, lv, al);
                            a[al] = '\0';
                            snprintf(b, sizeof(b), "%s", tilde + 1);
                            if (hplogc_level_parse(a, &la) != HPLOGC_OK
                                || hplogc_level_parse(b, &lb) != HPLOGC_OK) {
                                hp_err_printf("%s:%d: [rules] 非法级别区间",
                                              path, it->line);
                                return HPLOGC_ERR_CONFIG;
                            }
                            r->min_level = (int)la;
                            r->max_level = (int)lb;
                        }
                    }
                }
            }
            if (r->min_level > r->max_level) {
                hp_err_printf("%s:%d: [rules] min_level > max_level", path,
                              it->line);
                return HPLOGC_ERR_CONFIG;
            }
            /* value：格式为 "format_name -> sink1, sink2, ..."（§10.2） */
            {
                const char* arrow = strstr(v, "->");
                size_t fn;
                const char* p;
                if (arrow == NULL) {
                    hp_err_printf("%s:%d: [rules] 缺少 '->' 分隔符", path,
                                  it->line);
                    return HPLOGC_ERR_CONFIG;
                }
                fn = (size_t)(arrow - v);
                while (fn > 0 && (v[fn - 1] == ' ' || v[fn - 1] == '\t')) {
                    fn--;
                }
                if (fn >= sizeof(fmt)) {
                    fn = sizeof(fmt) - 1;
                }
                memcpy(fmt, v, fn);
                fmt[fn] = '\0';
                snprintf(r->format, sizeof(r->format), "%s", fmt);

                p = arrow + 2;
                while (*p != '\0' && r->sink_count < HPLOGC_MAX_SINKS) {
                    const char* c2;
                    size_t snl;
                    char nm[HPLOGC_MAX_NAME_LEN];
                    while (*p == ' ' || *p == '\t') {
                        p++;
                    }
                    if (*p == '\0') {
                        break;
                    }
                    c2 = strchr(p, ',');
                    if (c2 != NULL) {
                        snl = (size_t)(c2 - p);
                    } else {
                        snl = strlen(p);
                    }
                    while (snl > 0 && (p[snl - 1] == ' '
                                       || p[snl - 1] == '\t')) {
                        snl--;
                    }
                    if (snl > 0) {
                        if (snl >= sizeof(nm)) {
                            snl = sizeof(nm) - 1;
                        }
                        memcpy(nm, p, snl);
                        nm[snl] = '\0';
                        snprintf(r->sinks[r->sink_count], HPLOGC_MAX_NAME_LEN,
                                 "%s", nm);
                        r->sink_count++;
                    }
                    if (c2 == NULL) {
                        break;
                    }
                    p = c2 + 1;
                }
            }
            out->rule_count++;
            break;
        }
        case SEC_BUILD:
            break; /* 只读展示，忽略 */
        case SEC_GLOBAL:
        case SEC_BUFFER:
        case SEC_ADVANCED:
#ifndef HPLOGC_HAS_ASYNC
        case SEC_ASYNC:
#endif
#ifndef HPLOGC_HAS_THROTTLE
        case SEC_THROTTLE:
#endif
        {
            int r = hp_apply_kv(out, it->section, k, v);
            if (r == -1) {
                hp_err_printf("%s:%d: 配置值非法 '%s = %s'", path, it->line, k,
                              v);
                return HPLOGC_ERR_CONFIG;
            }
            if (r == 1) {
                /* 未知键：strict 模式失败，否则告警忽略 */
                if (out->strict_init) {
                    hp_err_printf(
                        "%s:%d: 未知配置键 '%s'（strict init = true）", path,
                        it->line, k);
                    return HPLOGC_ERR_CONFIG;
                }
                hp_warn_throttled("hplogc: 忽略未知键 '%s'（%s:%d）", k, path,
                                  it->line);
            }
            break;
        }
        default:
            break;
        }
    }

    /* 引用校验 */
    {
        size_t i2;
        int df_ok = 0;
        int bi;
        for (bi = 0; bi < hp_builtin_format_count(); bi++) {
            if (strcmp(out->default_format, hp_builtin_format_name(bi)) == 0) {
                df_ok = 1;
                break;
            }
        }
        if (!df_ok) {
            for (i2 = 0; i2 < out->format_count; i2++) {
                if (strcmp(out->default_format, out->formats[i2].name) == 0) {
                    df_ok = 1;
                    break;
                }
            }
        }
        if (!df_ok) {
            hp_err_printf("hplogc: default format '%s' 未定义",
                          out->default_format);
            return HPLOGC_ERR_CONFIG;
        }
        for (i2 = 0; i2 < out->default_sink_count; i2++) {
            int found = 0;
            size_t j;
            for (j = 0; j < out->sink_count; j++) {
                if (strcmp(out->default_sinks[i2], out->sinks[j].name) == 0) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                hp_err_printf("hplogc: default sink '%s' 未定义",
                              out->default_sinks[i2]);
                return HPLOGC_ERR_CONFIG;
            }
        }
        for (i2 = 0; i2 < out->rule_count; i2++) {
            int fok = 0;
            int bi2;
            size_t j;
            for (bi2 = 0; bi2 < hp_builtin_format_count(); bi2++) {
                if (strcmp(out->rules[i2].format,
                           hp_builtin_format_name(bi2)) == 0) {
                    fok = 1;
                    break;
                }
            }
            if (!fok) {
                for (j = 0; j < out->format_count; j++) {
                    if (strcmp(out->rules[i2].format,
                               out->formats[j].name) == 0) {
                        fok = 1;
                        break;
                    }
                }
            }
            if (!fok) {
                hp_err_printf("hplogc: rule 引用未定义 format '%s'",
                              out->rules[i2].format);
                return HPLOGC_ERR_CONFIG;
            }
            for (j = 0; j < out->rules[i2].sink_count; j++) {
                int found = 0;
                size_t k;
                for (k = 0; k < out->sink_count; k++) {
                    if (strcmp(out->rules[i2].sinks[j], out->sinks[k].name)
                        == 0) {
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    hp_err_printf("hplogc: rule 引用未定义 sink '%s'",
                                  out->rules[i2].sinks[j]);
                    return HPLOGC_ERR_CONFIG;
                }
            }
        }
        if (out->stats_output == HPLOGC_STATS_OUT_FILE
            && out->stats_file[0] == '\0') {
            hp_err_printf(
                "hplogc: stats output = file 但缺少 stats file（§18-C2）");
            return HPLOGC_ERR_CONFIG;
        }
    }

    (void)rc;
    return HPLOGC_OK;
}

/* ============================ 代码内配置装配 ============================ */

/** @brief 应用一条全局键值（依据键名判定所属节）。 */
static int hp_apply_global_option(hp_config_t* c, const char* key,
                                  const char* val)
{
    if (strcmp(key, "buffer size") == 0 || strcmp(key, "overflow policy") == 0) {
        return hp_apply_kv(c, SEC_BUFFER, key, val);
    }
    if (strcmp(key, "batch size") == 0 || strcmp(key, "flush interval") == 0
        || strcmp(key, "shutdown timeout") == 0) {
        return hp_apply_kv(c, SEC_ASYNC, key, val);
    }
    if (strcmp(key, "global rate limit") == 0
        || strcmp(key, "per category rate limit") == 0
        || strcmp(key, "sampling rate") == 0
        || strcmp(key, "burst size") == 0) {
        return hp_apply_kv(c, SEC_THROTTLE, key, val);
    }
    if (strcmp(key, "escape injection") == 0
        || strcmp(key, "max log length") == 0
        || strcmp(key, "truncation marker") == 0
        || strcmp(key, "fork behavior") == 0
        || strcmp(key, "signal safe") == 0 || strcmp(key, "crash safety") == 0
        || strcmp(key, "stats interval") == 0
        || strcmp(key, "stats output") == 0 || strcmp(key, "stats file") == 0) {
        return hp_apply_kv(c, SEC_ADVANCED, key, val);
    }
    /* 其余全部归入 global */
    return hp_apply_kv(c, SEC_GLOBAL, key, val);
}

int hp_conf_from_code(const hplogc_config_t* cfg, hp_config_t* out)
{
    size_t i;

    if (cfg == NULL || out == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    if (cfg->struct_size != (uint32_t)sizeof(hplogc_config_t)) {
        hp_err_printf(
            "hplogc: config struct_size=%u 与库内 %lu 不一致（ABI 自检）",
            cfg->struct_size, (unsigned long)sizeof(hplogc_config_t));
        return HPLOGC_ERR_CONFIG;
    }
    hp_conf_default(out);

    /* 直接字段 */
    if ((int)cfg->level < 0 || (int)cfg->level > (int)HPLOGC_LEVEL_OFF) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    out->level = (int)cfg->level;
    if (cfg->default_format != NULL) {
        int builtin = 0;
        int bi;
        for (bi = 0; bi < hp_builtin_format_count(); bi++) {
            if (strcmp(cfg->default_format, hp_builtin_format_name(bi)) == 0) {
                builtin = 1;
                break;
            }
        }
        if (!builtin) {
            return HPLOGC_ERR_INVALID_ARG; /* 代码路径 default format 仅限内置 */
        }
        snprintf(out->default_format, sizeof(out->default_format), "%s",
                 cfg->default_format);
    }
    if (cfg->default_sinks != NULL) {
        if (cfg->default_sink_count > HPLOGC_MAX_SINKS) {
            return HPLOGC_ERR_INVALID_ARG;
        }
        for (i = 0; i < cfg->default_sink_count; i++) {
            if (cfg->default_sinks[i] == NULL) {
                return HPLOGC_ERR_INVALID_ARG;
            }
            snprintf(out->default_sinks[i], HPLOGC_MAX_NAME_LEN, "%s",
                     cfg->default_sinks[i]);
        }
        out->default_sink_count = cfg->default_sink_count;
    }
    if (cfg->sinks != NULL) {
        if (cfg->sink_count > HPLOGC_MAX_SINKS) {
            return HPLOGC_ERR_INVALID_ARG;
        }
        for (i = 0; i < cfg->sink_count; i++) {
            const hplogc_sink_config_t* sc = &cfg->sinks[i];
            hp_sink_def_t* def = &out->sinks[out->sink_count];
            size_t j;
            if (sc->name == NULL || sc->type == NULL) {
                return HPLOGC_ERR_INVALID_ARG;
            }
            /* 同名检查 */
            for (j = 0; j < out->sink_count; j++) {
                if (strcmp(out->sinks[j].name, sc->name) == 0) {
                    return HPLOGC_ERR_INVALID_ARG;
                }
            }
            memset(def, 0, sizeof(*def));
            snprintf(def->name, HPLOGC_MAX_NAME_LEN, "%s", sc->name);
            snprintf(def->type, HPLOGC_MAX_NAME_LEN, "%s", sc->type);
            def->enabled = (sc->enabled != 0) ? 1 : 0;
            def->async = sc->async;
            if (sc->options != NULL) {
                if (sc->option_count > HPLOGC_MAX_OPTIONS) {
                    return HPLOGC_ERR_INVALID_ARG;
                }
                for (j = 0; j < sc->option_count; j++) {
                    if (sc->options[j].key == NULL
                        || sc->options[j].value == NULL) {
                        return HPLOGC_ERR_INVALID_ARG;
                    }
                    snprintf(def->opts[j].key, HPLOGC_KV_KEY_LEN, "%s",
                             sc->options[j].key);
                    snprintf(def->opts[j].val, HPLOGC_MAX_FMT_LEN, "%s",
                             sc->options[j].value);
                }
                def->opt_count = sc->option_count;
            }
            out->sink_count++;
        }
    }
    if (cfg->rules != NULL) {
        if (cfg->rule_count > HPLOGC_MAX_RULES) {
            return HPLOGC_ERR_INVALID_ARG;
        }
        for (i = 0; i < cfg->rule_count; i++) {
            const hplogc_rule_t* rl = &cfg->rules[i];
            hp_rule_def_t* r = &out->rules[out->rule_count];
            size_t j;
            int builtin = 0;
            int bi;
            if (rl->category == NULL || rl->format == NULL
                || rl->sinks == NULL) {
                return HPLOGC_ERR_INVALID_ARG;
            }
            for (bi = 0; bi < hp_builtin_format_count(); bi++) {
                if (strcmp(rl->format, hp_builtin_format_name(bi)) == 0) {
                    builtin = 1;
                    break;
                }
            }
            if (!builtin) {
                return HPLOGC_ERR_INVALID_ARG; /* 代码路径 format 仅限内置 */
            }
            memset(r, 0, sizeof(*r));
            snprintf(r->category, sizeof(r->category), "%s", rl->category);
            r->min_level = 0;
            r->max_level = (int)HPLOGC_LEVEL_OFF;
            snprintf(r->format, sizeof(r->format), "%s", rl->format);
            if (rl->sink_count > HPLOGC_MAX_SINKS) {
                return HPLOGC_ERR_INVALID_ARG;
            }
            for (j = 0; j < rl->sink_count; j++) {
                if (rl->sinks[j] == NULL) {
                    return HPLOGC_ERR_INVALID_ARG;
                }
                snprintf(r->sinks[j], HPLOGC_MAX_NAME_LEN, "%s", rl->sinks[j]);
            }
            r->sink_count = rl->sink_count;
            out->rule_count++;
        }
    }
    if (cfg->buffer_size != 0) {
        if (cfg->buffer_size < HPLOGC_MIN_BUFFER_SIZE) {
            return HPLOGC_ERR_INVALID_ARG;
        }
        out->buffer_size = cfg->buffer_size;
    }
    out->overflow_policy = (int)cfg->overflow_policy;
    if (cfg->batch_size != 0) {
        out->batch_size = cfg->batch_size;
    }
    if (cfg->flush_interval_ms != 0) {
        out->flush_interval_ms = cfg->flush_interval_ms;
    }
    if (cfg->shutdown_timeout_ms != 0) {
        out->shutdown_timeout_ms = cfg->shutdown_timeout_ms;
    }
    if (cfg->escape_injection != 0) {
        out->escape_injection = cfg->escape_injection;
    }
    if (cfg->max_log_length != 0) {
        if (cfg->max_log_length < HPLOGC_MIN_MAX_LOG_LEN
            || cfg->max_log_length > HPLOGC_MAX_MAX_LOG_LEN) {
            return HPLOGC_ERR_INVALID_ARG;
        }
        out->max_log_length = cfg->max_log_length;
    }
    if (cfg->truncation_marker != NULL) {
        snprintf(out->trunc_marker, sizeof(out->trunc_marker), "%s",
                 cfg->truncation_marker);
    }
    out->crash_safety = (int)cfg->crash_safety;
    out->signal_safe = (cfg->signal_safe != 0) ? 1 : 0;

    /* 全局键值（时区 / 时间格式 / 换行 / pid / tid / throttle / stats 等） */
    if (cfg->global_options != NULL) {
        for (i = 0; i < cfg->global_option_count; i++) {
            const hplogc_kv_t* kv = &cfg->global_options[i];
            int r;
            if (kv->key == NULL || kv->value == NULL) {
                return HPLOGC_ERR_INVALID_ARG;
            }
            r = hp_apply_global_option(out, kv->key, kv->value);
            if (r == -1) {
                return HPLOGC_ERR_INVALID_ARG;
            }
            if (r == 1) {
                hp_warn_throttled("hplogc: 忽略未知全局键 '%s'", kv->key);
            }
        }
    }

    /* 引用校验（与文件路径一致） */
    {
        size_t i2;
        int df_ok = 0;
        int bi;
        for (bi = 0; bi < hp_builtin_format_count(); bi++) {
            if (strcmp(out->default_format, hp_builtin_format_name(bi)) == 0) {
                df_ok = 1;
                break;
            }
        }
        if (!df_ok) {
            return HPLOGC_ERR_CONFIG;
        }
        for (i2 = 0; i2 < out->default_sink_count; i2++) {
            int found = 0;
            size_t j;
            for (j = 0; j < out->sink_count; j++) {
                if (strcmp(out->default_sinks[i2], out->sinks[j].name) == 0) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                return HPLOGC_ERR_CONFIG;
            }
        }
        for (i2 = 0; i2 < out->rule_count; i2++) {
            int fok = 0;
            int bi2;
            size_t j;
            for (bi2 = 0; bi2 < hp_builtin_format_count(); bi2++) {
                if (strcmp(out->rules[i2].format,
                           hp_builtin_format_name(bi2)) == 0) {
                    fok = 1;
                    break;
                }
            }
            if (!fok) {
                return HPLOGC_ERR_CONFIG;
            }
            for (j = 0; j < out->rules[i2].sink_count; j++) {
                int found = 0;
                size_t k;
                for (k = 0; k < out->sink_count; k++) {
                    if (strcmp(out->rules[i2].sinks[j], out->sinks[k].name)
                        == 0) {
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    return HPLOGC_ERR_CONFIG;
                }
            }
        }
        if (out->stats_output == HPLOGC_STATS_OUT_FILE
            && out->stats_file[0] == '\0') {
            return HPLOGC_ERR_CONFIG;
        }
    }
    return HPLOGC_OK;
}
