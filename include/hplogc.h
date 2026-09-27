/* -*- coding: utf-8 -*- */
/**
 * @file hplogc.h
 * @brief hplogc 的公共 API（v0.2）—— 一个纯 C 的高性能日志库。
 *
 * 本文件是 `docs/rd_v0.2.md` 的规范性配套头文件，**自 v0.2 起即为正式公共 API**
 * （已取代 v0.1 的同名文件），相对 v0.1 做了一次**受控的破坏性重构**，带来三件事：
 *
 * 1. **编译阻塞修复**：补齐 v0.1 中被 `hplogc_config_t` 引用却从未定义的
 *    `hplogc_overflow_policy_t` 与 `hplogc_crash_safety_t`；删除遗留的
 *    `hpulogc_*`（拼写错误、且引用未定义类型）异构声明。
 * 2. **多 sink 输出体系**：以 vtable + 注册表取代固定的 `hplogc_output_t` 枚举，
 *    核心不认识任何具体 sink；新增可向外扩展的 sink 注册 ABI。
 * 3. **结构化字段通道**：新增 `hplogc_field_t` 与 `hplogc_log_fields()` 系列，
 *    为后续接 ElasticSearch / ClickHouse / Loki 类检索型 sink 打通数据通道。
 *
 * 这是应用程序唯一需要包含的头文件。所有公共符号均使用 `hplogc_` 前缀。
 * 该头文件符合 C99/C11，且对 C++ 安全（extern "C"）。
 *
 * @note 例外：`HPLOGC_FIELD_*` 字段构造宏展开为 C99 **指定初始化器**（联合成员
 *       `.i = ...`）。C99/C11 下无误；该语法在 C++ 中自 C++20 起才入标准，C++20
 *       之前依赖编译器扩展——GCC/Clang 默认接受，但 `-pedantic-errors` 或 MSVC
 *       `/permissive-` 等严格配置下会报错。C++20 之前的严格模式调用方请改用
 *       `hplogc_field_t` 显式赋值（规范 §7.6）。
 *
 * @note 本文件自 v0.2 起为正式公共头文件，**参与构建与安装**（`make install`
 *       安装本文件，见规范 §11）。
 *
 * @ingroup hplogc
 */

#ifndef HPLOGC_H
#define HPLOGC_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*! @defgroup hplogc hplogc 公共 API
 *  @brief 纯 C 的高性能日志库。
 *  @{
 */

/* ---- 版本宏 ---- */

/** @brief 库的主版本号（语义化版本）。 */
#define HPLOGC_VERSION_MAJOR 0
/** @brief 库的次版本号（语义化版本）。 */
#define HPLOGC_VERSION_MINOR 2
/** @brief 库的修订号（语义化版本）。 */
#define HPLOGC_VERSION_PATCH 0

/** @brief 版本号字符串字面量，例如 "0.2.0"。 */
#define HPLOGC_VERSION_STRING \
    "0.2.0"

/* ---- 动态库导出属性 ---- */

/**
 * @brief 公共 API 函数的符号导出标注。
 *
 * 在 Windows 上，构建库 DLL 时（定义了 HPLOGC_BUILDING_DLL）展开为
 * `__declspec(dllexport)`，使用 DLL 时（定义了 HPLOGC_USE_DLL）展开为
 * `__declspec(dllimport)`。在 GCC/Clang 上则请求默认的 ELF 可见性，
 * 使符号在 `-fvisibility=hidden` 下依然可见。
 */
#ifndef HPLOGC_API
#  if defined(_WIN32)
#    if defined(HPLOGC_BUILDING_DLL)
#      define HPLOGC_API __declspec(dllexport)
#    elif defined(HPLOGC_USE_DLL)
#      define HPLOGC_API __declspec(dllimport)
#    else
#      define HPLOGC_API
#    endif
#  elif defined(__GNUC__) || defined(__clang__)
#    define HPLOGC_API __attribute__((visibility("default")))
#  else
#    define HPLOGC_API
#  endif
#endif

/**
 * @brief printf 格式属性包装宏（编译期格式字符串检查）。
 */
#ifndef HPLOGC_PRINTF
#  if defined(__GNUC__) || defined(__clang__)
#    define HPLOGC_PRINTF(fmt_idx, va_idx) \
        __attribute__((format(printf, fmt_idx, va_idx)))
#  else
#    define HPLOGC_PRINTF(fmt_idx, va_idx)
#  endif
#endif

/* ---- sink 操作表 ABI 版本 ---- */

/** @brief `hplogc_sink_ops_t` ABI 的主版本号；滚动表示不兼容变更。 */
#define HPLOGC_SINK_ABI_VERSION_MAJOR 1u
/** @brief `hplogc_sink_ops_t` ABI 的次版本号；滚动表示尾部追加字段（向下兼容）。 */
#define HPLOGC_SINK_ABI_VERSION_MINOR 0u

/** @brief 从 ABI 版本号中抽取主版本号。 */
#define HPLOGC_SINK_ABI_MAJOR_OF(v) (((uint32_t)(v)) >> 16)
/** @brief 从 ABI 版本号中抽取次版本号。 */
#define HPLOGC_SINK_ABI_MINOR_OF(v) (((uint32_t)(v)) & 0xFFFFu)

/**
 * @brief `hplogc_sink_ops_t` 的 ABI 版本号，编码为 `(主版本 << 16) | 次版本`。
 *
 * 注册时必须由调用方填入 `HPLOGC_SINK_ABI_VERSION`：注册表用
 * `HPLOGC_SINK_ABI_MAJOR_OF()` 抽取**主版本**并校验其与库内主版本严格相等，
 * 避免跨越不兼容边界的 ops 被载入；次版本不做相等要求。
 */
#define HPLOGC_SINK_ABI_VERSION \
    ((HPLOGC_SINK_ABI_VERSION_MAJOR << 16) | HPLOGC_SINK_ABI_VERSION_MINOR)

/* ---- 容量上限（可在编译期用 -D 覆盖） ---- */

#ifndef HPLOGC_MAX_SINKS
/** @brief sink 实例的最大数量（代码配置数组上限；配置文件同样使用该值）。 */
#define HPLOGC_MAX_SINKS 16
#endif

/**
 * @brief `HPLOGC_MAX_OUTPUTS` 的兼容别名（v0.1 名称）。
 *
 * @deprecated 语义已由 @ref HPLOGC_MAX_SINKS 接管；仅为平滑迁移保留，将于下一个
 * 主版本移除。
 */
#ifndef HPLOGC_MAX_OUTPUTS
#define HPLOGC_MAX_OUTPUTS HPLOGC_MAX_SINKS
#endif

#ifndef HPLOGC_MAX_SINK_TYPES
/** @brief 可注册的 sink 类型（含内置与自定义）上限。 */
#define HPLOGC_MAX_SINK_TYPES 32
#endif

#ifndef HPLOGC_MAX_OPTIONS
/** @brief 单个 sink 实例的代码内私有配置项上限；全局配置键数量同此上限。 */
#define HPLOGC_MAX_OPTIONS 64
#endif

#ifndef HPLOGC_MAX_FIELDS
/** @brief 单条日志可携带的结构化字段数量上限（超出部分丢弃并计数）。 */
#define HPLOGC_MAX_FIELDS 16
#endif

#ifndef HPLOGC_MAX_FIELD_STR_LEN
/**
 * @brief 单个字符串型字段入队时的拷贝上限（字节，不含结尾 NUL）。
 *
 * 字段（`key` 与字符串值）在生产者线程随消息体一起深拷贝进环形缓冲，因此调用方
 * 无需保证指针生命周期，允许传入栈上缓冲；超出该上限的部分被静默截断
 * （计入 `fields_dropped` 统计）。
 */
#define HPLOGC_MAX_FIELD_STR_LEN 128
#endif

#ifndef HPLOGC_MAX_RULES
/** @brief 路由规则的最大数量（代码配置数组上限；配置文件同样使用该值）。 */
#define HPLOGC_MAX_RULES 64
#endif

#ifndef HPLOGC_MAX_CATEGORIES
/** @brief 内部 category 注册表的容量。 */
#define HPLOGC_MAX_CATEGORIES 64
#endif

#ifndef HPLOGC_MAX_NAME_LEN
/** @brief 格式名 / sink 实例名 / sink 类型名 / category 名的最大长度（字节数，含结尾 NUL）。 */
#define HPLOGC_MAX_NAME_LEN 64
#endif

#ifndef HPLOGC_MAX_PATH_LEN
/** @brief 文件路径的最大长度（字节数，含结尾 NUL）。 */
#define HPLOGC_MAX_PATH_LEN 512
#endif

#ifndef HPLOGC_MAX_FMT_LEN
/** @brief 格式模板 / 配置字符串值的最大长度（字节数，含结尾 NUL）。 */
#define HPLOGC_MAX_FMT_LEN 256
#endif

/* ---- 日志级别 ---- */

/**
 * @brief 日志严重级别，按严重程度递增排列。
 *
 * HPLOGC_LEVEL_TRACE .. HPLOGC_LEVEL_FATAL 为合法的日志级别。
 * HPLOGC_LEVEL_OFF 仅作为合法的运行时过滤阈值（它会关闭全部输出）；
 * 把它作为写入 API 的 level 参数传入时，
 * 该条记录会被静默丢弃。
 *
 * @warning 枚举常量在**预处理期不可见**（会被求值为 0），因此不得把它们用作
 *          @ref HPLOGC_COMPILE_TIME_LEVEL 的取值或参与 `#if` 运算；编译期裁剪请
 *          使用 @c HPLOGC_CTL_* 系列数值宏。
 */
typedef enum {
    HPLOGC_LEVEL_TRACE = 0, /*!< 最详细的级别 */
    HPLOGC_LEVEL_DEBUG = 1, /*!< 调试信息 */
    HPLOGC_LEVEL_INFO  = 2, /*!< 一般提示信息 */
    HPLOGC_LEVEL_WARN  = 3, /*!< 警告 */
    HPLOGC_LEVEL_ERROR = 4, /*!< 错误 */
    HPLOGC_LEVEL_FATAL = 5, /*!< 致命错误 */
    HPLOGC_LEVEL_OFF   = 6  /*!< 仅作为过滤阈值，不是合法的日志级别 */
} hplogc_level_t;

/** @brief 最低受支持的级别别名，等价于 `HPLOGC_LEVEL_TRACE`（即"放行全部"）。 */
#define HPLOGC_LEVEL_ALL HPLOGC_LEVEL_TRACE

/**
 * @brief 查询级别对应的大写名称字符串。
 *
 * @param level  目标级别。
 * @return       级别名常量字符串（`TRACE` .. `OFF`）；未知级别返回 `"UNKNOWN"`。
 *               返回的指针指向静态存储，不得释放。
 */
HPLOGC_API const char* hplogc_level_name(hplogc_level_t level);

/**
 * @brief 解析级别名字符串（大小写不敏感）。
 *
 * @param name  级别名；接受 `TRACE` .. `FATAL` 与 `OFF`；NULL 视为非法。
 * @param out   成功时接收解析结果；不得为 NULL。
 * @return      成功返回 `HPLOGC_OK`；名字非法或参数为 NULL 返回
 *              `HPLOGC_ERR_INVALID_ARG`。可在任意时刻调用（无需初始化）。
 */
HPLOGC_API int hplogc_level_parse(const char* name, hplogc_level_t* out);

/* ---- 编译期裁剪级别（修正 v0.1 缺陷：仅接受数值） ---- */

/**
 * @def HPLOGC_COMPILE_TIME_LEVEL
 * @brief 便捷宏使用的编译期裁剪阈值，**必须为数值**。
 *
 * 取值为 0..6，或等价的 @c HPLOGC_CTL_* 数值宏（例如 @c HPLOGC_CTL_INFO）。
 * 低于该阈值的 `HPLOGC_xxx` / `HPLOGC_xxx_F` 宏展开为空语句 `((void)0)`，
 * 实现零运行时开销。
 *
 * @warning **不得**传入 @ref hplogc_level_t 的枚举符号名（如 `HPLOGC_LEVEL_INFO`）：
 *          枚举常量不是预处理期宏，`#if` 中会被求值为 0，导致"想裁剪却全部保留"
 *          的静默错误——这比编译失败更危险。CMake 目标负责接收 `TRACE..OFF`
 *          字符串并映射为数值后再定义本宏。
 *
 * 该宏只影响便捷宏，不影响直接调用 `hplogc_log()` / `hplogc_log_fields()`。
 */
#ifndef HPLOGC_COMPILE_TIME_LEVEL
#define HPLOGC_COMPILE_TIME_LEVEL HPLOGC_CTL_TRACE
#endif

/* 注意：空宏（例如构建系统把空字符串透传为 -DHPLOGC_COMPILE_TIME_LEVEL=）在预处理期
 * 无法被检测，且会使下方的 #if 因缺少操作数而直接报语法错误。构建系统必须把
 * "不裁剪"映射为数值 0（HPLOGC_CTL_TRACE），不得生成空定义。 */

/** @brief 编译期裁剪阈值取值：TRACE（不裁剪）。 */
#define HPLOGC_CTL_TRACE 0
/** @brief 编译期裁剪阈值取值：DEBUG。 */
#define HPLOGC_CTL_DEBUG 1
/** @brief 编译期裁剪阈值取值：INFO。 */
#define HPLOGC_CTL_INFO  2
/** @brief 编译期裁剪阈值取值：WARN。 */
#define HPLOGC_CTL_WARN  3
/** @brief 编译期裁剪阈值取值：ERROR。 */
#define HPLOGC_CTL_ERROR 4
/** @brief 编译期裁剪阈值取值：FATAL。 */
#define HPLOGC_CTL_FATAL 5
/** @brief 编译期裁剪阈值取值：OFF（移除全部便捷宏日志）。 */
#define HPLOGC_CTL_OFF   6

#if defined(HPLOGC_COMPILE_TIME_LEVEL) && \
    (HPLOGC_COMPILE_TIME_LEVEL < HPLOGC_CTL_TRACE || HPLOGC_COMPILE_TIME_LEVEL > HPLOGC_CTL_OFF)
#error "HPLOGC_COMPILE_TIME_LEVEL must be a number in 0..6 (see HPLOGC_CTL_*). " \
       "Enum symbols such as HPLOGC_LEVEL_INFO are NOT preprocessor macros and " \
       "silently evaluate to 0."
#endif

/* ---- 枚举 ---- */

/**
 * @brief 环形缓冲区满时的溢出处理策略（补齐 v0.1 头文件缺失定义）。
 */
typedef enum {
    HPLOGC_OVERFLOW_DISCARD   = 0, /*!< 丢弃新日志，dropped 计数 +1 */
    HPLOGC_OVERFLOW_OVERWRITE = 1, /*!< 覆盖最旧的未消费条目，overwritten 计数 +1 */
    HPLOGC_OVERFLOW_WAIT      = 2  /*!< 阻塞生产者直到有空闲空间 */
} hplogc_overflow_policy_t;

/**
 * @brief fsync（崩溃一致性）策略。
 *
 * @note 枚举值顺序 **不等于** fsync 严格度顺序；严格度为
 *       `none < shutdown < periodic < entry`（见规范 §9）。
 */
typedef enum {
    HPLOGC_CRASH_NONE     = 0, /*!< 不 fsync */
    HPLOGC_CRASH_PERIODIC = 1, /*!< 消费者按 flush interval 周期 fsync */
    HPLOGC_CRASH_ENTRY    = 2, /*!< 每条日志 fsync */
    HPLOGC_CRASH_SHUTDOWN = 3  /*!< 仅 shutdown 时 fsync */
} hplogc_crash_safety_t;

/**
 * @brief 时间戳来源。
 */
typedef enum {
    HPLOGC_TS_REALTIME  = 0, /*!< CLOCK_REALTIME（墙上时钟） */
    HPLOGC_TS_MONOTONIC = 1  /*!< 单调时钟，记录自 init 起的“秒.微秒” */
} hplogc_timestamp_source_t;

/**
 * @brief %n 占位符使用的换行风格。
 */
typedef enum {
    HPLOGC_NEWLINE_AUTO = 0, /*!< Unix=\n / Windows=\r\n */
    HPLOGC_NEWLINE_LF   = 1, /*!< 强制使用 \n */
    HPLOGC_NEWLINE_CRLF = 2  /*!< 强制使用 \r\n */
} hplogc_newline_t;

/**
 * @brief 结构化字段的值类型。
 */
typedef enum {
    HPLOGC_V_INT    = 0, /*!< 有符号整数 */
    HPLOGC_V_UINT   = 1, /*!< 无符号整数 */
    HPLOGC_V_DOUBLE = 2, /*!< 双精度浮点 */
    HPLOGC_V_STR    = 3, /*!< UTF-8 字符串 */
    HPLOGC_V_BOOL   = 4  /*!< 布尔值（0 / 非 0） */
} hplogc_vtype_t;

/**
 * @brief rollingfile sink 的轮转策略配置词表。
 *
 * 这些值已不再是某公共结构体的字段取值，而是 `rollingfile` sink 的配置键
 * `rotate` 的合法取值（以保持配置文件值集合受控）。
 */
typedef enum {
    HPLOGC_ROTATE_NONE = 0, /*!< 从不轮转 */
    HPLOGC_ROTATE_SIZE = 1, /*!< 达到 max size 时轮转 */
    HPLOGC_ROTATE_TIME = 2, /*!< 在 time unit 边界处轮转 */
    HPLOGC_ROTATE_BOTH = 3  /*!< 满足任一条件即轮转 */
} hplogc_rotate_t;

/**
 * @brief rollingfile sink 按时间轮转时的时间分桶单位。
 *
 * 分桶遵循所配置的时区（本地时间或 UTC）；一周自周一 00:00 开始（ISO-8601）。
 */
typedef enum {
    HPLOGC_TU_HOUR  = 0, /*!< 按小时分桶 */
    HPLOGC_TU_DAY   = 1, /*!< 按天分桶（自然日） */
    HPLOGC_TU_WEEK  = 2, /*!< 按周分桶（ISO-8601，周一起算） */
    HPLOGC_TU_MONTH = 3  /*!< 按月分桶（自然月） */
} hplogc_time_unit_t;

/* ---- sink 能力位 ---- */

/** @brief 可在调用方线程直接同步写出（同步模式 / 混合模式的同步部分使用）。 */
#define HPLOGC_CAP_SYNC   (1u << 0)
/** @brief 支持由消费者线程批量写出；异步模式下核心优先调用 emit_batch。 */
#define HPLOGC_CAP_ASYNC  (1u << 1)
/** @brief 能够消费结构化字段（纯文本 sink 不应设置该位）。 */
#define HPLOGC_CAP_STRUCT (1u << 2)
/** @brief 需要行 I/O 保护（单条 write 不保证原子性的目标应设置该位）。 */
#define HPLOGC_CAP_LINE_ATOMIC (1u << 3)
/** @brief 该 sink 支持 fsync 语义（per-sink fsync 与全局 crash_safety 取更严格者）。 */
#define HPLOGC_CAP_FSYNC  (1u << 4)

/* ---- 错误码 ---- */

/**
 * @brief 初始化 / 控制类 API 返回的错误码（0 表示成功）。
 */
typedef enum {
    HPLOGC_OK              =  0, /*!< 成功 */
    HPLOGC_ERR_INVALID_ARG = -1, /*!< 非法参数（NULL、越界的枚举值、代码配置取值超出范围） */
    HPLOGC_ERR_NO_MEM      = -2, /*!< 内存分配失败 */
    HPLOGC_ERR_IO          = -3, /*!< 文件 / 设备 I/O 失败（例如 init 时输出目标打开失败） */
    HPLOGC_ERR_CONFIG      = -4, /*!< 配置缺失或非法 */
    HPLOGC_ERR_STATE       = -5, /*!< 状态错误（未初始化、重复初始化、在 shutdown 之后调用） */
    HPLOGC_ERR_UNSUPPORTED = -6  /*!< 取值合法但当前构建不支持（未注册 / 被裁剪的 sink 类型、
                                  *   当前构建不支持的溢出策略等；fail-fast，不静默降级） */
} hplogc_error_t;

/* ============================ 结构化字段 ============================ */

/**
 * @brief 一个结构化字段（键值对）。
 *
 * 字段（含 @p key 与字符串值）连同消息体在**生产者线程**被深拷贝进环形缓冲，
 * 因此字符串指针无需保证生命周期，允许传入栈上缓冲；超长字符串按
 * `HPLOGC_MAX_FIELD_STR_LEN` 截断。
 * 这使 hplogc 区别于"借用调用方指针"的常见做法：环形缓冲是跨线程容器，
 * 借用栈上或临时内存会破坏异步路径的正确性。
 */
typedef struct {
    const char*    key;  /*!< 字段名；入队时深拷贝，允许指向栈上缓冲 */
    hplogc_vtype_t type; /*!< 值类型 */
    union {
        long long          i; /*!< HPLOGC_V_INT */
        unsigned long long u; /*!< HPLOGC_V_UINT */
        double             d; /*!< HPLOGC_V_DOUBLE */
        const char*        s; /*!< HPLOGC_V_STR（入队时深拷贝） */
        int                b; /*!< HPLOGC_V_BOOL */
    } v;                      /*!< 按 @p type 择一有效的联合值 */
} hplogc_field_t;

/* 字段构造宏：展开为 C99 指定初始化器，因此要求 C99 或 C++20（见文件头 @note）。 */

/** @brief 构造 `HPLOGC_V_INT` 字段的初始化器。 */
#define HPLOGC_FIELD_INT(k, x)    { (k), HPLOGC_V_INT,    { .i = (long long)(x) } }
/** @brief 构造 `HPLOGC_V_UINT` 字段的初始化器。 */
#define HPLOGC_FIELD_UINT(k, x)   { (k), HPLOGC_V_UINT,   { .u = (unsigned long long)(x) } }
/** @brief 构造 `HPLOGC_V_DOUBLE` 字段的初始化器。 */
#define HPLOGC_FIELD_DOUBLE(k, x) { (k), HPLOGC_V_DOUBLE, { .d = (double)(x) } }
/** @brief 构造 `HPLOGC_V_STR` 字段的初始化器（入队时深拷贝，无需保证指针生命周期）。 */
#define HPLOGC_FIELD_STR(k, x)    { (k), HPLOGC_V_STR,    { .s = (const char *)(x) } }
/** @brief 构造 `HPLOGC_V_BOOL` 字段的初始化器。 */
#define HPLOGC_FIELD_BOOL(k, x)   { (k), HPLOGC_V_BOOL,   { .b = (int)(x) } }

/* ============================ 日志事件 ============================ */

/**
 * @brief sink 的唯一输入：一条已路由、已格式化的日志事件。
 *
 * @warning 事件内的所有指针（`category` / `file` / `func` / `msg` / `formatted` /
 *          `fields`）均指向环形缓冲中的条目存储，**仅在 emit 回调执行期间有效**。
 *          sink 需要长期留存时必须自行拷贝。
 */
typedef struct {
    hplogc_level_t        level;         /*!< 记录级别 */
    const char*           category;      /*!< category 名；NULL / "" 等价于 "*" */
    uint64_t              ts_ns;         /*!< 时间戳（纳秒）：realtime 取自墙上时钟，
                                          *   monotonic 取自相对 init 的单调时钟 */
    uint32_t              pid;           /*!< 进程 ID */
    uint64_t              tid;           /*!< 线程 ID（平台原生语义） */
    const char*           file;          /*!< 源文件名；未捕获时为 NULL */
    int                   line;          /*!< 源文件行号 */
    const char*           func;          /*!< 函数名；未捕获时为 NULL */
    const char*           msg;           /*!< %msg 展开结果（已渲染的消息体） */
    size_t                msg_len;       /*!< @p msg 的字节长度（不含 NUL） */
    const char*           formatted;     /*!< 按命中规则格式化后的完整日志行（不含结尾换行） */
    size_t                formatted_len; /*!< @p formatted 的字节长度 */
    const hplogc_field_t* fields;        /*!< 结构化字段数组；无字段时为 NULL */
    size_t                field_count;   /*!< @p fields 的条目数 */
} hplogc_event_t;

/* ============================ Sink 契约（可扩展 ABI） ============================ */

struct hplogc_sink;

/**
 * @brief sink 句柄（不透明指针）。
 *
 * 内部结构对外不可见（规范 §15：运行时句柄一律使用 opaque pointer），私有数据
 * 通过 `hplogc_sink_priv()` 访问。
 */
typedef struct hplogc_sink hplogc_sink_t;

/**
 * @brief sink 操作表（vtable）。
 *
 * **ABI 契约（规范性）**：
 * - 字段顺序在 @c HPLOGC_SINK_ABI_VERSION 主版本内冻结；新增回调只能追加到
 *   @p reserved 之前（即消费 @p reserved 槽位）。
 * - 注册时必须填写 @ref HPLOGC_SINK_ABI_VERSION 到 @p abi_version。
 * - @p type 必须在当前进程注册表内唯一且长度不超过 `HPLOGC_MAX_NAME_LEN`。
 *
 * **生命周期顺序（规范性）**：
 * @code
 *   create -> configure* -> init -> start -> emit / emit_batch* -> flush -> destroy
 * @endcode
 *
 * @warning `init` 在**全部** `configure` 调用之后才执行，因此 `init` 内只能写
 *          `if (p->field == 0) p->field = default;` 形式的默认值填充，
 *          **不得无条件赋值**，否则会把已读入的配置值清零。
 *
 * @warning 回调中禁止调用除 `hplogc_sink_*` 之外的库 API（避免重入）；除首次
 *          私有初始化外，回调内不得动态分配内存（规范 §9 稳态零 malloc）。
 */
typedef struct hplogc_sink_ops {
    const char* type;       /*!< 配置里的 type 名，例如 "rollingfile" */
    uint32_t    abi_version;/*!< ABI 版本号；填 @ref HPLOGC_SINK_ABI_VERSION */
    uint32_t    caps;       /*!< `HPLOGC_CAP_*` 组合 */
    size_t      priv_size;  /*!< 私有数据大小；核心在 create 时分配并清零，0 表示无需私有数据 */

    /**
     * @brief 逐项灌入配置；可为 NULL（该 sink 无私有配置项）。
     *
     * 核心已先行消费通用键（`enabled` / `async`，见规范 §10.3），其余键原样交给
     * 本回调。未识别的键必须返回非 0，由核心告警（并视 `strict init` 决定成败）。
     *
     * @return 0 成功；非 0 表示该键非法或不可识别。
     */
    int  (*configure )(struct hplogc_sink* sink, const char* key, const char* val);

    /**
     * @brief 默认填充与资源预检；可为 NULL。
     *
     * 在所有 `configure` 之后、首次 `emit` 之前调用一次。
     *
     * @return 0 成功；非 0 导致该 sink 实例启动失败（进而使 init / 热加载失败）。
     */
    int  (*init      )(struct hplogc_sink* sink);

    /**
     * @brief 打开真实资源（文件、连接）；可为 NULL（`init` 已包含打开动作）。
     *
     * @return 0 成功；非 0 视为 I/O 失败。
     */
    int  (*start     )(struct hplogc_sink* sink);

    /**
     * @brief 同步路径：写出单条事件；可为 NULL（`emit` 与 `emit_batch` 必须至少实现其一）。
     *
     * 在同步模式下由调用方线程执行；不得返回失败，失败通过 per-sink 统计计数暴露。
     */
    void (*emit      )(struct hplogc_sink* sink, const hplogc_event_t* ev);

    /**
     * @brief 异步路径：批量写出。可为 NULL，此时核心退化为循环调用 `emit`。
     *
     * 网络型 / 聚合型 sink 必须实现本回调：一次往返写一批。
     *
     * @return 成功写出的条数（`0 <= n <= 入参 n`）；负值表示整批失败。核心据此记账：
     *         部分成功时 `n` 计入该 sink 的 `written`、剩余 `入参 n - n` 计入 `failed`；
     *         整批失败（负值）时全部计入 `failed`（规范 §4.10.3）。
     */
    int  (*emit_batch)(struct hplogc_sink* sink, const hplogc_event_t* const* evs, size_t n);

    /**
     * @brief 冲刷缓冲；可为 NULL（无缓冲语义）。
     *
     * @return 0 成功；非 0 表示仍有残留或失败。
     */
    int  (*flush     )(struct hplogc_sink* sink);

    /**
     * @brief 释放私有资源；可为 NULL。核心在调用本回调后释放实例本体。
     *
     * @note 本回调由核心保证**幂等**：`destroy` 返回后不得再次触碰 @p sink。
     */
    void (*destroy   )(struct hplogc_sink* sink);

    void (*reserved[4])(void); /*!< ABI 扩展槽；必须全部为 NULL */
} hplogc_sink_ops_t;

/**
 * @brief 单个 sink 实例的运行统计（可从任意线程读取，内部原子读取）。
 */
typedef struct {
    unsigned long long written;       /*!< 该 sink 已成功写出的记录数 */
    unsigned long long dropped;       /*!< 投递到该 sink 前被丢弃的记录数（队列满 / 异步背压） */
    unsigned long long failed;        /*!< 写失败（含重试后仍失败）的记录数 */
    unsigned long long fields_dropped;/*!< 被丢弃 / 被截断的结构化字段数；三种成因均计入：
                                      *   字段数超 HPLOGC_MAX_FIELDS、字符串超
                                      *   HPLOGC_MAX_FIELD_STR_LEN 被截断、整体超
                                      *   max_log_length 预算（规范 §4.11.2） */
    unsigned long long bytes_written; /*!< 累计写出字节数 */
} hplogc_sink_stats_t;

/* ============================ 配置数据结构 ============================ */

/**
 * @brief 通用键值对，用于承载无法用结构体字段穷举的配置项。
 *
 * v0.1 里"只能通过配置文件设置"的键（时区、时间格式、编码、换行、pid/tid 格式、
 * 热加载间隔、限流参数、stats 项等）在 v0.2 统一经本结构表达，从而消除
 * "min 预设（INI=OFF）无法设置这些项"的结构性缺陷。
 */
typedef struct {
    const char* key;   /*!< 配置键名（配置文件同名，采用空格分词风格） */
    const char* value; /*!< 配置值；不得为 NULL（NULL 视为无效键，init 返回
                        *   `HPLOGC_ERR_INVALID_ARG`） */
} hplogc_kv_t;

/**
 * @brief sink 实例的代码内配置。
 *
 * @p type 必须是已注册的 sink 类型名（内置或经 `hplogc_sink_register()` 注册）；
 * 未注册 / 被裁剪的类型在 init 时返回 `HPLOGC_ERR_UNSUPPORTED`（fail-fast）。
 */
typedef struct {
    const char*    type;         /*!< sink 类型名，例如 "console" / "rollingfile" */
    const char*    name;         /*!< 实例名，供 @ref hplogc_rule_t.sinks 引用 */
    int            enabled;      /*!< 是否启用，默认 1；0 时条目存在但不创建实例 */
    int            async;        /*!< 是否走异步批量路径；-1 表示由 caps 决定（默认 -1） */
    const hplogc_kv_t* options;  /*!< 私有配置键值对数组（如 path / rotate / max size） */
    size_t         option_count; /*!< @p options 中的条目数，上限 HPLOGC_MAX_OPTIONS */
} hplogc_sink_config_t;

/**
 * @brief 路由规则（代码内配置）。
 *
 * 规则自上而下依次匹配：第一条 category 选择器命中、且级别区间包含该记录级别的
 * 规则生效（仅生效一条）。@p sinks 引用 `hplogc_sink_config_t::name`。
 * @p format 只允许使用五个内置格式名。
 */
typedef struct {
    const char*       category;    /*!< 精确名称 / "name.*" / "*"；NULL 或 "" 等价于 "*" */
    hplogc_level_t    min_level;   /*!< 下界（含）；min > max 属于配置错误 */
    hplogc_level_t    max_level;   /*!< 上界（含） */
    const char*       format;      /*!< "minimal"/"standard"/"categorized"/"detailed"/"json" */
    const char* const* sinks;      /*!< sink 实例名数组 */
    size_t            sink_count;  /*!< @p sinks 中的条目数 */
} hplogc_rule_t;

/**
 * @brief 代码内配置。
 *
 * 在传给 `hplogc_init()` 之前，必须先用 `hplogc_config_default()` 初始化
 * （或显式地为每一个字段赋值）；未触及字段的内容是未定义的。非法取值会导致
 * `hplogc_init()` 以 `HPLOGC_ERR_INVALID_ARG` 或 `HPLOGC_ERR_CONFIG` 失败
 * （不做钳制）。
 *
 * @note 结构体数组 / 字符串只在本次 init 调用期间被读取；init 返回后调用方可释放。
 *
 * @warning 本结构相对 v0.1 发生了**破坏性变更**：`outputs` 数组已被 `sinks`
 *          取代，`hplogc_output_t` 整体移除。迁移方式见 "MIGRATION" 说明。
 */
typedef struct {
    /* ABI 自检（必须在首字段） */
    uint32_t             struct_size;   /*!< 调用方填写 sizeof(hplogc_config_t)；
                                         *   核心据此判定结构体版本兼容性 */

    /* 级别与回退配置 */
    hplogc_level_t       level;            /*!< 全局过滤阈值，默认 INFO */
    const char*          default_format;   /*!< 回退格式名；NULL 表示 "standard" */
    const char* const*   default_sinks;    /*!< 回退 sink 实例名；NULL 或空表示丢弃未命中的记录 */
    size_t               default_sink_count; /*!< @p default_sinks 中的条目数 */

    /* sink 与规则 */
    const hplogc_sink_config_t* sinks;     /*!< sink 实例数组（调用者持有，init 时被拷贝） */
    size_t sink_count;                     /*!< sink 实例数量，上限 HPLOGC_MAX_SINKS */
    const hplogc_rule_t*   rules;          /*!< 规则数组（调用者持有，init 时被拷贝） */
    size_t rule_count;                     /*!< 规则数量，上限 HPLOGC_MAX_RULES */

    /* 全局运行时键（替代 v0.1 中"仅配置文件可设"的键） */
    const hplogc_kv_t*   global_options;   /*!< 全局配置键值对（见 @ref hplogc_kv_t） */
    size_t               global_option_count; /*!< @p global_options 条目数，上限 HPLOGC_MAX_OPTIONS */

    /* 环形缓冲区 */
    size_t                       buffer_size;     /*!< 字节数，默认 1MB，取值范围 4KB~1GB；
                                                  *   小于 2 x max_log_length 时会被上调 */
    hplogc_overflow_policy_t     overflow_policy; /*!< 溢出策略，默认丢弃 */

    /* 异步相关（构建时 HPLOGC_ENABLE_ASYNC=OFF 时忽略） */
    uint32_t             batch_size;          /*!< 消费者批量大小，默认 64 */
    uint32_t             flush_interval_ms;   /*!< 强制提交间隔（毫秒），默认 100 */
    uint32_t             shutdown_timeout_ms; /*!< 关闭时的排空超时（毫秒），默认 5000，0 表示一直等待 */

    /* 高级选项 */
    int                  escape_injection;    /*!< 是否转义 %msg 中的换行符 / ANSI 序列，默认 1 */
    size_t               max_log_length;      /*!< 消息体与整行的最大字节数，默认 4096，
                                              *   取值范围 256~65536；字段占用同一预算 */
    const char*          truncation_marker;   /*!< 整行被截断时追加的标记，默认 "...[TRUNCATED]" */
    hplogc_crash_safety_t crash_safety;      /*!< 全局 fsync 策略，默认 shutdown */
    int                  signal_safe;         /*!< 是否启用 hplogc_log_signal_safe 输出，默认 0 */
} hplogc_config_t;

/* ============================ 初始化 / 关闭 ============================ */

/**
 * @brief 根据调用者提供的配置初始化日志库。
 *
 * @param cfg  配置；NULL 等价于 `hplogc_init_default()`。
 *             非 NULL 时，它必须先经过 `hplogc_config_default()` 初始化，
 *             且 `cfg->struct_size` 必须为 `sizeof(hplogc_config_t)`。
 *             字符串与数组仅在本调用期间被读取。
 * @return     成功返回 `HPLOGC_OK`；失败返回负的 `hplogc_error_t` 值。
 *             失败时库仍处于未初始化状态，本次尝试中打开的所有资源都会被释放。
 *
 * @note 与其他 API 调用之间非线程安全：调用者必须保证初始化完成之后，
 *       其他线程才会使用本库。
 *       已初始化时再次调用 init 会返回 `HPLOGC_ERR_STATE`，并且不影响正在运行的实例。
 */
HPLOGC_API int hplogc_init(const hplogc_config_t* cfg);

/**
 * @brief 根据 INI 配置文件初始化日志库。
 *
 * @param config_path  配置文件路径；NULL 非法，将得到 `HPLOGC_ERR_INVALID_ARG`。
 * @return             成功返回 `HPLOGC_OK`；解析 / 校验失败返回
 *                     `HPLOGC_ERR_CONFIG`（含文件名与行号的诊断信息输出到 stderr）；
 *                     输出目标无法打开时返回 `HPLOGC_ERR_IO`；
 *                     引用了未注册 / 被裁剪的 sink 类型时返回
 *                     `HPLOGC_ERR_UNSUPPORTED`；其他负值参见 `hplogc_error_t`。
 * @see hplogc_init()
 */
HPLOGC_API int hplogc_init_from_file(const char* config_path);

/**
 * @brief 使用内置默认配置初始化日志库。
 *
 * 默认语义：level=INFO、"standard" 格式、一个内置的 stderr console sink、
 * 1MB 缓冲区、丢弃式溢出策略。
 *
 * @return 成功返回 `HPLOGC_OK`，否则返回负值错误码。
 */
HPLOGC_API int hplogc_init_default(void);

/**
 * @brief 用默认值填充配置结构体。
 *
 * 同时把 `struct_size` 置为 `sizeof(hplogc_config_t)`。
 *
 * @param cfg  待填充的结构体；传入 NULL 时安全忽略。
 *
 * @note 调用者必须先执行本函数（或显式设置每一个字段），之后才能把该结构体传给
 *       `hplogc_init()`。
 */
HPLOGC_API void hplogc_config_default(hplogc_config_t* cfg);

/**
 * @brief 关闭日志库并释放所有资源。
 *
 * 先冲刷并排空队列（最多等待 `shutdown_timeout_ms`），然后 flush 并 destroy
 * 所有 sink 实例、释放内存。重复调用 shutdown 是安全的（幂等）。
 * 相对于并发的日志写入不是线程安全的。
 */
HPLOGC_API void hplogc_shutdown(void);

/* ============================ 冲刷 / 同步 ============================ */

/**
 * @brief 将队列中待写的日志刷到各自的 sink（不做 fsync）。
 *
 * 异步模式下，本函数通知消费者提交所有已入队的记录并对每个 sink 调用 `flush`；
 * 同步模式下则刷新 stdio 流。
 * 库未初始化时返回 `HPLOGC_ERR_STATE`。
 *
 * @return 成功返回 `HPLOGC_OK`，否则返回负值错误码。
 */
HPLOGC_API int hplogc_flush(void);

/**
 * @brief 与 `hplogc_flush()` 相同，并额外对所有支持 fsync 的 sink 执行 fsync。
 *
 * @return 成功返回 `HPLOGC_OK`，否则返回负值错误码。
 */
HPLOGC_API int hplogc_sync(void);

/* ============================ 运行时控制 ============================ */

/**
 * @brief 在运行时修改全局过滤阈值。
 *
 * @param level  新的阈值；`HPLOGC_LEVEL_OFF` 关闭全部输出。
 * @return       成功返回 `HPLOGC_OK`；未初始化时返回 `HPLOGC_ERR_STATE`；
 *               level 不是合法阈值时（高于 `HPLOGC_LEVEL_OFF`）返回
 *               `HPLOGC_ERR_INVALID_ARG`。
 */
HPLOGC_API int hplogc_set_level(hplogc_level_t level);

/**
 * @brief 为单个 category 覆盖过滤阈值。
 *
 * 覆盖语义：一旦某个 category 拥有自己的级别，该级别优先于全局阈值，直到调用
 * `hplogc_clear_level_for_category()` 撤销覆盖。
 *
 * @param category  category 名称；传入 NULL 返回 `HPLOGC_ERR_INVALID_ARG`。
 * @param level     该 category 的新阈值（TRACE..OFF）。
 * @return          成功返回 `HPLOGC_OK`，否则返回负值错误码。
 * @note            构建时 `HPLOGC_ENABLE_CATEGORY=OFF` 时本函数为 stub，恒返回
 *                   `HPLOGC_ERR_CONFIG`（规范 §4.8）。
 * @see hplogc_clear_level_for_category()
 */
HPLOGC_API int hplogc_set_level_for_category(const char* category,
                                              hplogc_level_t level);

/**
 * @brief 撤销某个 category 的级别覆盖，回退到全局阈值。
 *
 * 该 category 未设置过覆盖时返回 `HPLOGC_OK`（幂等）。
 *
 * @note v0.1 头文件曾用 `(hplogc_level_t)-1` 作为清除哨兵，但该语义从未进入规范；
 *       本版改为显式 API，避免在公共契约中依赖魔法值。
 *
 * @param category  category 名称；传入 NULL 返回 `HPLOGC_ERR_INVALID_ARG`。
 * @return          成功返回 `HPLOGC_OK`，否则返回负值错误码。
 * @note            构建时 `HPLOGC_ENABLE_CATEGORY=OFF` 时本函数为 stub，恒返回
 *                   `HPLOGC_ERR_CONFIG`（规范 §4.8）。
 */
HPLOGC_API int hplogc_clear_level_for_category(const char* category);

/**
 * @brief 快速预检：给定 category 与级别，判断是否会通过过滤。
 *
 * 用于在构造昂贵参数之前短路，等价于常见日志库的 "isEnabled" 语义：
 * 结果为假时调用日志写入 API 必定被丢弃。
 *
 * @param level     目标级别。
 * @param category  category 名称；NULL / "" 等价于 "*"。
 * @return          非零表示该条日志会被处理；未初始化时返回 0（不返回错误）。
 */
HPLOGC_API int hplogc_level_enabled(hplogc_level_t level, const char* category);

/* ============================ Sink 注册与生命周期 ============================ */

/**
 * @brief 注册一种 sink 类型（自定义 sink 的接入点）。
 *
 * 注册后即可在配置文件的 `[outputs]` 中通过 `type = <ops->type>` 引用。
 * 必须在 `hplogc_init*()` **之前**调用；已初始化后调用返回
 * `HPLOGC_ERR_STATE`。
 *
 * @param ops  操作表；不得为 NULL，且 `ops->type`、`abi_version` 必须有效，
 *             `type` 不得重复注册，必须至少实现 `emit` 或 `emit_batch` 之一。
 *             核心保存该指针（调用方须保证其生命周期覆盖整个库使用期），建议指向
 *             静态常量。
 * @return     成功返回 `HPLOGC_OK`；重复注册返回 `HPLOGC_ERR_INVALID_ARG`；
 *             ABI 版本不匹配或类型表已满返回 `HPLOGC_ERR_UNSUPPORTED`；
 *             内存不足返回 `HPLOGC_ERR_NO_MEM`。
 */
HPLOGC_API int hplogc_sink_register(const hplogc_sink_ops_t* ops);

/**
 * @brief 注销一种 sink 类型。
 *
 * 仅允许注销**尚未被任何实例使用**的类型；存在活跃实例时返回 `HPLOGC_ERR_STATE`。
 *
 * @param type  sink 类型名；NULL 返回 `HPLOGC_ERR_INVALID_ARG`。
 * @return      成功返回 `HPLOGC_OK`；未注册的类型返回 `HPLOGC_ERR_UNSUPPORTED`。
 */
HPLOGC_API int hplogc_sink_unregister(const char* type);

/**
 * @brief 查询某种 sink 类型是否已注册。
 *
 * @param type  sink 类型名。
 * @return      已注册返回 `HPLOGC_OK`；未注册（含被构建裁剪的内置类型）返回
 *              `HPLOGC_ERR_UNSUPPORTED`。可在 init 之前调用，用于特性探测。
 */
HPLOGC_API int hplogc_sink_is_registered(const char* type);

/**
 * @brief 手动创建一个 sink 实例（不走配置文件时使用）。
 *
 * 创建后依次调用 `hplogc_sink_configure()` 灌入私有配置，最后调用
 * `hplogc_sink_start()`。`init` 回调由 `start` 内部在全部 `configure` 之后触发。
 *
 * @param type  sink 类型名；未注册返回 `HPLOGC_ERR_UNSUPPORTED`。
 * @param name  实例名，须在库内唯一且长度不超过 `HPLOGC_MAX_NAME_LEN`。
 * @param out   成功时接收新创建的句柄；不得为 NULL。
 * @return      成功返回 `HPLOGC_OK`，否则返回负值错误码。
 */
HPLOGC_API int hplogc_sink_create(const char* type, const char* name,
                                   hplogc_sink_t** out);

/**
 * @brief 向 sink 实例灌入一项私有配置。
 *
 * 必须在 `hplogc_sink_start()` 之前调用；未识别的键由实现返回非 0。
 *
 * @param sink  已创建但尚未 start 的实例。
 * @param key   配置键名。
 * @param val   配置值。
 * @return      成功返回 `HPLOGC_OK`；键非法返回 `HPLOGC_ERR_CONFIG`；
 *              键不被该 sink 识别返回 `HPLOGC_ERR_UNSUPPORTED`。
 */
HPLOGC_API int hplogc_sink_configure(hplogc_sink_t* sink,
                                      const char* key, const char* val);

/**
 * @brief 启动 sink 实例：调用 `init` 回调（填入默认值）后调用 `start` 回调。
 *
 * @param sink  已 configure 完成的实例。
 * @return      成功返回 `HPLOGC_OK`；资源打开失败返回 `HPLOGC_ERR_IO`；
 *              已启动过返回 `HPLOGC_ERR_STATE`。
 */
HPLOGC_API int hplogc_sink_start(hplogc_sink_t* sink);

/**
 * @brief 取得 sink 实例的私有数据指针。
 *
 * 大小由 `hplogc_sink_ops_t::priv_size` 决定，已清零；`priv_size` 为 0 或
 * sink 为 NULL 时返回 NULL。
 *
 * @param sink  sink 实例句柄。
 * @return      私有数据指针。
 */
HPLOGC_API void* hplogc_sink_priv(hplogc_sink_t* sink);

/**
 * @brief 冲刷单个 sink 的缓冲（不 fsync）。
 *
 * @param sink  sink 实例句柄；NULL 返回 `HPLOGC_ERR_INVALID_ARG`。
 * @return      成功返回 `HPLOGC_OK`；该 sink 未声明缓冲语义时返回
 *              `HPLOGC_ERR_UNSUPPORTED`（非错误，不产生日志，规范 §7.5）。
 */
HPLOGC_API int hplogc_sink_flush(hplogc_sink_t* sink);

/**
 * @brief 读取单个 sink 的统计快照。
 *
 * @param sink   sink 实例句柄；NULL 返回 `HPLOGC_ERR_INVALID_ARG`。
 * @param stats  输出快照；NULL 返回 `HPLOGC_ERR_INVALID_ARG`。
 * @return       成功返回 `HPLOGC_OK`；库未初始化返回 `HPLOGC_ERR_STATE`。
 */
HPLOGC_API int hplogc_sink_get_stats(hplogc_sink_t* sink,
                                      hplogc_sink_stats_t* stats);

/**
 * @brief 按实例名查找 sink 句柄。
 *
 * @param name  实例名；NULL 或未找到均返回 NULL；库未初始化时返回 NULL。
 * @return      sink 句柄或 NULL。仅用于观测用途（如读取统计），不得长期缓存——
 *              实例可能在热加载中被 destroy，长期持有会悬垂（规范 §7.5、§10.5）。
 */
HPLOGC_API hplogc_sink_t* hplogc_sink_find(const char* name);

/**
 * @brief 销毁 sink 实例：flush 后调用 `destroy` 回调并释放资源。
 *
 * @note 由 `hplogc_init*()` 创建的实例由 `hplogc_shutdown()` 统一销毁；
 *       本函数供手动 `hplogc_sink_create()` 的场景使用。对 NULL 安全。
 *
 * @param sink  sink 实例句柄。
 */
HPLOGC_API void hplogc_sink_destroy(hplogc_sink_t* sink);

/* ============================ 构建信息 ============================ */

/**
 * @brief 库构建信息的静态描述。
 *
 * @note 本结构只允许**尾部追加**字段（规范 §15 ABI 稳定性要求）。
 */
typedef struct {
    const char* build_version; /*!< "full" / "min" / "sync_thread" / "async_single" / "custom" */
    int         has_async;     /*!< 非零表示编译时包含了异步消费者 */
    int         has_color;     /*!< 非零表示编译时包含了 ANSI 颜色支持 */
    int         has_rotate;    /*!< 非零表示编译时包含了日志轮转 */
    int         has_hot_reload;/*!< 非零表示编译时包含了配置热重载 */
    int         has_category;  /*!< 非零表示编译时包含了 category 路由 */
    int         has_throttle;  /*!< 非零表示编译时包含了限流 / 采样 */
    int         has_ini;       /*!< 非零表示编译时包含了 INI 解析器 */
    int         lockfree;      /*!< 非零表示编译时包含了无锁环形缓冲区 */
    const char* concurrency;   /*!< "spsc" 或 "mpsc" */
    int         has_fields;    /*!< 非零表示支持结构化字段通道（v0.2 起恒为 1） */
    const char* sinks;         /*!< 已编译进本库的 sink 类型列表，逗号分隔（如 "console,rollingfile"） */
} hplogc_build_info_t;

/**
 * @brief 查询所链接库的构建配置。
 *
 * @param info  用于填充构建描述；传入 NULL 时安全忽略。
 *              可在任意时刻调用（无需初始化）。
 */
HPLOGC_API void hplogc_get_build_info(hplogc_build_info_t* info);

/* ============================ 统计信息 ============================ */

/**
 * @brief 运行时计数器快照（原子读取，线程安全）。
 *
 * @note 本结构只允许**尾部追加**字段。per-sink 统计不在此结构中，见
 *       `hplogc_sink_get_stats()`。
 */
typedef struct {
    unsigned long long accepted;    /*!< 通过过滤并进入队列 / 流水线的记录数 */
    unsigned long long dropped;     /*!< 溢出丢弃策略导致的丢弃 + 运行时写失败导致的丢弃 */
    unsigned long long overwritten; /*!< 被覆盖策略移除的记录数 */
    unsigned long long throttled;   /*!< 被限流 / 采样丢弃的记录数（限流关闭时恒为 0） */
    unsigned long long written;     /*!< 已写出的记录数（按记录计数，而非按 sink 计数） */
    size_t             buffer_used; /*!< 环形缓冲区当前已占用的字节数 */
    size_t             buffer_size; /*!< 环形缓冲区总大小（字节） */
    unsigned long long fields_dropped; /*!< 被丢弃 / 被截断的结构化字段数；三种成因均计入：
                                        *   超数量（HPLOGC_MAX_FIELDS）、超长度
                                        *   （HPLOGC_MAX_FIELD_STR_LEN 截断）、超
                                        *   max_log_length 预算（规范 §4.11.2） */
} hplogc_stats_t;

/**
 * @brief 读取运行时统计信息的一致性快照。
 *
 * @param stats  输出的快照；传入 NULL 返回 `HPLOGC_ERR_INVALID_ARG`。
 * @return       成功返回 `HPLOGC_OK`；未初始化时返回 `HPLOGC_ERR_STATE`。
 */
HPLOGC_API int hplogc_get_stats(hplogc_stats_t* stats);

/* ============================ 日志写入 ============================ */

/**
 * @brief 写入一条日志记录（printf 风格）。
 *
 * 该调用不会向调用者返回失败：非法的级别、未初始化状态、配置错误以及队列溢出
 * 都会按配置的策略被静默处理，并反映在 `hplogc_get_stats()` 中。
 * 调用者的 errno 会被保留。
 *
 * @param level     记录的严重程度；`HPLOGC_LEVEL_OFF` 或非法值会被静默丢弃。
 * @param category  category 名称（任意非空字节串，'.' 为层级分隔符）；
 *                  NULL / "" 等价于 "*"。
 * @param file      源文件名（可为 NULL）。
 * @param line      源文件行号。
 * @param func      函数名（可为 NULL）。
 * @param fmt       printf 风格格式串；切勿把用户数据作为格式串传入。
 */
HPLOGC_API void hplogc_log(hplogc_level_t level, const char* category,
                            const char* file, int line, const char* func,
                            const char* fmt, ...) HPLOGC_PRINTF(6, 7);

/**
 * @brief `hplogc_log()` 的 va_list 版本。
 *
 * @param level     严重程度；参见 `hplogc_log()`。
 * @param category  category 名称；参见 `hplogc_log()`。
 * @param file      源文件名（可为 NULL）。
 * @param line      源文件行号。
 * @param func      函数名（可为 NULL）。
 * @param fmt       printf 风格格式串。
 * @param ap        参数列表；只被消费一次。
 */
HPLOGC_API void hplogc_vlog(hplogc_level_t level, const char* category,
                             const char* file, int line, const char* func,
                             const char* fmt, va_list ap);

/**
 * @brief 写入一条带结构化字段的日志记录（printf 风格）。
 *
 * 字段（含字段名 `key` 与字符串值）连同消息体在生产者线程被深拷贝进环形缓冲
 * （见 @ref hplogc_field_t），因此字符串可以指向栈上的临时缓冲。
 * 字段数与字符串长度分别受 `HPLOGC_MAX_FIELDS`、`HPLOGC_MAX_FIELD_STR_LEN`
 * 约束，且整体占用 `max_log_length` 预算；超限部分被丢弃并计入
 * `hplogc_stats_t::fields_dropped`。
 * 调用者的 errno 会被保留。
 *
 * @param level       记录的严重程度。
 * @param category    category 名称；NULL / "" 等价于 "*"。
 * @param file        源文件名（可为 NULL）。
 * @param line        源文件行号。
 * @param func        函数名（可为 NULL）。
 * @param fields      字段数组；可为 NULL（此时等价于 `hplogc_log()`）。
 * @param field_count @p fields 的条目数；超过 `HPLOGC_MAX_FIELDS` 的部分被丢弃。
 * @param fmt         printf 风格格式串。
 */
HPLOGC_API void hplogc_log_fields(hplogc_level_t level, const char* category,
                                  const char* file, int line, const char* func,
                                  const hplogc_field_t* fields, size_t field_count,
                                  const char* fmt, ...) HPLOGC_PRINTF(8, 9);

/**
 * @brief `hplogc_log_fields()` 的 va_list 版本。
 *
 * @param level       严重程度。
 * @param category    category 名称。
 * @param file        源文件名（可为 NULL）。
 * @param line        源文件行号。
 * @param func        函数名（可为 NULL）。
 * @param fields      字段数组（可为 NULL）。
 * @param field_count 字段条目数。
 * @param fmt         printf 风格格式串。
 * @param ap          参数列表；只被消费一次。
 */
HPLOGC_API void hplogc_vlog_fields(hplogc_level_t level, const char* category,
                                   const char* file, int line, const char* func,
                                   const hplogc_field_t* fields, size_t field_count,
                                   const char* fmt, va_list ap);

/**
 * @brief 异步信号安全的受限日志写入。
 *
 * 本函数始终是异步信号安全的：不加锁、不做格式化、不分配内存；它只是追加一个
 * 级别标签，然后通过 `write(2)` 把消息直接写到 stderr。它会保存并恢复 errno。
 * 当 `signal_safe` 配置开关为假（默认）时，该调用是静默的空操作。
 *
 * @note 本函数不携带结构化字段：字段拷贝与环形缓冲入队均不满足 async-signal-safe
 *       要求。
 *
 * @param level  严重程度，仅用于标签。
 * @param msg    以 NUL 结尾的消息，须位于调用者提供的静态或栈缓冲区中。
 */
HPLOGC_API void hplogc_log_signal_safe(hplogc_level_t level, const char* msg);

/* ============================ 便捷宏（编译期裁剪点） ============================ */

/* 可变参数宏的可移植性处理（参见规范 §7.3）。
 *
 * `HPLOGC_VA_ARGS` 是为"零个可变参数"场景保留的兼容层（依赖 GNU `##__VA_ARGS__`
 * 的吞逗号技巧）。由于 log / log_fields 的 @p fmt 是**必需参数**，本头文件生成的
 * 便捷宏内部一律直接展开 `__VA_ARGS__`：既在所有受支持编译器上可用，又不会在
 * -pedantic 下触发 "requires at least one argument" 告警。
 * 调用方若自行封装零参宏，仍可复用 HPLOGC_VA_ARGS。 */
#if defined(__GNUC__) || defined(__clang__)
#  define HPLOGC_VA_ARGS(fmt, ...) fmt, ##__VA_ARGS__
#  define HPLOGC_HAS_VA_ARGS 1
#elif defined(_MSC_VER) && (!defined(_MSVC_TRADITIONAL) || _MSVC_TRADITIONAL)
#  define HPLOGC_VA_ARGS(fmt, ...) fmt, __VA_ARGS__
#  define HPLOGC_HAS_VA_ARGS 1
#elif defined(_MSC_VER) && defined(_MSVC_TRADITIONAL) && (_MSVC_TRADITIONAL == 0)
#  define HPLOGC_VA_ARGS(fmt, ...) fmt __VA_OPT__(,) __VA_ARGS__
#  define HPLOGC_HAS_VA_ARGS 1
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
#  define HPLOGC_VA_ARGS(fmt, ...) fmt __VA_OPT__(,) __VA_ARGS__
#  define HPLOGC_HAS_VA_ARGS 1
#else
#  define HPLOGC_VA_ARGS(fmt, ...) fmt, __VA_ARGS__
#  define HPLOGC_HAS_VA_ARGS 0
#endif

/** @cond INTERNAL */
#define HPLOGC__LOG(lvl, cat, ...) \
    hplogc_log(lvl, cat, __FILE__, __LINE__, __func__, __VA_ARGS__)

#define HPLOGC__LOG_F(lvl, cat, fields, nfields, ...) \
    hplogc_log_fields(lvl, cat, __FILE__, __LINE__, __func__, fields, nfields, \
                      __VA_ARGS__)
/** @endcond */

#if HPLOGC_COMPILE_TIME_LEVEL <= HPLOGC_CTL_TRACE
/** @brief 以 printf 风格的 @p fmt 向 @p cat 输出 TRACE 级别日志。 */
#define HPLOGC_TRACE(cat, ...) HPLOGC__LOG(HPLOGC_LEVEL_TRACE, cat, __VA_ARGS__)
/** @brief 带结构化字段的 TRACE 级别日志。 */
#define HPLOGC_TRACE_F(cat, fields, nfields, ...) \
    HPLOGC__LOG_F(HPLOGC_LEVEL_TRACE, cat, fields, nfields, __VA_ARGS__)
#else
#define HPLOGC_TRACE(cat, ...) ((void)0)
#define HPLOGC_TRACE_F(cat, fields, nfields, ...) ((void)0)
#endif

#if HPLOGC_COMPILE_TIME_LEVEL <= HPLOGC_CTL_DEBUG
/** @brief 以 printf 风格的 @p fmt 向 @p cat 输出 DEBUG 级别日志。 */
#define HPLOGC_DEBUG(cat, ...) HPLOGC__LOG(HPLOGC_LEVEL_DEBUG, cat, __VA_ARGS__)
/** @brief 带结构化字段的 DEBUG 级别日志。 */
#define HPLOGC_DEBUG_F(cat, fields, nfields, ...) \
    HPLOGC__LOG_F(HPLOGC_LEVEL_DEBUG, cat, fields, nfields, __VA_ARGS__)
#else
#define HPLOGC_DEBUG(cat, ...) ((void)0)
#define HPLOGC_DEBUG_F(cat, fields, nfields, ...) ((void)0)
#endif

#if HPLOGC_COMPILE_TIME_LEVEL <= HPLOGC_CTL_INFO
/** @brief 以 printf 风格的 @p fmt 向 @p cat 输出 INFO 级别日志。 */
#define HPLOGC_INFO(cat, ...) HPLOGC__LOG(HPLOGC_LEVEL_INFO, cat, __VA_ARGS__)
/** @brief 带结构化字段的 INFO 级别日志。 */
#define HPLOGC_INFO_F(cat, fields, nfields, ...) \
    HPLOGC__LOG_F(HPLOGC_LEVEL_INFO, cat, fields, nfields, __VA_ARGS__)
#else
#define HPLOGC_INFO(cat, ...) ((void)0)
#define HPLOGC_INFO_F(cat, fields, nfields, ...) ((void)0)
#endif

#if HPLOGC_COMPILE_TIME_LEVEL <= HPLOGC_CTL_WARN
/** @brief 以 printf 风格的 @p fmt 向 @p cat 输出 WARN 级别日志。 */
#define HPLOGC_WARN(cat, ...) HPLOGC__LOG(HPLOGC_LEVEL_WARN, cat, __VA_ARGS__)
/** @brief 带结构化字段的 WARN 级别日志。 */
#define HPLOGC_WARN_F(cat, fields, nfields, ...) \
    HPLOGC__LOG_F(HPLOGC_LEVEL_WARN, cat, fields, nfields, __VA_ARGS__)
#else
#define HPLOGC_WARN(cat, ...) ((void)0)
#define HPLOGC_WARN_F(cat, fields, nfields, ...) ((void)0)
#endif

#if HPLOGC_COMPILE_TIME_LEVEL <= HPLOGC_CTL_ERROR
/** @brief 以 printf 风格的 @p fmt 向 @p cat 输出 ERROR 级别日志。 */
#define HPLOGC_ERROR(cat, ...) HPLOGC__LOG(HPLOGC_LEVEL_ERROR, cat, __VA_ARGS__)
/** @brief 带结构化字段的 ERROR 级别日志。 */
#define HPLOGC_ERROR_F(cat, fields, nfields, ...) \
    HPLOGC__LOG_F(HPLOGC_LEVEL_ERROR, cat, fields, nfields, __VA_ARGS__)
#else
#define HPLOGC_ERROR(cat, ...) ((void)0)
#define HPLOGC_ERROR_F(cat, fields, nfields, ...) ((void)0)
#endif

#if HPLOGC_COMPILE_TIME_LEVEL <= HPLOGC_CTL_FATAL
/** @brief 以 printf 风格的 @p fmt 向 @p cat 输出 FATAL 级别日志。 */
#define HPLOGC_FATAL(cat, ...) HPLOGC__LOG(HPLOGC_LEVEL_FATAL, cat, __VA_ARGS__)
/** @brief 带结构化字段的 FATAL 级别日志。 */
#define HPLOGC_FATAL_F(cat, fields, nfields, ...) \
    HPLOGC__LOG_F(HPLOGC_LEVEL_FATAL, cat, fields, nfields, __VA_ARGS__)
#else
#define HPLOGC_FATAL(cat, ...) ((void)0)
#define HPLOGC_FATAL_F(cat, fields, nfields, ...) ((void)0)
#endif

/* ---- 级别预检宏（昂贵参数构造前短路） ---- */

/** @brief 预检 TRACE 是否对 @p cat 放行；等价于 `hplogc_level_enabled(HPLOGC_LEVEL_TRACE, cat)`。 */
#define HPLOGC_TRACE_ENABLED(cat) hplogc_level_enabled(HPLOGC_LEVEL_TRACE, cat)
/** @brief 预检 DEBUG 是否对 @p cat 放行。 */
#define HPLOGC_DEBUG_ENABLED(cat) hplogc_level_enabled(HPLOGC_LEVEL_DEBUG, cat)
/** @brief 预检 INFO 是否对 @p cat 放行。 */
#define HPLOGC_INFO_ENABLED(cat)  hplogc_level_enabled(HPLOGC_LEVEL_INFO, cat)
/** @brief 预检 WARN 是否对 @p cat 放行。 */
#define HPLOGC_WARN_ENABLED(cat)  hplogc_level_enabled(HPLOGC_LEVEL_WARN, cat)
/** @brief 预检 ERROR 是否对 @p cat 放行。 */
#define HPLOGC_ERROR_ENABLED(cat) hplogc_level_enabled(HPLOGC_LEVEL_ERROR, cat)
/** @brief 预检 FATAL 是否对 @p cat 放行。 */
#define HPLOGC_FATAL_ENABLED(cat) hplogc_level_enabled(HPLOGC_LEVEL_FATAL, cat)

/* ---- 语义子流：审计流与指标流 ---- */

/**
 * @brief 审计流埋点：固定 category 为 "audit"，级别 INFO，带结构化字段。
 *
 * 典型路由为「审计日志 → Kafka / S3 / 审计文件」，与普通业务日志分离。
 */
#define HPLOGC_AUDIT(fields, nfields, ...) \
    HPLOGC__LOG_F(HPLOGC_LEVEL_INFO, "audit", fields, nfields, __VA_ARGS__)

/**
 * @brief 指标流埋点：固定 category 为 "metric"，级别 INFO，带结构化字段。
 *
 * 典型路由为「指标 → Prometheus / OpenTelemetry」，不进入文本文件。
 */
#define HPLOGC_METRIC(fields, nfields, ...) \
    HPLOGC__LOG_F(HPLOGC_LEVEL_INFO, "metric", fields, nfields, __VA_ARGS__)

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* HPLOGC_H */
