/* -*- coding: utf-8 -*- */
/**
 * @file hplogc_internal.h
 * @brief hplogc 内部共享声明（不对外暴露）。
 *
 * 包含：运行时状态、配置快照、环形缓冲中的记录载荷布局、格式预编译结构，
 * 以及各内部模块之间的接口原型。
 *
 * @ingroup internal
 */

#ifndef HPLOGC_INTERNAL_H
#define HPLOGC_INTERNAL_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hplogc_atomic.h"
#include "hplogc_platform.h"
#include "ring.h"

#include <hplogc.h>

/** @brief 编译期可覆盖的 printf 属性（GCC/Clang；MSVC 为空）。 */
#ifndef HPLOGC_PRINTF
#if defined(__GNUC__) || defined(__clang__)
#define HPLOGC_PRINTF(fmt, va) __attribute__((format(printf, fmt, va)))
#else
#define HPLOGC_PRINTF(fmt, va)
#endif
#endif

/* ============================ 内部容量与默认值 ============================ */

/** @brief 命名格式表的最大条目数（实现上限）。 */
#define HPLOGC_MAX_FORMATS 32
/** @brief 单个格式模板预编译后的最大动作数。 */
#define HPLOGC_FMT_NODES 32
/** @brief 配置键名的最大长度（字节，含 NUL）。 */
#define HPLOGC_KV_KEY_LEN 48
/** @brief `truncation marker` 的内部存储上限。 */
#define HPLOGC_TRUNC_MARKER_MAX 64

/** @brief 默认缓冲区大小（1 MB）。 */
#define HPLOGC_DEFAULT_BUFFER_SIZE (1024u * 1024u)
/** @brief 缓冲区下限（4 KB）。 */
#define HPLOGC_MIN_BUFFER_SIZE 4096u
/** @brief 缓冲区上限（1 GB）。 */
#define HPLOGC_MAX_BUFFER_SIZE (1024u * 1024u * 1024u)
/** @brief 默认单条日志上限（4 KB）。 */
#define HPLOGC_DEFAULT_MAX_LOG_LEN 4096u
/** @brief 单条日志上限的下界。 */
#define HPLOGC_MIN_MAX_LOG_LEN 256u
/** @brief 单条日志上限的上界。 */
#define HPLOGC_MAX_MAX_LOG_LEN 65536u

/** @brief 物理行长度上限（§10.1）。 */
#define HPLOGC_CONF_LINE_MAX 1024

/** @brief 记录负载除字符串外的最大头部与字段记录开销（预算估算用）。 */
#define HPLOGC_REC_EXTRA 1024

/* ---- 枚举：内部取值 ---- */

/** @brief pid / tid 的显示格式。 */
typedef enum {
    HPLOGC_IDF_DECIMAL = 0, /*!< 十进制 */
    HPLOGC_IDF_HEX     = 1, /*!< 十六进制（带 0x 前缀） */
    HPLOGC_IDF_NONE    = 2  /*!< 空串 */
} hp_id_format_t;

/** @brief fork() 后子进程行为（§9）。 */
typedef enum {
    HPLOGC_FORK_REINIT  = 0, /*!< 惰性重建 */
    HPLOGC_FORK_DISABLE = 1, /*!< 子进程静默丢弃 */
    HPLOGC_FORK_INHERIT = 2  /*!< 继承父进程状态（不安全，仅调试） */
} hp_fork_behavior_t;

/** @brief 统计输出目标。 */
typedef enum {
    HPLOGC_STATS_OUT_STDERR = 0, /*!< stderr */
    HPLOGC_STATS_OUT_FILE   = 1  /*!< 文件（stats file） */
} hp_stats_out_t;

/** @brief 格式模板预编译后的占位符编号（§12.1）。 */
typedef enum {
    HP_PH_LEVEL = 0,    /*!< %level */
    HP_PH_TIME,         /*!< %time */
    HP_PH_PID,          /*!< %pid */
    HP_PH_TID,          /*!< %tid */
    HP_PH_FILE,         /*!< %file */
    HP_PH_LINE,         /*!< %line */
    HP_PH_FUNC,         /*!< %func */
    HP_PH_MSG,          /*!< %msg */
    HP_PH_CATEGORY,     /*!< %category */
    HP_PH_NEWLINE,      /*!< %n */
    HP_PH_COUNT         /*!< 占位符总数 */
} hp_ph_t;

/* ============================ 记录载荷布局 ============================ */

/**
 * @brief 环形缓冲中每条记录的**定长头部**（不含变长字符串）。
 *
 * 头部之后依次排列：msg、category、file、func（均含结尾 NUL），
 * 随后按 8 字节对齐排列 `hp_field_rec_t` 数组，最后是字段的 key / 字符串区。
 */
typedef struct {
    uint64_t ts_ns;       /*!< 时间戳（纳秒） */
    uint64_t tid;         /*!< 线程 ID */
    uint32_t pid;         /*!< 进程 ID */
    uint32_t level;       /*!< 级别 */
    int32_t  line;        /*!< 行号 */
    uint32_t msg_len;     /*!< msg 字节数（含 NUL） */
    uint32_t cat_len;     /*!< category 字节数（含 NUL） */
    uint32_t file_len;    /*!< file 字节数（含 NUL） */
    uint32_t func_len;    /*!< func 字节数（含 NUL） */
    uint32_t field_count; /*!< 字段数量 */
    uint32_t fields_dropped; /*!< 入队时因超量 / 超长 / 超预算被丢弃的字段数 */
    uint32_t reserved;       /*!< 对齐填充（保持头部 8 字节对齐） */
} hp_rec_hdr_t;

/**
 * @brief 记录中的单个字段记录（定长部分）。
 *
 * @p key_len / @p str_len 均为**含 NUL** 的字节数；非字符串类型 @p str_len 为 0。
 */
typedef struct {
    uint32_t key_len; /*!< key 字节数（含 NUL） */
    uint32_t str_len; /*!< 字符串值字节数（含 NUL），非字符串类型为 0 */
    uint32_t type;    /*!< `hplogc_vtype_t` */
    union {
        long long          i; /*!< HPLOGC_V_INT */
        unsigned long long u; /*!< HPLOGC_V_UINT */
        double             d; /*!< HPLOGC_V_DOUBLE */
        int                b; /*!< HPLOGC_V_BOOL */
    } v;                      /*!< 按 type 择一有效 */
} hp_field_rec_t;

/**
 * @brief 一条记录的解析视图（指针指向记录存储，仅在本次处理期间有效）。
 */
typedef struct {
    hp_rec_hdr_t*   h;     /*!< 定长头部 */
    char*           msg;   /*!< %msg 展开结果 */
    char*           cat;   /*!< category */
    char*           file;  /*!< 源文件 */
    char*           func;  /*!< 函数 */
    hp_field_rec_t* frec;  /*!< 字段记录数组（field_count 个） */
    char*           fblob; /*!< 字段 key / 字符串区 */
} hp_rec_view_t;

/** @brief 解析记录存储，填充视图。返回 0 成功，非 0 表示格式非法。 */
int hp_rec_parse(unsigned char* base, size_t len, hp_rec_view_t* out);

/** @brief 计算某条记录视图所需的总字节数（用于预算估算）。 */
size_t hp_rec_view_size(const hp_rec_view_t* v);

/* ============================ 配置数据结构 ============================ */

/** @brief 单个配置项（键 / 值）。 */
typedef struct {
    char key[HPLOGC_KV_KEY_LEN];      /*!< 键名 */
    char val[HPLOGC_MAX_FMT_LEN];     /*!< 值 */
} hp_kv_t;

/** @brief sink 实例的定义（配置文件条目或代码内配置的等价物）。 */
typedef struct {
    char    name[HPLOGC_MAX_NAME_LEN]; /*!< 实例名 */
    char    type[HPLOGC_MAX_NAME_LEN]; /*!< 类型名 */
    int     enabled;                   /*!< 是否启用 */
    int     async;                     /*!< -1 自动 / 0 同步 / 1 异步 */
    hp_kv_t opts[HPLOGC_MAX_OPTIONS];  /*!< 私有配置 */
    size_t  opt_count;                 /*!< 私有配置条目数 */
} hp_sink_def_t;

/** @brief 路由规则定义。 */
typedef struct {
    char    category[HPLOGC_MAX_NAME_LEN]; /*!< category 选择器 */
    int     min_level;                     /*!< 下界（含） */
    int     max_level;                     /*!< 上界（含） */
    char    format[HPLOGC_MAX_NAME_LEN];   /*!< 格式名 */
    char    sinks[HPLOGC_MAX_SINKS][HPLOGC_MAX_NAME_LEN]; /*!< sink 实例名 */
    size_t  sink_count;                    /*!< sink 数量 */
} hp_rule_def_t;

/** @brief 命名格式定义。 */
typedef struct {
    char name[HPLOGC_MAX_NAME_LEN];   /*!< 格式名 */
    char tmpl[HPLOGC_MAX_FMT_LEN];    /*!< 模板 */
} hp_format_def_t;

/**
 * @brief 运行时配置快照（配置文件与代码内配置的统一内部表示）。
 *
 * 热加载时整体重建一份，校验通过后原子替换（§10.5）。
 */
typedef struct {
    /* ---- [global] ---- */
    int               strict_init;        /*!< 未知键是否致命 */
    int               level;              /*!< 全局过滤阈值 */
    char              default_format[HPLOGC_MAX_NAME_LEN]; /*!< 兜底格式名 */
    char              default_sinks[HPLOGC_MAX_SINKS][HPLOGC_MAX_NAME_LEN];
    size_t            default_sink_count; /*!< 兜底 sink 数量 */
    int               utc;                /*!< 时区：非零为 UTC */
    int               ts_monotonic;       /*!< 时间戳源：非零为单调时钟 */
    char              time_format[HPLOGC_MAX_FMT_LEN]; /*!< 时间格式模板 */
    int               capture_source_loc; /*!< 运行时是否捕获源码位置 */
    int               newline;            /*!< `hplogc_newline_t` */
    int               pid_fmt;            /*!< `hp_id_format_t` */
    int               tid_fmt;            /*!< `hp_id_format_t` */
    unsigned          hot_reload_interval;/*!< 轮询间隔（秒），0 = 禁用 */
    int               signal_reload;      /*!< 是否注册 SIGHUP */

    /* ---- [formats] ---- */
    hp_format_def_t   formats[HPLOGC_MAX_FORMATS]; /*!< 命名格式 */
    size_t            format_count;                /*!< 格式条目数 */

    /* ---- [outputs] ---- */
    hp_sink_def_t     sinks[HPLOGC_MAX_SINKS]; /*!< sink 实例定义 */
    size_t            sink_count;               /*!< sink 实例数 */

    /* ---- [rules] ---- */
    hp_rule_def_t     rules[HPLOGC_MAX_RULES];  /*!< 路由规则定义 */
    size_t            rule_count;               /*!< 规则数 */

    /* ---- [buffer] ---- */
    size_t            buffer_size;          /*!< 环形缓冲字节数 */
    int               overflow_policy;      /*!< `hplogc_overflow_policy_t` */

    /* ---- [async] ---- */
    uint32_t          batch_size;           /*!< 批量条数 */
    uint32_t          flush_interval_ms;    /*!< 强制提交间隔 */
    uint32_t          shutdown_timeout_ms;  /*!< shutdown 排空超时 */

    /* ---- [throttle] ---- */
    unsigned          global_rate;          /*!< 全局令牌桶速率（条/秒） */
    unsigned          per_cat_rate;         /*!< 分类令牌桶速率（条/秒） */
    unsigned          burst;                /*!< 桶容量 */
    double            sampling_rate;        /*!< 采样率 0.0~1.0 */

    /* ---- [advanced] ---- */
    int               escape_injection;     /*!< 注入转义开关 */
    size_t            max_log_length;       /*!< 单条上限 */
    char              trunc_marker[HPLOGC_TRUNC_MARKER_MAX]; /*!< 截断标记 */
    int               fork_behavior;        /*!< `hp_fork_behavior_t` */
    int               signal_safe;          /*!< 是否允许 signal safe 输出 */
    int               crash_safety;         /*!< `hplogc_crash_safety_t` */
    unsigned          stats_interval;       /*!< 统计输出间隔（秒） */
    int               stats_output;         /*!< `hp_stats_out_t` */
    char              stats_file[HPLOGC_MAX_PATH_LEN]; /*!< 统计文件路径 */
} hp_config_t;

/* ============================ 预编译后的运行时 ============================ */

/** @brief 预编译格式（动作序列 + 字面文本池）。 */
typedef struct {
    char  name[HPLOGC_MAX_NAME_LEN]; /*!< 格式名 */
    int   is_json;                   /*!< 是否为 `json` 格式（§12.3，按名判定） */
    int   node_count;                /*!< 动作数 */
    struct {
        int             kind; /*!< 0 = 字面文本，1 = 占位符 */
        unsigned short  off;  /*!< 字面文本在 lits 中的偏移 / 占位符编号 */
        unsigned short  len;  /*!< 字面文本长度 */
    } nodes[HPLOGC_FMT_NODES];       /*!< 动作序列 */
    char  lits[HPLOGC_MAX_FMT_LEN];  /*!< 字面文本池 */
    int   lit_used;                  /*!< 字面文本池已用字节数 */
} hp_fmt_t;

/** @brief 预编译规则（索引化后的 sink 引用）。 */
typedef struct {
    char category[HPLOGC_MAX_NAME_LEN];  /*!< category 选择器 */
    int  min_level;                      /*!< 下界 */
    int  max_level;                      /*!< 上界 */
    int  fmt_idx;                        /*!< 格式索引，-1 表示未解析 */
    int  sink_idx[HPLOGC_MAX_SINKS];     /*!< sink 实例索引 */
    int  sink_count;                     /*!< sink 数量 */
} hp_rule_t;

/** @brief 类别登记表条目（per-category 级别覆盖与限流状态）。 */
typedef struct {
    char           name[HPLOGC_MAX_NAME_LEN]; /*!< 类别名 */
    int            used;                      /*!< 该槽位是否已占用 */
    int            level_set;                 /*!< 是否设置了级别覆盖 */
    int            level;                     /*!< 覆盖级别 */
    hp_atomic_u64  tokens;                    /*!< 分类令牌桶（放大 1000 倍） */
    hp_atomic_u64  last_ms;                   /*!< 上次补充时间（毫秒） */
} hp_cat_t;

/**
 * @brief 可整体替换的运行时集合（配置 + 预编译格式 / 规则 + sink 实例）。
 *
 * 热加载时新建一份并原子发布；旧实例进入回收队列，宽限期后销毁。
 */
typedef struct hp_runtime {
    hp_config_t       cfg;                       /*!< 配置快照 */
    hp_fmt_t          fmts[HPLOGC_MAX_FORMATS];  /*!< 预编译格式 */
    int               fmt_count;                 /*!< 格式数 */
    int               default_fmt_idx;           /*!< 兜底格式索引 */
    hp_rule_t         rules[HPLOGC_MAX_RULES];   /*!< 预编译规则 */
    int               rule_count;                /*!< 规则数 */
    struct hplogc_sink* sinks[HPLOGC_MAX_SINKS]; /*!< sink 实例 */
    int               sink_count;                /*!< sink 数 */
    int               default_sinks[HPLOGC_MAX_SINKS]; /*!< 兜底 sink 索引 */
    int               default_sink_count;        /*!< 兜底 sink 数 */
    struct hp_runtime* next_retired;             /*!< 回收队列链接 */
    uint64_t          retire_at_ms;              /*!< 可回收时刻（毫秒） */
    int               owned_sinks[HPLOGC_MAX_SINKS]; /*!< 本集合是否拥有该实例 */
} hp_runtime_t;

/**
 * @brief 全局运行时状态（不随热加载替换的部分）。
 */
typedef struct {
    hp_atomic_i32 init_state;   /*!< 0 未初始化 / 1 已初始化 / 2 关闭中 */
    hp_atomic_u64 active_ptr;   /*!< 当前生效集合的指针（原子发布） */
    hp_mutex_t    mgr_mu;       /*!< 替换 / shutdown 的串行化锁 */
    hp_runtime_t* retired;      /*!< 待回收集合链表 */

    hp_ring_t     ring;         /*!< 环形缓冲（ASYNC=OFF 时不初始化） */
    int           has_ring;     /*!< 是否使用了环形缓冲 */

    uint64_t      init_mono_ns; /*!< init 时的单调时钟（monotonic 时间戳基准） */

    hp_mutex_t    inst_mu;      /*!< 全局实例表锁（观测用） */
    struct hplogc_sink* insts[HPLOGC_MAX_SINKS]; /*!< 全局实例表 */
    int           inst_count;   /*!< 全局实例数 */

    hp_mutex_t    cat_mu;       /*!< 类别登记表锁 */
    hp_cat_t      cats[HPLOGC_MAX_CATEGORIES]; /*!< 类别登记表 */
    int           cat_count;    /*!< 已登记类别数 */

    hp_atomic_u64 accepted;        /*!< 通过过滤并入队的条数 */
    hp_atomic_u64 dropped;         /*!< 丢弃条数（按条计） */
    hp_atomic_u64 throttled;       /*!< 限流 / 采样丢弃条数 */
    hp_atomic_u64 written;         /*!< 已写出条数（按条计） */
    hp_atomic_u64 fields_dropped;  /*!< 被丢弃 / 截断的字段数 */

    /* 异步消费者 */
    hp_thread_t   consumer_th;      /*!< 消费者线程 */
    hp_atomic_i32 consumer_stop;    /*!< 停止标志 */
    int           consumer_started; /*!< 线程是否已创建 */
    hp_cond_t     consumer_cv;      /*!< 消费者等待条件 */
    hp_mutex_t    consumer_mu;      /*!< 配合 consumer_cv */
    size_t        flush_gen;        /*!< flush 请求代次（consumer_mu 保护） */
    size_t        flush_done_gen;   /*!< 已完成 flush 代次（consumer_mu 保护） */

    /* 热加载监视线程 */
    hp_thread_t   reload_th;      /*!< 监视线程 */
    hp_atomic_i32 reload_stop;    /*!< 停止标志 */
    int           reload_started; /*!< 线程是否已创建 */
    hp_atomic_i32 reload_signal;  /*!< SIGHUP 设置的原子标志 */
    hp_mutex_t    reload_mu;      /*!< 配合 reload_cv */
    hp_cond_t     reload_cv;      /*!< 热加载线程等待条件 */

    char          config_path[HPLOGC_MAX_PATH_LEN]; /*!< 配置文件路径 */
    int           has_config_path;                  /*!< 是否来自配置文件 */
    hp_config_t   saved_cfg;                        /*!< 代码内配置副本 */
    int           has_saved_cfg;                    /*!< 是否有副本 */

    hp_atomic_i32 signal_safe;      /*!< 是否允许 signal safe 输出 */
    hp_atomic_i32 forked_dirty;     /*!< fork 后需要惰性重建 */
    hp_atomic_i32 child_disabled;   /*!< 子进程中禁用日志 */
    hp_atomic_i32 need_reinit;      /*!< 子进程需惰性重新初始化（reinit） */

    hp_atomic_u64 sample_counter;   /*!< 确定性采样计数器 */
    hp_atomic_u64 g_tokens;         /*!< 全局令牌桶（放大 1000 倍） */
    hp_atomic_u64 g_last_ms;        /*!< 全局桶上次补充时间 */

    hp_tls_t      scratch_tls;      /*!< 每线程渲染缓冲 */
    int           tls_ready;        /*!< TLS 键是否创建 */
    size_t        scratch_size;     /*!< 单侧缓冲大小（2 * max_log_length） */

    hp_atomic_i32 stderr_seq;       /*!< 限频 stderr 告警的节流计数 */
    hp_atomic_u64 last_warn_ms;     /*!< 上次告警时间（毫秒） */
} hplogc_rt_t;

/** @brief 全局运行时实例。 */
extern hplogc_rt_t g_rt;

/* ============================ sink 实例 ============================ */

/**
 * @brief sink 实例（内部结构，公共层只持有不透明指针）。
 */
struct hplogc_sink {
    const hplogc_sink_ops_t* ops;              /*!< 操作表（核心不拷贝） */
    char        name[HPLOGC_MAX_NAME_LEN];     /*!< 实例名 */
    char        type[HPLOGC_MAX_NAME_LEN];     /*!< 类型名 */
    unsigned char* priv;                       /*!< 私有数据（清零） */
    uint32_t    caps;                          /*!< 能力位 */
    int         async_mode;                    /*!< -1 自动 / 0 同步 / 1 异步 */
    int         started;                       /*!< 是否已 start */
    int         fsync_level;                   /*!< 该实例的有效 fsync 严格度 */
    hp_mutex_t  io_mu;                         /*!< 行级原子写的串行化 */
    hp_kv_t     opts[HPLOGC_MAX_OPTIONS];      /*!< 配置快照（热加载复用比较） */
    size_t      opt_count;                     /*!< 配置条目数 */
    hp_atomic_u64 written;                     /*!< 成功写出条数 */
    hp_atomic_u64 dropped;                     /*!< 本 sink 丢弃条数 */
    hp_atomic_u64 failed;                      /*!< 写失败条数 */
    hp_atomic_u64 fields_dropped;              /*!< 字段丢弃数 */
    hp_atomic_u64 bytes_written;               /*!< 写出字节数 */
};

/* ============================ 内部接口原型 ============================ */

/* ---- core/core.c ---- */
/** @brief 用配置快照建立并发布运行时集合；失败返回负值。 */
int hp_runtime_apply(hp_config_t* cfg, hp_runtime_t** out);
/** @brief 热加载：装配新配置并原子替换生效集合（失败保持旧配置，§10.5）。 */
int hp_runtime_reload_apply(hp_config_t* cfg);
/** @brief 销毁运行时集合（含其拥有的 sink 实例）。 */
void hp_runtime_destroy(hp_runtime_t* rt);
/** @brief 限频输出到 stderr（每 5 秒至多 1 条，§9）。 */
void hp_warn_throttled(const char* fmt, ...) HPLOGC_PRINTF(1, 2);
/** @brief 无条件输出到 stderr（init 期诊断）。 */
void hp_err_printf(const char* fmt, ...) HPLOGC_PRINTF(1, 2);
/** @brief 取当前生效运行时集合（原子读取）。 */
hp_runtime_t* hp_rt_active(void);

/* ---- core/format.c ---- */
/** @brief 预编译格式模板；失败返回负值并输出诊断。 */
int hp_format_compile(const char* name, const char* tmpl, hp_fmt_t* out);
/** @brief 取内置格式模板；未找到返回 NULL。 */
const char* hp_builtin_format_tmpl(const char* name);
/** @brief 取第 @p idx 个内置格式名；越界返回 NULL。 */
const char* hp_builtin_format_name(int idx);
/** @brief 内置格式数量。 */
int hp_builtin_format_count(void);
/**
 * @brief 按预编译格式渲染整行。
 *
 * @param out      输出缓冲（不含结尾换行；换行由 %n 占位符产生）。
 * @param cap      缓冲容量；超出时按 max_log_length 截断并附加 marker。
 * @return         写入的字节数（不含 NUL）。
 */
size_t hp_format_render(const hp_fmt_t* f, const hp_rec_view_t* v,
                        const hp_config_t* cfg, char* out, size_t cap);

/* ---- core/route.c ---- */
/** @brief 登记 category（不存在时分配槽位），返回槽位索引或 -1（表满）。 */
int hp_cat_intern(const char* category);
/** @brief 判断级别是否通过过滤（含 per-category 覆盖）。 */
int hp_level_pass(int level, int cat_idx);
/** @brief 按规则路由，返回命中规则索引；-1 表示未命中。 */
int hp_route_match(const hp_runtime_t* rt, const char* category, int level);

/* ---- core/log.c ---- */
/** @brief 内部写入入口（已完成过滤与节流前的检查）。 */
void hp_log_write(int level, const char* category, const char* file, int line,
                  const char* func, const hplogc_field_t* fields,
                  size_t field_count, const char* fmt, va_list ap);
/** @brief 处理一条已构建的记录（路由 → 格式化 → 投递）。 */
void hp_process_record(unsigned char* base, size_t len);
/** @brief 取当前线程的渲染缓冲（按需分配，§9 允许的例外）。 */
unsigned char* hp_scratch(size_t need);

/* ---- core/async.c ---- */
/** @brief 启动消费者线程；返回 0 成功。 */
int hp_async_start(void);
/** @brief 停止消费者线程并排空队列。 */
void hp_async_stop(void);
/** @brief 通知消费者立即提交（flush）。 */
void hp_async_kick(void);
/** @brief 阻塞直到已入队日志被消费者排空（flush 用）。 */
void hp_async_flush_wait(void);

/* ---- core/reload.c ---- */
/** @brief 启动热加载监视线程；返回 0 成功。 */
int hp_reload_start(void);
/** @brief 停止热加载监视线程。 */
void hp_reload_stop(void);
/** @brief 执行一次热加载（重新解析 + 装配 + 原子替换）。 */
int hp_reload_do(void);
/** @brief 执行一次配置重载（内部使用，测试可见）。 */
int hp_reload_once(void);

/* ---- core/signal.c ---- */
/** @brief 初始化 fork / signal 相关设施。 */
void hp_signal_init(void);
/** @brief 反初始化。 */
void hp_signal_fini(void);

/* ---- core/time.c ---- */
/** @brief 渲染时间戳文本（§12.2 / §18-C4）。 */
size_t hp_time_render(char* buf, size_t cap, uint64_t ts_ns,
                      const hp_config_t* cfg);

/* ---- conf/ini.c ---- */
/** @brief 解析出的原始条目。 */
typedef struct {
    int  section;                    /*!< 节编号 */
    char key[HPLOGC_MAX_NAME_LEN];   /*!< 键（formats/outputs/rules 为名，其余为键名） */
    char val[HPLOGC_MAX_FMT_LEN];    /*!< 值 */
    int  line;                       /*!< 行号（续行按首行计） */
} hp_ini_item_t;

/** @brief INI 文件解析结果。 */
typedef struct {
    hp_ini_item_t* items;  /*!< 条目数组 */
    size_t         count;  /*!< 条目数 */
    size_t         cap;    /*!< 容量 */
} hp_ini_t;

/** @brief 解析配置文件；返回 0 成功（失败时输出文件名与行号）。 */
int hp_ini_parse(const char* path, hp_ini_t* out);
/** @brief 释放解析结果。 */
void hp_ini_free(hp_ini_t* ini);

/* ---- conf/build.c ---- */
/** @brief 由 INI 条目装配并校验配置快照；返回 0 成功。 */
int hp_conf_build(const hp_ini_t* ini, const char* path, hp_config_t* out);
/** @brief 由代码内配置装配配置快照；返回 0 成功。 */
int hp_conf_from_code(const hplogc_config_t* cfg, hp_config_t* out);
/** @brief 填充默认配置快照。 */
void hp_conf_default(hp_config_t* out);

/* ---- sink/sink_registry.c ---- */
/** @brief 注册全部内置 sink 类型（按 HPLOGC_SINK_* 宏裁剪）。 */
void hp_sink_register_builtins(void);
/** @brief 查找类型对应的 ops；未找到返回 NULL（"file" 别名映射到 rollingfile）。 */
const hplogc_sink_ops_t* hp_sink_lookup(const char* type);
/** @brief 注册一个 ops（供 `hplogc_sink_register()` 校验后调用）。 */
int hp_sink_register_ops(const hplogc_sink_ops_t* ops);
/** @brief 注销一个类型；存在活跃实例时返回 `HPLOGC_ERR_STATE`。 */
int hp_sink_unregister_ops(const char* type);
/**
 * @brief 对声明 `HPLOGC_CAP_FSYNC` 的实例执行 fsync（内置类型走内部钩子）。
 *
 * §4.10.3 的 ops 表在 v0.2 已冻结且没有 fsync 回调槽位，故核心经本内部钩子
 * 驱动 fsync（§18-C8）；未声明该位或非内置类型返回 `HPLOGC_ERR_UNSUPPORTED`。
 */
int hp_sink_internal_fsync(hplogc_sink_t* sink);

/* ---- core/sink.c ---- */
/** @brief 由 sink 定义创建并启动实例（配置装配期使用）。 */
int hp_sink_create_from_def(const hp_sink_def_t* def, hplogc_sink_t** out);
/** @brief 判断两个实例定义是否等价（热加载 fd 复用判定，§10.5）。 */
int hp_sink_def_equal(const hp_sink_def_t* a, const hp_sink_def_t* b);

/* ---- sink 各内置类型的 ops ---- */
/** @brief console sink 的操作表。 */
extern const hplogc_sink_ops_t hp_sink_console_ops;
/** @brief rollingfile sink 的操作表。 */
extern const hplogc_sink_ops_t hp_sink_rollingfile_ops;
/** @brief null sink 的操作表。 */
extern const hplogc_sink_ops_t hp_sink_null_ops;
/** @brief syslog sink 的操作表。 */
extern const hplogc_sink_ops_t hp_sink_syslog_ops;

/** @brief 按索引取 sink 实例（内部）。 */
struct hplogc_sink* hp_sink_by_index(const hp_runtime_t* rt, int idx);
/** @brief 内置 sink 登记一次写失败（emit 为 void，失败只能由 sink 自报，§4.10.3）。 */
void hp_sink_note_failed(struct hplogc_sink* sink);
/** @brief 内置 sink 登记一次丢弃（背压，§4.10.4）。 */
void hp_sink_note_dropped(struct hplogc_sink* sink);
/** @brief 全局时区设置（UTC 为 1，本地为 0）；供内置 sink 计算时间桶。 */
int hp_global_utc(void);
/** @brief ASCII 大小写不敏感比较；相等返回 0。 */
int hp_ieq_str(const char* a, const char* b);
/** @brief 解析布尔值；非法返回 -1。 */
int hp_parse_bool_str(const char* v);
/** @brief 解析尺寸值（含 1k / 1kb / 1m / 1mb / 1g / 1gb 后缀）；失败返回 -1。 */
int hp_parse_size_str(const char* v, unsigned long long* out);
/** @brief rollingfile 的内部 fsync 钩子（§18-C8）。 */
int hp_rollingfile_fsync(struct hplogc_sink* sink);
/** @brief 异步消费者使用的批量处理入口。 */
size_t hp_process_batch(unsigned char* recs, size_t rec_stride,
                        const size_t* lens, size_t n, char* lines,
                        size_t line_stride);

#endif /* HPLOGC_INTERNAL_H */
