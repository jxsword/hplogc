/* -*- coding: utf-8 -*- */
/**
 * @file config_test.c
 * @brief INI 解析 + 配置装配单元测试（代码内配置契约的等价路径）。
 */
#include "test_common.h"

#include "hplogc_internal.h"

#include <stdio.h>

static int write_temp_ini(const char* path, const char* text)
{
    FILE* f = fopen(path, "w");
    if (f == NULL) {
        return -1;
    }
    fputs(text, f);
    if (fclose(f) != 0) {
        return -1;
    }
    return 0;
}

int main(void)
{
    char path[512];
    char logpath[512];
    char text[1024];
    hp_ini_t ini;
    hp_config_t cfg;
    int rc;

    /* 临时文件路径按平台取（Windows 无 /tmp），不得硬编码 */
    snprintf(path, sizeof(path), "%s/hplogc_cfg_test.ini", test_tmpdir());
    snprintf(logpath, sizeof(logpath), "%s/hplogc_cfg_test.log",
             test_tmpdir());
    snprintf(text, sizeof(text),
             "[global]\n"
             "level = DEBUG\n"
             "time format = %%Y-%%m-%%d %%H:%%M:%%S.%%f\n"
             "capture source loc = false\n"
             "\n"
             "[formats]\n"
             "myfmt = %%level %%msg\n"
             "\n"
             "[outputs]\n"
             "file1 = rollingfile, path=%s, rotate=none, enabled=true\n"
             "\n"
             "[rules]\n"
             "app.*.ERROR = myfmt -> file1\n"
             "*.* = standard -> file1\n",
             logpath);

    if (write_temp_ini(path, text) != 0) {
        CHECK(0);
        return test_summary("config");
    }

    memset(&ini, 0, sizeof(ini));
    rc = hp_ini_parse(path, &ini);
    CHECK_EQ(rc, HPLOGC_OK);
    CHECK(ini.count > 0);

    memset(&cfg, 0, sizeof(cfg));
    rc = hp_conf_build(&ini, path, &cfg);
    CHECK_EQ(rc, HPLOGC_OK);
    CHECK(cfg.level <= HPLOGC_LEVEL_DEBUG);
    CHECK_EQ(cfg.sink_count, (size_t)1);
    CHECK_EQ(cfg.rule_count, (size_t)2);
    CHECK(cfg.capture_source_loc == 0);
    CHECK(strcmp(cfg.time_format, "%Y-%m-%d %H:%M:%S.%f") == 0);

    hp_ini_free(&ini);
    return test_summary("config");
}
