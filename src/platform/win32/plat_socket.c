/* -*- coding: utf-8 -*- */
/**
 * @file plat_socket.c
 * @brief Windows 专属：Winsock2 实现（§4.7.3 的零依赖 UDP / TCP 输出）。
 *
 * 设计约束与 POSIX 版一致（同一契约、同一语义）：
 * - **绝不无限阻塞**：TCP 连接走"非阻塞 connect + select 超时"。
 * - **不引入第三方依赖**：仅用 Winsock2（系统库 `ws2_32`，由 CMake 链接）。
 * - `WSAStartup` 由本文件内部**幂等**完成：使用 `InterlockedCompareExchange`
 *   保证只初始化一次，且其它线程会等待初始化完成。
 */

#include "hplogc_platform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hplogc.h>

/** @brief socket 句柄。 */
struct hp_socket {
    SOCKET s; /*!< Winsock 套接字；INVALID_SOCKET 表示已关闭 */
};

/** @brief WSAStartup 的状态：0 未开始、1 进行中、2 完成。 */
static volatile long g_wsa_state = 0;

/** @brief 幂等地完成 WSAStartup（线程安全）。 */
static void hp_wsa_ensure(void)
{
    if (InterlockedCompareExchange(&g_wsa_state, 1L, 0L) == 0L) {
        WSADATA data;
        (void)WSAStartup(MAKEWORD(2, 2), &data);
        (void)InterlockedExchange(&g_wsa_state, 2L);
        return;
    }
    while (InterlockedCompareExchange(&g_wsa_state, 2L, 2L) != 2L) {
        SwitchToThread(); /* 等待其它线程完成初始化 */
    }
}

/**
 * @brief 等待非阻塞 connect 完成。
 * @return 0 已连接；负值表示超时或失败。
 */
static int hp_wait_connect(SOCKET s, unsigned timeout_ms)
{
    fd_set wset;
    fd_set eset;
    struct timeval tv;
    int rc;
    int err = 0;
    int len = (int)sizeof(err);

    FD_ZERO(&wset);
    FD_ZERO(&eset);
    FD_SET(s, &wset);
    FD_SET(s, &eset);
    tv.tv_sec = (long)(timeout_ms / 1000u);
    tv.tv_usec = (long)((timeout_ms % 1000u) * 1000u);
    rc = select(0, NULL, &wset, &eset, &tv);
    if (rc == 0) {
        return HPLOGC_ERR_IO; /* 超时 */
    }
    if (rc == SOCKET_ERROR) {
        return HPLOGC_ERR_IO;
    }
    if (FD_ISSET(s, &eset)) {
        return HPLOGC_ERR_IO;
    }
    if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &len) != 0
        || err != 0) {
        return HPLOGC_ERR_IO;
    }
    return HPLOGC_OK;
}

hp_socket_t* hp_socket_open(const char* host, unsigned short port, int proto,
                            unsigned timeout_ms)
{
    struct addrinfo hints;
    struct addrinfo* res = NULL;
    struct addrinfo* it = NULL;
    char portstr[16];
    hp_socket_t* s;
    SOCKET sock = INVALID_SOCKET;

    if (host == NULL || port == 0) {
        return NULL;
    }
    hp_wsa_ensure();
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = (proto == HP_SOCKET_TCP) ? SOCK_STREAM : SOCK_DGRAM;
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || res == NULL) {
        return NULL;
    }
    for (it = res; it != NULL; it = it->ai_next) {
        u_long nonblock = 1UL;

        sock = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (sock == INVALID_SOCKET) {
            continue;
        }
        (void)ioctlsocket(sock, FIONBIO, &nonblock);
        if (connect(sock, it->ai_addr, (int)it->ai_addrlen) == 0) {
            nonblock = 0UL;
            (void)ioctlsocket(sock, FIONBIO, &nonblock);
            break;
        }
        if (WSAGetLastError() == WSAEWOULDBLOCK
            || WSAGetLastError() == WSAEINPROGRESS) {
            if (hp_wait_connect(sock, timeout_ms) == HPLOGC_OK) {
                nonblock = 0UL;
                (void)ioctlsocket(sock, FIONBIO, &nonblock);
                break;
            }
        }
        closesocket(sock);
        sock = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (sock == INVALID_SOCKET) {
        return NULL;
    }
    s = (hp_socket_t*)malloc(sizeof(*s));
    if (s == NULL) {
        closesocket(sock);
        return NULL;
    }
    s->s = sock;
    return s;
}

int hp_socket_send(hp_socket_t* s, const void* buf, size_t len)
{
    size_t off = 0;

    if (s == NULL || s->s == INVALID_SOCKET || (buf == NULL && len > 0)) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    while (off < len) {
        int chunk = (len - off > 0x7FFFFFFFu) ? 0x7FFFFFFF : (int)(len - off);
        int n = send(s->s, (const char*)buf + off, chunk, 0);
        if (n == SOCKET_ERROR) {
            int e = WSAGetLastError();
            if (e == WSAEINTR) {
                continue;
            }
            if (e == WSAEWOULDBLOCK) {
                return HPLOGC_ERR_IO; /* 不重试，避免阻塞调用线程 */
            }
            return HPLOGC_ERR_IO;
        }
        if (n == 0) {
            return HPLOGC_ERR_IO;
        }
        off += (size_t)n;
    }
    return HPLOGC_OK;
}

void hp_socket_close(hp_socket_t* s)
{
    if (s == NULL) {
        return;
    }
    if (s->s != INVALID_SOCKET) {
        closesocket(s->s);
        s->s = INVALID_SOCKET;
    }
    free(s);
}
