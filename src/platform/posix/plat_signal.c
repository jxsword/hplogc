/* -*- coding: utf-8 -*- */
/**
 * @file plat_signal.c
 * @brief POSIX 共享层：SIGHUP 处理器安装与 fork child handler 注册。
 */

#include "hplogc_platform.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>

#include <hplogc.h>

int hp_install_sighup(void (*handler)(int))
{
    struct sigaction cur;
    struct sigaction act;

    if (handler == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    /* 宿主已安装处理器时不覆盖（§10.3：输出警告并继续） */
    if (sigaction(SIGHUP, NULL, &cur) != 0) {
        return HPLOGC_ERR_IO;
    }
    if (cur.sa_handler != SIG_DFL && cur.sa_handler != SIG_IGN) {
        return 1;
    }

    memset(&act, 0, sizeof(act));
    act.sa_handler = handler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_RESTART;
    return (sigaction(SIGHUP, &act, NULL) == 0) ? HPLOGC_OK : HPLOGC_ERR_IO;
}

void hp_uninstall_sighup(void)
{
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = SIG_DFL;
    sigemptyset(&act.sa_mask);
    sigaction(SIGHUP, &act, NULL);
}

int hp_atfork_child(void (*child)(void))
{
    if (child == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    /* prepare / parent 不需要：子进程重建完全由 child handler 置脏标记驱动 */
    return (pthread_atfork(NULL, NULL, child) == 0) ? HPLOGC_OK
                                                    : HPLOGC_ERR_NO_MEM;
}
