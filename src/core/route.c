/* -*- coding: utf-8 -*- */
/**
 * @file route.c
 * @brief category 登记、级别过滤、规则匹配与 sink 投递（§4.9 步骤 ②⑥⑧、§12.4）。
 */

#include "hplogc_internal.h"

#include <stdio.h>

#include <stdlib.h>
#include <string.h>

#include <hplogc.h>

/** @brief 单批最多参与攒批的事件数（等于 batch_size 上限）。 */
#define HP_DISPATCH_MAX 4096

/** @brief 一次投递的中间表示（路由 + 格式化结果）。 */
typedef struct {
    hplogc_event_t ev;                    /*!< 事件（指针指向记录与行缓冲） */
    hplogc_field_t fld[HPLOGC_MAX_FIELDS]; /*!< 物化后的字段数组 */
    int            sink_idx[HPLOGC_MAX_SINKS]; /*!< 命中的 sink 索引 */
    int            sink_count;                 /*!< sink 数量 */
    size_t         line_len;                   /*!< 格式化后的行长度 */
    uint32_t       fields_dropped;             /*!< 本条记录被丢弃的字段数 */
} hp_dispatch_t;

/* ============================ 记录解析 ============================ */

int hp_rec_parse(unsigned char* base, size_t len, hp_rec_view_t* out)
{
    hp_rec_hdr_t* h;
    size_t off;

    if (base == NULL || out == NULL || len < sizeof(hp_rec_hdr_t)) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    h = (hp_rec_hdr_t*)(void*)base;
    out->h = h;
    off = sizeof(hp_rec_hdr_t);

    /** @brief 取一段 NUL 结尾的字符串区。 */
#define HP_TAKE(field, n)                       \
    do {                                        \
        if ((n) > 0) {                          \
            if (off + (n) > len) {              \
                return -1;                      \
            }                                   \
            out->field = (char*)(void*)(base + off); \
            off += (n);                         \
        } else {                                \
            out->field = NULL;                  \
        }                                       \
    } while (0)

    HP_TAKE(msg, h->msg_len);
    HP_TAKE(cat, h->cat_len);
    HP_TAKE(file, h->file_len);
    HP_TAKE(func, h->func_len);
#undef HP_TAKE

    off = (off + 7u) & ~(size_t)7u; /* 字段记录需 8 字节对齐 */
    if (h->field_count > 0) {
        size_t need = (size_t)h->field_count * sizeof(hp_field_rec_t);
        if (off + need > len) {
            return -1;
        }
        out->frec = (hp_field_rec_t*)(void*)(base + off);
        off += need;
    }
    out->fblob = (char*)(void*)(base + off);
    if (off > len) {
        return -1;
    }
    return 0;
}

size_t hp_rec_view_size(const hp_rec_view_t* v)
{
    size_t n = sizeof(hp_rec_hdr_t);
    if (v == NULL || v->h == NULL) {
        return 0;
    }
    n += v->h->msg_len + v->h->cat_len + v->h->file_len + v->h->func_len;
    n = (n + 7u) & ~(size_t)7u;
    n += (size_t)v->h->field_count * sizeof(hp_field_rec_t);
    n += (size_t)v->h->field_count * (HPLOGC_MAX_FIELD_STR_LEN + 1) * 2u;
    return n;
}

/* ============================ category 登记表 ============================ */

int hp_cat_intern(const char* category)
{
    int i;
    int free_slot = -1;

    if (category == NULL || category[0] == '\0') {
        return -1; /* NULL / 空串等价 "*"，不登记（§4.9） */
    }
#ifndef HPLOGC_HAS_CATEGORY
    return -1; /* CATEGORY=OFF：不做登记 */
#else
    hp_mutex_lock(&g_rt.cat_mu);
    for (i = 0; i < HPLOGC_MAX_CATEGORIES; i++) {
        if (g_rt.cats[i].used && strcmp(g_rt.cats[i].name, category) == 0) {
            hp_mutex_unlock(&g_rt.cat_mu);
            return i;
        }
        if (!g_rt.cats[i].used && free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot < 0) {
        hp_mutex_unlock(&g_rt.cat_mu);
        return -1; /* 表满：调用方按 "*" 兜底并告警一次 */
    }
    snprintf(g_rt.cats[free_slot].name, sizeof(g_rt.cats[free_slot].name),
             "%s", category);
    g_rt.cats[free_slot].used = 1;
    g_rt.cats[free_slot].level_set = 0;
    g_rt.cats[free_slot].level = 0;
    hp_atomic_init_u64(&g_rt.cats[free_slot].tokens, 0ull);
    hp_atomic_init_u64(&g_rt.cats[free_slot].last_ms, 0ull);
    if (free_slot + 1 > g_rt.cat_count) {
        g_rt.cat_count = free_slot + 1;
    }
    hp_mutex_unlock(&g_rt.cat_mu);
    return free_slot;
#endif
}

int hp_level_pass(int level, int cat_idx)
{
    hp_runtime_t* rt = hp_rt_active();
    int threshold;

    if (rt == NULL) {
        return 0;
    }
    threshold = rt->cfg.level;
#ifdef HPLOGC_HAS_CATEGORY
    if (cat_idx >= 0 && cat_idx < HPLOGC_MAX_CATEGORIES) {
        if (g_rt.cats[cat_idx].used && g_rt.cats[cat_idx].level_set) {
            threshold = g_rt.cats[cat_idx].level; /* 覆盖语义（§4.1） */
        }
    }
#else
    (void)cat_idx;
#endif
    return (level >= threshold) ? 1 : 0;
}

/* ============================ 规则匹配 ============================ */

/** @brief category 选择器匹配（§10.3）。 */
static int hp_cat_selector_match(const char* sel, const char* cat)
{
    size_t n;

    if (sel == NULL || sel[0] == '\0' || strcmp(sel, "*") == 0) {
        return 1;
    }
#ifndef HPLOGC_HAS_CATEGORY
    (void)cat;
    return 1; /* CATEGORY=OFF：所有规则按 "*" 匹配（§4.8） */
#else
    if (cat == NULL || cat[0] == '\0') {
        cat = "*"; /* NULL / 空串等价 "*"（§4.9） */
    }
    n = strlen(sel);
    if (n > 2 && sel[n - 2] == '.' && sel[n - 1] == '*') {
        size_t base_len = n - 2;
        if (strlen(cat) == base_len && strncmp(cat, sel, base_len) == 0) {
            return 1; /* 含 name 自身 */
        }
        return (strncmp(cat, sel, base_len) == 0 && cat[base_len] == '.')
                   ? 1
                   : 0;
    }
    return (strcmp(sel, cat) == 0) ? 1 : 0;
#endif
}

int hp_route_match(const hp_runtime_t* rt, const char* category, int level)
{
    int i;

    if (rt == NULL) {
        return -1;
    }
    for (i = 0; i < rt->rule_count; i++) {
        const hp_rule_t* r = &rt->rules[i];
        if (!hp_cat_selector_match(r->category, category)) {
            continue;
        }
        if (level < r->min_level || level > r->max_level) {
            continue;
        }
        return i;
    }
    return -1;
}

/* ============================ 路由 + 格式化 ============================ */

/** @brief 路由并格式化一条记录，产出投递中间表示。 */
static int hp_route_prepare(unsigned char* base, size_t len, char* line,
                            size_t line_cap, hp_dispatch_t* d)
{
    hp_runtime_t* rt = hp_rt_active();
    hp_rec_view_t v;
    int ridx;
    int fmt_idx;
    const int* sinks;
    int sink_count;
    int i;
    char* p;
    size_t n;

    if (rt == NULL) {
        return -1;
    }
    if (hp_rec_parse(base, len, &v) != 0) {
        return -1;
    }
    ridx = hp_route_match(rt, v.cat, (int)v.h->level);
    if (ridx >= 0) {
        fmt_idx = rt->rules[ridx].fmt_idx;
        sinks = rt->rules[ridx].sink_idx;
        sink_count = rt->rules[ridx].sink_count;
    } else {
        fmt_idx = rt->default_fmt_idx;
        sinks = rt->default_sinks;
        sink_count = rt->default_sink_count;
    }
    if (sink_count <= 0) {
        /* 未命中规则且无兜底 sink：按 §12.4 计入 dropped，不产生 per-sink 计数 */
        hp_atomic_fetch_add_u64(&g_rt.dropped, 1ull);
        return -1;
    }
    if (fmt_idx < 0) {
        return -1;
    }

    /* 事件字段 */
    memset(d, 0, sizeof(*d));
    d->ev.level = (hplogc_level_t)v.h->level;
    d->ev.category = v.cat;
    d->ev.ts_ns = v.h->ts_ns;
    d->ev.pid = v.h->pid;
    d->ev.tid = v.h->tid;
    d->ev.file = v.file;
    d->ev.line = v.h->line;
    d->ev.func = v.func;
    d->ev.msg = v.msg;
    d->ev.msg_len = (v.h->msg_len > 0) ? (size_t)(v.h->msg_len - 1) : 0u;
    d->ev.formatted = NULL;
    d->ev.formatted_len = 0;
    d->ev.fields = NULL;
    d->ev.field_count = 0;
    d->fields_dropped = v.h->fields_dropped;

    /* 物化结构化字段（供 HPLOGC_CAP_STRUCT 的 sink 消费，§4.10.3） */
    if (v.h->field_count > 0 && v.frec != NULL) {
        size_t cnt = v.h->field_count;
        if (cnt > HPLOGC_MAX_FIELDS) {
            cnt = HPLOGC_MAX_FIELDS;
        }
        p = v.fblob;
        for (i = 0; i < (int)cnt; i++) {
            d->fld[i].key = p;
            p += v.frec[i].key_len;
            d->fld[i].type = (hplogc_vtype_t)v.frec[i].type;
            memcpy(&d->fld[i].v, &v.frec[i].v, sizeof(d->fld[i].v));
            if (v.frec[i].type == (uint32_t)HPLOGC_V_STR
                && v.frec[i].str_len > 0) {
                d->fld[i].v.s = p;
                p += v.frec[i].str_len;
            }
        }
        d->ev.fields = d->fld;
        d->ev.field_count = cnt;
    }

    /* 整行格式化（§4.9 步骤 ⑦） */
    n = hp_format_render(&rt->fmts[fmt_idx], &v, &rt->cfg, line, line_cap);
    d->ev.formatted = line;
    d->ev.formatted_len = n;
    d->line_len = n;

    /* 拷贝 sink 索引 */
    d->sink_count = (sink_count > HPLOGC_MAX_SINKS) ? HPLOGC_MAX_SINKS
                                                    : sink_count;
    for (i = 0; i < d->sink_count; i++) {
        d->sink_idx[i] = sinks[i];
    }
    return 0;
}

/* ============================ 投递与记账 ============================ */

hplogc_sink_t* hp_sink_by_index(const hp_runtime_t* rt, int idx)
{
    if (rt == NULL || idx < 0 || idx >= rt->sink_count) {
        return NULL;
    }
    return rt->sinks[idx];
}

/**
 * @brief 单个 sink 的攒批组。
 *
 * 事件指针与下标**不内联**：`HP_DISPATCH_MAX`（4096）的内联数组会让
 * `HPLOGC_MAX_SINKS`（16）个组占去约 1 MB **栈**空间，而 macOS 非主线程默认栈
 * 仅 512 KB、Windows 默认线程栈 1 MB——在异步消费者线程上必然越界。
 * 故改为指向按实际批量 `n` 分配的堆缓冲切片（见 `hp_dispatch_emit`）。
 */
typedef struct {
    int                    sink; /*!< sink 索引 */
    const hplogc_event_t** evs;  /*!< 事件指针（堆缓冲切片，容量 n） */
    size_t*                idxs; /*!< 对应 items 下标（堆缓冲切片，容量 n） */
    size_t                 cnt;  /*!< 组内条数 */
} hp_group_t;

/**
 * @brief 把一批中间表示按 sink 分组投递，并完成 §12.4 的记账。
 */
static void hp_dispatch_emit(hp_dispatch_t* items, size_t n)
{
    hp_runtime_t* rt = hp_rt_active();
    hp_group_t groups[HPLOGC_MAX_SINKS];
    int gcount = 0;
    size_t i;
    int g;
    unsigned char ok[HP_DISPATCH_MAX];
    const hplogc_event_t** ev_pool = NULL; /*!< 事件指针池（堆） */
    size_t* idx_pool = NULL;               /*!< items 下标池（堆） */

    if (rt == NULL || items == NULL || n == 0) {
        return;
    }
    if (n > HP_DISPATCH_MAX) {
        n = HP_DISPATCH_MAX;
    }
    /* 分组缓冲按实际批量 n 从堆分配：避免 ~1 MB 栈帧在非主线程上越界
       （macOS 非主线程 512 KB、Windows 默认 1 MB）。 */
    ev_pool = (const hplogc_event_t**)calloc(HPLOGC_MAX_SINKS * n,
                                            sizeof(*ev_pool));
    idx_pool = (size_t*)calloc(HPLOGC_MAX_SINKS * n, sizeof(*idx_pool));
    if (ev_pool == NULL || idx_pool == NULL) {
        /* 分配失败：不写日志，按 §12.4 把本批计入 dropped（绝不崩溃） */
        free(ev_pool);
        free(idx_pool);
        for (i = 0; i < n; i++) {
            if (items[i].sink_count > 0) {
                hp_atomic_fetch_add_u64(&g_rt.dropped, 1ull);
            }
        }
        return;
    }
    memset(groups, 0, sizeof(groups));
    for (g = 0; g < HPLOGC_MAX_SINKS; g++) {
        groups[g].sink = -1;
        groups[g].evs = ev_pool + (size_t)g * n;
        groups[g].idxs = idx_pool + (size_t)g * n;
    }
    memset(ok, 0, n);

    /* 分组：保持每个 sink 内部的原始顺序（§4.10.4） */
    for (i = 0; i < n; i++) {
        int k;
        for (k = 0; k < items[i].sink_count; k++) {
            int s = items[i].sink_idx[k];
            int gi = -1;
            int j;
            for (j = 0; j < gcount; j++) {
                if (groups[j].sink == s) {
                    gi = j;
                    break;
                }
            }
            if (gi < 0) {
                if (gcount >= HPLOGC_MAX_SINKS) {
                    continue;
                }
                gi = gcount++;
                groups[gi].sink = s;
            }
            if (groups[gi].cnt < HP_DISPATCH_MAX) {
                groups[gi].evs[groups[gi].cnt] = &items[i].ev;
                groups[gi].idxs[groups[gi].cnt] = i;
                groups[gi].cnt++;
            }
        }
    }

    for (g = 0; g < gcount; g++) {
        hplogc_sink_t* s = hp_sink_by_index(rt, groups[g].sink);
        size_t k;
        if (s == NULL || s->ops == NULL) {
            continue;
        }
        hp_mutex_lock(&s->io_mu); /* 行级原子写：对实例串行化（§4.10.3） */
        if (s->ops->emit_batch != NULL && s->async_mode != 0) {
            int r = s->ops->emit_batch(s, groups[g].evs, groups[g].cnt);
            if (r >= 0) {
                size_t done = (size_t)r;
                if (done > groups[g].cnt) {
                    done = groups[g].cnt;
                }
                hp_atomic_fetch_add_u64(&s->written, (unsigned long long)done);
                hp_atomic_fetch_add_u64(
                    &s->failed, (unsigned long long)(groups[g].cnt - done));
                for (k = 0; k < done; k++) {
                    ok[groups[g].idxs[k]] = 1;
                    hp_atomic_fetch_add_u64(
                        &s->bytes_written,
                        (unsigned long long)groups[g].evs[k]->formatted_len);
                }
            } else {
                hp_atomic_fetch_add_u64(&s->failed,
                                        (unsigned long long)groups[g].cnt);
            }
        } else if (s->ops->emit != NULL) {
            for (k = 0; k < groups[g].cnt; k++) {
                s->ops->emit(s, groups[g].evs[k]);
                hp_atomic_fetch_add_u64(&s->written, 1ull);
                hp_atomic_fetch_add_u64(
                    &s->bytes_written,
                    (unsigned long long)groups[g].evs[k]->formatted_len);
                ok[groups[g].idxs[k]] = 1;
            }
        }
        hp_mutex_unlock(&s->io_mu);
    }

    /* 全局按条记账：至少 1 个 sink 成功 → written++；全部失败 → dropped++ */
    for (i = 0; i < n; i++) {
        if (items[i].sink_count <= 0) {
            continue;
        }
        if (ok[i]) {
            hp_atomic_fetch_add_u64(&g_rt.written, 1ull);
        } else {
            hp_atomic_fetch_add_u64(&g_rt.dropped, 1ull);
        }
    }
    free(ev_pool);
    free(idx_pool);
}

/* ============================ 对外（内部）入口 ============================ */

/** @brief 处理一条已构建的记录（同步路径：路由 → 格式化 → 投递）。 */
void hp_process_record(unsigned char* base, size_t len)
{
    hp_runtime_t* rt = hp_rt_active();
    hp_dispatch_t d;
    char* line;
    size_t cap;

    if (rt == NULL) {
        return;
    }
    if (g_rt.scratch_size == 0) {
        return;
    }
    /* 记录位于 scratch 前半区，行缓冲使用后半区 */
    line = (char*)hp_scratch(0) + g_rt.scratch_size + 1;
    cap = rt->cfg.max_log_length;
    if (hp_route_prepare(base, len, line, cap, &d) == 0) {
        hp_dispatch_emit(&d, 1);
    }
}

/** @brief 供异步消费者使用的批量处理入口（返回实际处理的条数）。 */
size_t hp_process_batch(unsigned char* recs, size_t rec_stride,
                        const size_t* lens, size_t n, char* lines,
                        size_t line_stride);

size_t hp_process_batch(unsigned char* recs, size_t rec_stride,
                        const size_t* lens, size_t n, char* lines,
                        size_t line_stride)
{
    static hp_dispatch_t* cache = NULL;
    static size_t cache_n = 0;
    size_t i;
    size_t kept = 0;

    if (n == 0) {
        return 0;
    }
    if (cache == NULL || cache_n < n) {
        free(cache);
        cache = (hp_dispatch_t*)malloc(sizeof(hp_dispatch_t) * n);
        if (cache == NULL) {
            return 0;
        }
        cache_n = n;
    }
    for (i = 0; i < n; i++) {
        if (hp_route_prepare(recs + i * rec_stride, lens[i],
                             lines + kept * line_stride, line_stride,
                             &cache[kept]) == 0) {
            kept++;
        }
    }
    if (kept > 0) {
        hp_dispatch_emit(cache, kept);
    }
    return kept;
}
