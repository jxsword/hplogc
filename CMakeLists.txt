# -*- coding: utf-8 -*-
# hplogc —— 纯 C 高性能日志库（规范：docs/rd_v0.2.md）
#
# 本文件实现规范 §11「构建与分发」与 §5「构建版本预设」。

cmake_minimum_required(VERSION 3.16)

project(hplogc
        VERSION 0.2.0
        DESCRIPTION "High Performance Log in C"
        LANGUAGES C)

include(CheckCSourceCompiles)
include(CheckIncludeFile)
include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

# ============================================================================
# 1. 功能裁剪选项（§4.8）
# ============================================================================

option(HPLOGC_ENABLE_ASYNC      "异步写入（后台消费者线程 + 批量提交）" ON)
option(HPLOGC_ENABLE_COLOR      "ANSI 彩色输出"                        ON)
option(HPLOGC_ENABLE_ROTATE     "日志轮转"                             ON)
option(HPLOGC_ENABLE_INI        "INI 配置解析器"                       ON)
option(HPLOGC_ENABLE_HOT_RELOAD "配置热加载"                           ON)
option(HPLOGC_ENABLE_CATEGORY   "多 Category 路由与 per-category 级别"  ON)
option(HPLOGC_ENABLE_THROTTLE   "限流与采样"                           OFF)
option(HPLOGC_ENABLE_SOURCE_LOC "捕获 __FILE__ / __LINE__"              ON)

set(HPLOGC_LOCKFREE      OFF     CACHE BOOL   "使用无锁环形缓冲（否则有锁）")
set(HPLOGC_CONCURRENCY   "MPSC"  CACHE STRING "并发模式：SPSC 或 MPSC")
set(HPLOGC_C_STANDARD    "99"    CACHE STRING "C 语言标准：99 或 11")
set(HPLOGC_ATOMIC_BACKEND "auto" CACHE STRING
    "原子后端：auto / stdatomic / gcc-atomic / gcc-sync / msvc-interlocked")
set(HPLOGC_SINKS "console;rollingfile;null" CACHE STRING
    "参与编译的内置 sink 类型列表（分号分隔）")
set(HPLOGC_COMPILE_TIME_LEVEL "" CACHE STRING
    "编译期裁剪阈值：TRACE / DEBUG / INFO / WARN / ERROR / FATAL / OFF（空 = 不裁剪）")
set(HPLOGC_SANITIZER "none" CACHE STRING
    "Sanitizer：none / address / undefined / thread")

set(HPLOGC_BUILD_PRESET "" CACHE STRING
    "构建预设：空 / full / min / sync_thread / async_single")

option(HPLOGC_BUILD_TESTS    "构建测试"   ON)
option(HPLOGC_BUILD_EXAMPLES "构建示例"   ON)
option(HPLOGC_ENABLE_COVERAGE "启用覆盖率插桩" OFF)

# ============================================================================
# 2. 构建预设（§5）：以预设为准，与用户选项冲突时输出警告
# ============================================================================

set(HPLOGC_PRESET_NAME "custom")

if(HPLOGC_BUILD_PRESET STREQUAL "")
  # 未指定预设：若各项均为默认（full 形态），版本名记为 full
  if(HPLOGC_ENABLE_ASYNC AND HPLOGC_ENABLE_COLOR AND HPLOGC_ENABLE_ROTATE
     AND HPLOGC_ENABLE_INI AND HPLOGC_ENABLE_HOT_RELOAD
     AND HPLOGC_ENABLE_CATEGORY AND NOT HPLOGC_ENABLE_THROTTLE
     AND NOT HPLOGC_LOCKFREE AND HPLOGC_CONCURRENCY STREQUAL "MPSC")
    set(HPLOGC_PRESET_NAME "full")
  endif()
elseif(HPLOGC_BUILD_PRESET STREQUAL "full")
  set(HPLOGC_PRESET_NAME "full")
  set(HPLOGC_ENABLE_ASYNC ON  CACHE BOOL "" FORCE)
  set(HPLOGC_ENABLE_COLOR ON  CACHE BOOL "" FORCE)
  set(HPLOGC_ENABLE_ROTATE ON CACHE BOOL "" FORCE)
  set(HPLOGC_ENABLE_INI ON    CACHE BOOL "" FORCE)
  set(HPLOGC_ENABLE_HOT_RELOAD ON CACHE BOOL "" FORCE)
  set(HPLOGC_ENABLE_CATEGORY ON  CACHE BOOL "" FORCE)
  set(HPLOGC_ENABLE_THROTTLE OFF CACHE BOOL "" FORCE)
  set(HPLOGC_LOCKFREE OFF CACHE BOOL "" FORCE)
  set(HPLOGC_CONCURRENCY "MPSC" CACHE STRING "" FORCE)
elseif(HPLOGC_BUILD_PRESET STREQUAL "min")
  set(HPLOGC_PRESET_NAME "min")
  set(HPLOGC_ENABLE_ASYNC OFF CACHE BOOL "" FORCE)
  set(HPLOGC_ENABLE_ROTATE OFF CACHE BOOL "" FORCE)
  set(HPLOGC_ENABLE_COLOR OFF CACHE BOOL "" FORCE)
  set(HPLOGC_ENABLE_CATEGORY OFF CACHE BOOL "" FORCE)
  set(HPLOGC_ENABLE_HOT_RELOAD OFF CACHE BOOL "" FORCE)
  set(HPLOGC_ENABLE_THROTTLE OFF CACHE BOOL "" FORCE)
  set(HPLOGC_ENABLE_INI OFF CACHE BOOL "" FORCE)
  set(HPLOGC_LOCKFREE OFF CACHE BOOL "" FORCE)
  set(HPLOGC_CONCURRENCY "SPSC" CACHE STRING "" FORCE)
  set(HPLOGC_SINKS "console;rollingfile" CACHE STRING "" FORCE)
elseif(HPLOGC_BUILD_PRESET STREQUAL "sync_thread")
  set(HPLOGC_PRESET_NAME "sync_thread")
  set(HPLOGC_ENABLE_ASYNC OFF CACHE BOOL "" FORCE)
  set(HPLOGC_LOCKFREE OFF CACHE BOOL "" FORCE)
  set(HPLOGC_CONCURRENCY "MPSC" CACHE STRING "" FORCE)
elseif(HPLOGC_BUILD_PRESET STREQUAL "async_single")
  set(HPLOGC_PRESET_NAME "async_single")
  set(HPLOGC_ENABLE_ASYNC ON CACHE BOOL "" FORCE)
  set(HPLOGC_LOCKFREE ON CACHE BOOL "" FORCE)
  set(HPLOGC_CONCURRENCY "SPSC" CACHE STRING "" FORCE)
else()
  message(FATAL_ERROR
          "未知的 HPLOGC_BUILD_PRESET='${HPLOGC_BUILD_PRESET}'"
          "（可选：full / min / sync_thread / async_single）")
endif()

if(NOT HPLOGC_BUILD_PRESET STREQUAL "")
  message(STATUS "hplogc: 应用构建预设 '${HPLOGC_BUILD_PRESET}'")
  message(WARNING
          "构建预设 '${HPLOGC_BUILD_PRESET}' 已生效：与预设冲突的 HPLOGC_* 选项被覆盖（§5）")
endif()

# ============================================================================
# 3. 选项校验
# ============================================================================

if(NOT HPLOGC_C_STANDARD MATCHES "^(99|11)$")
  message(FATAL_ERROR "HPLOGC_C_STANDARD 只能取 99 或 11（当前：${HPLOGC_C_STANDARD}）")
endif()

string(TOUPPER "${HPLOGC_CONCURRENCY}" HPLOGC_CONCURRENCY_UPPER)
if(NOT HPLOGC_CONCURRENCY_UPPER MATCHES "^(SPSC|MPSC)$")
  message(FATAL_ERROR "HPLOGC_CONCURRENCY 只能取 SPSC 或 MPSC（当前：${HPLOGC_CONCURRENCY}）")
endif()

# HPLOGC_SANITIZER 允许单个取值或以分号分隔的组合（address 与 undefined 可叠加）；
# thread 与 address / undefined 互斥（§11）
set(HPLOGC_SANITIZER_FLAGS "")
set(_san_has_thread FALSE)
set(_san_has_other FALSE)
foreach(_s IN LISTS HPLOGC_SANITIZER)
  if(_s STREQUAL "none")
    if(HPLOGC_SANITIZER STREQUAL "none")
      break()
    endif()
  endif()
  if(NOT _s MATCHES "^(none|address|undefined|thread)$")
    message(FATAL_ERROR "HPLOGC_SANITIZER 只能取 none / address / undefined / thread")
  endif()
  if(_s STREQUAL "thread")
    set(_san_has_thread TRUE)
  elseif(NOT _s STREQUAL "none")
    set(_san_has_other TRUE)
  endif()
endforeach()
if(_san_has_thread AND _san_has_other)
  message(FATAL_ERROR "thread 与 address / undefined 互斥（§11）")
endif()
foreach(_s IN LISTS HPLOGC_SANITIZER)
  if(_s STREQUAL "address")
    list(APPEND HPLOGC_SANITIZER_FLAGS -fsanitize=address -fno-omit-frame-pointer)
  elseif(_s STREQUAL "undefined")
    list(APPEND HPLOGC_SANITIZER_FLAGS -fsanitize=undefined -fno-sanitize-recover=undefined)
  elseif(_s STREQUAL "thread")
    list(APPEND HPLOGC_SANITIZER_FLAGS -fsanitize=thread)
  endif()
endforeach()

# 并发模式 × 锁实现的溢出策略可用性（§4.3）：仅用于记录，实际 fail-fast 在 init 期
set(HPLOGC_OVERFLOW_SUPPORT "discard")
if(HPLOGC_LOCKFREE)
  if(HPLOGC_CONCURRENCY_UPPER STREQUAL "SPSC")
    set(HPLOGC_OVERFLOW_SUPPORT "discard,overwrite")
  endif()
else()
  set(HPLOGC_OVERFLOW_SUPPORT "discard,overwrite,wait")
endif()
message(STATUS "hplogc: 可用溢出策略 = ${HPLOGC_OVERFLOW_SUPPORT}")

# ---- sink 列表校验（§4.8 / §11） ----
set(HPLOGC_SINK_KNOWN_TYPES console rollingfile syslog null socket)
set(HPLOGC_SINK_FLAGS "")
foreach(_t IN LISTS HPLOGC_SINKS)
  if(NOT _t IN_LIST HPLOGC_SINK_KNOWN_TYPES)
    message(FATAL_ERROR
            "HPLOGC_SINKS 含未知 sink 类型 '${_t}'（已知：${HPLOGC_SINK_KNOWN_TYPES}）")
  endif()
  string(TOUPPER "${_t}" _tu)
  string(REGEX REPLACE "[^A-Z0-9]" "_" _tu "${_tu}")
  list(APPEND HPLOGC_SINK_FLAGS "HPLOGC_SINK_HAS_${_tu}=1")
endforeach()
# 供 hplogc_build_info_t::sinks 使用的逗号分隔字符串（§7.3）
string(REPLACE ";" "," HPLOGC_SINK_LIST "${HPLOGC_SINKS}")

# ============================================================================
# 4. 编译期裁剪级别：CMake 是唯一有效防线（§4.8 / §7.3 / §11）
# ============================================================================

set(HPLOGC_CTL_LEVEL_NUM 0)
if(NOT HPLOGC_COMPILE_TIME_LEVEL STREQUAL "")
  string(TOUPPER "${HPLOGC_COMPILE_TIME_LEVEL}" _ctl)
  if(_ctl STREQUAL "TRACE")
    set(HPLOGC_CTL_LEVEL_NUM 0)
  elseif(_ctl STREQUAL "DEBUG")
    set(HPLOGC_CTL_LEVEL_NUM 1)
  elseif(_ctl STREQUAL "INFO")
    set(HPLOGC_CTL_LEVEL_NUM 2)
  elseif(_ctl STREQUAL "WARN")
    set(HPLOGC_CTL_LEVEL_NUM 3)
  elseif(_ctl STREQUAL "ERROR")
    set(HPLOGC_CTL_LEVEL_NUM 4)
  elseif(_ctl STREQUAL "FATAL")
    set(HPLOGC_CTL_LEVEL_NUM 5)
  elseif(_ctl STREQUAL "OFF")
    set(HPLOGC_CTL_LEVEL_NUM 6)
  else()
    message(FATAL_ERROR
            "HPLOGC_COMPILE_TIME_LEVEL='${HPLOGC_COMPILE_TIME_LEVEL}' 不在白名单内。"
            "只允许 TRACE / DEBUG / INFO / WARN / ERROR / FATAL / OFF 或空值；"
            "严禁使用 HPLOGC_LEVEL_* 枚举符号名（它们在 #if 中被求值为 0，"
            "会导致'想裁剪却全部保留'的静默错误，§7.3）")
  endif()
endif()
message(STATUS "hplogc: HPLOGC_COMPILE_TIME_LEVEL = ${HPLOGC_CTL_LEVEL_NUM}")

# ============================================================================
# 5. 原子后端探测（§4.3 / §11）
# ============================================================================

if(HPLOGC_C_STANDARD STREQUAL "11")
  set(CMAKE_REQUIRED_FLAGS "-std=c11")
else()
  set(CMAKE_REQUIRED_FLAGS "-std=c99")
endif()

if(WIN32)
  set(HPLOGC_ATOMIC_BACKEND_RESOLVED "MSVC")
else()
  set(HPLOGC_ATOMIC_BACKEND_RESOLVED "GCC_ATOMIC")
  if(HPLOGC_ATOMIC_BACKEND STREQUAL "auto")
    if(HPLOGC_C_STANDARD STREQUAL "11")
      check_include_file("stdatomic.h" HPLOGC_HAVE_STDATOMIC)
      if(HPLOGC_HAVE_STDATOMIC)
        set(HPLOGC_ATOMIC_BACKEND_RESOLVED "STDATOMIC")
      endif()
    endif()
    if(HPLOGC_ATOMIC_BACKEND_RESOLVED STREQUAL "GCC_ATOMIC")
      check_c_source_compiles("
        int main(void) {
          long long x = 0;
          __atomic_store_n(&x, 1, __ATOMIC_RELEASE);
          return (int)__atomic_load_n(&x, __ATOMIC_ACQUIRE);
        }" HPLOGC_HAVE_GCC_ATOMIC)
      if(NOT HPLOGC_HAVE_GCC_ATOMIC)
        set(HPLOGC_ATOMIC_BACKEND_RESOLVED "GCC_SYNC")
      endif()
    endif()
  else()
    string(TOUPPER "${HPLOGC_ATOMIC_BACKEND}" _be)
    string(REPLACE "-" "_" _be "${_be}")
    if(NOT _be MATCHES "^(STDATOMIC|GCC_ATOMIC|GCC_SYNC|MSVC_INTERLOCKED)$")
      message(FATAL_ERROR "未知的 HPLOGC_ATOMIC_BACKEND='${HPLOGC_ATOMIC_BACKEND}'")
    endif()
    if(_be STREQUAL "MSVC_INTERLOCKED")
      message(FATAL_ERROR "msvc-interlocked 仅适用于 Windows（§11）")
    endif()
    set(HPLOGC_ATOMIC_BACKEND_RESOLVED "${_be}")
  endif()
endif()

if(HPLOGC_ATOMIC_BACKEND_RESOLVED STREQUAL "MSVC" AND NOT WIN32)
  message(FATAL_ERROR "msvc-interlocked 仅适用于 Windows（§11）")
endif()
if(NOT HPLOGC_ATOMIC_BACKEND_RESOLVED STREQUAL "MSVC"
   AND HPLOGC_ATOMIC_BACKEND MATCHES "msvc")
  message(FATAL_ERROR "msvc-interlocked 仅适用于 Windows（§11）")
endif()

message(STATUS "hplogc: 原子后端 = ${HPLOGC_ATOMIC_BACKEND_RESOLVED}")

# ============================================================================
# 6. 平台选择（§3.3）
# ============================================================================

if(WIN32)
  set(HPLOGC_PLATFORM "win32")
elseif(APPLE)
  set(HPLOGC_PLATFORM "darwin")
else()
  set(HPLOGC_PLATFORM "linux")
endif()
message(STATUS "hplogc: 平台层 = posix + ${HPLOGC_PLATFORM}")

# ============================================================================
# 7. 子目录
# ============================================================================

add_subdirectory(src)

if(HPLOGC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(HPLOGC_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

# ============================================================================
# 8. 安装（§11）
# ============================================================================

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/hplogcConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/hplogcConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/hplogc")

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/hplogcConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/hplogcConfig.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/hplogcConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/hplogc")
