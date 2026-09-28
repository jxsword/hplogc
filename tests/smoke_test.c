/* -*- coding: utf-8 -*- */
/**
 * @file smoke_test.c
 * @brief 端到端冒烟测试：代码内配置 -> 文件落地 -> 校验输出包含预期内容。
 */
#include "test_common.h"

#include <hplogc.h>

#include <stdio.h>
#include <string.h>

static const char* const g_sinks[] = {"f1"};

static hplogc_kv_t g_opts[] = {
    {"path", "/tmp/hplogc_smoke.log"},
    {"rotate", "none"},
};

static hplogc_sink_config_t g_sink_cfgs[] = {
    {"rollingfile", "f1", 1, -1, g_opts,
     sizeof(g_opts) / sizeof(g_opts[0])},
};

static hplogc_rule_t g_rules[] = {
    {"*", HPLOGC_LEVEL_TRACE, HPLOGC_LEVEL_OFF, "standard", g_sinks, 1},
};

int main(void)
{
    hplogc_config_t cfg;
    FILE* f = NULL;
    char line[2048];
    int found = 0;

    remove("/tmp/hplogc_smoke.log");

    hplogc_config_default(&cfg);
    cfg.sinks = g_sink_cfgs;
    cfg.sink_count = sizeof(g_sink_cfgs) / sizeof(g_sink_cfgs[0]);
    cfg.rules = g_rules;
    cfg.rule_count = sizeof(g_rules) / sizeof(g_rules[0]);
    cfg.default_sinks = g_sinks;
    cfg.default_sink_count = 1;

    if (hplogc_init(&cfg) != HPLOGC_OK) {
        CHECK(0);
        return test_summary("smoke");
    }

    HPLOGC_INFO("smoke", "hello world marker=XYZ");
    HPLOGC_ERROR("smoke.sub", "error path code=%d", 7);
    (void)hplogc_flush();
    hplogc_shutdown();

    f = fopen("/tmp/hplogc_smoke.log", "r");
    CHECK(f != NULL);
    while (f != NULL && fgets(line, sizeof(line), f) != NULL) {
        if (strstr(line, "marker=XYZ") != NULL) {
            found = 1;
        }
    }
    if (f != NULL) {
        fclose(f);
    }
    CHECK(found);
    return test_summary("smoke");
}
