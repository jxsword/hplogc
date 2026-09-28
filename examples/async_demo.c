/* -*- coding: utf-8 -*- */
/**
 * @example async_demo.c
 * @brief 异步批量模式示例：生产者不阻塞，消费线程批量落地。
 */
#include <hplogc.h>

#include <stdio.h>
#include <string.h>

static const char* const g_default_sinks[] = {"file1"};

static hplogc_kv_t g_file_opts[] = {
    {"path", "async.log"},
    {"rotate", "none"},
};

static hplogc_sink_config_t g_sinks[] = {
    /* async=1 显式启用异步批量路径 */
    {"rollingfile", "file1", 1, 1, g_file_opts,
     sizeof(g_file_opts) / sizeof(g_file_opts[0])},
};

static hplogc_rule_t g_rules[] = {
    {"*", HPLOGC_LEVEL_TRACE, HPLOGC_LEVEL_OFF, "standard",
     g_default_sinks, 1},
};

int main(void)
{
    hplogc_config_t cfg;
    int i;

    hplogc_config_default(&cfg);
    cfg.sinks = g_sinks;
    cfg.sink_count = sizeof(g_sinks) / sizeof(g_sinks[0]);
    cfg.rules = g_rules;
    cfg.rule_count = sizeof(g_rules) / sizeof(g_rules[0]);
    cfg.default_sinks = g_default_sinks;
    cfg.default_sink_count = 1;
    cfg.buffer_size = 1024 * 1024;

    if (hplogc_init(&cfg) != HPLOGC_OK) {
        fprintf(stderr, "hplogc_init failed\n");
        return 1;
    }

    for (i = 0; i < 2000; i++) {
        HPLOGC_INFO("producer", "async event #%d payload=%d", i, i * 7);
    }

    (void)hplogc_flush(); /* 阻塞直到消费线程排空 */

    {
        hplogc_stats_t st;
        if (hplogc_get_stats(&st) == HPLOGC_OK) {
            printf("async demo done; accepted=%llu written=%llu dropped=%llu\n",
                   (unsigned long long)st.accepted,
                   (unsigned long long)st.written,
                   (unsigned long long)st.dropped);
        }
    }

    hplogc_shutdown();
    return 0;
}
