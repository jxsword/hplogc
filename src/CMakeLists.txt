# -*- coding: utf-8 -*-
# 库目标与源文件组织（§3.3：平台差异收敛到 platform / atomic 目录）

set(HPLOGC_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/include")

# ---- 平台层：POSIX 共享层 + 平台专属 ----
set(HPLOGC_PLATFORM_POSIX_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/platform/posix/plat_sync.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/platform/posix/plat_thread.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/platform/posix/plat_time.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/platform/posix/plat_file.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/platform/posix/plat_signal.c")

if(HPLOGC_PLATFORM STREQUAL "linux")
  set(HPLOGC_PLATFORM_SRCS
      "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/plat_watcher.c")
elseif(HPLOGC_PLATFORM STREQUAL "darwin")
  set(HPLOGC_PLATFORM_SRCS
      "${CMAKE_CURRENT_SOURCE_DIR}/platform/darwin/plat_watcher.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/platform/darwin/plat_thread.c")
else()
  set(HPLOGC_PLATFORM_SRCS
      "${CMAKE_CURRENT_SOURCE_DIR}/platform/win32/plat_sync.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/platform/win32/plat_thread.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/platform/win32/plat_time.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/platform/win32/plat_file.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/platform/win32/plat_signal.c")
endif()

# ---- 环形缓冲：二选一编译（§3.3） ----
if(HPLOGC_LOCKFREE)
  set(HPLOGC_RING_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/ring/ringbuf_lockfree.c")
else()
  set(HPLOGC_RING_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/ring/ringbuf_locked.c")
endif()

# ---- 核心、配置与 sink ----
set(HPLOGC_CORE_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/core/core.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/build_info.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/log.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/format.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/route.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/sink.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/time.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/async.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/reload.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/signal.c")

set(HPLOGC_CONF_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/conf/ini.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/conf/build.c")

set(HPLOGC_SINK_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/sink/sink_registry.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/sink/sink_null.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/sink/sink_console.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/sink/sink_rollingfile.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/sink/sink_syslog.c")

add_library(hplogc
            ${HPLOGC_CORE_SRCS}
            ${HPLOGC_CONF_SRCS}
            ${HPLOGC_SINK_SRCS}
            ${HPLOGC_RING_SRCS}
            ${HPLOGC_PLATFORM_POSIX_SRCS}
            ${HPLOGC_PLATFORM_SRCS})

target_include_directories(hplogc
    PUBLIC  "$<BUILD_INTERFACE:${HPLOGC_INCLUDE_DIR}>"
            "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform"
            "${CMAKE_CURRENT_SOURCE_DIR}/atomic"
            "${CMAKE_CURRENT_SOURCE_DIR}/ring")

set_target_properties(hplogc PROPERTIES
    C_STANDARD ${HPLOGC_C_STANDARD}
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS ON
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    DEFINE_SYMBOL HPLOGC_BUILDING_DLL)

# ---- 编译期配置宏 ----
target_compile_definitions(hplogc PRIVATE
    "HPLOGC_ATOMIC_BACKEND_${HPLOGC_ATOMIC_BACKEND_RESOLVED}=1"
    "HPLOGC_COMPILE_TIME_LEVEL=${HPLOGC_CTL_LEVEL_NUM}"
    "HPLOGC_PRESET_NAME=\"${HPLOGC_PRESET_NAME}\""
    "HPLOGC_SINK_LIST=\"${HPLOGC_SINK_LIST}\""
    ${HPLOGC_SINK_FLAGS})

if(HPLOGC_CONCURRENCY_UPPER STREQUAL "SPSC")
  target_compile_definitions(hplogc PRIVATE HPLOGC_CONCURRENCY_SPSC=1)
else()
  target_compile_definitions(hplogc PRIVATE HPLOGC_CONCURRENCY_MPSC=1)
endif()

if(HPLOGC_ENABLE_ASYNC)
  target_compile_definitions(hplogc PRIVATE HPLOGC_HAS_ASYNC=1)
endif()
if(HPLOGC_ENABLE_COLOR)
  target_compile_definitions(hplogc PRIVATE HPLOGC_HAS_COLOR=1)
endif()
if(HPLOGC_ENABLE_ROTATE)
  target_compile_definitions(hplogc PRIVATE HPLOGC_HAS_ROTATE=1)
endif()
if(HPLOGC_ENABLE_INI)
  target_compile_definitions(hplogc PRIVATE HPLOGC_HAS_INI=1)
endif()
if(HPLOGC_ENABLE_HOT_RELOAD)
  target_compile_definitions(hplogc PRIVATE HPLOGC_HAS_HOT_RELOAD=1)
endif()
if(HPLOGC_ENABLE_CATEGORY)
  target_compile_definitions(hplogc PRIVATE HPLOGC_HAS_CATEGORY=1)
endif()
if(HPLOGC_ENABLE_THROTTLE)
  target_compile_definitions(hplogc PRIVATE HPLOGC_HAS_THROTTLE=1)
endif()
if(HPLOGC_ENABLE_SOURCE_LOC)
  target_compile_definitions(hplogc PRIVATE HPLOGC_HAS_SOURCE_LOC=1)
endif()
if(HPLOGC_LOCKFREE)
  target_compile_definitions(hplogc PRIVATE HPLOGC_USE_LOCKFREE=1)
endif()
if(HPLOGC_PLATFORM STREQUAL "linux")
  target_compile_definitions(hplogc PRIVATE HPLOGC_PLATFORM_LINUX=1)
elseif(HPLOGC_PLATFORM STREQUAL "darwin")
  target_compile_definitions(hplogc PRIVATE HPLOGC_PLATFORM_DARWIN=1)
else()
  target_compile_definitions(hplogc PRIVATE HPLOGC_PLATFORM_WIN32=1)
endif()

# ---- 警告与 sanitizer（§11） ----
if(MSVC)
  target_compile_options(hplogc PRIVATE /W4)
else()
  target_compile_options(hplogc PRIVATE -Wall -Wextra)
  set_target_properties(hplogc PROPERTIES
      C_VISIBILITY_PRESET hidden
      VISIBILITY_INLINES_HIDDEN ON)
endif()

if(NOT HPLOGC_SANITIZER MATCHES "none")
  target_compile_options(hplogc PUBLIC ${HPLOGC_SANITIZER_FLAGS})
  target_link_options(hplogc PUBLIC ${HPLOGC_SANITIZER_FLAGS})
endif()

if(HPLOGC_ENABLE_COVERAGE AND NOT MSVC)
  target_compile_options(hplogc PUBLIC --coverage -O0 -g)
  target_link_options(hplogc PUBLIC --coverage)
endif()

# ---- 平台库 ----
if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(hplogc PUBLIC Threads::Threads)
  target_link_libraries(hplogc PRIVATE m)
endif()

# ---- 安装 ----
install(TARGETS hplogc
        EXPORT hplogcTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES "${HPLOGC_INCLUDE_DIR}/hplogc.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(EXPORT hplogcTargets
        FILE hplogcTargets.cmake
        NAMESPACE hplogc::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/hplogc")
