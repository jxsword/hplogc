/* -*- coding: utf-8 -*- */
/**
 * @file log.c
 * @brief 日志写入 API：过滤、限流、渲染、入队与同步路径的投递（§4.9）。
 */

#include "hplogc_internal.h"

#include <errno.h>
#include <stdio.h>

#include <hplogc.h>

/* ============================ 每线程渲染缓冲 ============================ */

/** @brief 每线程的渲染缓冲（§9 允许的例外：首次使用与按需增长）。 */
typedef struct {
    unsigned char* buf;                  /*!< 缓冲本体 */
    size_t         cap;                  /*!< 当前容量 */
    char           last_cat[HPLOGC_MAX_NAME_LEN]; /*!< 最近一次 category（热路径缓存） */
    int            last_idx;             /*!< 最近一次 category 的槽位 */
} hp_scratch_t;

unsigned char* hp_scratch(size_t need)
{
    hp_scratch_t* s;
    size_t want;

    if (!g_rt.tls_ready) {
        return NULL;
    }
    s = (hp_scratch_t*)hp_tls_get(&g_rt.scratch_tls);
    want = 2u * (g_rt.scratch_size + 1u);
    if (need > want) {
        want = need;
    }
    if (s == NULL) {
        s = (hp_scratch_t*)malloc(sizeof(*s));
        if (s == NULL) {
            return NULL;
        }
        s->buf = NULL;
        s->cap = 0;
        s->last_cat[0] = '\0';
        s->last_idx = -1;
        hp_tls_set(&g_rt.scratch_tls, s);
    }
    if (s->cap < want) {
        unsigned char* nb = (unsigned char*)realloc(s->buf, want);
        if (nb == NULL) {
            return NULL;
        }
        s->buf = nb;
        s->cap = want;
    }
    return s->buf;
}

/** @brief 取当前线程的 category 缓存槽位（避免每次线性查表）。 */
static hp_scratch_t* hp_scratch_obj(void)
{
    if (!g_rt.tls_ready) {
        return NULL;
    }
    return (hp_scratch_t*)hp_tls_get(&g_rt.scratch_tls);
}

/** @brief 带缓存的 category 登记；表满时返回 -1（调用方按 "*" 兜底）。 */
static int hp_cat_lookup(const char* cat, int* warned)
{
    hp_scratch_t* s = hp_scratch_obj();
    int idx;

    if (cat == NULL || cat[0] == '\0') {
        return -1;
    }
    if (s != NULL && s->last_idx >= 0 && strcmp(s->last_cat, cat) == 0) {
        return s->last_idx;
    }
    idx = hp_cat_intern(cat);
    if (s != NULL && idx >= 0) {
        snprintf(s->last_cat, sizeof(s->last_cat), "%s", cat);
        s->last_idx = idx;
    }
    if (idx < 0 && warned != NULL && !*warned) {
        *warned = 1;
        hp_err_printf("hplogc: category 登记表已满（%d），'%s' 按 '*' 兜底路由",
                      HPLOGC_MAX_CATEGORIES, cat);
    }
    return idx;
}

/* ============================ 限流与采样 ============================ */

#ifdef HPLOGC_HAS_THROTTLE
/** @brief 令牌桶取票；成功返回 1（已扣除 1 张票）。 */
static int hp_bucket_take(hp_atomic_u64* tokens, hp_atomic_u64* last_ms,
                          unsigned rate, unsigned burst, uint64_t now_ms)
{
    unsigned long long cap;
    unsigned long long cur;
    unsigned long long prev;
    unsigned long long add;
    unsigned long long next;

    if (rate == 0u) {
        return 1; /* 不限流 */
    }
    cap = (unsigned long long)burst * 1000ull;
    if (cap == 0ull) {
        cap = 1000ull;
    }
    prev = hp_atomic_load_u64(last_ms);
    if (prev == 0ull) {
        hp_atomic_store_u64(last_ms, now_ms);
        prev = now_ms;
    }
    add = (now_ms > prev) ? ((now_ms - prev) * (unsigned long long)rate) : 0ull;
    cur = hp_atomic_load_u64(tokens);
    for (;;) {
        unsigned long long have = cur + add;
        if (have > cap) {
            have = cap;
        }
        if (have < 1000ull) {
            /* 令牌不足：仅更新时间戳，不放行 */
            hp_atomic_store_u64(last_ms, now_ms);
            hp_atomic_store_u64(tokens, have);
            return 0;
        }
        next = have - 1000ull;
        if (hp_atomic_cas_u64(tokens, &cur, next)) {
            hp_atomic_store_u64(last_ms, now_ms);
            return 1;
        }
        /* CAS 失败：重新读取并重算（add 不计入下一轮，避免重复补充） */
        add = 0ull;
    }
}
#endif /* HPLOGC_HAS_THROTTLE */

/** @brief 限流 + 采样（§4.9 步骤 ③：先限流后采样）。 */
static int hp_throttle_pass(int cat_idx, const hp_config_t* cfg)
{
#ifndef HPLOGC_HAS_THROTTLE
    (void)cat_idx;
    (void)cfg;
    return 1; /* THROTTLE=OFF：throttled 恒为 0（§7.3） */
#else
    uint64_t now_ms;
    unsigned long long n;
    unsigned long long nmod;

    now_ms = hp_now_monotonic_ns() / 1000000ull;
    if (!hp_bucket_take(&g_rt.g_tokens, &g_rt.g_last_ms, cfg->global_rate,
                        cfg->burst, now_ms)) {
        return 0;
    }
    if (cat_idx >= 0 && cfg->per_cat_rate > 0u) {
        hp_cat_t* c = &g_rt.cats[cat_idx];
        if (!hp_bucket_take(&c->tokens, &c->last_ms, cfg->per_cat_rate,
                            cfg->burst, now_ms)) {
            return 0;
        }
    }
    if (cfg->sampling_rate >= 1.0) {
        return 1;
    }
    if (cfg->sampling_rate <= 0.0) {
        return 0; /* 0.0 = 全部丢弃（§10.3） */
    }
    {
        unsigned long long nn = (unsigned long long)(1.0 / cfg->sampling_rate
                                                     + 0.5);
        if (nn < 1ull) {
            nn = 1ull;
        }
        n = hp_atomic_fetch_add_u64(&g_rt.sample_counter, 1ull);
        nmod = (n + 1ull) % nn;
        return (nmod == 0ull) ? 1 : 0;
    }
#endif
}

/* ============================ 记录构建 ============================ */

/**
 * @brief 在 @p dst 中构建一条记录（消息体渲染 + 字段深拷贝 + 预算裁剪）。
 *
 * @return 记录总字节数；0 表示失败。
 */
static size_t hp_build_record(unsigned char* dst, size_t cap, int level,
                              const char* cat, const char* file, int line,
                              const char* func, const hplogc_field_t* fields,
                              size_t field_count, const char* fmt, va_list ap)
{
    hp_rec_hdr_t* h;
    size_t off = sizeof(hp_rec_hdr_t);
    size_t budget;
    size_t msg_budget;
    int n;
    size_t i;
    uint32_t kept = 0;
    uint32_t fdrop = 0;
    size_t frec_off;
    size_t blob_off;
    char* blob;

    if (dst == NULL || cap <= sizeof(hp_rec_hdr_t)) {
        return 0;
    }
    budget = cap;
    msg_budget = budget - off - 16u;

    /* ① 消息体渲染（上限 max log length，超出静默丢弃、不加 marker，§4.9④） */
    {
        va_list cp;
        va_copy(cp, ap);
        n = vsnprintf((char*)(void*)(dst + off), msg_budget, fmt, cp);
        va_end(cp);
    }
    if (n < 0) {
        n = 0;
        dst[off] = '\0';
    } else if ((size_t)n >= msg_budget) {
        n = (int)(msg_budget - 1);
    }
    /* 渲染结果长度上限为 max log length（§9） */
    off += (size_t)n + 1u; /* 含结尾 NUL */
    dst[off - 1] = '\0';

    /* ② category / file / func */
    /** @brief 追加一个字符串（含 NUL），空间不足时写空串。 */
#define HP_PUT_STR(s)                                     \
    do {                                                  \
        const char* _s = (s);                             \
        size_t _n;                                        \
        if (_s == NULL) {                                 \
            _s = "";                                      \
        }                                                 \
        _n = strlen(_s) + 1u;                             \
        if (off + _n > budget) {                          \
            _s = "";                                      \
            _n = 1u;                                      \
        }                                                 \
        memcpy(dst + off, _s, _n);                        \
        off += _n;                                        \
    } while (0)

    {
        size_t cat_len;
        size_t file_len;
        size_t func_len;
        size_t before = off;
        HP_PUT_STR(cat);
        cat_len = off - before;
        before = off;
        HP_PUT_STR(file);
        file_len = off - before;
        before = off;
        HP_PUT_STR(func);
        func_len = off - before;

        /* ③ 字段区（与消息体共享 max log length 预算，§4.11.2） */
        off = (off + 7u) & ~(size_t)7u;
        frec_off = off;
        blob_off = frec_off;
        if (field_count > 0 && fields != NULL) {
            blob_off += HPLOGC_MAX_FIELDS * sizeof(hp_field_rec_t);
        }
        blob = NULL;
        if (blob_off < budget) {
            blob = (char*)(void*)(dst + blob_off);
        }
        {
            size_t bcnt = 0;
            for (i = 0; i < field_count && fields != NULL; i++) {
                const hplogc_field_t* f = &fields[i];
                size_t key_len;
                size_t str_len = 0;
                size_t cost;
                if (kept >= HPLOGC_MAX_FIELDS) {
                    fdrop++; /* 超数量 */
                    continue;
                }
                key_len = (f->key != NULL) ? strlen(f->key) : 0u;
                if (key_len > HPLOGC_MAX_FIELD_STR_LEN) {
                    key_len = HPLOGC_MAX_FIELD_STR_LEN;
                    fdrop++; /* 超长度截断计入（§4.11.2） */
                }
                if (f->type == HPLOGC_V_STR && f->v.s != NULL) {
                    str_len = strlen(f->v.s);
                    if (str_len > HPLOGC_MAX_FIELD_STR_LEN) {
                        str_len = HPLOGC_MAX_FIELD_STR_LEN;
                        fdrop++;
                    }
                }
                cost = sizeof(hp_field_rec_t) + key_len + 1u + str_len + 1u;
                if (blob == NULL || blob_off + bcnt + cost > budget) {
                    fdrop++; /* 超预算 */
                    continue;
                }
                {
                    hp_field_rec_t* fr =
                        (hp_field_rec_t*)(void*)(dst + frec_off
                                                 + kept * sizeof(hp_field_rec_t));
                    fr->key_len = (uint32_t)(key_len + 1u);
                    fr->str_len = (uint32_t)((str_len > 0) ? (str_len + 1u)
                                                           : 0u);
                    fr->type = (uint32_t)f->type;
                    memset(&fr->v, 0, sizeof(fr->v));
                    switch (f->type) {
                    case HPLOGC_V_INT:
                        fr->v.i = f->v.i;
                        break;
                    case HPLOGC_V_UINT:
                        fr->v.u = f->v.u;
                        break;
                    case HPLOGC_V_DOUBLE:
                        fr->v.d = f->v.d;
                        break;
                    case HPLOGC_V_BOOL:
                        fr->v.b = f->v.b;
                        break;
                    case HPLOGC_V_STR:
                    default:
                        break;
                    }
                }
                if (key_len > 0) {
                    memcpy(blob + bcnt, f->key, key_len);
                }
                blob[bcnt + key_len] = '\0';
                bcnt += key_len + 1u;
                if (str_len > 0) {
                    memcpy(blob + bcnt, f->v.s, str_len);
                    blob[bcnt + str_len] = '\0';
                    bcnt += str_len + 1u;
                }
                kept++;
            }
            blob_off += bcnt;
            off = blob_off;
        }

        /* ④ 回填定长头部 */
        h = (hp_rec_hdr_t*)(void*)dst;
        h->ts_ns = 0; /* 由调用方填写 */
        h->tid = 0;
        h->pid = 0;
        h->level = (uint32_t)level;
        h->line = line;
        h->msg_len = (uint32_t)n + 1u;
        h->cat_len = (uint32_t)cat_len;
        h->file_len = (uint32_t)file_len;
        h->func_len = (uint32_t)func_len;
        h->field_count = kept;
        h->fields_dropped = fdrop;
        h->reserved = 0;
        return off;
    }
#undef HP_PUT_STR
}

/* ============================ 写入入口 ============================ */

void hp_log_write(int level, const char* category, const char* file, int line,
                  const char* func, const hplogc_field_t* fields,
                  size_t field_count, const char* fmt, va_list ap)
{
    int saved_errno = errno;
    hp_runtime_t* rt;
    hp_config_t* cfg;
    int cat_idx;
    unsigned char* rec;
    size_t len;
    size_t budget;
    hp_rec_hdr_t* h;
    static int cat_full_warned = 0;

    rt = hp_rt_active();
    if (rt == NULL || hp_atomic_load_i32(&g_rt.init_state) != 1) {
        errno = saved_errno;
        return; /* 未初始化：静默丢弃（§7.5） */
    }
    if (hp_atomic_load_i32(&g_rt.child_disabled) != 0) {
        errno = saved_errno;
        return; /* fork behavior = disable 的子进程 */
    }
    cfg = &rt->cfg;

    /* ② 级别过滤 */
    cat_idx = hp_cat_lookup(category, &cat_full_warned);
    if (!hp_level_pass(level, cat_idx)) {
        errno = saved_errno;
        return;
    }
    /* ③ 限流 / 采样 */
    if (!hp_throttle_pass(cat_idx, cfg)) {
        hp_atomic_fetch_add_u64(&g_rt.throttled, 1ull);
        errno = saved_errno;
        return;
    }

    /* ④ 渲染 + 入队 */
    if (g_rt.scratch_size == 0 || hp_scratch(0) == NULL) {
        errno = saved_errno;
        return;
    }
    rec = hp_scratch(0);
    budget = cfg->max_log_length + HPLOGC_REC_EXTRA;
    if (budget > g_rt.scratch_size) {
        budget = g_rt.scratch_size;
    }
    len = hp_build_record(rec, budget, level, category, file, line, func,
                          fields, field_count, fmt, ap);
    if (len == 0) {
        errno = saved_errno;
        return;
    }
    h = (hp_rec_hdr_t*)(void*)rec;
    h->ts_ns = cfg->ts_monotonic ? hp_now_monotonic_ns() : hp_now_realtime_ns();
    h->tid = hp_tid();
    h->pid = hp_pid();
#ifndef HPLOGC_HAS_SOURCE_LOC
    h->file_len = 0;
    h->func_len = 0;
    h->line = 0;
#endif
    if (h->fields_dropped > 0) {
        hp_atomic_fetch_add_u64(&g_rt.fields_dropped,
                                (unsigned long long)h->fields_dropped);
    }

    if (!g_rt.has_ring) {
        /* 同步构建：②~⑧ 全部在调用者线程（§4.9） */
        hp_atomic_fetch_add_u64(&g_rt.accepted, 1ull);
        hp_process_record(rec, len);
        errno = saved_errno;
        return;
    }

    {
        unsigned char* slot = NULL;
        size_t rcap = 0;
        int rc = hp_ring_reserve(&g_rt.ring, len, &slot, &rcap);
        if (rc == HP_RING_DISCARD) {
            /* 队列已满：按溢出策略丢弃 */
            hp_atomic_fetch_add_u64(&g_rt.dropped, 1ull);
        } else {
            memcpy(slot, rec, len);
            hp_ring_commit(&g_rt.ring, len);
            hp_atomic_fetch_add_u64(&g_rt.accepted, 1ull);
        }
    }
    errno = saved_errno;
}

/* ============================ 公共写入 API ============================ */

void hplogc_log(hplogc_level_t level, const char* category, const char* file,
                int line, const char* func, const char* fmt, ...)
{
    va_list ap;
    if (fmt == NULL) {
        return;
    }
    va_start(ap, fmt);
    hp_log_write((int)level, category, file, line, func, NULL, 0, fmt, ap);
    va_end(ap);
}

void hplogc_vlog(hplogc_level_t level, const char* category, const char* file,
                 int line, const char* func, const char* fmt, va_list ap)
{
    if (fmt == NULL) {
        return;
    }
    hp_log_write((int)level, category, file, line, func, NULL, 0, fmt, ap);
}

void hplogc_log_fields(hplogc_level_t level, const char* category,
                       const char* file, int line, const char* func,
                       const hplogc_field_t* fields, size_t field_count,
                       const char* fmt, ...)
{
    va_list ap;
    if (fmt == NULL) {
        return;
    }
    va_start(ap, fmt);
    hp_log_write((int)level, category, file, line, func, fields, field_count,
                 fmt, ap);
    va_end(ap);
}

void hplogc_vlog_fields(hplogc_level_t level, const char* category,
                        const char* file, int line, const char* func,
                        const hplogc_field_t* fields, size_t field_count,
                        const char* fmt, va_list ap)
{
    if (fmt == NULL) {
        return;
    }
    hp_log_write((int)level, category, file, line, func, fields, field_count,
                 fmt, ap);
}

void hplogc_log_signal_safe(hplogc_level_t level, const char* msg)
{
    int saved_errno = errno;
    char buf[64];
    size_t n;

    if (hp_atomic_load_i32(&g_rt.signal_safe) == 0 || msg == NULL) {
        errno = saved_errno;
        return; /* signal safe = false（默认）：静默空操作（§7.3） */
    }
    /* 仅追加级别标签后直写 stderr：无锁、无格式化、无分配（async-signal-safe） */
    n = 0;
    {
        const char* tag = hplogc_level_name(level);
        while (*tag != '\0' && n + 1 < sizeof(buf)) {
            buf[n++] = *tag++;
        }
        if (n + 2 < sizeof(buf)) {
            buf[n++] = ':';
            buf[n++] = ' ';
        }
    }
    hp_console_write(2, buf, n);
    {
        size_t m = 0;
        while (msg[m] != '\0') {
            m++;
        }
        hp_console_write(2, msg, m);
    }
    hp_console_write(2, hp_platform_eol(), strlen(hp_platform_eol()));
    errno = saved_errno;
}

/* ============================ 级别辅助 ============================ */

const char* hplogc_level_name(hplogc_level_t level)
{
    switch (level) {
    case HPLOGC_LEVEL_TRACE:
        return "TRACE";
    case HPLOGC_LEVEL_DEBUG:
        return "DEBUG";
    case HPLOGC_LEVEL_INFO:
        return "INFO";
    case HPLOGC_LEVEL_WARN:
        return "WARN";
    case HPLOGC_LEVEL_ERROR:
        return "ERROR";
    case HPLOGC_LEVEL_FATAL:
        return "FATAL";
    case HPLOGC_LEVEL_OFF:
        return "OFF";
    default:
        return "UNKNOWN";
    }
}

/** @brief ASCII 大小写不敏感比较（级别名为纯 ASCII，无需 locale）。 */
static int hp_ieq(const char* a, const char* b)
{
    while (*a != '\0' && *b != '\0') {
        int ca = *a;
        int cb = *b;
        if (ca >= 'A' && ca <= 'Z') {
            ca += 32;
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb += 32;
        }
        if (ca != cb) {
            return 0;
        }
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0') ? 1 : 0;
}

int hplogc_level_parse(const char* name, hplogc_level_t* out)
{
    if (name == NULL || out == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    /** @brief 大小写不敏感比较。 */
#define HP_EQ(s) (hp_ieq(name, (s)) == 1)
    if (HP_EQ("TRACE")) {
        *out = HPLOGC_LEVEL_TRACE;
    } else if (HP_EQ("DEBUG")) {
        *out = HPLOGC_LEVEL_DEBUG;
    } else if (HP_EQ("INFO")) {
        *out = HPLOGC_LEVEL_INFO;
    } else if (HP_EQ("WARN")) {
        *out = HPLOGC_LEVEL_WARN;
    } else if (HP_EQ("ERROR")) {
        *out = HPLOGC_LEVEL_ERROR;
    } else if (HP_EQ("FATAL")) {
        *out = HPLOGC_LEVEL_FATAL;
    } else if (HP_EQ("OFF")) {
        *out = HPLOGC_LEVEL_OFF;
    } else {
        return HPLOGC_ERR_INVALID_ARG;
    }
#undef HP_EQ
    return HPLOGC_OK;
}

int hplogc_level_enabled(hplogc_level_t level, const char* category)
{
    hp_runtime_t* rt = hp_rt_active();
    int cat_idx;

    if (rt == NULL || hp_atomic_load_i32(&g_rt.init_state) != 1) {
        return 0; /* 未初始化返回 0，不返回错误（§7.3） */
    }
    if ((int)level < 0 || (int)level > (int)HPLOGC_LEVEL_OFF) {
        return 0;
    }
    cat_idx = hp_cat_lookup(category, NULL);
    return hp_level_pass((int)level, cat_idx);
}
