/* -*- coding: utf-8 -*- */
/**
 * @file hplogc_atomic.h
 * @brief 原子操作统一接口（按构建选择后端，§4.3 / §2.2）。
 *
 * 后端由 CMake 通过下列宏之一选择：
 * - `HPLOGC_ATOMIC_BACKEND_STDATOMIC`：C11 `<stdatomic.h>`；
 * - `HPLOGC_ATOMIC_BACKEND_GCC_ATOMIC`：GCC / Clang `__atomic_*`；
 * - `HPLOGC_ATOMIC_BACKEND_GCC_SYNC`：`__sync_*` 回退；
 * - `HPLOGC_ATOMIC_BACKEND_MSVC`：MSVC / MinGW `Interlocked*`（Windows 全标准）。
 *
 * 三个后端头文件提供**同名同义**的 `hp_atomic_*` 内联函数，因此公共层代码
 * 与具体后端解耦（§9：所有跨线程计数必须走本接口，禁止依赖"事实原子性"）。
 *
 * @ingroup internal
 */

#ifndef HPLOGC_ATOMIC_H
#define HPLOGC_ATOMIC_H

#if defined(HPLOGC_ATOMIC_BACKEND_STDATOMIC)
#  include "atomic_stdatomic.h"
#elif defined(HPLOGC_ATOMIC_BACKEND_GCC_ATOMIC) \
    || defined(HPLOGC_ATOMIC_BACKEND_GCC_SYNC)
#  include "atomic_gcc.h"
#elif defined(HPLOGC_ATOMIC_BACKEND_MSVC)
#  include "atomic_msvc.h"
#else
/* 未由 CMake 显式指定时，依据编译器能力自动回退到 C11 stdatomic（§2.2）。 */
#  if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L) \
       && !defined(__STDC_NO_ATOMICS__)
#    define HPLOGC_ATOMIC_BACKEND_STDATOMIC
#    include "atomic_stdatomic.h"
#  elif defined(__GNUC__) || defined(__clang__)
#    define HPLOGC_ATOMIC_BACKEND_GCC_ATOMIC
#    include "atomic_gcc.h"
#  else
#    error "未选择原子后端：请检查 HPLOGC_ATOMIC_BACKEND 的 CMake 探测结果"
#  endif
#endif

#endif /* HPLOGC_ATOMIC_H */
