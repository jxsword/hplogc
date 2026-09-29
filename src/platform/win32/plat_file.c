/* -*- coding: utf-8 -*- */
/**
 * @file plat_file.c
 * @brief Windows 专属：文件 I/O、控制台、路径与目录操作。
 *
 * 路径一律按 **UTF-8 入口 / UTF-16 系统调用**处理：`hplogc` 对外（配置文件、
 * 公共 API）约定 UTF-8，而 Win32 的 `CreateFileW` 系列要求 UTF-16，
 * 因此每个入口都做一次转换（不使用 `CreateFileA`，避免受 ANSI 代码页影响）。
 *
 * 两类被规范明确豁免的能力（`file perms` / `symlink latest`，§4.7.2）
 * 返回 `HPLOGC_ERR_UNSUPPORTED`，由上层告警后忽略。
 */

#include "hplogc_platform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hplogc.h>

/** @brief 宽字符路径缓冲的容量（wchar_t 个数）。 */
#define HP_WPATH_CAP (HPLOGC_MAX_PATH_LEN * 2)

/** @brief 文件句柄。 */
struct hp_file {
    HANDLE h; /*!< Win32 文件句柄 */
};

/** @brief 分隔符判定：`/` 与 `\` 均视为路径分隔符。 */
static int hp_is_sep(char c)
{
    return (c == '/' || c == '\\');
}

/**
 * @brief UTF-8 → UTF-16 转换。
 * @return 0 成功；负值表示转换失败或缓冲不足。
 */
static int hp_utf8_to_wide(const char* u8, wchar_t* out, int cap)
{
    int n;

    if (u8 == NULL || out == NULL || cap <= 0) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    n = MultiByteToWideChar(CP_UTF8, 0, u8, -1, out, cap);
    return (n > 0) ? HPLOGC_OK : HPLOGC_ERR_INVALID_ARG;
}

/**
 * @brief UTF-16 → UTF-8 转换。
 * @return 0 成功；负值表示转换失败或缓冲不足。
 */
static int hp_wide_to_utf8(const wchar_t* w, char* out, int cap)
{
    int n;

    if (w == NULL || out == NULL || cap <= 0) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL);
    return (n > 0) ? HPLOGC_OK : HPLOGC_ERR_INVALID_ARG;
}

hp_file_t* hp_fopen(const char* path, const char* mode)
{
    wchar_t wpath[HP_WPATH_CAP];
    DWORD access = 0;
    DWORD creation = 0;
    DWORD share = FILE_SHARE_READ;
    HANDLE h;
    hp_file_t* f;
    char m;

    if (path == NULL || mode == NULL || mode[0] == '\0') {
        return NULL;
    }
    if (hp_utf8_to_wide(path, wpath, (int)HP_WPATH_CAP) != HPLOGC_OK) {
        return NULL;
    }
    m = mode[0];
    if (m == 'a') {
        /* 追加写：附带 FILE_READ_ATTRIBUTES 以便 hp_ftell_size 取大小 */
        access = FILE_APPEND_DATA | FILE_READ_ATTRIBUTES;
        creation = OPEN_ALWAYS;
    } else if (m == 'w') {
        access = GENERIC_WRITE | FILE_READ_ATTRIBUTES;
        creation = CREATE_ALWAYS;
    } else if (m == 'r') {
        access = GENERIC_READ;
        creation = OPEN_EXISTING;
    } else {
        return NULL;
    }
    h = CreateFileW(wpath, access, share, NULL, creation,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    f = (hp_file_t*)malloc(sizeof(*f));
    if (f == NULL) {
        CloseHandle(h);
        return NULL;
    }
    f->h = h;
    return f;
}

int hp_fwrite(hp_file_t* f, const void* buf, size_t len)
{
    size_t off = 0;

    if (f == NULL || (buf == NULL && len > 0)) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    while (off < len) {
        DWORD chunk = (len - off > 0x7FFFFFFFu) ? 0x7FFFFFFFu
                                               : (DWORD)(len - off);
        DWORD written = 0;
        if (!WriteFile(f->h, (const char*)buf + off, chunk, &written, NULL)
            || written == 0) {
            return HPLOGC_ERR_IO;
        }
        off += (size_t)written;
    }
    return HPLOGC_OK;
}

int hp_fflush(hp_file_t* f)
{
    /* 本实现不使用用户态缓冲：WriteFile 直达 OS 缓存，无需冲刷动作 */
    if (f == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    return HPLOGC_OK;
}

int hp_fsync_file(hp_file_t* f)
{
    if (f == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    return FlushFileBuffers(f->h) ? HPLOGC_OK : HPLOGC_ERR_IO;
}

int hp_ftell_size(hp_file_t* f, uint64_t* size)
{
    LARGE_INTEGER li;

    if (f == NULL || size == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    if (!GetFileSizeEx(f->h, &li)) {
        return HPLOGC_ERR_IO;
    }
    *size = (uint64_t)li.QuadPart;
    return HPLOGC_OK;
}

void hp_fclose(hp_file_t* f)
{
    if (f == NULL) {
        return;
    }
    if (f->h != INVALID_HANDLE_VALUE && f->h != NULL) {
        CloseHandle(f->h);
    }
    free(f);
}

/** @brief 取标准输出 / 标准错误的句柄。 */
static HANDLE hp_std_handle(int stream)
{
    return GetStdHandle((stream == 2) ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
}

int hp_console_write(int stream, const void* buf, size_t len)
{
    HANDLE h;
    size_t off = 0;

    if (buf == NULL && len > 0) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    h = hp_std_handle(stream);
    if (h == INVALID_HANDLE_VALUE || h == NULL) {
        return HPLOGC_ERR_IO;
    }
    while (off < len) {
        DWORD chunk = (len - off > 0x7FFFFFFFu) ? 0x7FFFFFFFu
                                               : (DWORD)(len - off);
        DWORD written = 0;
        if (!WriteFile(h, (const char*)buf + off, chunk, &written, NULL)) {
            return HPLOGC_ERR_IO;
        }
        off += (size_t)written;
    }
    return HPLOGC_OK;
}

int hp_console_isatty(int stream)
{
    HANDLE h;
    DWORD mode = 0;

    h = hp_std_handle(stream);
    if (h == INVALID_HANDLE_VALUE || h == NULL) {
        return 0;
    }
    return GetConsoleMode(h, &mode) ? 1 : 0;
}

int hp_console_enable_vt(int stream)
{
    HANDLE h;
    DWORD mode = 0;

    h = hp_std_handle(stream);
    if (h == INVALID_HANDLE_VALUE || h == NULL) {
        return HPLOGC_OK; /* 非控制台：无需动作 */
    }
    if (!GetConsoleMode(h, &mode)) {
        return HPLOGC_OK; /* 重定向到文件 / 管道：无 VT 可启用 */
    }
    /* 启用 ANSI 转义序列（Windows 10+），并让控制台按 UTF-8 解释输出 */
    (void)SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    (void)SetConsoleOutputCP(CP_UTF8);
    return HPLOGC_OK;
}

const char* hp_platform_eol(void)
{
    return "\r\n";
}

int hp_path_exists(const char* path)
{
    wchar_t wpath[HP_WPATH_CAP];

    if (path == NULL) {
        return 0;
    }
    if (hp_utf8_to_wide(path, wpath, (int)HP_WPATH_CAP) != HPLOGC_OK) {
        return 0;
    }
    return (GetFileAttributesW(wpath) != INVALID_FILE_ATTRIBUTES) ? 1 : 0;
}

int hp_is_regular_file(const char* path)
{
    wchar_t wpath[HP_WPATH_CAP];
    DWORD attrs;

    if (path == NULL) {
        return 0;
    }
    if (hp_utf8_to_wide(path, wpath, (int)HP_WPATH_CAP) != HPLOGC_OK) {
        return 0;
    }
    attrs = GetFileAttributesW(wpath);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return 0;
    }
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) ? 0 : 1;
}

int hp_file_stat(const char* path, uint64_t* size, uint64_t* mtime_ns)
{
    wchar_t wpath[HP_WPATH_CAP];
    WIN32_FILE_ATTRIBUTE_DATA data;
    ULARGE_INTEGER ul;

    if (path == NULL || size == NULL || mtime_ns == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    if (hp_utf8_to_wide(path, wpath, (int)HP_WPATH_CAP) != HPLOGC_OK) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    if (!GetFileAttributesExW(wpath, GetFileExInfoStandard, &data)) {
        return HPLOGC_ERR_IO;
    }
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        return HPLOGC_ERR_INVALID_ARG; /* 目录不是普通文件 */
    }
    ul.HighPart = data.nFileSizeHigh;
    ul.LowPart = data.nFileSizeLow;
    *size = (uint64_t)ul.QuadPart;

    ul.HighPart = data.ftLastWriteTime.dwHighDateTime;
    ul.LowPart = data.ftLastWriteTime.dwLowDateTime;
    /* FILETIME（1601 起算，100ns）→ Unix 纳秒（1970 起算） */
    *mtime_ns = (ul.QuadPart < 116444736000000000ULL)
                    ? 0
                    : (uint64_t)((ul.QuadPart - 116444736000000000ULL) * 100ULL);
    return HPLOGC_OK;
}

int hp_rename(const char* from, const char* to)
{
    wchar_t wfrom[HP_WPATH_CAP];
    wchar_t wto[HP_WPATH_CAP];

    if (from == NULL || to == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    if (hp_utf8_to_wide(from, wfrom, (int)HP_WPATH_CAP) != HPLOGC_OK
        || hp_utf8_to_wide(to, wto, (int)HP_WPATH_CAP) != HPLOGC_OK) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    /* 目标存在时覆盖（对齐 POSIX rename 的轮转覆盖语义） */
    return MoveFileExW(wfrom, wto, MOVEFILE_REPLACE_EXISTING)
               ? HPLOGC_OK
               : HPLOGC_ERR_IO;
}

int hp_unlink(const char* path)
{
    wchar_t wpath[HP_WPATH_CAP];

    if (path == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    if (hp_utf8_to_wide(path, wpath, (int)HP_WPATH_CAP) != HPLOGC_OK) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    return DeleteFileW(wpath) ? HPLOGC_OK : HPLOGC_ERR_IO;
}

int hp_mkdirs(const char* dir, unsigned mode)
{
    char buf[HPLOGC_MAX_PATH_LEN];
    size_t i;
    size_t len;

    (void)mode; /* Windows 忽略权限位（§4.7.2：dir perms 在 Windows 忽略） */
    if (dir == NULL || dir[0] == '\0') {
        return HPLOGC_ERR_INVALID_ARG;
    }
    len = strlen(dir);
    if (len >= sizeof(buf)) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    memcpy(buf, dir, len + 1);
    /* 逐级创建：先建各级父目录，最后建目标目录 */
    for (i = 0; i < len; i++) {
        wchar_t wpart[HP_WPATH_CAP];
        char saved;

        if (!hp_is_sep(buf[i])) {
            continue;
        }
        if (i == 0) {
            continue; /* 根目录 */
        }
        if (buf[i - 1] == ':') {
            continue; /* 盘符后的分隔符，如 "C:\" */
        }
        saved = buf[i];
        buf[i] = '\0';
        if (hp_utf8_to_wide(buf, wpart, (int)HP_WPATH_CAP) == HPLOGC_OK) {
            DWORD attrs = GetFileAttributesW(wpart);
            if (attrs == INVALID_FILE_ATTRIBUTES) {
                if (!CreateDirectoryW(wpart, NULL)) {
                    buf[i] = saved;
                    return HPLOGC_ERR_IO;
                }
            } else if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                buf[i] = saved; /* 同名普通文件存在 */
                return HPLOGC_ERR_IO;
            }
        }
        buf[i] = saved;
    }
    {
        wchar_t wfull[HP_WPATH_CAP];
        DWORD attrs;

        if (hp_utf8_to_wide(dir, wfull, (int)HP_WPATH_CAP) != HPLOGC_OK) {
            return HPLOGC_ERR_INVALID_ARG;
        }
        attrs = GetFileAttributesW(wfull);
        if (attrs != INVALID_FILE_ATTRIBUTES) {
            return (attrs & FILE_ATTRIBUTE_DIRECTORY) ? HPLOGC_OK
                                                      : HPLOGC_ERR_IO;
        }
        return CreateDirectoryW(wfull, NULL) ? HPLOGC_OK : HPLOGC_ERR_IO;
    }
}

int hp_chmod_file(const char* path, unsigned mode)
{
    (void)path;
    (void)mode;
    /* Windows 无 POSIX 权限位（§4.7.2：file perms 在 Windows 忽略并告警） */
    return HPLOGC_ERR_UNSUPPORTED;
}

int hp_symlink(const char* target, const char* linkpath)
{
    (void)target;
    (void)linkpath;
    /* 创建符号链接通常需要特权；按 §4.7.2 在 Windows 忽略并告警 */
    return HPLOGC_ERR_UNSUPPORTED;
}

int hp_dir_scan(const char* dir, const char* prefix,
                void (*cb)(const char* name, void* ctx), void* ctx)
{
    wchar_t wpattern[HP_WPATH_CAP];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    char name[HPLOGC_MAX_PATH_LEN];
    size_t dlen;
    char pattern[HPLOGC_MAX_PATH_LEN];

    if (dir == NULL || cb == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    dlen = strlen(dir);
    if (dlen + 3 > sizeof(pattern)) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    memcpy(pattern, dir, dlen);
    pattern[dlen] = '\\';
    pattern[dlen + 1] = '*';
    pattern[dlen + 2] = '\0';
    if (hp_utf8_to_wide(pattern, wpattern, (int)HP_WPATH_CAP) != HPLOGC_OK) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    h = FindFirstFileW(wpattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return HPLOGC_ERR_IO; /* 目录不可读 */
    }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            continue; /* 只枚举文件条目 */
        }
        if (hp_wide_to_utf8(fd.cFileName, name, (int)sizeof(name))
            != HPLOGC_OK) {
            continue;
        }
        if (prefix != NULL && prefix[0] != '\0'
            && strncmp(name, prefix, strlen(prefix)) != 0) {
            continue;
        }
        cb(name, ctx);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return HPLOGC_OK;
}

int hp_path_dirname(const char* path, char* out, size_t cap)
{
    size_t last = (size_t)-1;
    size_t i;

    if (path == NULL || out == NULL || cap == 0) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    for (i = 0; path[i] != '\0'; i++) {
        if (hp_is_sep(path[i])) {
            last = i;
        }
    }
    if (last == (size_t)-1) {
        if (cap < 2) {
            return HPLOGC_ERR_INVALID_ARG;
        }
        out[0] = '.';
        out[1] = '\0';
        return HPLOGC_OK;
    }
    if (last == 0) {
        if (cap < 2) {
            return HPLOGC_ERR_INVALID_ARG;
        }
        out[0] = path[0]; /* 根："/" 或 "\" */
        out[1] = '\0';
        return HPLOGC_OK;
    }
    if (last >= cap) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    memcpy(out, path, last);
    out[last] = '\0';
    return HPLOGC_OK;
}

const char* hp_path_basename(const char* path)
{
    const char* last;

    if (path == NULL) {
        return NULL;
    }
    last = path;
    for (; *path != '\0'; path++) {
        if (hp_is_sep(*path)) {
            last = path + 1;
        }
    }
    return last;
}

int hp_path_is_absolute(const char* path)
{
    if (path == NULL || path[0] == '\0') {
        return 0;
    }
    if (hp_is_sep(path[0])) {
        return 1; /* "/..." 或 "\..." */
    }
    /* 盘符形式："C:\..." 或 "C:/..." */
    if (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z'))
        && path[1] == ':') {
        return 1;
    }
    return 0;
}

int hp_path_normalize(const char* path, char* out, size_t cap)
{
    const char* p = path;
    size_t used = 0;
    int rooted = 0;
    char sep = '\\';

    if (path == NULL || out == NULL || cap == 0) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    if (hp_is_sep(path[0])) {
        rooted = 1;
        sep = path[0];
        out[used++] = sep;
        p++;
    } else if (path[0] != '\0' && path[1] == ':') {
        /* 盘符：保留 "X:"，随后的分隔符作为根 */
        out[used++] = path[0];
        out[used++] = path[1];
        rooted = 1;
        p += 2;
        if (hp_is_sep(*p)) {
            sep = *p;
            out[used++] = sep;
            p++;
        }
    }
    while (*p != '\0') {
        const char* start;
        size_t len;
        size_t k;

        while (hp_is_sep(*p)) {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        start = p;
        while (*p != '\0' && !hp_is_sep(*p)) {
            p++;
        }
        len = (size_t)(p - start);
        if (len == 1 && start[0] == '.') {
            continue; /* "." 分量：跳过 */
        }
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            /* ".."：弹出上一个分量 */
            k = used;
            while (k > 0 && out[k - 1] != sep) {
                k--;
            }
            if (k == 0) {
                used = rooted ? 1 : 0; /* 已在根：保持根（或空） */
            } else {
                used = (rooted && k == 1) ? 1 : k - 1;
            }
            out[used] = '\0';
            continue;
        }
        if (used > 0 && out[used - 1] != sep) {
            if (used + 1 >= cap) {
                return HPLOGC_ERR_INVALID_ARG;
            }
            out[used++] = sep;
        }
        if (used + len >= cap) {
            return HPLOGC_ERR_INVALID_ARG;
        }
        memcpy(out + used, start, len);
        used += len;
        out[used] = '\0';
    }
    if (used == 0) {
        if (cap < 2) {
            return HPLOGC_ERR_INVALID_ARG;
        }
        out[0] = '.';
        out[1] = '\0';
    }
    return HPLOGC_OK;
}
