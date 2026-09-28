/* -*- coding: utf-8 -*- */
/**
 * @example basic.c
 * @brief 最小可运行示例：代码内配置 + 文件落地 + 同步写出。
 *
 * 演示 v0.2 的代码内配置 API（hplogc_config_t / hplogc_sink_config_t /
 * hplogc_rule_t）。运行后会在当前目录生成 `example.log`。
 */
#include <hplogc.h>

#include <stdio.h>
#include <string.h>

static const char* const g_default_sinks[] = {"file1"};

static hplogc_kv_t g_file_opts[] = {
    {"path", "example.log"},
    {"rotate", "none"},
};

static hplogc_sink_config_t g_sinks[] = {
    {"rollingfile", "file1", 1, -1, g_file_opts,
     sizeof(g_file_opts) / sizeof(g_file_opts[0])},
};

static hplogc_rule_t g_rules[] = {
    {"*", HPLOGC_LEVEL_TRACE, HPLOGC_LEVEL_OFF, "standard",
     g_default_sinks, 1},
};

int main(void)
{
    hplogc_config_t cfg;
    int rc;

    hplogc_config_default(&cfg);
    cfg.sinks = g_sinks;
    cfg.sink_count = sizeof(g_sinks) / sizeof(g_sinks[0]);
    cfg.rules = g_rules;
    cfg.rule_count = sizeof(g_rules) / sizeof(g_rules[0]);
    cfg.default_sinks = g_default_sinks;
    cfg.default_sink_count = 1;

    rc = hplogc_init(&cfg);
    if (rc != HPLOGC_OK) {
        fprintf(stderr, "hplogc_init failed: %d\n", rc);
        return 1;
    }

    HPLOGC_INFO("app", "hello %s", "world");
    HPLOGC_WARN("app.db", "connection pool low: used=%d", 42);
    HPLOGC_ERROR("app.net", "failed to connect: %s", "timeout");

    (void)hplogc_flush();
    hplogc_shutdown();

    printf("basic example done; see example.log\n");
    return 0;
}
