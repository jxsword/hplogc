/* -*- coding: utf-8 -*- */
/**
 * @file hplogc_platform.h
 * @brief 平台接口契约（Phase 1 冻结，作为 win32 / darwin 的移植依据）。
 *
 * 本头文件是 **src/ 中唯一允许出现平台相关声明的地方**（实现在
 * `src/platform/{posix,linux,darwin,win32}/` 下按平台选择编译）。
 * 公共层（`core/`、`ring/`、`sink/`、`conf/`）只能通过本契约访问 OS，
 * 不得直接调用平台 API，也不得出现 `#ifdef _WIN32` 之类的平台分支。
 *
 * 所有句柄类型均为**不透明存储块**（opaque storage），因此本头文件
 * 不需要包含任何平台头文件，也无需平台 `#ifdef`。
 *
 * 契约一经冻结，后续阶段（win32 / darwin）只负责实现，不得要求改签名。
 *
 * @ingroup internal
 */

#ifndef HPLOGC_PLATFORM_H
#define HPLOGC_PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 不透明存储块 ---- */

/** @brief 存储块尺寸（字节）；需容纳各平台最大的原生同步对象。 */
#define HP_PLAT_STORAGE 64

/**
 * @brief 互斥量存储（不透明）。
 *
 * 必须经 `hp_mutex_init()` 初始化后使用，`hp_mutex_destroy()` 后失效。
 * 非递归：同一线程重复加锁是未定义行为。
 */
typedef union {
    void*  p;                    /*!< 保证指针对齐 */
    long long ll;                /*!< 保证 64 位对齐 */
    long double ld;              /*!< 保证最大基本对齐 */
    char   bytes[HP_PLAT_STORAGE]; /*!< 原始存储 */
} hp_mutex_t;

/** @brief 条件变量存储（不透明）。 */
typedef union {
    void*  p;
    long long ll;
    long double ld;
    char   bytes[HP_PLAT_STORAGE];
} hp_cond_t;

/** @brief 线程句柄存储（不透明）。 */
typedef union {
    void*  p;
    long long ll;
    char   bytes[HP_PLAT_STORAGE];
} hp_thread_t;

/** @brief 线程局部存储键（不透明）。 */
typedef union {
    long long ll;
    char   bytes[16];
} hp_tls_t;

/** @brief 文件句柄（不透明指针）。 */
typedef struct hp_file hp_file_t;

/** @brief 配置文件监视器（不透明指针）。 */
typedef struct hp_watcher hp_watcher_t;

/* ---- 同步原语 ---- */

/**
 * @brief 初始化互斥量。
 * @param m 待初始化的存储块。
 * @return 0 成功；负值 `hplogc_error_t`。
 */
int hp_mutex_init(hp_mutex_t* m);

/** @brief 销毁互斥量。 */
void hp_mutex_destroy(hp_mutex_t* m);

/** @brief 加锁（阻塞）。 */
void hp_mutex_lock(hp_mutex_t* m);

/** @brief 解锁。 */
void hp_mutex_unlock(hp_mutex_t* m);

/**
 * @brief 初始化条件变量。
 * @return 0 成功；负值 `hplogc_error_t`。
 */
int hp_cond_init(hp_cond_t* c);

/** @brief 销毁条件变量。 */
void hp_cond_destroy(hp_cond_t* c);

/** @brief 在已加锁的 @p m 上等待条件变量。 */
void hp_cond_wait(hp_cond_t* c, hp_mutex_t* m);

/**
 * @brief 限时等待条件变量。
 * @param timeout_ms 超时（毫秒）。
 * @return 1 被唤醒；0 超时；负值错误。
 */
int hp_cond_timedwait(hp_cond_t* c, hp_mutex_t* m, unsigned timeout_ms);

/** @brief 唤醒一个等待者。 */
void hp_cond_signal(hp_cond_t* c);

/** @brief 唤醒全部等待者。 */
void hp_cond_broadcast(hp_cond_t* c);

/* ---- 线程 ---- */

/**
 * @brief 创建线程。
 *
 * @param t    接收线程句柄。
 * @param fn   线程入口（不得为 NULL）。
 * @param arg  入口参数。
 * @return     0 成功；负值 `hplogc_error_t`。
 */
int hp_thread_create(hp_thread_t* t, void (*fn)(void*), void* arg);

/**
 * @brief 等待线程结束。
 * @return 0 成功；负值 `hplogc_error_t`。
 */
int hp_thread_join(hp_thread_t* t);

/** @brief 让当前线程睡眠 @p ms 毫秒。 */
void hp_sleep_ms(unsigned ms);

/** @brief 让出当前 CPU 时间片（用于无锁路径的退避）。 */
void hp_yield(void);

/* ---- 线程局部存储 ---- */

/**
 * @brief 创建 TLS 键。
 *
 * @param key  接收键。
 * @param dtor 线程退出时对非 NULL 值调用的析构函数；可为 NULL。
 * @return     0 成功；负值 `hplogc_error_t`。
 */
int hp_tls_create(hp_tls_t* key, void (*dtor)(void*));

/** @brief 销毁 TLS 键（不调用析构函数）。 */
void hp_tls_destroy(hp_tls_t* key);

/** @brief 设置当前线程的 TLS 值。 */
void hp_tls_set(hp_tls_t* key, void* val);

/** @brief 取得当前线程的 TLS 值（未设置时为 NULL）。 */
void* hp_tls_get(hp_tls_t* key);

/* ---- 时钟与时间 ---- */

/** @brief 返回墙上时钟的纳秒时间戳（epoch 起算）。 */
uint64_t hp_now_realtime_ns(void);

/** @brief 返回单调时钟的纳秒时间戳（起点任意，仅用于差值）。 */
uint64_t hp_now_monotonic_ns(void);

/**
 * @brief 把 epoch 秒转换为拆分时间。
 *
 * @param sec  epoch 秒（可含负值）。
 * @param utc  非零表示 UTC，否则本地时区。
 * @param out  输出 `struct tm`。
 * @return     0 成功；负值错误。
 */
int hp_localtime(int64_t sec, int utc, struct tm* out);

/**
 * @brief 带 hplogc 扩展占位符的时间格式化（§12.2）。
 *
 * 在 `strftime` 语义之外支持：`%f`（6 位微秒）与 `%F<n>`（n = 1..6 位秒小数）。
 * 其余说明符原样交给系统 `strftime`。
 *
 * @param buf  输出缓冲。
 * @param cap  缓冲容量（含结尾 NUL）。
 * @param fmt  格式串（由调用方保证已通过 §12.1 / §12.2 校验）。
 * @param tm   拆分时间。
 * @param usec 微秒部分（0 ~ 999999）。
 * @return     写入的字节数（不含 NUL）；缓冲不足时返回已写入的长度。
 */
size_t hp_strftime_ext(char* buf, size_t cap, const char* fmt,
                       const struct tm* tm, long usec);

/* ---- 文件与目录 ---- */

/**
 * @brief 打开文件。
 *
 * @param path 路径（UTF-8）。
 * @param mode 与 `fopen` 相同的模式串（"a" / "w" / "r"）。
 * @return     句柄；失败返回 NULL。
 */
hp_file_t* hp_fopen(const char* path, const char* mode);

/**
 * @brief 同步写入。
 * @return 0 成功；负值 `hplogc_error_t`（`HPLOGC_ERR_IO`）。
 */
int hp_fwrite(hp_file_t* f, const void* buf, size_t len);

/** @brief 冲刷用户态缓冲（不 fsync）。 */
int hp_fflush(hp_file_t* f);

/**
 * @brief 把数据刷到存储（fsync 语义）。
 * @return 0 成功；平台不支持返回 `HPLOGC_ERR_UNSUPPORTED`。
 */
int hp_fsync_file(hp_file_t* f);

/** @brief 取得当前文件大小（字节）。 */
int hp_ftell_size(hp_file_t* f, uint64_t* size);

/** @brief 关闭并释放句柄；@p f 为 NULL 时安全返回。 */
void hp_fclose(hp_file_t* f);

/**
 * @brief 向标准流做一次同步写（用于 console sink 与 signal safe 通道）。
 *
 * @param stream 1 = stdout，2 = stderr。
 * @return       0 成功；负值 `hplogc_error_t`。
 */
int hp_console_write(int stream, const void* buf, size_t len);

/**
 * @brief 判断标准流是否为终端设备（彩色输出的启用依据）。
 * @return 1 是终端；0 不是；负值错误。
 */
int hp_console_isatty(int stream);

/**
 * @brief 启用终端的 ANSI 转义处理（Windows VT；POSIX 无需动作）。
 * @return 0 成功或无需动作；负值错误。
 */
int hp_console_enable_vt(int stream);

/** @brief 路径是否存在（任意类型）。 */
int hp_path_exists(const char* path);

/** @brief 路径是否为普通文件。 */
int hp_is_regular_file(const char* path);

/** @brief 取得文件大小与修改时间（用于热加载轮询）。 */
int hp_file_stat(const char* path, uint64_t* size, uint64_t* mtime_ns);

/** @brief 重命名（轮转用）；目标存在时的覆盖语义跟随平台。 */
int hp_rename(const char* from, const char* to);

/** @brief 删除文件。 */
int hp_unlink(const char* path);

/**
 * @brief 递归创建目录。
 * @param mode 权限位（Windows 忽略）。
 * @return     0 成功（含已存在）；负值 `hplogc_error_t`。
 */
int hp_mkdirs(const char* dir, unsigned mode);

/**
 * @brief 修改文件权限。
 * @return 0 成功；平台不支持返回 `HPLOGC_ERR_UNSUPPORTED`。
 */
int hp_chmod_file(const char* path, unsigned mode);

/**
 * @brief 创建符号链接。
 * @return 0 成功；平台不支持或需特权返回 `HPLOGC_ERR_UNSUPPORTED`。
 */
int hp_symlink(const char* target, const char* linkpath);

/**
 * @brief 扫描目录下具有指定前缀的条目名。
 *
 * @param dir    目录路径。
 * @param prefix 文件名前缀；NULL / 空串表示不过滤。
 * @param cb     每个匹配条目回调一次（仅传文件名，不含目录）。
 * @param ctx    透传给回调。
 * @return       0 成功；目录不可读返回负值。
 */
int hp_dir_scan(const char* dir, const char* prefix,
                void (*cb)(const char* name, void* ctx), void* ctx);

/**
 * @brief 取目录部分（不含结尾分隔符）；结果为 "." 表示无目录成分。
 * @return 0 成功；缓冲不足返回 `HPLOGC_ERR_INVALID_ARG`。
 */
int hp_path_dirname(const char* path, char* out, size_t cap);

/**
 * @brief 取文件名部分（指向 @p path 内部，不拷贝）。
 */
const char* hp_path_basename(const char* path);

/** @brief 路径是否为绝对路径。 */
int hp_path_is_absolute(const char* path);

/**
 * @brief 规范化路径（去 `.` / 重复分隔符，解析 `..` 的词法形式）。
 *
 * 用于 §10.4 "两个 file 类 sink 路径相同"的重复检测；不做真实路径解析
 * （不解析符号链接），保证 O(1) 语义与跨平台一致。
 *
 * @return 0 成功；缓冲不足返回 `HPLOGC_ERR_INVALID_ARG`。
 */
int hp_path_normalize(const char* path, char* out, size_t cap);

/* ---- 配置文件监视 ---- */

/**
 * @brief 创建配置文件监视器。
 *
 * 优先使用平台原生机制（Linux：inotify）；不可用时返回 NULL，
 * 由调用方回退到 `hp_watcher_poll_create()` 的轮询实现。
 *
 * @param path 配置文件路径。
 * @return     句柄；不可用返回 NULL。
 */
hp_watcher_t* hp_watcher_create(const char* path);

/**
 * @brief 等待配置变更。
 *
 * @param timeout_ms 最长等待（毫秒）。
 * @return           1 变更；0 超时；-1 错误（调用方应回退到轮询）。
 */
int hp_watcher_wait(hp_watcher_t* w, unsigned timeout_ms);

/** @brief 销毁监视器；@p w 为 NULL 时安全返回。 */
void hp_watcher_destroy(hp_watcher_t* w);

/**
 * @brief 创建**轮询式**监视器（平台原生机制不可用时的回退）。
 *
 * 以 mtime + size 比对判定变更；创建时记录基线。
 */
hp_watcher_t* hp_watcher_poll_create(const char* path);

/* ---- 进程 / 线程标识 ---- */

/**
 * @brief 返回本平台的默认行结束符（`newline = auto` 时使用，§12）。
 *
 * @return `"\r\n"`（Windows）或 `"\n"`（其余平台）；静态存储，不得释放。
 */
const char* hp_platform_eol(void);

/** @brief 当前进程 ID。 */
uint32_t hp_pid(void);

/** @brief 当前线程 ID（平台原生语义，仅用于展示与区分）。 */
uint64_t hp_tid(void);

/* ---- 信号与 fork ---- */

/**
 * @brief 安装 SIGHUP 处理器（热加载触发源之一，§4.5）。
 *
 * 若宿主已安装非默认处理器，则**不覆盖**并返回非 0，由调用方输出警告后继续
 * （热加载仅靠 watcher / 轮询）。
 *
 * @param handler 处理器；内部只允许设置原子标志。
 * @return        0 成功；1 已被宿主占用；负值表示平台不支持该信号。
 */
int hp_install_sighup(void (*handler)(int));

/** @brief 卸载 SIGHUP 处理器（恢复 SIG_DFL）。 */
void hp_uninstall_sighup(void);

/**
 * @brief 注册 fork 的 child handler（§9）。
 *
 * @param child 子进程回调；只允许设置原子标志（async-signal-safe）。
 * @return      0 成功；平台无 fork 返回 `HPLOGC_ERR_UNSUPPORTED`。
 */
int hp_atfork_child(void (*child)(void));

#ifdef __cplusplus
}
#endif

#endif /* HPLOGC_PLATFORM_H */
