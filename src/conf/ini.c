/* -*- coding: utf-8 -*- */
/**
 * @file ini.c
 * @brief 自研行式 INI 解析器（zlog conf 兼容词法，§10.1）。
 *
 * 仅识别 `#` 注释、成对双引号、行尾 `\` 续行、首个 `=` 分割键值、节序校验。
 * 不依赖任何第三方库；解析失败带文件名与行号（§10.4）。
 */

#include "hplogc_internal.h"

#include <stdio.h>

#include <hplogc.h>

/** @brief 节编号（出现顺序见 §10.2）。 */
typedef enum {
    SEC_NONE = -1,
    SEC_BUILD = 0,
    SEC_GLOBAL = 1,
    SEC_FORMATS = 2,
    SEC_OUTPUTS = 3,
    SEC_BUFFER = 4,
    SEC_ASYNC = 5,
    SEC_THROTTLE = 6,
    SEC_RULES = 7,
    SEC_ADVANCED = 8
} hp_ini_sec_t;

/** @brief 节名到编号的映射。 */
static int hp_sec_of(const char* name)
{
    if (strcmp(name, "build") == 0) {
        return SEC_BUILD;
    }
    if (strcmp(name, "global") == 0) {
        return SEC_GLOBAL;
    }
    if (strncmp(name, "formats", 7) == 0) {
        return SEC_FORMATS;
    }
    if (strncmp(name, "outputs", 7) == 0) {
        return SEC_OUTPUTS;
    }
    if (strcmp(name, "buffer") == 0) {
        return SEC_BUFFER;
    }
    if (strcmp(name, "async") == 0) {
        return SEC_ASYNC;
    }
    if (strcmp(name, "throttle") == 0) {
        return SEC_THROTTLE;
    }
    if (strcmp(name, "rules") == 0) {
        return SEC_RULES;
    }
    if (strcmp(name, "advanced") == 0) {
        return SEC_ADVANCED;
    }
    return -2; /* 未知节 */
}

/** @brief 去掉行首尾空白（含 \r）。 */
static char* hp_ini_trim(char* s)
{
    char* e;
    while (*s == ' ' || *s == '\t' || *s == '\r') {
        s++;
    }
    e = s;
    while (*e != '\0') {
        e++;
    }
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'
                     || e[-1] == '\n')) {
        e--;
    }
    *e = '\0';
    return s;
}

/** @brief 去掉外层成对双引号。 */
static void hp_ini_unquote(char* s)
{
    size_t n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        memmove(s, s + 1, n - 2);
        s[n - 2] = '\0';
    }
}

/** @brief 在引号之外、且前置空白的 `#` 处截断行内注释。 */
static void hp_ini_strip_comment(char* s)
{
    int in_quote = 0;
    size_t i;
    for (i = 0; s[i] != '\0'; i++) {
        if (s[i] == '"') {
            in_quote = !in_quote;
        } else if (s[i] == '#' && !in_quote) {
            if (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t') {
                s[i] = '\0';
                break;
            }
        }
    }
}

/** @brief 追加一条解析项（动态数组）。 */
static int hp_ini_push(hp_ini_t* ini, int sec, const char* key,
                       const char* val, int line)
{
    size_t idx;
    if (ini->count >= ini->cap) {
        size_t ncap = (ini->cap == 0) ? 64 : (ini->cap * 2);
        hp_ini_item_t* nitems =
            (hp_ini_item_t*)realloc(ini->items, ncap * sizeof(hp_ini_item_t));
        if (nitems == NULL) {
            return -1;
        }
        ini->items = nitems;
        ini->cap = ncap;
    }
    idx = ini->count++;
    snprintf(ini->items[idx].key, sizeof(ini->items[idx].key), "%s", key);
    snprintf(ini->items[idx].val, sizeof(ini->items[idx].val), "%s", val);
    ini->items[idx].section = sec;
    ini->items[idx].line = line;
    return 0;
}

int hp_ini_parse(const char* path, hp_ini_t* out)
{
    FILE* fp = NULL;
    char raw[HPLOGC_CONF_LINE_MAX + 256];
    char* logical = NULL;
    size_t logical_cap = 0;
    size_t logical_len = 0;
    int line_no = 0;
    int sec = SEC_NONE;
    int prev_sec = SEC_NONE;
    int rc = HPLOGC_ERR_CONFIG;

    if (path == NULL || out == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    /** @brief 处理一条已拼接的逻辑行（不含续行符）。 */
#define HP_HANDLE_LINE()                                                     \
    do {                                                                     \
        char* p = hp_ini_trim(logical);                                      \
        if (p[0] != '\0') {                                                  \
            if (p[0] == '#') {                                               \
                /* 注释行，忽略 */                                          \
            } else if (p[0] == '[') {                                        \
                char* e = strrchr(p, ']');                                   \
                char secname[HPLOGC_MAX_NAME_LEN];                           \
                int ns;                                                      \
                if (e == NULL || e == p) {                                   \
                    hp_err_printf("%s:%d: 非法的节头", path, line_no);       \
                    goto done;                                               \
                }                                                            \
                *e = '\0';                                                   \
                snprintf(secname, sizeof(secname), "%s", p + 1);             \
                ns = hp_sec_of(secname);                                     \
                if (ns == -2) {                                              \
                    hp_err_printf("%s:%d: 未知节 [%s]", path, line_no,       \
                                  secname);                                  \
                    goto done;                                               \
                }                                                            \
                if (ns < prev_sec) {                                         \
                    hp_err_printf("%s:%d: 节顺序违反 §10.2（[%s] 早于已出现节）", \
                                  path, line_no, secname);                   \
                    goto done;                                               \
                }                                                            \
                sec = ns;                                                    \
                prev_sec = ns;                                               \
            } else {                                                         \
                char* eq = strchr(p, '=');                                   \
                char* k;                                                     \
                char* v;                                                     \
                if (eq == NULL) {                                            \
                    hp_err_printf("%s:%d: 期望 key = value", path, line_no); \
                    goto done;                                               \
                }                                                            \
                *eq = '\0';                                                  \
                k = hp_ini_trim(p);                                          \
                v = hp_ini_trim(eq + 1);                                     \
                hp_ini_strip_comment(v);                                     \
                hp_ini_unquote(v);                                           \
                if (sec == SEC_NONE) {                                       \
                    hp_err_printf("%s:%d: 节外的键 '%s'", path, line_no, k); \
                    goto done;                                               \
                }                                                            \
                if (hp_ini_push(out, sec, k, v, line_no) != 0) {             \
                    hp_err_printf("%s:%d: 内存不足", path, line_no);         \
                    goto done;                                               \
                }                                                            \
            }                                                                \
        }                                                                    \
    } while (0)

    fp = fopen(path, "rb");
    if (fp == NULL) {
        hp_err_printf("hplogc: 无法打开配置文件 '%s'", path);
        return HPLOGC_ERR_CONFIG;
    }
    if (hp_is_regular_file(path) != 1) {
        hp_err_printf("hplogc: 配置文件 '%s' 不是普通文件", path);
        fclose(fp);
        return HPLOGC_ERR_CONFIG;
    }

    while (fgets(raw, sizeof(raw), fp) != NULL) {
        size_t rlen = strlen(raw);
        line_no++;
        if (rlen == sizeof(raw) - 1 && raw[sizeof(raw) - 2] != '\n') {
            hp_err_printf("%s:%d: 物理行超过 %d 字节", path, line_no,
                          HPLOGC_CONF_LINE_MAX);
            goto done; /* 行过长 */
        }
        /* 去除行尾换行（保留 \r 由 trim 处理） */
        {
            size_t t = rlen;
            while (t > 0 && (raw[t - 1] == '\n' || raw[t - 1] == '\r')) {
                t--;
            }
            raw[t] = '\0';
            rlen = t;
        }
        /* 续行：行尾独立反斜杠 */
        if (rlen > 0 && raw[rlen - 1] == '\\') {
            raw[rlen - 1] = '\0';
            {
                size_t add = rlen; /* 不含反斜杠 */
                size_t need = logical_len + add + 1;
                if (need > logical_cap) {
                    char* nb;
                    size_t ncap = (logical_cap == 0) ? 1024 : (logical_cap * 2);
                    while (ncap < need) {
                        ncap *= 2;
                    }
                    nb = (char*)realloc(logical, ncap);
                    if (nb == NULL) {
                        hp_err_printf("%s:%d: 内存不足", path, line_no);
                        goto done;
                    }
                    logical = nb;
                    logical_cap = ncap;
                }
                memcpy(logical + logical_len, raw, add);
                logical_len += add;
                continue; /* 续行至下一物理行 */
            }
        }
        {
            size_t add = rlen + 1;
            size_t need = logical_len + add;
            if (need > logical_cap) {
                char* nb;
                size_t ncap = (logical_cap == 0) ? 1024 : (logical_cap * 2);
                while (ncap < need) {
                    ncap *= 2;
                }
                nb = (char*)realloc(logical, ncap);
                if (nb == NULL) {
                    hp_err_printf("%s:%d: 内存不足", path, line_no);
                    goto done;
                }
                logical = nb;
                logical_cap = ncap;
            }
            if (logical_len > 0) {
                memcpy(logical + logical_len, raw, add);
                logical_len += add;
            } else {
                memcpy(logical, raw, add);
                logical_len = add;
            }
            logical[logical_len - 1] = '\0';
            HP_HANDLE_LINE();
            logical_len = 0;
        }
    }
    rc = HPLOGC_OK;

done:
    if (logical != NULL) {
        free(logical);
    }
    if (fp != NULL) {
        fclose(fp);
    }
    if (rc != HPLOGC_OK) {
        hp_ini_free(out);
    }
#undef HP_HANDLE_LINE
    return rc;
}

void hp_ini_free(hp_ini_t* ini)
{
    if (ini == NULL) {
        return;
    }
    free(ini->items);
    ini->items = NULL;
    ini->count = 0;
    ini->cap = 0;
}
