/* -*- coding: utf-8 -*- */
/**
 * @file format.c
 * @brief 格式预编译与整行渲染（§12、§12.1 ~ §12.3、§4.9 步骤 ⑦）。
 *
 * 模板在 init / 热加载时预编译为动作序列，热路径不再做字符串解析（§4.9）。
 */

#include "hplogc_internal.h"

#include <stdio.h>

#include <hplogc.h>

/** @brief 默认格式模板（§10.3；内置格式名始终可用，§7.2）。 */
typedef struct {
    const char* name; /*!< 格式名 */
    const char* tmpl; /*!< 默认模板 */
} hp_builtin_fmt_t;

static const hp_builtin_fmt_t g_builtin_fmts[] = {
    { "minimal",     "%level: %msg%n" },
    { "standard",    "%time [%level] %msg%n" },
    { "categorized", "%time [%level] [%category] %msg%n" },
    { "detailed",
      "%time [%level] [pid:%pid tid:%tid] [%file:%line %func] [%category] "
      "%msg%n" },
    { "json",
      "{\"time\":\"%time\",\"level\":\"%level\",\"category\":\"%category\","
      "\"pid\":%pid,\"tid\":%tid,\"file\":\"%file\",\"line\":%line,"
      "\"msg\":\"%msg\"}%n" }
};

/** @brief 内置格式数量。 */
#define HP_BUILTIN_FMT_COUNT \
    ((int)(sizeof(g_builtin_fmts) / sizeof(g_builtin_fmts[0])))

/** @brief 占位符名表（与 `hp_ph_t` 顺序一致）。 */
static const char* const g_ph_names[] = {
    "level", "time", "pid", "tid", "file", "line", "func", "msg",
    "category", "n"
};

int hp_format_compile(const char* name, const char* tmpl, hp_fmt_t* out)
{
    const char* p;
    int lit_start = -1;
    size_t lit_len = 0;

    if (name == NULL || tmpl == NULL || out == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s", name);
    out->is_json = (strcmp(name, "json") == 0) ? 1 : 0; /* §12.3：按名判定 */
    out->node_count = 0;
    out->lit_used = 0;

    /** @brief 把已积累的字面文本刷入动作序列。 */
#define HP_FLUSH_LIT()                                                        \
    do {                                                                      \
        if (lit_len > 0) {                                                    \
            if (out->node_count >= HPLOGC_FMT_NODES                            \
                || (int)out->lit_used + (int)lit_len                          \
                       > (int)sizeof(out->lits) - 1) {                        \
                return HPLOGC_ERR_CONFIG;                                     \
            }                                                                 \
            memcpy(out->lits + out->lit_used, tmpl + lit_start, lit_len);      \
            out->nodes[out->node_count].kind = 0;                              \
            out->nodes[out->node_count].off = (unsigned short)out->lit_used;   \
            out->nodes[out->node_count].len = (unsigned short)lit_len;         \
            out->lit_used += (int)lit_len;                                    \
            out->node_count++;                                                 \
            lit_len = 0;                                                       \
        }                                                                      \
    } while (0)

    for (p = tmpl; *p != '\0';) {
        if (*p != '%') {
            if (lit_len == 0) {
                lit_start = (int)(p - tmpl);
            }
            lit_len++;
            p++;
            continue;
        }
        /* 占位符 */
        if (p[1] == '%') {
            if (lit_len == 0) {
                lit_start = (int)(p - tmpl);
            }
            lit_len++; /* 先记 '%'，随后跳过第二个 '%' */
            p += 2;
            continue;
        }
        HP_FLUSH_LIT();
        {
            int i;
            int found = -1;
            size_t plen;
            const char* q = p + 1;
            while (*q != '\0' && (q - p) < 16
                   && ((*q >= 'a' && *q <= 'z')
                       || (*q >= 'A' && *q <= 'Z'))) {
                q++;
            }
            plen = (size_t)(q - (p + 1));
            for (i = 0; i < HP_PH_COUNT; i++) {
                if (strlen(g_ph_names[i]) == plen
                    && strncmp(g_ph_names[i], p + 1, plen) == 0) {
                    found = i;
                    break;
                }
            }
            if (found < 0) {
                return HPLOGC_ERR_CONFIG; /* 未知占位符 / 行尾孤立 %（§12.1） */
            }
            if (out->node_count >= HPLOGC_FMT_NODES) {
                return HPLOGC_ERR_CONFIG;
            }
            out->nodes[out->node_count].kind = 1;
            out->nodes[out->node_count].off = (unsigned short)found;
            out->nodes[out->node_count].len = 0;
            out->node_count++;
            p = q;
        }
    }
    HP_FLUSH_LIT();
#undef HP_FLUSH_LIT
    return HPLOGC_OK;
}

/** @brief 取内置格式模板；未找到返回 NULL。 */
const char* hp_builtin_format_tmpl(const char* name);

const char* hp_builtin_format_tmpl(const char* name)
{
    int i;
    for (i = 0; i < HP_BUILTIN_FMT_COUNT; i++) {
        if (strcmp(g_builtin_fmts[i].name, name) == 0) {
            return g_builtin_fmts[i].tmpl;
        }
    }
    return NULL;
}

/** @brief 内置格式名列表（供配置装配期补全）。 */
const char* hp_builtin_format_name(int idx);

const char* hp_builtin_format_name(int idx)
{
    if (idx < 0 || idx >= HP_BUILTIN_FMT_COUNT) {
        return NULL;
    }
    return g_builtin_fmts[idx].name;
}

/** @brief 内置格式数量。 */
int hp_builtin_format_count(void);

int hp_builtin_format_count(void)
{
    return HP_BUILTIN_FMT_COUNT;
}

/* ------------------------------- 渲染 ------------------------------- */

/** @brief 追加定长文本（带上限）。 */
static void hp_out(char* dst, size_t* used, size_t limit, const char* s,
                   size_t n)
{
    size_t i;
    for (i = 0; i < n && *used < limit; i++) {
        dst[(*used)++] = s[i];
    }
}

/**
 * @brief 追加 JSON 字符串转义后的文本（§12 JSON 转义行）。
 *
 * 仅转义 `"` `\` 与控制字符，不做注入转义（json 格式跳过 escape injection）。
 */
static void hp_out_json_escape(char* dst, size_t* used, size_t limit,
                               const char* s)
{
    static const char* hex = "0123456789abcdef";
    size_t i;

    for (i = 0; s[i] != '\0' && *used < limit; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            if (*used + 2 > limit) {
                break;
            }
            dst[(*used)++] = '\\';
            dst[(*used)++] = (char)c;
        } else if (c == '\n') {
            if (*used + 2 > limit) {
                break;
            }
            dst[(*used)++] = '\\';
            dst[(*used)++] = 'n';
        } else if (c == '\r') {
            if (*used + 2 > limit) {
                break;
            }
            dst[(*used)++] = '\\';
            dst[(*used)++] = 'r';
        } else if (c == '\t') {
            if (*used + 2 > limit) {
                break;
            }
            dst[(*used)++] = '\\';
            dst[(*used)++] = 't';
        } else if (c < 0x20) {
            if (*used + 6 > limit) {
                break;
            }
            dst[(*used)++] = '\\';
            dst[(*used)++] = 'u';
            dst[(*used)++] = '0';
            dst[(*used)++] = '0';
            dst[(*used)++] = hex[(c >> 4) & 0xF];
            dst[(*used)++] = hex[c & 0xF];
        } else {
            dst[(*used)++] = (char)c;
        }
    }
}

/**
 * @brief 追加注入转义后的文本（§4.9 步骤 ⑦）。
 *
 * 仅转义换行符（`\n` / `\r` → 字面文本）与 ANSI 转义序列（`ESC` → `\x1b`）。
 */
static void hp_out_inject_escape(char* dst, size_t* used, size_t limit,
                                 const char* s)
{
    size_t i;

    for (i = 0; s[i] != '\0' && *used < limit; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\n') {
            if (*used + 2 > limit) {
                break;
            }
            dst[(*used)++] = '\\';
            dst[(*used)++] = 'n';
        } else if (c == '\r') {
            if (*used + 2 > limit) {
                break;
            }
            dst[(*used)++] = '\\';
            dst[(*used)++] = 'r';
        } else if (c == 0x1b) {
            if (*used + 4 > limit) {
                break;
            }
            dst[(*used)++] = '\\';
            dst[(*used)++] = 'x';
            dst[(*used)++] = '1';
            dst[(*used)++] = 'b';
        } else {
            dst[(*used)++] = (char)c;
        }
    }
}

/** @brief 判定源码位置是否被捕获（§12）。 */
static int hp_src_loc_on(const hp_config_t* cfg)
{
#ifndef HPLOGC_HAS_SOURCE_LOC
    (void)cfg;
    return 0;
#else
    return cfg->capture_source_loc ? 1 : 0;
#endif
}

size_t hp_format_render(const hp_fmt_t* f, const hp_rec_view_t* v,
                        const hp_config_t* cfg, char* out, size_t cap)
{
    size_t used = 0;
    size_t limit;
    size_t soft;
    size_t marker_len;
    int i;
    const char* eol = "\n";

    if (f == NULL || v == NULL || cfg == NULL || out == NULL || cap == 0) {
        return 0;
    }
    marker_len = strlen(cfg->trunc_marker);
    limit = cap - 1; /* 预留结尾 NUL */
    soft = (limit > marker_len) ? (limit - marker_len) : limit;

    if (cfg->newline == (int)HPLOGC_NEWLINE_CRLF) {
        eol = "\r\n";
    } else if (cfg->newline == (int)HPLOGC_NEWLINE_AUTO) {
        eol = hp_platform_eol();
    } else {
        eol = "\n";
    }

    for (i = 0; i < f->node_count; i++) {
        if (f->nodes[i].kind == 0) {
            size_t n = f->nodes[i].len;
            if (used + n > soft) {
                /* 触及截断边界 */
                if (used < soft) {
                    hp_out(out, &used, soft, f->lits + f->nodes[i].off,
                           soft - used);
                }
                used = soft;
                break;
            }
            hp_out(out, &used, soft, f->lits + f->nodes[i].off, n);
            continue;
        }
        {
            char tmp[64];
            switch (f->nodes[i].off) {
            case HP_PH_LEVEL:
                hp_out(out, &used, soft, hplogc_level_name(
                           (hplogc_level_t)v->h->level),
                       strlen(hplogc_level_name((hplogc_level_t)v->h->level)));
                break;
            case HP_PH_TIME: {
                char tbuf[HPLOGC_MAX_FMT_LEN];
                size_t n = hp_time_render(tbuf, sizeof(tbuf), v->h->ts_ns, cfg);
                hp_out(out, &used, soft, tbuf, n);
                break;
            }
            case HP_PH_PID:
            case HP_PH_TID: {
                int fmtid = (f->nodes[i].off == HP_PH_PID) ? cfg->pid_fmt
                                                           : cfg->tid_fmt;
                unsigned long long val = (f->nodes[i].off == HP_PH_PID)
                                             ? (unsigned long long)v->h->pid
                                             : v->h->tid;
                if (f->is_json) {
                    fmtid = HPLOGC_IDF_DECIMAL; /* §12：json 强制十进制 */
                }
                if (fmtid == HPLOGC_IDF_NONE) {
                    /* 空串：不删除模板中的相邻字面文本（§12.1） */
                } else if (fmtid == HPLOGC_IDF_HEX) {
                    snprintf(tmp, sizeof(tmp), "0x%llx", val);
                    hp_out(out, &used, soft, tmp, strlen(tmp));
                } else {
                    snprintf(tmp, sizeof(tmp), "%llu", val);
                    hp_out(out, &used, soft, tmp, strlen(tmp));
                }
                break;
            }
            case HP_PH_FILE: {
                const char* s = hp_src_loc_on(cfg) ? v->file : "";
                if (s == NULL) {
                    s = "";
                }
                if (f->is_json) {
                    hp_out_json_escape(out, &used, soft, s);
                } else if (cfg->escape_injection) {
                    hp_out_inject_escape(out, &used, soft, s);
                } else {
                    hp_out(out, &used, soft, s, strlen(s));
                }
                break;
            }
            case HP_PH_FUNC: {
                const char* s = hp_src_loc_on(cfg) ? v->func : "";
                if (s == NULL) {
                    s = "";
                }
                if (f->is_json) {
                    hp_out_json_escape(out, &used, soft, s);
                } else if (cfg->escape_injection) {
                    hp_out_inject_escape(out, &used, soft, s);
                } else {
                    hp_out(out, &used, soft, s, strlen(s));
                }
                break;
            }
            case HP_PH_LINE: {
                int ln = hp_src_loc_on(cfg) ? v->h->line : 0;
                snprintf(tmp, sizeof(tmp), "%d", ln);
                hp_out(out, &used, soft, tmp, strlen(tmp));
                break;
            }
            case HP_PH_MSG: {
                const char* s = (v->msg != NULL) ? v->msg : "";
                if (f->is_json) {
                    hp_out_json_escape(out, &used, soft, s);
                } else if (cfg->escape_injection) {
                    hp_out_inject_escape(out, &used, soft, s);
                } else {
                    hp_out(out, &used, soft, s, strlen(s));
                }
                break;
            }
            case HP_PH_CATEGORY: {
                const char* s = v->cat;
#ifndef HPLOGC_HAS_CATEGORY
                s = ""; /* §4.8：CATEGORY=OFF 时 %category 展开为空串 */
#else
                if (s == NULL) {
                    s = "";
                }
#endif
                if (f->is_json) {
                    hp_out_json_escape(out, &used, soft, s);
                } else if (cfg->escape_injection) {
                    hp_out_inject_escape(out, &used, soft, s);
                } else {
                    hp_out(out, &used, soft, s, strlen(s));
                }
                break;
            }
            case HP_PH_NEWLINE:
                hp_out(out, &used, soft, eol, strlen(eol));
                break;
            default:
                break;
            }
        }
        if (used >= soft) {
            break;
        }
    }

    if (used >= soft) {
        /* 触发截断：附加 marker，截断后总长仍不超过 max_log_length（§9） */
        if (used > soft) {
            used = soft;
        }
        hp_out(out, &used, limit, cfg->trunc_marker, marker_len);
    }
    out[used < limit ? used : limit] = '\0';
    return used;
}
