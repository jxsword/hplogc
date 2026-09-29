/* -*- coding: utf-8 -*- */
/**
 * @file sink_socket.c
 * @brief 内置 `socket` sink：UDP / TCP 网络输出（§4.7.3，零第三方依赖）。
 *
 * 语义要点（规范性）：
 * - **绝不阻塞调用线程**：连接受 `connect timeout` 约束，发送失败立即返回；
 *   重连采用**有上限**的指数退避（初始 100 ms，上限 5 s），不在热路径无限重试。
 * - **目标不可达不使 init 失败**：本 sink 是 best-effort 通道，`start` 即便
 *   未能立即连通也返回成功，后续按退避重试；失败由 per-sink 统计暴露（§4.10.3）。
 * - **UDP 无连接**：best-effort，不保证送达、不重传。
 * - 只发送**已格式化**的文本（`ev->formatted`），行尾换行由格式模板负责，
 *   sink 不再追加（§4.10.3）。
 */

#include "hplogc_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hplogc.h>

/** @brief 主机名缓冲容量。 */
#define HP_SOCK_HOST_CAP 256

/** @brief 未配置 `connect timeout` 时使用的默认值（毫秒）。 */
#define HP_SOCK_TIMEOUT_DEFAULT 3000u

/** @brief 重连退避初始值（毫秒）。 */
#define HP_SOCK_BACKOFF_INIT 100u

/** @brief 重连退避上限（毫秒）。 */
#define HP_SOCK_BACKOFF_MAX 5000u

/** @brief socket sink 私有数据。 */
typedef struct {
    char          host[HP_SOCK_HOST_CAP]; /*!< 目标主机 */
    unsigned short port;                  /*!< 目标端口 */
    int           proto;                  /*!< `HP_SOCKET_UDP` / `HP_SOCKET_TCP` */
    unsigned      timeout_ms;             /*!< 连接超时（毫秒） */
    int           reconnect;              /*!< 断线后是否重连 */
    hp_socket_t*  sock;                   /*!< 平台 socket 句柄；NULL 表示未连通 */
    uint64_t      next_retry_ns;          /*!< 下次允许重连的单调时刻 */
    unsigned      backoff_ms;             /*!< 当前退避步长（毫秒） */
} hp_socket_priv_t;

/** @brief 解析非负整数；失败返回 -1。 */
static int hp_sock_parse_uint(const char* v, unsigned* out)
{
    char* end = NULL;
    unsigned long n;

    if (v == NULL || out == NULL || v[0] == '\0') {
        return -1;
    }
    n = strtoul(v, &end, 10);
    if (end == NULL || *end != '\0') {
        return -1;
    }
    *out = (unsigned)n;
    return 0;
}

/** @brief 解析布尔（true/false/on/off/1/0，大小写不敏感）；失败返回 -1。 */
static int hp_sock_parse_bool(const char* v, int* out)
{
    if (v == NULL || out == NULL) {
        return -1;
    }
    if (strcmp(v, "true") == 0 || strcmp(v, "on") == 0 || strcmp(v, "1") == 0
        || strcmp(v, "yes") == 0) {
        *out = 1;
        return 0;
    }
    if (strcmp(v, "false") == 0 || strcmp(v, "off") == 0 || strcmp(v, "0") == 0
        || strcmp(v, "no") == 0) {
        *out = 0;
        return 0;
    }
    return -1;
}

/** @brief 小写的字符串比较辅助（用于协议名）。 */
static int hp_sock_ieq(const char* a, const char* b)
{
    size_t i;

    if (a == NULL || b == NULL) {
        return 0;
    }
    for (i = 0; a[i] != '\0' && b[i] != '\0'; i++) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
    }
    return a[i] == '\0' && b[i] == '\0';
}

static int hp_socket_configure(hplogc_sink_t* sink, const char* key,
                               const char* val)
{
    hp_socket_priv_t* p = (hp_socket_priv_t*)hplogc_sink_priv(sink);
    unsigned n = 0;
    int b = 0;

    if (p == NULL || key == NULL || val == NULL) {
        return -1;
    }
    if (strcmp(key, "host") == 0) {
        if (val[0] == '\0' || strlen(val) >= sizeof(p->host)) {
            return -1;
        }
        snprintf(p->host, sizeof(p->host), "%s", val);
        return 0;
    }
    if (strcmp(key, "port") == 0) {
        if (hp_sock_parse_uint(val, &n) != 0 || n == 0 || n > 65535u) {
            return -1;
        }
        p->port = (unsigned short)n;
        return 0;
    }
    if (strcmp(key, "protocol") == 0) {
        if (hp_sock_ieq(val, "udp")) {
            p->proto = HP_SOCKET_UDP;
            return 0;
        }
        if (hp_sock_ieq(val, "tcp")) {
            p->proto = HP_SOCKET_TCP;
            return 0;
        }
        return -1;
    }
    if (strcmp(key, "connect timeout") == 0) {
        if (hp_sock_parse_uint(val, &n) != 0) {
            return -1;
        }
        p->timeout_ms = n;
        return 0;
    }
    if (strcmp(key, "reconnect") == 0) {
        if (hp_sock_parse_bool(val, &b) != 0) {
            return -1;
        }
        p->reconnect = b;
        return 0;
    }
    return -1; /* 未识别的键（交由 §10.1 未知键流程） */
}

static int hp_socket_init(hplogc_sink_t* sink)
{
    hp_socket_priv_t* p = (hp_socket_priv_t*)hplogc_sink_priv(sink);

    if (p == NULL) {
        return -1;
    }
    /* 默认值填充：仅条件赋值（§4.10.2），不得覆盖已读入的配置 */
    if (p->port == 0) {
        return -1; /* host / port 为必填：缺失即启动失败 */
    }
    if (p->host[0] == '\0') {
        return -1;
    }
    if (p->timeout_ms == 0) {
        p->timeout_ms = HP_SOCK_TIMEOUT_DEFAULT; /* 0 = 系统默认 */
    }
    if (p->backoff_ms == 0) {
        p->backoff_ms = HP_SOCK_BACKOFF_INIT;
    }
    return 0;
}

/** @brief 安排下一次重连（退避有上限）。 */
static void hp_socket_schedule_retry(hp_socket_priv_t* p)
{
    p->next_retry_ns = hp_now_monotonic_ns()
                       + (uint64_t)p->backoff_ms * 1000000ULL;
    if (p->backoff_ms < HP_SOCK_BACKOFF_MAX) {
        p->backoff_ms = (p->backoff_ms < HP_SOCK_BACKOFF_INIT)
                            ? HP_SOCK_BACKOFF_INIT
                            : p->backoff_ms * 2u;
        if (p->backoff_ms > HP_SOCK_BACKOFF_MAX) {
            p->backoff_ms = HP_SOCK_BACKOFF_MAX;
        }
    }
}

/** @brief 在退避到期后尝试重连；成功返回 0。 */
static int hp_socket_try_reopen(hp_socket_priv_t* p)
{
    if (!p->reconnect) {
        return -1;
    }
    if (p->next_retry_ns != 0 && hp_now_monotonic_ns() < p->next_retry_ns) {
        return -1; /* 退避未到：本次不连，避免热路径反复重连 */
    }
    p->sock = hp_socket_open(p->host, p->port, p->proto, p->timeout_ms);
    if (p->sock == NULL) {
        hp_socket_schedule_retry(p);
        return -1;
    }
    p->backoff_ms = HP_SOCK_BACKOFF_INIT;
    p->next_retry_ns = 0;
    return 0;
}

static int hp_socket_start(hplogc_sink_t* sink)
{
    hp_socket_priv_t* p = (hp_socket_priv_t*)hplogc_sink_priv(sink);

    if (p == NULL) {
        return -1;
    }
    /* best-effort：连不上也应启动成功（后续按退避重连），失败由统计暴露 */
    p->sock = hp_socket_open(p->host, p->port, p->proto, p->timeout_ms);
    if (p->sock == NULL) {
        hp_socket_schedule_retry(p);
    }
    return 0;
}

/**
 * @brief 发送单条事件。
 * @return 0 成功；负值表示失败（调用方据此记账）。
 */
static int hp_socket_send_one(hp_socket_priv_t* p, const hplogc_event_t* ev)
{
    if (p == NULL || ev == NULL || ev->formatted == NULL
        || ev->formatted_len == 0) {
        return -1;
    }
    if (p->sock == NULL && hp_socket_try_reopen(p) != 0) {
        return -1; /* 未连通：本次不出，后续可重连 */
    }
    if (p->sock == NULL) {
        return -1;
    }
    if (hp_socket_send(p->sock, ev->formatted, ev->formatted_len)
        != HPLOGC_OK) {
        /* 发送失败：关闭并按退避重连；绝不在此重试（避免阻塞调用线程） */
        hp_socket_close(p->sock);
        p->sock = NULL;
        hp_socket_schedule_retry(p);
        return -1;
    }
    return 0;
}

static void hp_socket_emit(hplogc_sink_t* sink, const hplogc_event_t* ev)
{
    hp_socket_priv_t* p = (hp_socket_priv_t*)hplogc_sink_priv(sink);

    (void)hp_socket_send_one(p, ev);
}

static int hp_socket_emit_batch(hplogc_sink_t* sink,
                                const hplogc_event_t* const* evs, size_t n)
{
    hp_socket_priv_t* p = (hp_socket_priv_t*)hplogc_sink_priv(sink);
    size_t i;
    size_t ok = 0;

    if (p == NULL || evs == NULL) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (hp_socket_send_one(p, evs[i]) == 0) {
            ok++;
        }
    }
    return (int)ok; /* 部分成功按 §4.10.3 记账 */
}

static void hp_socket_destroy(hplogc_sink_t* sink)
{
    hp_socket_priv_t* p = (hp_socket_priv_t*)hplogc_sink_priv(sink);

    if (p == NULL) {
        return;
    }
    hp_socket_close(p->sock); /* 幂等 */
    p->sock = NULL;
}

const hplogc_sink_ops_t hp_sink_socket_ops = {
    "socket",
    HPLOGC_SINK_ABI_VERSION,
    HPLOGC_CAP_SYNC | HPLOGC_CAP_ASYNC,
    sizeof(hp_socket_priv_t),
    hp_socket_configure,
    hp_socket_init,
    hp_socket_start,
    hp_socket_emit,
    hp_socket_emit_batch,
    NULL, /* flush：直写 socket，无缓冲语义 */
    hp_socket_destroy,
    { NULL, NULL, NULL, NULL }
};
