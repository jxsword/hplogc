/* -*- coding: utf-8 -*- */
/**
 * @file sink_console.c
 * @brief console sink：stdout / stderr 输出与 ANSI 彩色（§4.7.1、§4.7.2）。
 *
 * 通过平台契约的 `hp_console_write()` 访问标准流，不含平台分支。
 */

#include "hplogc_internal.h"

#include <hplogc.h>

/** @brief console sink 私有数据。 */
typedef struct {
    int stream;   /*!< 1 = stdout，2 = stderr */
    int color;    /*!< -1 未设置；0 关闭；1 开启 */
} hp_console_priv_t;

/** @brief 各级别的 ANSI 颜色前缀（仅终端设备启用，§4.7.1）。 */
static const char* const g_level_color[] = {
    "\x1b[90m",    /*!< TRACE：灰 */
    "\x1b[36m",    /*!< DEBUG：青 */
    "\x1b[32m",    /*!< INFO：绿 */
    "\x1b[33m",    /*!< WARN：黄 */
    "\x1b[31m",    /*!< ERROR：红 */
    "\x1b[1;31m",  /*!< FATAL：粗红 */
    "\x1b[0m"      /*!< OFF（不会出现） */
};

/** @brief ANSI 复位序列。 */
static const char g_color_reset[] = "\x1b[0m";

/** @brief 解析布尔值；非法返回 -1。 */
static int hp_parse_bool(const char* v)
{
    if (v == NULL) {
        return -1;
    }
    if (strcmp(v, "true") == 0 || strcmp(v, "1") == 0 || strcmp(v, "on") == 0
        || strcmp(v, "yes") == 0) {
        return 1;
    }
    if (strcmp(v, "false") == 0 || strcmp(v, "0") == 0 || strcmp(v, "off") == 0
        || strcmp(v, "no") == 0) {
        return 0;
    }
    return -1;
}

static int hp_console_configure(hplogc_sink_t* sink, const char* key,
                                const char* val)
{
    hp_console_priv_t* p = (hp_console_priv_t*)hplogc_sink_priv(sink);

    if (p == NULL) {
        return -1;
    }
    if (strcmp(key, "stream") == 0) {
        if (strcmp(val, "stdout") == 0) {
            p->stream = 1;
            return 0;
        }
        if (strcmp(val, "stderr") == 0) {
            p->stream = 2;
            return 0;
        }
        return -1;
    }
    if (strcmp(key, "color") == 0) {
        int b = hp_parse_bool(val);
        if (b < 0) {
            return -1;
        }
        p->color = b;
        return 0;
    }
    return -1; /* 未识别的键（§4.10.2） */
}

static int hp_console_init(hplogc_sink_t* sink)
{
    hp_console_priv_t* p = (hp_console_priv_t*)hplogc_sink_priv(sink);

    if (p == NULL) {
        return -1;
    }
    /* 只做条件赋值：绝不覆盖已 configure 的值（§4.10.2 关键约束） */
    if (p->stream == 0) {
        p->stream = 1; /* 默认 stdout（§4.7.2） */
    }
#ifndef HPLOGC_HAS_COLOR
    if (p->color != 0) {
        p->color = 0; /* 构建裁剪彩色（§4.8） */
    }
#else
    if (p->color == -1) {
        /* 默认由 HPLOGC_ENABLE_COLOR 决定，且仅终端设备启用 */
        p->color = (hp_console_isatty(p->stream) == 1) ? 1 : 0;
    }
#endif
    return 0;
}

static int hp_console_start(hplogc_sink_t* sink)
{
    hp_console_priv_t* p = (hp_console_priv_t*)hplogc_sink_priv(sink);

    if (p == NULL) {
        return -1;
    }
    if (p->color == 1) {
        hp_console_enable_vt(p->stream); /* Windows VT 处理（§16.2） */
    }
    return 0;
}

static void hp_console_emit(hplogc_sink_t* sink, const hplogc_event_t* ev)
{
    hp_console_priv_t* p = (hp_console_priv_t*)hplogc_sink_priv(sink);

    if (p == NULL || ev == NULL || ev->formatted == NULL) {
        return;
    }
    if (p->color == 1 && (int)ev->level >= 0
        && (int)ev->level < (int)HPLOGC_LEVEL_OFF) {
        hp_console_write(p->stream, g_level_color[ev->level],
                         strlen(g_level_color[ev->level]));
    }
    /* 行尾换行由格式模板的 %n 占位符负责，sink 不再追加（§4.10.3） */
    hp_console_write(p->stream, ev->formatted, ev->formatted_len);
    if (p->color == 1) {
        hp_console_write(p->stream, g_color_reset, sizeof(g_color_reset) - 1);
    }
}

static int hp_console_emit_batch(hplogc_sink_t* sink,
                                 const hplogc_event_t* const* evs, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        hp_console_emit(sink, evs[i]);
    }
    return (int)n;
}

const hplogc_sink_ops_t hp_sink_console_ops = {
    "console",
    HPLOGC_SINK_ABI_VERSION,
    HPLOGC_CAP_SYNC | HPLOGC_CAP_ASYNC,
    sizeof(hp_console_priv_t),
    hp_console_configure,
    hp_console_init,
    hp_console_start,
    hp_console_emit,
    hp_console_emit_batch,
    NULL, /* flush：直写标准流，无缓冲语义 */
    NULL, /* destroy */
    { NULL, NULL, NULL, NULL }
};
