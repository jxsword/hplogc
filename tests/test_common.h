/* -*- coding: utf-8 -*- */
/**
 * @file test_common.h
 * @brief 极简测试框架：断言计数 + 退出码。
 */
#ifndef HPLOGC_TEST_COMMON_H
#define HPLOGC_TEST_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_test_failures = 0;
static int g_test_checks = 0;

/**
 * @brief 返回可用的临时目录（跨平台）。
 *
 * Windows 没有 `/tmp`，故按环境变量依次回退：`TMPDIR` → `TEMP` → `TMP` → `/tmp`。
 * 测试中的临时文件一律以此目录为根，不得硬编码 POSIX 路径。
 */
static const char* test_tmpdir(void)
{
    const char* d = getenv("TMPDIR");
    if (d == NULL || d[0] == '\0') {
        d = getenv("TEMP");
    }
    if (d == NULL || d[0] == '\0') {
        d = getenv("TMP");
    }
    if (d == NULL || d[0] == '\0') {
        d = "/tmp";
    }
    return d;
}

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_test_checks++;                                                      \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: (%s)\n", __FILE__, __LINE__, #cond);\
            g_test_failures++;                                               \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        g_test_checks++;                                                      \
        if ((a) != (b)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s (%lld) != %s (%lld)\n",          \
                    __FILE__, __LINE__, #a, (long long)(a), #b,              \
                    (long long)(b));                                         \
            g_test_failures++;                                               \
        }                                                                    \
    } while (0)

static int test_summary(const char* name)
{
    if (g_test_failures) {
        printf("[%s] FAILED: %d/%d checks failed\n", name,
               g_test_failures, g_test_checks);
        return 1;
    }
    printf("[%s] PASS: %d checks\n", name, g_test_checks);
    return 0;
}

#endif /* HPLOGC_TEST_COMMON_H */
