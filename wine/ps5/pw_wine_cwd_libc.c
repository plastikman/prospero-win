/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The C library names behind pw_wine_cwd. Each module is linked with
 * --wrap=<name> for every __wrap_<name> below (tools/build_wine_ps5.sh and
 * the Makefile derive the list from this file), so the module's own calls
 * come here and __real_<name> is the kernel's. Relative paths are resolved
 * against the module's directory; absolute paths pass through unchanged.
 * Every successful open records its path, without a system call, so that
 * fchdir() and the *at() calls can resolve against the descriptor.
 *
 * Only calls a title has linked are used (FW 12.02): libkernel_sys, where
 * openat(), fstatat(), mkdirat(), unlinkat(), renameat(), symlink(),
 * readlink(), link() and statfs() live, is not given to a title, and a
 * system call from title code kills it. The *at calls resolve to an absolute
 * path and use the libkernel call by path; symbolic links are virtual
 * (pw_wine_cwd.h), and the calls that create a file (open() and openat()
 * with O_CREAT, fopen() to write or append) read the link table of its
 * directory first; link() is EPERM and statfs() ENOSYS, as fstatfs() is in
 * pw_wine_compat.
 *
 * The calls that can add, remove or rename a name count a change once the
 * kernel call returns, and pw_cwd_scan_directory() lists directories for
 * Wine's lookups of names that are not found; see pw_wine_cwd.h. */
#include "pw_wine_cwd.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/vfs.h>
#else
#include <sys/mount.h>
#endif

int __real_open(const char *path, int flags, ...);
int __real_close(int fd);
int __real_dup(int fd);
extern int sceKernelRdup(int fd);

/* ⚠ dup() RETURNS EPERM ON THIS KERNEL. Measured 2026-10-07: wineserver's
 * dup_fd_object (server/fd.c) failed with errno 1, which file_set_error turned
 * into STATUS_ACCESS_DENIED, which create_user_data_mapping returned as NULL,
 * which init_directories handed to release_object -- a null dereference four
 * steps from the cause. orbis-compat records the same refusal for dup2 onto fd
 * 1 or 2 ("the descriptor table is not ours to rearrange"); this is the plain
 * dup of an arbitrary descriptor.
 *
 * fcntl(F_DUPFD) is the POSIX equivalent and is not refused, so fall back to
 * it. Wine's server duplicates descriptors routinely, so this is not a
 * one-site fix. */
int __wrap_dup(int fd)
{
    int result, candidate;

    if ((result = __real_dup(fd)) != -1) return result;        /* EPERM here */
    if ((result = fcntl(fd, F_DUPFD, 0)) != -1) return result; /* EINVAL here */

    /* The POSIX calls are all refused on this kernel -- dup is EPERM,
     * fcntl(F_DUPFD) is EINVAL, and dup2 onto a free high number fails too, so
     * the descriptor table really is not ours to rearrange (orbis-compat says
     * the same of dup2 onto fd 1 or 2). libkernel has its own primitive for
     * it, which is what a title is evidently meant to use. */
    (void)candidate;
    result = sceKernelRdup(fd);
    /* ⚠ AN SCE CALL BEHIND A POSIX NAME. orbis-compat records the same of
     * sceKernelStat: failure is 0x8002_0000 | errno, not -1 with errno set.
     * Returned as-is it is a large POSITIVE integer, so the caller takes it for
     * a descriptor and the first fstat of it fails EBADF -- four frames from
     * here, in mapping_init. The low 16 bits are the FreeBSD errno. */
    if ((unsigned)result >= 0x80000000u)
    {
        errno = ((unsigned)result >> 16) == 0x8002 ? (result & 0xFFFF) : EBADF;
        return -1;
    }
    return result;
}

int __real_stat(const char *path, struct stat *st);
int __real_lstat(const char *path, struct stat *st);

/* ⚠ stat() DOES NOT REPORT st_dev OR st_ino ON THIS CONSOLE. Measured
 * 2026-10-07 (src/stat-probe): orbis-compat's stat() goes through
 * sceKernelStat, which leaves st_dev, st_ino, st_nlink and st_uid zero, while
 * its fstat() goes through _fstat and fills them. Same translate(), different
 * kernel call. st_mode and st_size agree.
 *
 * Wine identifies files by (st_dev, st_ino), so zeros are not a cosmetic gap:
 * wineserver creates its server directory, chdir()s in, open(".")s it and
 * compares a stat() against an fstat() -- which can never match, and it died
 * with "chdir did not end up in <dir>" every time.
 *
 * Fill them the only way that works here: open the path and fstat it. Only
 * when they came back zero, so a file system that does report them pays
 * nothing. A failure to open leaves the stat as-is rather than failing the
 * call -- the mode and size are still good. */
static void fill_identity(const char *path, struct stat *st)
{
    struct stat from_fd;
    int fd;

    if (st->st_dev || st->st_ino) return;
    if ((fd = __real_open(path, O_RDONLY)) < 0) return;
    if (!fstat(fd, &from_fd)) {
        st->st_dev = from_fd.st_dev;
        st->st_ino = from_fd.st_ino;
        if (!st->st_nlink) st->st_nlink = from_fd.st_nlink;
        if (!st->st_uid) st->st_uid = from_fd.st_uid;
    }
    __real_close(fd);
}

/* ⚠ A path this layer hands to libc must end within PW_CWD_PATH_MAX. Sony's
 * libc reads it with no bound, so an unterminated buffer walks off the end of
 * the mapping and faults inside libSceLibcInternal, where it reads as a crash
 * in a syscall rather than as a bad argument here:
 *
 *     syscall fault 0xc0000005 at libSceLibcInternal.sprx+0x51eca,
 *                   address 2202daf860
 *     syscall fault in syscall 0x147a (win32u #1146)
 *     syscall fault called from win32u.prx+0x242393  (NtUserCreateWindowStation)
 *     syscall fault called from ntdll.prx+0xb58b2    (stat_resolved)
 *
 * Every buffer here is a char[PW_CWD_PATH_MAX] inside struct resolved, so
 * reading that many bytes is always in bounds; what is not guaranteed is that
 * something wrote a terminator. Say so and refuse, rather than letting libc
 * find out. If this never fires, the stack walk above pointed at a stale slot
 * and the fault is somewhere else -- which is worth knowing too. */
static int pw_path_is_terminated(const char *path, const char *who)
{
    extern int sceKernelDebugOutText(int channel, const char *text);
    static int reported;
    char line[160];

    if (path && strnlen(path, PW_CWD_PATH_MAX) < PW_CWD_PATH_MAX) return 1;
    if (reported < 8) {
        reported++;
        snprintf(line, sizeof(line),
                 "pw_cwd: %s given a path with no terminator in %d bytes (%p)\n",
                 who, (int)PW_CWD_PATH_MAX, (const void *)path);
        sceKernelDebugOutText(0, line);
    }
    errno = EINVAL;
    return 0;
}

/* Every raw stat in this file goes through these, so any identity comparison
 * anywhere sees real st_dev/st_ino rather than zeros. */
static int real_stat_full(const char *path, struct stat *st)
{
    int result;

    if (!pw_path_is_terminated(path, "stat")) return -1;
    result = __real_stat(path, st);
    if (!result) fill_identity(path, st);
    return result;
}

static int real_lstat_full(const char *path, struct stat *st)
{
    int result;

    if (!pw_path_is_terminated(path, "lstat")) return -1;
    result = __real_lstat(path, st);
    if (!result) fill_identity(path, st);
    return result;
}

int __real_access(const char *path, int mode);
int __real_mkdir(const char *path, mode_t mode);
int __real_rmdir(const char *path);
int __real_unlink(const char *path);
int __real_rename(const char *from, const char *to);
int __real_chmod(const char *path, mode_t mode);
int __real_truncate(const char *path, off_t length);
int __real_utimes(const char *path, const struct timeval times[2]);
char *__real_realpath(const char *path, char *resolved);
FILE *__real_fopen(const char *path, const char *mode);
DIR *__real_opendir(const char *path);

/* A path as the caller named it (absolute, links unresolved) and as the
 * kernel gets it. */
struct resolved {
    char logical[PW_CWD_PATH_MAX], full[PW_CWD_PATH_MAX];
    int follow_last;
};

static int resolve_at(struct resolved *r, int dirfd, const char *path, int follow_last)
{
    r->follow_last = follow_last;
    if (pw_cwd_resolve_at(dirfd, AT_FDCWD, path, r->logical, sizeof(r->logical))) return -1;
    return pw_cwd_follow(r->logical, follow_last, r->full, sizeof(r->full));
}

static int resolve(struct resolved *r, const char *path, int follow_last)
{
    return resolve_at(r, AT_FDCWD, path, follow_last);
}

/* The directory part of an absolute path, "/" for a top-level name. */
static void parent_of(const char *path, char *out)
{
    const char *slash = strrchr(path, '/');
    size_t length = slash == path ? 1 : (size_t)(slash - path);

    memcpy(out, path, length);
    out[length] = 0;
}

static void table_of(const char *directory, char *out, size_t size)
{
    snprintf(out, size, "%s/%s", strcmp(directory, "/") ? directory : "", PW_CWD_LINK_TABLE);
}

/* Read the link table of a directory (folded, with no link in it) the first
 * time it is asked about; how many links were new. A directory without one
 * is remembered too, so asking again costs no system call. */
static int load_table(const char *directory)
{
    char table[PW_CWD_PATH_MAX + sizeof(PW_CWD_LINK_TABLE)], text[4096];
    ssize_t got;
    int fd;

    if (!pw_cwd_probe_directory(directory)) return 0;
    table_of(directory, table, sizeof(table));
    if ((fd = __real_open(table, O_RDONLY)) < 0) return 0;
    got = read(fd, text, sizeof(text));
    __real_close(fd);
    return got > 0 ? pw_cwd_links_parse(directory, text, (size_t)got) : 0;
}

/* Read the tables of every directory on the way to path. Each directory is
 * looked at where it really is, after the links before it (and the tables
 * read so far) are followed: a table below dosdevices/c: is in drive_c, which
 * the kernel knows, and its links are kept under that name, the one
 * pw_cwd_follow() meets. How many links were new. */
static int load_tables(const char *path)
{
    char directory[PW_CWD_PATH_MAX], resolved[PW_CWD_PATH_MAX], folded[PW_CWD_PATH_MAX];
    const char *slash = path;
    int added = 0;

    while ((slash = strchr(slash + 1, '/'))) {
        size_t length = (size_t)(slash - path);

        memcpy(directory, path, length);
        directory[length] = 0;
        if (!pw_cwd_follow(directory, 1, resolved, sizeof(resolved)) &&
            !pw_cwd_fold(resolved, folded, sizeof(folded)))
            added += load_table(folded);
    }
    return added;
}

/* Before a call that may create r's last name: a missing name is no failed
 * lookup, so read the table of the directory it would be made in (once per
 * directory) and follow a link it names. 0, or -1 with errno. */
static int load_parent_table(struct resolved *r)
{
    char folded[PW_CWD_PATH_MAX], directory[PW_CWD_PATH_MAX];

    if (pw_cwd_fold(r->full, folded, sizeof(folded))) return -1;
    parent_of(folded, directory);
    if (!load_table(directory)) return 0;
    return pw_cwd_follow(r->logical, r->follow_last, r->full, sizeof(r->full));
}

/* After a lookup failed for want of a directory, read the tables on the way
 * and resolve again. 1 when the call is worth repeating; 0 keeps errno. */
static int reload(struct resolved *r)
{
    int saved = errno;

    if ((saved != ENOENT && saved != ENOTDIR) || !load_tables(r->logical) ||
        pw_cwd_follow(r->logical, r->follow_last, r->full, sizeof(r->full))) {
        errno = saved;
        return 0;
    }
    return 1;
}

/* Rewrite the table of the directory holding a link, or remove it once the
 * directory has none left. */
static int save_table(const char *link)
{
    char directory[PW_CWD_PATH_MAX], table[PW_CWD_PATH_MAX + sizeof(PW_CWD_LINK_TABLE)];
    char next[sizeof(table) + 4], text[4096];
    ssize_t size;
    int fd, status;

    parent_of(link, directory);
    if ((size = pw_cwd_links_format(directory, text, sizeof(text))) < 0) return -1;
    table_of(directory, table, sizeof(table));
    if (!size) {
        status = __real_unlink(table) && errno != ENOENT ? -1 : 0;
        pw_cwd_changed();
        return status;
    }
    snprintf(next, sizeof(next), "%s.new", table);
    fd = __real_open(next, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    pw_cwd_changed();
    if (fd < 0) return -1;
    status = write(fd, text, (size_t)size) == size ? 0 : -1;
    if (__real_close(fd)) status = -1;
    if (!status) status = __real_rename(next, table);
    pw_cwd_changed();
    return status;
}

/* lstat() of a virtual link: its directory's device, owner and times. */
static int link_stat(const char *full, ssize_t target_length, struct stat *st)
{
    char directory[PW_CWD_PATH_MAX];
    struct stat parent;
    size_t hash = 5381;

    parent_of(full, directory);
    if (real_stat_full(directory, &parent)) return -1;
    while (*full) hash = hash * 33 + (unsigned char)*full++;
    *st = parent;
    st->st_mode = S_IFLNK | 0777;
    st->st_nlink = 1;
    st->st_size = target_length;
    st->st_ino = (ino_t)(hash | 1);
    return 0;
}

static int is_link(const char *full, ssize_t *length)
{
    char target[PW_CWD_PATH_MAX];
    int saved = errno;

    *length = pw_cwd_link_target(full, target, sizeof(target));
    errno = saved;
    return *length >= 0;
}

static int lstat_resolved(struct resolved *r, struct stat *st)
{
    ssize_t length;
    int result;

    do {
        if (is_link(r->full, &length)) return link_stat(r->full, length, st);
    } while ((result = real_lstat_full(r->full, st)) && reload(r));
    return result;
}

static int stat_resolved(struct resolved *r, struct stat *st)
{
    int result;

    while ((result = real_stat_full(r->full, st)) && reload(r)) {}
    if (!result) fill_identity(r->full, st);
    return result;
}

int __wrap_chdir(const char *path)
{
    char folded[PW_CWD_PATH_MAX];
    struct resolved r;
    struct stat st;

    if (resolve(&r, path, 1) || pw_cwd_fold(r.logical, folded, sizeof(folded)) ||
        stat_resolved(&r, &st))
        return -1;
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
    return pw_cwd_set(folded);
}

int __wrap_fchdir(int fd)
{
    char path[PW_CWD_PATH_MAX];
    struct stat by_fd, by_path;

    if (fstat(fd, &by_fd)) return -1;
    if (!S_ISDIR(by_fd.st_mode)) { errno = ENOTDIR; return -1; }
    /* Another module may have closed and reused the number: the path must
     * still name the descriptor's directory. */
    if (pw_cwd_fd_path(fd, path, sizeof(path)) || real_stat_full(path, &by_path) ||
        by_path.st_dev != by_fd.st_dev || by_path.st_ino != by_fd.st_ino) {
        errno = EACCES;
        return -1;
    }
    return pw_cwd_set(path);
}

char *__wrap_getcwd(char *buffer, size_t size)
{
    char path[PW_CWD_PATH_MAX];
    size_t length = pw_cwd_get(path, sizeof(path));

    if (!buffer) {
        if (!size) size = length + 1;
        if (!(buffer = malloc(size))) return NULL;
        if (size <= length) { free(buffer); errno = ERANGE; return NULL; }
    }
    else if (!size) { errno = EINVAL; return NULL; }
    if (size <= length) { errno = ERANGE; return NULL; }
    memcpy(buffer, path, length + 1);
    return buffer;
}

/* The PS5 counts every open file against a per-process cap of about 268,
 * however many descriptors share it: copies made with dup() add nothing,
 * and the file stays counted until its last descriptor closes (measured on
 * the console, notes/gtaiv/fd-exhaustion.md). /dev/null is opened over and
 * over (GTA IV's Social Club stand-in logs there and opens it for every
 * line), so every open of it is a copy of one descriptor held for the life
 * of the module: one file, whatever the number of handles. Writes are
 * discarded and reads end at once whatever the flags, so sharing the file's
 * mode changes nothing; only O_CLOEXEC is per descriptor. O_EXCL and
 * O_DIRECTORY still go to the kernel, which refuses them. */
static int shared_null = -1;
static pthread_mutex_t shared_null_lock = PTHREAD_MUTEX_INITIALIZER;

static int open_null_copy(int flags)
{
    int fd;

    pthread_mutex_lock(&shared_null_lock);
    if (shared_null < 0) shared_null = __real_open("/dev/null", O_RDWR | O_CLOEXEC);
    fd = shared_null < 0 ? -1 : dup(shared_null);
    pthread_mutex_unlock(&shared_null_lock);
    if (fd >= 0 && (flags & O_CLOEXEC)) fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

/* An open that may create its file counts a change only when it did, so a
 * file opened again and again to append a line drops no listing. Without
 * O_EXCL the file is opened as it is first, then created exclusively, and
 * one that appears in between is opened again. Anything else (O_EXCL asked
 * for, another refusal, a name that keeps changing) makes the call as asked
 * and counts it when it succeeds. */
static int open_counting(const char *full, int flags, mode_t mode)
{
    int fd = -1, tries;

    if (!(flags & O_CREAT)) return __real_open(full, flags);
    for (tries = 0; !(flags & O_EXCL) && tries < 4; tries++) {
        if ((fd = __real_open(full, flags & ~O_CREAT)) >= 0) return fd;
        if (errno != ENOENT) break;
        if ((fd = __real_open(full, flags | O_EXCL, mode)) >= 0) break;
        if (errno != EEXIST) break;
    }
    if (fd < 0 && (fd = __real_open(full, flags, mode)) < 0) return fd;
    pw_cwd_changed();
    return fd;
}

static int open_resolved(struct resolved *r, int flags, mode_t mode)
{
    int fd;

    if (!strcmp(r->full, "/dev/null") && !(flags & (O_EXCL | O_DIRECTORY)) && (fd = open_null_copy(flags)) >= 0) {
        pw_cwd_track(fd, r->full);
        return fd;
    }
    if ((flags & O_CREAT) && load_parent_table(r)) return -1;
    while ((fd = open_counting(r->full, flags, mode)) < 0 && reload(r)) {}
    if (fd >= 0) pw_cwd_track(fd, r->full);
    return fd;
}

int __wrap_open(const char *path, int flags, ...)
{
    struct resolved r;
    mode_t mode = 0;

    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = (mode_t)va_arg(args, int);
        va_end(args);
    }
    return resolve(&r, path, 1) ? -1 : open_resolved(&r, flags, mode);
}

int __wrap_openat(int dirfd, const char *path, int flags, ...)
{
    struct resolved r;
    mode_t mode = 0;

    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = (mode_t)va_arg(args, int);
        va_end(args);
    }
    return resolve_at(&r, dirfd, path, 1) ? -1 : open_resolved(&r, flags, mode);
}

/* The C library opens by itself: wineserver loads the registry with
 * fopen("system.reg") after entering the prefix. */
FILE *__wrap_fopen(const char *path, const char *mode)
{
    struct resolved r;
    FILE *file;

    if (resolve(&r, path, 1)) return NULL;
    if (mode && strpbrk(mode, "wa") && load_parent_table(&r)) return NULL;
    while (!(file = __real_fopen(r.full, mode)) && reload(&r)) {}
    if (mode && strpbrk(mode, "wa")) pw_cwd_changed();
    return file;
}

int __wrap_close(int fd)
{
    pw_cwd_forget(fd);
    return __real_close(fd);
}

int __wrap_stat(const char *path, struct stat *st)
{
    struct resolved r;
    return resolve(&r, path, 1) ? -1 : stat_resolved(&r, st);
}

int __wrap_lstat(const char *path, struct stat *st)
{
    struct resolved r;
    return resolve(&r, path, 0) ? -1 : lstat_resolved(&r, st);
}

int __wrap_fstatat(int dirfd, const char *path, struct stat *st, int flags)
{
    struct resolved r;
    int follow = !(flags & AT_SYMLINK_NOFOLLOW);

    if (resolve_at(&r, dirfd, path, follow)) return -1;
    return follow ? stat_resolved(&r, st) : lstat_resolved(&r, st);
}

int __wrap_access(const char *path, int mode)
{
    struct resolved r;
    struct stat st;

    if (resolve(&r, path, 1)) return -1;
    while (__real_access(r.full, mode)) {
        if (reload(&r)) continue;
        if (errno != EPERM || real_stat_full(r.full, &st)) return -1;
        if (pw_cwd_mode_allows(st.st_mode, mode)) return 0;
        errno = EACCES;
        return -1;
    }
    return 0;
}

static int mkdir_resolved(struct resolved *r, mode_t mode)
{
    int result;

    while ((result = __real_mkdir(r->full, mode)) && reload(r)) {}
    pw_cwd_changed();
    return result;
}

int __wrap_mkdir(const char *path, mode_t mode)
{
    struct resolved r;
    return resolve(&r, path, 0) ? -1 : mkdir_resolved(&r, mode);
}

int __wrap_mkdirat(int dirfd, const char *path, mode_t mode)
{
    struct resolved r;
    return resolve_at(&r, dirfd, path, 0) ? -1 : mkdir_resolved(&r, mode);
}

static int rmdir_resolved(struct resolved *r)
{
    int result;

    while ((result = __real_rmdir(r->full)) && reload(r)) {}
    pw_cwd_changed();
    return result;
}

/* unlink() of a virtual link forgets it and rewrites its table. */
static int unlink_resolved(struct resolved *r)
{
    char folded[PW_CWD_PATH_MAX];
    ssize_t length;
    int result;

    do {
        if (is_link(r->full, &length)) {
            if (pw_cwd_fold(r->full, folded, sizeof(folded)) || pw_cwd_link_remove(folded)) return -1;
            return save_table(folded);
        }
    } while ((result = __real_unlink(r->full)) && reload(r));
    pw_cwd_changed();
    return result;
}

int __wrap_rmdir(const char *path)
{
    struct resolved r;
    return resolve(&r, path, 0) ? -1 : rmdir_resolved(&r);
}

int __wrap_unlink(const char *path)
{
    struct resolved r;
    return resolve(&r, path, 0) ? -1 : unlink_resolved(&r);
}

int __wrap_unlinkat(int dirfd, const char *path, int flags)
{
    struct resolved r;

    if (resolve_at(&r, dirfd, path, 0)) return -1;
    return flags & AT_REMOVEDIR ? rmdir_resolved(&r) : unlink_resolved(&r);
}

static int rename_resolved(struct resolved *from, struct resolved *to)
{
    int result;

    while ((result = __real_rename(from->full, to->full)) && (reload(from) | reload(to))) {}
    pw_cwd_changed();
    return result;
}

int __wrap_rename(const char *from, const char *to)
{
    struct resolved r_from, r_to;

    if (resolve(&r_from, from, 0) || resolve(&r_to, to, 0)) return -1;
    return rename_resolved(&r_from, &r_to);
}

int __wrap_renameat(int from_dirfd, const char *from, int to_dirfd, const char *to)
{
    struct resolved r_from, r_to;

    if (resolve_at(&r_from, from_dirfd, from, 0) || resolve_at(&r_to, to_dirfd, to, 0)) return -1;
    return rename_resolved(&r_from, &r_to);
}

/* The target is stored as given: a relative target stays relative to the
 * link, which is what Wine's "../drive_c" means. The link's directory must
 * exist and nothing may be at its name. */
static int symlink_resolved(const char *target, struct resolved *r)
{
    char folded[PW_CWD_PATH_MAX], directory[PW_CWD_PATH_MAX];
    struct stat st;

    load_tables(r->logical);
    if (pw_cwd_follow(r->logical, 0, r->full, sizeof(r->full)) ||
        pw_cwd_fold(r->full, folded, sizeof(folded)))
        return -1;
    if (!lstat_resolved(r, &st)) { errno = EEXIST; return -1; }
    if (errno != ENOENT) return -1;
    parent_of(folded, directory);
    if (real_stat_full(directory, &st)) return -1;
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
    if (pw_cwd_link_add(folded, target)) return -1;
    if (!save_table(folded)) return 0;
    pw_cwd_link_remove(folded);
    return -1;
}

int __wrap_symlink(const char *target, const char *path)
{
    struct resolved r;
    return resolve(&r, path, 0) ? -1 : symlink_resolved(target, &r);
}

int __wrap_symlinkat(const char *target, int dirfd, const char *path)
{
    struct resolved r;
    return resolve_at(&r, dirfd, path, 0) ? -1 : symlink_resolved(target, &r);
}

/* Only virtual links can be read: anything else that exists is no link. */
ssize_t __wrap_readlink(const char *path, char *buffer, size_t size)
{
    struct resolved r;
    struct stat st;
    ssize_t length;
    int result;

    if (resolve(&r, path, 0)) return -1;
    do {
        if ((length = pw_cwd_link_target(r.full, buffer, size)) >= 0) return length;
    } while ((result = real_lstat_full(r.full, &st)) && reload(&r));
    if (!result) errno = EINVAL;
    return -1;
}

int __wrap_link(const char *from, const char *to)
{
    (void)from;
    (void)to;
    errno = EPERM;
    return -1;
}

int __wrap_chmod(const char *path, mode_t mode)
{
    struct resolved r;
    int result;

    if (resolve(&r, path, 1)) return -1;
    while ((result = __real_chmod(r.full, mode)) && reload(&r)) {}
    return result;
}

int __wrap_truncate(const char *path, off_t length)
{
    struct resolved r;
    int result;

    if (resolve(&r, path, 1)) return -1;
    while ((result = __real_truncate(r.full, length)) && reload(&r)) {}
    return result;
}

int __wrap_utimes(const char *path, const struct timeval times[2])
{
    struct resolved r;
    int result;

    if (resolve(&r, path, 1)) return -1;
    while ((result = __real_utimes(r.full, times)) && reload(&r)) {}
    return result;
}

int __wrap_statfs(const char *path, struct statfs *buffer)
{
    (void)path;
    (void)buffer;
    errno = ENOSYS;
    return -1;
}

char *__wrap_realpath(const char *path, char *resolved_path)
{
    struct resolved r;
    char *result;

    if (resolve(&r, path, 1)) return NULL;
    while (!(result = __real_realpath(r.full, resolved_path)) && reload(&r)) {}
    return result;
}

DIR *__wrap_opendir(const char *path)
{
    struct resolved r;
    DIR *result;

    if (resolve(&r, path, 1)) return NULL;
    while (!(result = __real_opendir(r.full)) && reload(&r)) {}
    return result;
}

/* The names of the directory r names, read whole: NUL-terminated in a
 * buffer the caller frees. 0, or -1 with errno. */
/* Append one name to the growing buffer; 0, or -1 with errno. */
static int append_name(const char *name, size_t length, char **names, size_t *bytes,
                       size_t *count, size_t *size)
{
    if (*bytes + length + 1 > *size) {
        char *bigger;

        while (*bytes + length + 1 > *size) *size *= 2;
        if (!(bigger = realloc(*names, *size))) { errno = ENOMEM; return -1; }
        *names = bigger;
    }
    memcpy(*names + *bytes, name, length);
    (*names)[*bytes + length] = 0;
    *bytes += length + 1;
    ++*count;
    return 0;
}

/* ⚠ readdir does not list /data on this console. libc reads a directory with
 * a buffer of its own choosing -- a page, 16 KiB here -- and /data's
 * directories use 64 KiB blocks, so the call is refused for a buffer smaller
 * than one block: EINVAL, or EPERM, depending on where it is tried. The
 * launcher met this first and answered it with getdents over a whole block
 * (native/pw_wine_library.c), and that is the method used here.
 *
 * Unlike the launcher's parser this one keeps every entry, directories
 * included: these names are what resolves the components of a path, and
 * dropping them loses \windows and \syswow64 along with the files.
 *
 * Nothing had noticed because the only caller is the case-insensitive search,
 * which a prefix written in lower case reaches only when a name is spelled
 * some other way -- and then every lookup fails at once. A 32-bit game asks
 * for its imports exactly as its import table spells them, so GTA IV lost all
 * twenty-one of them and the loader ended the process with c0000135. */
/* ⚠ One buffer, not one per call. A whole directory block is 64 KiB here, and
 * this console's libc refuses an allocation that size -- the same allocator
 * that turns calloc of 145 KiB into EINVAL (docs/FINDINGS.md) -- so asking for
 * one on every lookup fails as soon as the heap is busy, which arrives as
 * STATUS_NO_MEMORY out of a DLL load and looks nothing like a directory
 * problem. The launcher has always kept this buffer static; so does this. */
static pthread_mutex_t dirents_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t dirents_buffer[64 * 1024];

static int read_names_getdents(int fd, char **names, size_t *bytes, size_t *count, size_t *size)
{
    static const size_t block = sizeof(dirents_buffer);
    uint8_t *buffer = dirents_buffer;
    int got, saved;

    pthread_mutex_lock(&dirents_lock);
    while ((got = (int)getdents(fd, (struct dirent *)buffer, block)) > 0) {
        size_t offset = 0;

        while (offset < (size_t)got) {
            uint16_t reclen;
            uint8_t namlen;

            if ((size_t)got - offset < 8) { saved = EINVAL; goto failed; }
            memcpy(&reclen, buffer + offset + 4, 2);
            namlen = buffer[offset + 7];
            if (reclen < 8 || reclen > (size_t)got - offset || (size_t)namlen + 8 > reclen) {
                saved = EINVAL; goto failed;
            }
            if (namlen && append_name((const char *)buffer + offset + 8, namlen,
                                      names, bytes, count, size)) {
                saved = errno; goto failed;
            }
            offset += reclen;
        }
    }
    saved = errno;
    pthread_mutex_unlock(&dirents_lock);
    if (got < 0) { errno = saved; return -1; }
    return 0;

failed:
    pthread_mutex_unlock(&dirents_lock);
    errno = saved;
    return -1;
}

static int read_names(const struct resolved *r, char **names, size_t *bytes, size_t *count)
{
    struct dirent *entry;
    size_t size = 4096;
    DIR *dir;
    int fd, saved;

    *bytes = *count = 0;
    if (!(*names = malloc(size))) { errno = ENOMEM; return -1; }
    if ((fd = __real_open(r->full, O_RDONLY)) < 0) { saved = errno; goto failed; }

    /* The console's method first; see read_names_getdents. */
    if (!read_names_getdents(fd, names, bytes, count, &size) && *count) {
        __real_close(fd);
        return 0;
    }
    *bytes = *count = 0;

    /* Then libc's, for a file system where it does work. fdopendir owns the
     * descriptor once it succeeds, so it must not be closed here as well. */
    if (!(dir = fdopendir(fd))) {
        saved = errno;
        __real_close(fd);
        goto failed;
    }
    errno = 0;   /* readdir ends by returning NULL; only errno tells which end */
    while ((entry = readdir(dir))) {
        if (append_name(entry->d_name, strlen(entry->d_name), names, bytes, count, &size)) {
            saved = errno;
            closedir(dir);
            goto failed;
        }
        errno = 0;
    }
    if ((saved = errno)) { closedir(dir); goto failed; }
    closedir(dir);
    return 0;

failed:
    free(*names);
    *names = NULL;
    *bytes = *count = 0;
    errno = saved;
    return -1;
}

int pw_cwd_scan_directory(int dirfd, const char *path, pw_cwd_visit visit, void *context,
                          unsigned int *flags)
{
    /* Read first: a change counted after this may be missing from the names. */
    unsigned long changes_before = pw_cwd_changes();
    char folded[PW_CWD_PATH_MAX], *names, *name;
    struct resolved r;
    struct stat st;
    size_t bytes, count, i;
    int result = 0, reparse_names = 0;

    *flags = 0;
    if (resolve_at(&r, dirfd, path, 1) || stat_resolved(&r, &st)) return -1;
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
    if (pw_cwd_fold(r.full, folded, sizeof(folded))) return -1;
    load_table(folded);
    if (!pw_cwd_listing_visit(&st, changes_before, visit, context, &result, &reparse_names)) {
        if (read_names(&r, &names, &bytes, &count)) return -1;
        for (i = 0, name = names; i < count; i++, name += strlen(name) + 1) {
            reparse_names |= name[0] && name[strlen(name) - 1] == '?';
            if (!result) result = visit(name, context);
        }
        pw_cwd_listing_store(&st, changes_before, names, bytes, count);
        free(names);
    }
    if (!result && !reparse_names && !pw_cwd_links_end_with(folded, '?'))
        *flags |= PW_CWD_NO_REPARSE_NAMES;
    return result;
}
