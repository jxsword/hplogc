/* -*- coding: utf-8 -*- */
/**
 * @file sink_syslog.c
 * @brief syslog sink（Phase 2，默认不参与编译；仅当 HPLOGC_SINK_HAS_SYSLOG=1）。
 *
 * 本文件整体受宏包裹：未启用时编译为空，避免在非 POSIX 平台（如 Windows）因
 * 缺 `<syslog.h>` 而编译失败。
 */

#include "hplogc_internal.h"

#ifdef HPLOGC_SINK_HAS_SYSLOG

#include <syslog.h>

#include <hplogc.h>

/** @brief syslog sink 私有数据。 */
typedef struct {
    int facility; /*!< LOG_USER 等 */
    int opened;   /*!< 是否已 openlog */
} hp_syslog_priv_t;

static int hp_syslog_configure(hplogc_sink_t* sink, const char* key,
                              const char* val)
{
    hp_syslog_priv_t* p = (hp_syslog_priv_t*)hplogc_sink_priv(sink);

    if (p == NULL) {
        return -1;
    }
    if (strcmp(key, "facility") == 0) {
        if (hp_ieq_str(val, "user") == 0) {
            p->facility = LOG_USER;
        } else if (hp_ieq_str(val, "daemon") == 0) {
            p->facility = LOG_DAEMON;
        } else if (hp_ieq_str(val, "local0") == 0) {
            p->facility = LOG_LOCAL0;
        } else if (hp_ieq_str(val, "local1") == 0) {
            p->facility = LOG_LOCAL1;
        } else if (hp_ieq_str(val, "local2") == 0) {
            p->facility = LOG_LOCAL2;
        } else if (hp_ieq_str(val, "local3") == 0) {
            p->facility = LOG_LOCAL3;
        } else if (hp_ieq_str(val, "local4") == 0) {
            p->facility = LOG_LOCAL4;
        } else if (hp_ieq_str(val, "local5") == 0) {
            p->facility = LOG_LOCAL5;
        } else if (hp_ieq_str(val, "local6") == 0) {
            p->facility = LOG_LOCAL6;
        } else if (hp_ieq_str(val, "local7") == 0) {
            p->facility = LOG_LOCAL7;
        } else {
            return -1;
        }
        return 0;
    }
    return -1;
}

static int hp_syslog_init(hplogc_sink_t* sink)
{
    hp_syslog_priv_t* p = (hp_syslog_priv_t*)hplogc_sink_priv(sink);
    if (p == NULL) {
        return -1;
    }
    if (p->facility == 0) {
        p->facility = LOG_USER; /* 默认 LOG_USER */
    }
    return 0;
}

static int hp_syslog_start(hplogc_sink_t* sink)
{
    hp_syslog_priv_t* p = (hp_syslog_priv_t*)hplogc_sink_priv(sink);
    if (p == NULL) {
        return -1;
    }
    openlog(NULL, LOG_PID, p->facility);
    p->opened = 1;
    return 0;
}

static int hp_level_to_syslog(hplogc_level_t level)
{
    switch (level) {
    case HPLOGC_LEVEL_TRACE:
        return LOG_DEBUG;
    case HPLOGC_LEVEL_DEBUG:
        return LOG_DEBUG;
    case HPLOGC_LEVEL_INFO:
        return LOG_INFO;
    case HPLOGC_LEVEL_WARN:
        return LOG_WARNING;
    case HPLOGC_LEVEL_ERROR:
        return LOG_ERR;
    case HPLOGC_LEVEL_FATAL:
        return LOG_CRIT;
    default:
        return LOG_INFO;
    }
}

static void hp_syslog_emit(hplogc_sink_t* sink, const hplogc_event_t* ev)
{
    int prio;
    (void)sink;
    if (ev == NULL || ev->formatted == NULL) {
        return;
    }
    prio = hp_level_to_syslog(ev->level);
    syslog(prio, "%.*s", (int)ev->formatted_len, ev->formatted);
}

static void hp_syslog_destroy(hplogc_sink_t* sink)
{
    hp_syslog_priv_t* p = (hp_syslog_priv_t*)hplogc_sink_priv(sink);
    if (p != NULL && p->opened) {
        closelog();
        p->opened = 0;
    }
}

const hplogc_sink_ops_t hp_sink_syslog_ops = {
    "syslog",
    HPLOGC_SINK_ABI_VERSION,
    HPLOGC_CAP_SYNC | HPLOGC_CAP_ASYNC,
    sizeof(hp_syslog_priv_t),
    hp_syslog_configure,
    hp_syslog_init,
    hp_syslog_start,
    hp_syslog_emit,
    NULL, /* emit_batch：核心退化为逐条 emit */
    NULL, /* flush：syslog 无缓冲语义 */
    hp_syslog_destroy,
    { NULL, NULL, NULL, NULL }
};

#endif /* HPLOGC_SINK_HAS_SYSLOG */
