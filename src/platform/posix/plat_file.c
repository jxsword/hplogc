/* -*- coding: utf-8 -*- */
/**
 * @file plat_file.c
 * @brief POSIX 共享层：文件 I/O、标准流、目录与路径处理。
 */

#include "hplogc_platform.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <hplogc.h>

/** @brief 文件句柄的 POSIX 实现。 */
struct hp_file {
    FILE* fp; /*!< 标准 I/O 流 */
};

hp_file_t* hp_fopen(const char* path, const char* mode)
{
    hp_file_t* f;
    FILE* fp;

    if (path == NULL || mode == NULL) {
        return NULL;
    }
    fp = fopen(path, mode);
    if (fp == NULL) {
        return NULL;
    }
    f = (hp_file_t*)malloc(sizeof(*f));
    if (f == NULL) {
        fclose(fp);
        return NULL;
    }
    f->fp = fp;
    return f;
}

int hp_fwrite(hp_file_t* f, const void* buf, size_t len)
{
    if (f == NULL || f->fp == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    if (len == 0) {
        return HPLOGC_OK;
    }
    if (fwrite(buf, 1, len, f->fp) != len) {
        return HPLOGC_ERR_IO;
    }
    return HPLOGC_OK;
}

int hp_fflush(hp_file_t* f)
{
    if (f == NULL || f->fp == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    return (fflush(f->fp) == 0) ? HPLOGC_OK : HPLOGC_ERR_IO;
}

int hp_fsync_file(hp_file_t* f)
{
    int fd;
    if (f == NULL || f->fp == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    fflush(f->fp);
    fd = fileno(f->fp);
    if (fd < 0) {
        return HPLOGC_ERR_IO;
    }
    return (fsync(fd) == 0) ? HPLOGC_OK : HPLOGC_ERR_IO;
}

int hp_ftell_size(hp_file_t* f, uint64_t* size)
{
    long pos;
    if (f == NULL || f->fp == NULL || size == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    fflush(f->fp);
    pos = ftell(f->fp);
    if (pos < 0) {
        return HPLOGC_ERR_IO;
    }
    *size = (uint64_t)pos;
    return HPLOGC_OK;
}

void hp_fclose(hp_file_t* f)
{
    if (f == NULL) {
        return;
    }
    if (f->fp != NULL) {
        fclose(f->fp);
    }
    free(f);
}

int hp_console_write(int stream, const void* buf, size_t len)
{
    int fd = (stream == 2) ? STDERR_FILENO : STDOUT_FILENO;
    const char* p = (const char*)buf;
    size_t done = 0;

    while (done < len) {
        ssize_t n = write(fd, p + done, len - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return HPLOGC_ERR_IO;
        }
        if (n == 0) {
            return HPLOGC_ERR_IO;
        }
        done += (size_t)n;
    }
    return HPLOGC_OK;
}

int hp_console_isatty(int stream)
{
    int fd = (stream == 2) ? STDERR_FILENO : STDOUT_FILENO;
    return isatty(fd) ? 1 : 0;
}

int hp_console_enable_vt(int stream)
{
    /* POSIX 终端原生支持 ANSI 序列，无需额外处理 */
    (void)stream;
    return HPLOGC_OK;
}

const char* hp_platform_eol(void)
{
    return "\n";
}

int hp_path_exists(const char* path)
{
    struct stat st;
    if (path == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    return (stat(path, &st) == 0) ? 1 : 0;
}

int hp_is_regular_file(const char* path)
{
    struct stat st;
    if (path == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    if (stat(path, &st) != 0) {
        return 0;
    }
    return S_ISREG(st.st_mode) ? 1 : 0;
}

int hp_file_stat(const char* path, uint64_t* size, uint64_t* mtime_ns)
{
    struct stat st;
    if (path == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    if (stat(path, &st) != 0) {
        return HPLOGC_ERR_IO;
    }
    if (size != NULL) {
        *size = (uint64_t)st.st_size;
    }
    if (mtime_ns != NULL) {
#if defined(__APPLE__)
        /* macOS 的 struct stat 使用 st_mtimespec（无 POSIX.1-2008 的 st_mtim） */
        *mtime_ns = (uint64_t)st.st_mtimespec.tv_sec * 1000000000ull
                    + (uint64_t)st.st_mtimespec.tv_nsec;
#else
        *mtime_ns = (uint64_t)st.st_mtim.tv_sec * 1000000000ull
                    + (uint64_t)st.st_mtim.tv_nsec;
#endif
    }
    return HPLOGC_OK;
}

int hp_rename(const char* from, const char* to)
{
    if (from == NULL || to == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    return (rename(from, to) == 0) ? HPLOGC_OK : HPLOGC_ERR_IO;
}

int hp_unlink(const char* path)
{
    if (path == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    return (unlink(path) == 0) ? HPLOGC_OK : HPLOGC_ERR_IO;
}

int hp_mkdirs(const char* dir, unsigned mode)
{
    char buf[HPLOGC_MAX_PATH_LEN];
    size_t i;
    size_t len;

    if (dir == NULL || dir[0] == '\0') {
        return HPLOGC_ERR_INVALID_ARG;
    }
    len = strlen(dir);
    if (len >= sizeof(buf)) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    memcpy(buf, dir, len + 1);

    /* 逐段创建：遇到已存在的段直接跳过（不区分"目录已存在"与"是文件"） */
    for (i = 1; i <= len; i++) {
        if (buf[i] != '/' && buf[i] != '\0') {
            continue;
        }
        {
            char save = buf[i];
            buf[i] = '\0';
            if (mkdir(buf, (mode_t)mode) != 0 && errno != EEXIST) {
                return HPLOGC_ERR_IO;
            }
            buf[i] = save;
        }
    }
    if (buf[len - 1] != '/') {
        if (mkdir(buf, (mode_t)mode) != 0 && errno != EEXIST) {
            return HPLOGC_ERR_IO;
        }
    }
    return HPLOGC_OK;
}

int hp_chmod_file(const char* path, unsigned mode)
{
    if (path == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    return (chmod(path, (mode_t)mode) == 0) ? HPLOGC_OK : HPLOGC_ERR_IO;
}

int hp_symlink(const char* target, const char* linkpath)
{
    if (target == NULL || linkpath == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    unlink(linkpath); /* 目标已存在时先移除，保证 .latest 语义可用 */
    return (symlink(target, linkpath) == 0) ? HPLOGC_OK : HPLOGC_ERR_IO;
}

int hp_dir_scan(const char* dir, const char* prefix,
                void (*cb)(const char* name, void* ctx), void* ctx)
{
    DIR* d;
    struct dirent* ent;

    if (dir == NULL || cb == NULL) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    d = opendir(dir);
    if (d == NULL) {
        return HPLOGC_ERR_IO;
    }
    while ((ent = readdir(d)) != NULL) {
        if (prefix != NULL && prefix[0] != '\0'
            && strncmp(ent->d_name, prefix, strlen(prefix)) != 0) {
            continue;
        }
        cb(ent->d_name, ctx);
    }
    closedir(d);
    return HPLOGC_OK;
}

int hp_path_dirname(const char* path, char* out, size_t cap)
{
    const char* slash;
    size_t n;

    if (path == NULL || out == NULL || cap == 0) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    slash = strrchr(path, '/');
    if (slash == NULL) {
        if (cap < 2) {
            return HPLOGC_ERR_INVALID_ARG;
        }
        out[0] = '.';
        out[1] = '\0';
        return HPLOGC_OK;
    }
    if (slash == path) {
        n = 1; /* 根目录的目录部分是 "/" */
    } else {
        n = (size_t)(slash - path);
    }
    if (n + 1 > cap) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    memcpy(out, path, n);
    out[n] = '\0';
    return HPLOGC_OK;
}

const char* hp_path_basename(const char* path)
{
    const char* slash;
    if (path == NULL) {
        return NULL;
    }
    slash = strrchr(path, '/');
    return (slash == NULL) ? path : slash + 1;
}

int hp_path_is_absolute(const char* path)
{
    if (path == NULL || path[0] == '\0') {
        return 0;
    }
    return (path[0] == '/') ? 1 : 0;
}

int hp_path_normalize(const char* path, char* out, size_t cap)
{
    size_t len;
    size_t o = 0;
    size_t i = 0;

    if (path == NULL || out == NULL || cap == 0) {
        return HPLOGC_ERR_INVALID_ARG;
    }
    len = strlen(path);

    /* 逐段处理：'.' 段丢弃，'..' 弹出上一段，其余段追加 */
    while (i <= len) {
        size_t start = i;
        size_t seglen;
        while (i < len && path[i] != '/') {
            i++;
        }
        seglen = i - start;
        if (seglen > 0) {
            if (seglen == 1 && path[start] == '.') {
                /* 跳过 */
            } else if (seglen == 2 && path[start] == '.' && path[start + 1] == '.') {
                /* 弹出上一段 */
                while (o > 0 && out[o - 1] != '/') {
                    o--;
                }
                if (o > 0) {
                    o--; /* 去掉分隔的 '/' */
                }
            } else {
                if (seglen + 1 > cap - o) {
                    out[o < cap ? o : cap - 1] = '\0';
                    return HPLOGC_ERR_INVALID_ARG;
                }
                memcpy(out + o, path + start, seglen);
                o += seglen;
            }
        }
        if (i < len) {
            /* path[i] == '/'：写入分隔符（末位 '/' 也保留，便于目录比较） */
            if (o + 2 > cap) {
                out[o < cap ? o : cap - 1] = '\0';
                return HPLOGC_ERR_INVALID_ARG;
            }
            out[o++] = '/';
            i++;
        } else {
            break;
        }
    }
    if (o == 0) {
        if (cap < 2) {
            return HPLOGC_ERR_INVALID_ARG;
        }
        out[0] = '.';
        out[1] = '\0';
        return HPLOGC_OK;
    }
    /* 去掉末尾多余的分隔符（保留根 "/"） */
    if (o > 1 && out[o - 1] == '/') {
        o--;
    }
    out[o] = '\0';
    return HPLOGC_OK;
}
