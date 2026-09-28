/* -*- coding: utf-8 -*- */
/**
 * @file sink_rollingfile.c
 * @brief rollingfile sink：文件写 + 轮转 / 备份 / 命名模板 / 软链 / fsync（§4.6、§4.7）。
 *
 * 通过 §3.3 的平台接口契约访问文件系统，**不含任何平台分支**。
 */

#include "hplogc_internal.h"

#include <stdlib.h>

#include <hplogc.h>

/** @brief 备份清理时可收集的最大条目数。 */
#define HP_RF_SCAN_MAX 512

/** @brief rollingfile sink 私有数据。 */
typedef struct {
    char        path[HPLOGC_MAX_PATH_LEN];   /*!< 活动文件路径 */
    char        dir[HPLOGC_MAX_PATH_LEN];    /*!< 所在目录 */
    char        base[HPLOGC_MAX_NAME_LEN];   /*!< 基础文件名（去目录与扩展名） */
    char        naming[HPLOGC_MAX_FMT_LEN];  /*!< 归档命名模板 */
    int         rotate;                      /*!< `hplogc_rotate_t` */
    unsigned long long max_size;             /*!< 按大小轮转阈值 */
    int         time_unit;                   /*!< `hplogc_time_unit_t` */
    unsigned    max_files;                   /*!< 备份数量（0 = 不限） */
    int         fsync;                       /*!< 私有 fsync（true ⇒ 至少 entry 级） */
    int         symlink_latest;              /*!< 是否创建 .latest 软链 */
    unsigned    file_perms;                  /*!< 文件权限 */
    unsigned    dir_perms;                   /*!< 目录权限 */
    hp_file_t*  fp;                          /*!< 活动文件句柄 */
    unsigned long long cur_size;             /*!< 当前文件大小（O(1) 检查） */
    int64_t     next_boundary;               /*!< 下一时间桶边界（epoch 秒） */
    int         naming_has_ts;               /*!< 模板是否含 {timestamp} */
    int         naming_has_idx;              /*!< 模板是否含 {index} */
    char        latest[HPLOGC_MAX_PATH_LEN]; /*!< .latest 软链路径 */
} hp_rf_priv_t;

/* ---------------------- 时间桶计算（O(1) 检查的基础） ---------------------- */

/** @brief 由年月日计算自 1970-01-01 起的天数（Howard Hinnant 算法）。 */
static long long hp_days_from_civil(long long y, unsigned m, unsigned d)
{
    long long era;
    unsigned  yoe;
    unsigned  mp;
    unsigned  doy;
    unsigned  doe;

    y -= (m <= 2);
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (unsigned)(y - era * 400);        /* [0,399] */
    mp = (m + 9u) % 12u;                    /* 3 月 = 0 */
    doy = (153u * mp + 2u) / 5u + d - 1u;   /* [0,365] */
    doe = yoe * 365u + yoe / 4u - yoe / 100u + doy; /* [0,146096] */
    return era * 146097 + (long long)doe - 719468;
}

/** @brief 计算下一个时间桶边界（epoch 秒）。 */
static int64_t hp_bucket_next(int64_t now_sec, int unit, int utc)
{
    struct tm tm;
    long long days;
    long long base_c;
    long long next_c;
    long long offset;
    int y;
    unsigned m;
    unsigned d;

    if (hp_localtime(now_sec, utc, &tm) != HPLOGC_OK) {
        return now_sec + 3600;
    }
    y = tm.tm_year + 1900;
    m = (unsigned)(tm.tm_mon + 1);
    d = (unsigned)tm.tm_mday;
    days = hp_days_from_civil(y, m, d);
    base_c = days * 86400 + (long long)tm.tm_hour * 3600
             + (long long)tm.tm_min * 60 + (long long)tm.tm_sec;
    offset = (long long)now_sec - base_c; /* 本地时区偏移（含 DST） */

    switch (unit) {
    case HPLOGC_TU_HOUR:
        next_c = base_c - ((long long)tm.tm_min * 60 + tm.tm_sec) + 3600;
        break;
    case HPLOGC_TU_WEEK: {
        long long dow = (days + 3) % 7; /* 1970-01-01 为周四 → 周一 = 0 */
        next_c = (days - dow) * 86400 + 7 * 86400;
        break;
    }
    case HPLOGC_TU_MONTH:
        if (m >= 12) {
            next_c = hp_days_from_civil((long long)y + 1, 1, 1) * 86400;
        } else {
            next_c = hp_days_from_civil(y, m + 1, 1) * 86400;
        }
        break;
    case HPLOGC_TU_DAY:
    default:
        next_c = days * 86400 + 86400;
        break;
    }
    return (int64_t)(next_c + offset);
}

/* ------------------------------ 配置与启动 ------------------------------ */

static int hp_rf_configure(hplogc_sink_t* sink, const char* key,
                           const char* val)
{
    hp_rf_priv_t* p = (hp_rf_priv_t*)hplogc_sink_priv(sink);

    if (p == NULL || key == NULL || val == NULL) {
        return -1;
    }
    if (strcmp(key, "path") == 0) {
        if (val[0] == '\0' || strlen(val) >= sizeof(p->path)) {
            return -1;
        }
        snprintf(p->path, sizeof(p->path), "%s", val);
        return 0;
    }
    if (strcmp(key, "rotate") == 0) {
        if (hp_ieq_str(val, "none") == 0) {
            p->rotate = HPLOGC_ROTATE_NONE;
        } else if (hp_ieq_str(val, "size") == 0) {
            p->rotate = HPLOGC_ROTATE_SIZE;
        } else if (hp_ieq_str(val, "time") == 0) {
            p->rotate = HPLOGC_ROTATE_TIME;
        } else if (hp_ieq_str(val, "both") == 0) {
            p->rotate = HPLOGC_ROTATE_BOTH;
        } else {
            return -1;
        }
        return 0;
    }
    if (strcmp(key, "max size") == 0) {
        return hp_parse_size_str(val, &p->max_size);
    }
    if (strcmp(key, "time unit") == 0) {
        if (hp_ieq_str(val, "hour") == 0) {
            p->time_unit = HPLOGC_TU_HOUR;
        } else if (hp_ieq_str(val, "day") == 0) {
            p->time_unit = HPLOGC_TU_DAY;
        } else if (hp_ieq_str(val, "week") == 0) {
            p->time_unit = HPLOGC_TU_WEEK;
        } else if (hp_ieq_str(val, "month") == 0) {
            p->time_unit = HPLOGC_TU_MONTH;
        } else {
            return -1;
        }
        return 0;
    }
    if (strcmp(key, "max files") == 0) {
        p->max_files = (unsigned)strtoul(val, NULL, 10);
        return 0;
    }
    if (strcmp(key, "fsync") == 0) {
        int b = hp_parse_bool_str(val);
        if (b < 0) {
            return -1;
        }
        p->fsync = b;
        return 0;
    }
    if (strcmp(key, "symlink latest") == 0) {
        int b = hp_parse_bool_str(val);
        if (b < 0) {
            return -1;
        }
        p->symlink_latest = b;
        return 0;
    }
    if (strcmp(key, "rotate naming") == 0) {
        if (strlen(val) >= sizeof(p->naming)) {
            return -1;
        }
        snprintf(p->naming, sizeof(p->naming), "%s", val);
        return 0;
    }
    if (strcmp(key, "file perms") == 0) {
        p->file_perms = (unsigned)strtoul(val, NULL, 8);
        return 0;
    }
    if (strcmp(key, "dir perms") == 0) {
        p->dir_perms = (unsigned)strtoul(val, NULL, 8);
        return 0;
    }
    return -1;
}

static int hp_rf_init(hplogc_sink_t* sink)
{
    hp_rf_priv_t* p = (hp_rf_priv_t*)hplogc_sink_priv(sink);

    if (p == NULL) {
        return -1;
    }
    /* 默认值填充：一律条件赋值（§4.10.2） */
    if (p->naming[0] == '\0') {
        snprintf(p->naming, sizeof(p->naming), "%s",
                 "{base}.{timestamp}.{index}.log");
    }
    if (p->file_perms == 0) {
        p->file_perms = 0644u;
    }
    if (p->dir_perms == 0) {
        p->dir_perms = 0755u;
    }
    p->naming_has_ts = (strstr(p->naming, "{timestamp}") != NULL) ? 1 : 0;
    p->naming_has_idx = (strstr(p->naming, "{index}") != NULL) ? 1 : 0;
    if (!p->naming_has_ts && !p->naming_has_idx) {
        hp_err_printf("hplogc: rotate naming 模板必须含 {index} 或 {timestamp}");
        return -1; /* §10.4 */
    }
#ifndef HPLOGC_HAS_ROTATE
    if (p->rotate != HPLOGC_ROTATE_NONE) {
        hp_err_printf("hplogc: 本构建已裁剪轮转（HPLOGC_ENABLE_ROTATE=OFF）");
    }
    p->rotate = HPLOGC_ROTATE_NONE;
#endif
    return 0;
}

/** @brief 打开活动文件；成功返回 0。 */
static int hp_rf_open(hp_rf_priv_t* p)
{
    if (p->fp != NULL) {
        return 0;
    }
    p->fp = hp_fopen(p->path, "a");
    if (p->fp == NULL) {
        return -1;
    }
    {
        uint64_t sz = 0;
        if (hp_ftell_size(p->fp, &sz) == HPLOGC_OK) {
            p->cur_size = sz;
        }
    }
    if (p->file_perms != 0) {
        hp_chmod_file(p->path, p->file_perms);
    }
    if (p->rotate == HPLOGC_ROTATE_TIME || p->rotate == HPLOGC_ROTATE_BOTH) {
        int64_t now = (int64_t)(hp_now_realtime_ns() / 1000000000ull);
        p->next_boundary = hp_bucket_next(now, p->time_unit, hp_global_utc());
    }
    if (p->symlink_latest) {
        hp_symlink(p->path, p->latest);
    }
    return 0;
}

static int hp_rf_start(hplogc_sink_t* sink)
{
    hp_rf_priv_t* p = (hp_rf_priv_t*)hplogc_sink_priv(sink);
    const char* slash;
    size_t i;

    if (p == NULL || p->path[0] == '\0') {
        return -1; /* path 必填（§4.7.2） */
    }
    /* 目录与基础名 */
    if (hp_path_dirname(p->path, p->dir, sizeof(p->dir)) != HPLOGC_OK) {
        return -1;
    }
    {
        const char* bn = hp_path_basename(p->path);
        if (bn == NULL) {
            return -1;
        }
        snprintf(p->base, sizeof(p->base), "%s", bn);
    }
    slash = strrchr(p->base, '.');
    if (slash != NULL) {
        p->base[slash - p->base] = '\0'; /* 去掉最后一段扩展名 */
    }
    snprintf(p->latest, sizeof(p->latest), "%s.latest", p->path);
    (void)i;

    if (hp_mkdirs(p->dir, p->dir_perms) != HPLOGC_OK) {
        return -1;
    }
    if (hp_rf_open(p) != 0) {
        return -1; /* init 时文件打开失败 → 启动失败（§4.7.1 / §9） */
    }
    return 0;
}

/* ------------------------------ 归档命名与清理 ------------------------------ */

/** @brief 按模板生成归档名（index 递增至可用名，§4.6）。 */
static void hp_rf_archive_name(hp_rf_priv_t* p, int64_t now_sec,
                               unsigned long long idx, char* out, size_t cap)
{
    char tbuf[32];
    const char* t = p->naming;
    size_t used = 0;
    struct tm tm;

    if (hp_localtime(now_sec, hp_global_utc(), &tm) == HPLOGC_OK) {
        hp_strftime_ext(tbuf, sizeof(tbuf), "%Y%m%d_%H%M%S", &tm, 0);
    } else {
        snprintf(tbuf, sizeof(tbuf), "19700101_000000");
    }
    out[0] = '\0';
    while (*t != '\0' && used + 1 < cap) {
        if (strncmp(t, "{base}", 6) == 0) {
            used += (size_t)snprintf(out + used, cap - used, "%s", p->base);
            t += 6;
        } else if (strncmp(t, "{timestamp}", 11) == 0) {
            used += (size_t)snprintf(out + used, cap - used, "%s", tbuf);
            t += 11;
        } else if (strncmp(t, "{index}", 7) == 0) {
            used += (size_t)snprintf(out + used, cap - used, "%llu", idx);
            t += 7;
        } else {
            out[used++] = *t++;
            out[used] = '\0';
        }
    }
    out[used < cap ? used : cap - 1] = '\0';
}

/** @brief 备份条目（清理排序用）。 */
typedef struct {
    char  name[HPLOGC_MAX_NAME_LEN]; /*!< 文件名 */
    char  ts[32];                    /*!< 时间戳串（无则空） */
    unsigned long long idx;          /*!< 序号（无则 0） */
} hp_rf_entry_t;

/** @brief 目录扫描上下文。 */
typedef struct {
    hp_rf_priv_t* p;                  /*!< sink 私有数据 */
    hp_rf_entry_t ents[HP_RF_SCAN_MAX]; /*!< 收集到的条目 */
    size_t        count;              /*!< 条目数 */
} hp_rf_scan_ctx_t;

/** @brief 判断文件名是否匹配命名模板，并解析出 index / timestamp。 */
static int hp_rf_match(const hp_rf_priv_t* p, const char* name,
                       hp_rf_entry_t* ent);

static int hp_rf_match(const hp_rf_priv_t* p, const char* name,
                       hp_rf_entry_t* ent)
{
    const char* t = p->naming;
    const char* n = name;
    int got_idx = -1;
    char tsbuf[32];
    size_t ts_len = 0;

    ent->ts[0] = '\0';
    ent->idx = 0;
    tsbuf[0] = '\0';
    while (*t != '\0') {
        if (strncmp(t, "{base}", 6) == 0) {
            size_t bl = strlen(p->base);
            if (strncmp(n, p->base, bl) != 0) {
                return 0;
            }
            n += bl;
            t += 6;
        } else if (strncmp(t, "{timestamp}", 11) == 0) {
            size_t k = 0;
            while (n[k] >= '0' && n[k] <= '9' && k < 15) {
                k++;
            }
            if (k == 0) {
                return 0;
            }
            if (k < sizeof(tsbuf)) {
                memcpy(tsbuf, n, k);
                tsbuf[k] = '\0';
                ts_len = k;
            }
            n += k;
            t += 11;
        } else if (strncmp(t, "{index}", 7) == 0) {
            size_t k = 0;
            unsigned long long v = 0;
            while (n[k] >= '0' && n[k] <= '9') {
                v = v * 10ull + (unsigned long long)(n[k] - '0');
                k++;
            }
            if (k == 0) {
                return 0;
            }
            ent->idx = v;
            got_idx = 1;
            n += k;
            t += 7;
        } else {
            if (*n != *t) {
                return 0;
            }
            n++;
            t++;
        }
    }
    if (*n != '\0') {
        return 0; /* 名称有多余后缀 */
    }
    (void)got_idx;
    if (ts_len > 0 && ts_len < sizeof(ent->ts)) {
        memcpy(ent->ts, tsbuf, ts_len + 1);
    }
    snprintf(ent->name, sizeof(ent->name), "%s", name);
    return 1;
}

/** @brief 目录扫描回调：收集匹配的备份条目。 */
static void hp_rf_scan_cb(const char* name, void* ctx)
{
    hp_rf_scan_ctx_t* c = (hp_rf_scan_ctx_t*)ctx;
    hp_rf_entry_t ent;

    if (c == NULL || c->count >= HP_RF_SCAN_MAX) {
        return;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return;
    }
    if (!hp_rf_match(c->p, name, &ent)) {
        return;
    }
    c->ents[c->count] = ent;
    c->count++;
}

/** @brief 清理超出 `max files` 的最旧备份（§4.6）。 */
static void hp_rf_cleanup(hp_rf_priv_t* p)
{
    hp_rf_scan_ctx_t ctx;
    size_t i;
    size_t j;

    if (p->max_files == 0u) {
        return; /* 0 = 不限制 */
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.p = p;
    {
        /* 以模板首个占位符之前的固定前缀过滤，避免扫描整个目录 */
        char prefix[HPLOGC_MAX_NAME_LEN];
        size_t k = 0;
        const char* t = p->naming;
        while (*t != '\0' && strncmp(t, "{", 1) != 0
               && k + 1 < sizeof(prefix)) {
            if (strncmp(t, "{base}", 6) == 0) {
                size_t bl = strlen(p->base);
                if (k + bl + 1 > sizeof(prefix)) {
                    break;
                }
                memcpy(prefix + k, p->base, bl);
                k += bl;
                t += 6;
                continue;
            }
            prefix[k++] = *t++;
        }
        prefix[k] = '\0';
        hp_dir_scan(p->dir, (k > 0) ? prefix : NULL, hp_rf_scan_cb, &ctx);
    }
    if (ctx.count <= (size_t)p->max_files) {
        return;
    }
    /* 选择排序：按时间戳（含 {timestamp}）或序号升序，删除最旧的 */
    for (i = 0; i + 1 < ctx.count; i++) {
        for (j = i + 1; j < ctx.count; j++) {
            int cmp;
            if (p->naming_has_ts) {
                cmp = strcmp(ctx.ents[i].ts, ctx.ents[j].ts);
            } else {
                cmp = (ctx.ents[i].idx < ctx.ents[j].idx) ? -1
                      : (ctx.ents[i].idx > ctx.ents[j].idx) ? 1
                                                            : 0;
            }
            if (cmp > 0) {
                hp_rf_entry_t tmp = ctx.ents[i];
                ctx.ents[i] = ctx.ents[j];
                ctx.ents[j] = tmp;
            }
        }
    }
    {
        size_t del = ctx.count - (size_t)p->max_files;
        char full[HPLOGC_MAX_PATH_LEN];
        for (i = 0; i < del; i++) {
            snprintf(full, sizeof(full), "%s/%s", p->dir, ctx.ents[i].name);
            hp_unlink(full);
        }
    }
}

/** @brief 执行一次轮转。 */
static void hp_rf_rotate(hp_rf_priv_t* p)
{
    char name[HPLOGC_MAX_NAME_LEN];
    char full[HPLOGC_MAX_PATH_LEN];
    int64_t now = (int64_t)(hp_now_realtime_ns() / 1000000000ull);
    unsigned long long idx;

    if (p->fp != NULL) {
        hp_fflush(p->fp);
        hp_fclose(p->fp);
        p->fp = NULL;
    }
    if (!hp_path_exists(p->path)) {
        /* 文件被外部移除：直接重建活动文件 */
        (void)hp_rf_open(p);
        return;
    }
    idx = 1;
    for (;;) {
        hp_rf_archive_name(p, now, idx, name, sizeof(name));
        snprintf(full, sizeof(full), "%s/%s", p->dir, name);
        if (!hp_path_exists(full)) {
            break;
        }
        idx++;
        if (idx > 100000ull) {
            break; /* 兜底，避免异常情况下的死循环 */
        }
    }
    if (hp_rename(p->path, full) != HPLOGC_OK) {
        /* 重命名失败：恢复写原文件 */
        (void)hp_rf_open(p);
        return;
    }
    p->cur_size = 0;
    (void)hp_rf_open(p);
    hp_rf_cleanup(p);
}

/* ------------------------------ 写出 ------------------------------ */

static void hp_rf_write(hp_rf_priv_t* p, hplogc_sink_t* sink,
                        const char* data, size_t len)
{
    if (p->fp == NULL) {
        if (hp_rf_open(p) != 0) {
            hp_sink_note_failed(sink);
            return;
        }
    }
    if (hp_fwrite(p->fp, data, len) != HPLOGC_OK) {
        /* 运行期写失败：尝试重开一次，仍失败则本条在该 sink 上丢弃（§9） */
        hp_fclose(p->fp);
        p->fp = NULL;
        if (hp_rf_open(p) != 0 || hp_fwrite(p->fp, data, len) != HPLOGC_OK) {
            hp_sink_note_failed(sink);
            hp_warn_throttled("hplogc: 写入 '%s' 失败，该条已丢弃", p->path);
            return;
        }
    }
    p->cur_size += len;
}

static void hp_rf_emit(hplogc_sink_t* sink, const hplogc_event_t* ev)
{
    hp_rf_priv_t* p = (hp_rf_priv_t*)hplogc_sink_priv(sink);
    const char* eol;

    if (p == NULL || ev == NULL || ev->formatted == NULL) {
        return;
    }
    eol = hp_platform_eol();
#ifdef HPLOGC_HAS_ROTATE
    /* 轮转检查：O(1)（不逐条 stat，§4.6） */
    if ((p->rotate == HPLOGC_ROTATE_SIZE || p->rotate == HPLOGC_ROTATE_BOTH)
        && p->max_size > 0
        && p->cur_size + ev->formatted_len + strlen(eol) >= p->max_size) {
        hp_rf_rotate(p);
    } else if ((p->rotate == HPLOGC_ROTATE_TIME
                || p->rotate == HPLOGC_ROTATE_BOTH)
               && p->next_boundary > 0) {
        int64_t now = (int64_t)(hp_now_realtime_ns() / 1000000000ull);
        if (now >= p->next_boundary) {
            hp_rf_rotate(p);
        }
    }
#endif
    /* 整行一次写入（CAP_LINE_ATOMIC，核心已串行化）。
     * 行尾换行由格式模板的 %n 占位符负责，sink 不再追加，避免重复（§4.10.3）。 */
    hp_rf_write(p, sink, ev->formatted, ev->formatted_len);
    if (p->fsync && p->fp != NULL) {
        hp_fsync_file(p->fp);
    }
}

static int hp_rf_emit_batch(hplogc_sink_t* sink,
                            const hplogc_event_t* const* evs, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        hp_rf_emit(sink, evs[i]);
    }
    return (int)n;
}

static int hp_rf_flush(hplogc_sink_t* sink)
{
    hp_rf_priv_t* p = (hp_rf_priv_t*)hplogc_sink_priv(sink);
    if (p == NULL || p->fp == NULL) {
        return 0;
    }
    return hp_fflush(p->fp);
}

static void hp_rf_destroy(hplogc_sink_t* sink)
{
    hp_rf_priv_t* p = (hp_rf_priv_t*)hplogc_sink_priv(sink);
    if (p == NULL) {
        return;
    }
    if (p->fp != NULL) {
        hp_fflush(p->fp);
        hp_fclose(p->fp);
        p->fp = NULL;
    }
}

/** @brief 内置 fsync 钩子（§18-C8）。 */
int hp_rollingfile_fsync(hplogc_sink_t* sink);

int hp_rollingfile_fsync(hplogc_sink_t* sink)
{
    hp_rf_priv_t* p = (hp_rf_priv_t*)hplogc_sink_priv(sink);
    if (p == NULL || p->fp == NULL) {
        return HPLOGC_ERR_UNSUPPORTED;
    }
    return hp_fsync_file(p->fp);
}

const hplogc_sink_ops_t hp_sink_rollingfile_ops = {
    "rollingfile",
    HPLOGC_SINK_ABI_VERSION,
    HPLOGC_CAP_SYNC | HPLOGC_CAP_ASYNC | HPLOGC_CAP_LINE_ATOMIC
        | HPLOGC_CAP_FSYNC,
    sizeof(hp_rf_priv_t),
    hp_rf_configure,
    hp_rf_init,
    hp_rf_start,
    hp_rf_emit,
    hp_rf_emit_batch,
    hp_rf_flush,
    hp_rf_destroy,
    { NULL, NULL, NULL, NULL }
};
