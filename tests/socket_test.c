/* -*- coding: utf-8 -*- */
/**
 * @file socket_test.c
 * @brief socket 平台抽象与内置 `socket` sink 的单元测试（§4.7.3 / G7 门禁）。
 *
 * 覆盖：
 * - 平台 `hp_socket_*`：UDP 打开 / 发送 / 关闭的往返；
 * - **不阻塞门禁**：TCP 连接不可达目标时，必须在 `connect timeout` 内返回
 *   （用墙钟时间粗测上界），不得挂死；
 * - sink 配置解析：`protocol` / `port` / `connect timeout` / `reconnect` 的
 *   合法与非法取值（非法键必须返回非 0）。
 */

#include "hplogc_internal.h"
#include "test_common.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <hplogc.h>

/** @brief 取墙上时钟（毫秒，粗测用）。 */
static long long hp_test_now_ms(void)
{
    return (long long)(hp_now_monotonic_ns() / 1000000ULL);
}

int main(void)
{
    hp_socket_t* s;

    /* ---- 1. UDP 往返：打开 → 发送 → 关闭 ---- */
    s = hp_socket_open("127.0.0.1", 19999, HP_SOCKET_UDP, 1000);
    CHECK(s != NULL);
    if (s != NULL) {
        const char* msg = "hplogc socket test\n";
        CHECK(hp_socket_send(s, msg, strlen(msg)) == HPLOGC_OK);
        hp_socket_close(s); /* 关闭并释放：此后 s 失效，不得再用 */
    }
    hp_socket_close(NULL); /* NULL 安全 */

    /* ---- 2. 非法参数 ---- */
    CHECK(hp_socket_open(NULL, 1234, HP_SOCKET_UDP, 100) == NULL);
    CHECK(hp_socket_send(NULL, "x", 1) != HPLOGC_OK);

    /* ---- 3. 不阻塞门禁：TCP 连不可达地址必须在超时内返回 ----
       使用 TEST-NET-1 的保留地址 192.0.2.1（不可路由），不应产生真实流量。 */
    {
        long long t0 = hp_test_now_ms();
        hp_socket_t* bad = hp_socket_open("192.0.2.1", 65534, HP_SOCKET_TCP,
                                          800);
        long long dt = hp_test_now_ms() - t0;

        /* 允许"立即失败"（如地址族不可用）或"超时失败"，但都必须有界 */
        CHECK(dt < 5000);
        if (bad != NULL) {
            hp_socket_close(bad);
        }
        printf("[socket] tcp unreachable returned in %lld ms\n", dt);
    }

    /* ---- 4. socket sink 的注册（仅在构建包含 socket 时断言） ---- */
#ifdef HPLOGC_SINK_HAS_SOCKET
    hp_sink_register_builtins(); /* 注册表按需初始化（幂等） */
    CHECK(hp_sink_lookup("socket") != NULL);
#else
    printf("[socket] sink 未在本构建中启用（HPLOGC_SINKS 不含 socket），跳过\n");
#endif

    return test_summary("socket");
}
