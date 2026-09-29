/* -*- coding: utf-8 -*- */
/**
 * @file plat_socket.c
 * @brief POSIX 共享层：BSD sockets 实现（§4.7.3 的零依赖 UDP / TCP 输出）。
 *
 * 设计约束：
 * - **绝不无限阻塞**：TCP 连接走"非阻塞 connect + poll 超时"；超时即失败返回。
 * - **不引入第三方依赖**：仅用 libc / POSIX socket API。
 * - **不干扰进程信号语义**：写关闭的 TCP 连接会触发 `SIGPIPE`，故优先使用
 *   `MSG_NOSIGNAL`（Linux）或 `SO_NOSIGPIPE`（BSD / macOS）抑制；两者都不可用时
 *   退化为普通 `send`（此时由调用方保证不因 SIGPIPE 崩溃——hplogc 不安装
 *   SIGPIPE 处理器，交由宿主决定）。
 */

#include "hplogc_platform.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <hplogc.h>

/** @brief socket 句柄。 */
struct hp_socket {
    int fd; /*!< 文件描述符；-1 表示已关闭 */
};

/** @brief 把 fd 设为非阻塞；返回原标志（失败返回 -1）。 */
static int hp_set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }
    return flags;
}

/** @brief 恢复 fd 的标志位。 */
static void hp_restore_flags(int fd, int flags)
{
    if (flags >= 0) {
        (void)fcntl(fd, F_SETFL, flags);
    }
}

/** @brief 抑制 SIGPIPE（尽力而为，失败不影响功能）。 */
static void hp_disable_sigpipe(int fd)
{
#if defined(SO_NOSIGPIPE)
    int on = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#else
    (void)fd;
#endif
}

/**
 * @brief 等待非阻塞 connect 完成。
 * @return 0 已连接；负值表示超时或失败。
 */
static int hp_wait_connect(int fd, unsigned timeout_ms)
{
    struct pollfd pfd;
    int rc;
    int err = 0;
    socklen_t len = sizeof(err);

    pfd.fd = fd;
    pfd.events = POLLOUT;
    pfd.revents = 0;
    rc = poll(&pfd, 1, (int)timeout_ms);
    if (rc == 0) {
        return HPLOGC_ERR_IO; /* 超时：绝不无限等待 */
    }
    if (rc < 0) {
        return (errno == EINTR) ? HPLOGC_ERR_IO : HPLOGC_ERR_IO;
    }
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
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
    int fd = -1;

    if (host == NULL || port == 0) {
        return NULL;
    }
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = (proto == HP_SOCKET_TCP) ? SOCK_STREAM : SOCK_DGRAM;
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || res == NULL) {
        return NULL;
    }
    /* 逐个候选地址尝试（IPv6 / IPv4 双栈） */
    for (it = res; it != NULL; it = it->ai_next) {
        int flags;

        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) {
            continue;
        }
        hp_disable_sigpipe(fd);
        flags = hp_set_nonblock(fd);
        if (connect(fd, it->ai_addr, it->ai_addrlen) == 0) {
            hp_restore_flags(fd, flags);
            break; /* 立即连接成功（UDP 的 connect 不发包，通常直接成功） */
        }
        if (errno == EINPROGRESS || errno == EAGAIN) {
            if (hp_wait_connect(fd, timeout_ms) == HPLOGC_OK) {
                hp_restore_flags(fd, flags);
                break;
            }
        }
        hp_restore_flags(fd, flags);
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        return NULL;
    }
    s = (hp_socket_t*)malloc(sizeof(*s));
    if (s == NULL) {
        close(fd);
        return NULL;
    }
    s->fd = fd;
    return s;
}

int hp_socket_send(hp_socket_t* s, const void* buf, size_t len)
{
    size_t off = 0;
    int flags = 0;

    if (s == NULL || s->fd < 0 || (buf == NULL && len > 0)) {
        return HPLOGC_ERR_INVALID_ARG;
    }
#if defined(MSG_NOSIGNAL)
    flags |= MSG_NOSIGNAL;
#endif
    while (off < len) {
        ssize_t n = send(s->fd, (const char*)buf + off, len - off, flags);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return HPLOGC_ERR_IO; /* 非阻塞语义下不重试，避免阻塞调用线程 */
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
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
    free(s);
}
