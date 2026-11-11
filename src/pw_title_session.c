/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "pw_title_session.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

/* The PS4 SDK's path stat omits uid and inode on /data, although fstat on an
 * opened handle reports them. Session ownership checks need the latter. */
static int session_lstat(const char *path, struct stat *out)
{
    if (lstat(path, out) != 0) return -1;
#if defined(__PS4__)
    if (out->st_ino == 0 && out->st_dev == 0 &&
        (S_ISDIR(out->st_mode) || S_ISREG(out->st_mode))) {
        const int directory = S_ISDIR(out->st_mode);
        const int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW |
                                  (directory ? O_DIRECTORY : 0));
        struct stat full;
        if (fd < 0) return -1;
        const int result = fstat(fd, &full);
        const int saved = errno;
        close(fd);
        if (result != 0 || (full.st_mode & S_IFMT) != (out->st_mode & S_IFMT)) {
            errno = result != 0 ? saved : EINVAL;
            return -1;
        }
        *out = full;
    }
#endif
    return 0;
}

static void set_error(char *error, size_t size, const char *format, ...)
{
    va_list args;

    if (!error || !size) return;
    va_start(args, format);
    (void)vsnprintf(error, size, format, args);
    va_end(args);
}

static int is_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

int pw_title_session_id_valid(const char *id)
{
    if (!id || strlen(id) != PW_TITLE_SESSION_ID_SIZE - 1) return 0;
    for (size_t i = 0; i < PW_TITLE_SESSION_ID_SIZE - 1; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (id[i] != '-') return 0;
        } else if (!is_hex(id[i])) {
            return 0;
        }
    }
    return 1;
}

static int identifier_valid(const char *value, size_t capacity)
{
    size_t length;

    if (!value || !(length = strnlen(value, capacity)) || length == capacity) return 0;
    for (size_t i = 0; i < length; i++) {
        char c = value[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
            return 0;
    }
    return 1;
}

static int generate_id(char id[PW_TITLE_SESSION_ID_SIZE])
{
    static const char hex[] = "0123456789abcdef";
    uint8_t random[16];
    size_t used = 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);

    if (fd < 0) return -1;
    while (used < sizeof(random)) {
        ssize_t got = read(fd, random + used, sizeof(random) - used);
        if (got <= 0) {
            close(fd);
            return -1;
        }
        used += (size_t)got;
    }
    close(fd);
    random[6] = (uint8_t)((random[6] & 0x0f) | 0x40); /* UUID version 4 */
    random[8] = (uint8_t)((random[8] & 0x3f) | 0x80); /* RFC 4122 variant */
    snprintf(id, PW_TITLE_SESSION_ID_SIZE,
             "%c%c%c%c%c%c%c%c-%c%c%c%c-%c%c%c%c-%c%c%c%c-%c%c%c%c%c%c%c%c%c%c%c%c",
             hex[random[0] >> 4], hex[random[0] & 15], hex[random[1] >> 4], hex[random[1] & 15],
             hex[random[2] >> 4], hex[random[2] & 15], hex[random[3] >> 4], hex[random[3] & 15],
             hex[random[4] >> 4], hex[random[4] & 15], hex[random[5] >> 4], hex[random[5] & 15],
             hex[random[6] >> 4], hex[random[6] & 15], hex[random[7] >> 4], hex[random[7] & 15],
             hex[random[8] >> 4], hex[random[8] & 15], hex[random[9] >> 4], hex[random[9] & 15],
             hex[random[10] >> 4], hex[random[10] & 15], hex[random[11] >> 4], hex[random[11] & 15],
             hex[random[12] >> 4], hex[random[12] & 15], hex[random[13] >> 4], hex[random[13] & 15],
             hex[random[14] >> 4], hex[random[14] & 15], hex[random[15] >> 4], hex[random[15] & 15]);
    return 0;
}

static int directory_safe(const char *path)
{
    struct stat st;

    return session_lstat(path, &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode);
}

static int path_components(const char *path, int final_directory)
{
    char component[PW_TITLE_SESSION_PATH_SIZE];
    size_t length, used = 0;

    if (!path || path[0] != '/' || (length = strlen(path)) >= sizeof(component)) return 0;
    for (size_t i = 1; i <= length;) {
        size_t start = i;
        struct stat st;

        while (i < length && path[i] != '/') i++;
        if (i == start) { i++; continue; }
        if (used + 1 + i - start >= sizeof(component)) return 0;
        component[used++] = '/';
        memcpy(component + used, path + start, i - start);
        used += i - start;
        component[used] = 0;
        if (session_lstat(component, &st) != 0 || S_ISLNK(st.st_mode)) return 0;
        while (i < length && path[i] == '/') i++;
        if (i < length) {
            if (!S_ISDIR(st.st_mode)) return 0;
        } else if (final_directory ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode)) {
            return 0;
        }
    }
    return used != 0;
}

int pw_title_path_is_directory(const char *path)
{
    return path_components(path, 1);
}

int pw_title_path_is_regular_file(const char *path)
{
    return path_components(path, 0);
}

static int sync_directory(const char *path)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int sync_status, close_status;

    if (fd < 0) return -1;
    sync_status = fsync(fd);
    close_status = close(fd);
    return sync_status == 0 && close_status == 0 ? 0 : -1;
}

static int remove_and_sync(const char *path, const char *directory)
{
    if (unlink(path) != 0 && errno != ENOENT) return -1;
    return sync_directory(directory);
}

static volatile int reservation_guard;

static void reservation_lock(void)
{
    while (__atomic_exchange_n(&reservation_guard, 1, __ATOMIC_ACQUIRE)) {}
}

static void reservation_unlock(void)
{
    __atomic_store_n(&reservation_guard, 0, __ATOMIC_RELEASE);
}

static int private_directory_safe(const char *path)
{
    struct stat st;
    return directory_safe(path) && session_lstat(path, &st) == 0 &&
           st.st_uid == geteuid() && (st.st_mode & 07777) == 0700;
}

static int private_file_fd(int fd)
{
    struct stat st;
    return fchmod(fd, 0600) == 0 && fstat(fd, &st) == 0 &&
           S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
           (st.st_mode & 07777) == 0600;
}

static int ensure_directory(const char *path, const char *parent,
                            char *error, size_t error_size)
{
    int created = 0;

    if (mkdir(path, 0700) == 0) created = 1;
    else if (errno != EEXIST) {
        set_error(error, error_size, "cannot create %s: %s", path, strerror(errno));
        return -1;
    }
    if (!private_directory_safe(path)) {
        set_error(error, error_size, "%s directory is not private or app-owned", path);
        return -1;
    }
    if (created && (!parent || sync_directory(parent) != 0)) {
        set_error(error, error_size, "cannot persist %s directory", path);
        return -1;
    }
    return 0;
}

static void phase_lock(PwTitleSession *session)
{
    while (__atomic_exchange_n(&session->phase_lock, 1, __ATOMIC_ACQUIRE)) {}
}

static void phase_unlock(PwTitleSession *session)
{
    __atomic_store_n(&session->phase_lock, 0, __ATOMIC_RELEASE);
}

static int write_phase_unlocked(PwTitleSession *session, const char *phase,
                                int has_exit_code, int exit_code)
{
    char temporary[PW_TITLE_SESSION_PATH_SIZE + 8];
    char json[1024];
    int fd, length, sync_status, close_status;
    size_t used = 0;

    if (!session || !session->active || !private_directory_safe(session->directory))
        return -1;
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", session->path) >=
        (int)sizeof(temporary))
        return -1;
    {
        struct stat stale;
        if (session_lstat(temporary, &stale) == 0) {
            if (!S_ISREG(stale.st_mode) || S_ISLNK(stale.st_mode) ||
                remove_and_sync(temporary, session->directory) != 0)
                return -1;
        } else if (errno != ENOENT) {
            return -1;
        }
    }
    if (has_exit_code)
        length = snprintf(json, sizeof(json),
                          "{\"schema_version\":1,\"session_id\":\"%s\","
                          "\"install_id\":\"%s\",\"title_id\":\"%s\","
                          "\"origin\":\"%s\",\"settings_revision\":%llu,"
                          "\"runtime_bundle_id\":\"%s\",\"phase\":\"%s\","
                          "\"exit_code\":%d}\n",
                          session->session_id, session->launch.install_id,
                          session->launch.title_id, session->origin,
                          (unsigned long long)session->launch.settings_revision,
                          session->launch.runtime_bundle_id, phase, exit_code);
    else
        length = snprintf(json, sizeof(json),
                          "{\"schema_version\":1,\"session_id\":\"%s\","
                          "\"install_id\":\"%s\",\"title_id\":\"%s\","
                          "\"origin\":\"%s\",\"settings_revision\":%llu,"
                          "\"runtime_bundle_id\":\"%s\",\"phase\":\"%s\"}\n",
                          session->session_id, session->launch.install_id,
                          session->launch.title_id, session->origin,
                          (unsigned long long)session->launch.settings_revision,
                          session->launch.runtime_bundle_id, phase);
    if (length < 0 || (size_t)length >= sizeof(json)) return -1;
    fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    if (!private_file_fd(fd)) {
        close(fd);
        unlink(temporary);
        return -1;
    }
    while (used < (size_t)length) {
        ssize_t wrote = write(fd, json + used, (size_t)length - used);
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote <= 0) {
            close(fd);
            unlink(temporary);
            return -1;
        }
        used += (size_t)wrote;
    }
    sync_status = fsync(fd);
    close_status = close(fd);
    if (sync_status != 0 || close_status != 0 || rename(temporary, session->path) != 0) {
        int saved = errno;
        unlink(temporary);
        errno = saved;
        return -1;
    }
    return sync_directory(session->directory);
}

int pw_title_session_start(PwTitleSession *session, const PwTitleLaunch *launch,
                           const char *session_argument, char *error, size_t error_size)
{
    char logs[PW_TITLE_SESSION_PATH_SIZE], sessions[PW_TITLE_SESSION_PATH_SIZE];
    char reservation[PW_TITLE_SESSION_PATH_SIZE + 8];
    char temporary[PW_TITLE_SESSION_PATH_SIZE + 8];
    struct stat root_stat;
    int reservation_fd = -1, sync_status, close_status;
    const char *requested = NULL;

    if (!session || !launch) {
        set_error(error, error_size, "session arguments are missing");
        return -1;
    }
    memset(session, 0, sizeof(*session));
    if (!identifier_valid(launch->install_id, sizeof(launch->install_id)) ||
        !identifier_valid(launch->title_id, sizeof(launch->title_id)) ||
        !identifier_valid(launch->runtime_bundle_id, sizeof(launch->runtime_bundle_id))) {
        set_error(error, error_size, "session identity is invalid");
        return -1;
    }
    if (session_argument) {
        static const char prefix[] = "session=";
        if (strncmp(session_argument, prefix, sizeof(prefix) - 1) != 0 ||
            !pw_title_session_id_valid(session_argument + sizeof(prefix) - 1)) {
            set_error(error, error_size, "session argument is invalid");
            return -1;
        }
        requested = session_argument + sizeof(prefix) - 1;
        strcpy(session->origin, "playden");
        strcpy(session->session_id, requested);
    } else {
        strcpy(session->origin, "home");
        if (generate_id(session->session_id) != 0) {
            set_error(error, error_size, "cannot generate session id");
            return -1;
        }
    }
    session->launch = *launch;
    if (session_lstat(launch->data_root, &root_stat) != 0 || !S_ISDIR(root_stat.st_mode) ||
        S_ISLNK(root_stat.st_mode) || root_stat.st_uid != geteuid() ||
        (root_stat.st_mode & 07777) != 0700) {
        set_error(error, error_size, "data root is unavailable or unsafe");
        goto fail;
    }
    if (snprintf(logs, sizeof(logs), "%s/logs", launch->data_root) >= (int)sizeof(logs) ||
        snprintf(sessions, sizeof(sessions), "%s/sessions", logs) >= (int)sizeof(sessions)) {
        set_error(error, error_size, "session directory path is too long");
        goto fail;
    }
    if (ensure_directory(logs, launch->data_root, error, error_size) != 0 ||
        ensure_directory(sessions, logs, error, error_size) != 0)
        goto fail;
    strcpy(session->directory, sessions);
    session->active = 1;
    if (snprintf(session->path, sizeof(session->path), "%s/%s.json", sessions,
                 session->session_id) >= (int)sizeof(session->path)) {
        set_error(error, error_size, "session path is too long");
        goto fail;
    }
    if (snprintf(reservation, sizeof(reservation), "%s.lock", session->path) >=
            (int)sizeof(reservation) ||
        snprintf(temporary, sizeof(temporary), "%s.tmp", session->path) >=
            (int)sizeof(temporary)) {
        set_error(error, error_size, "session reservation path is too long");
        goto fail;
    }
    /* Sony runs one native process for a title. Serialize starts inside that
     * process; a lock with no record therefore belongs to a dead predecessor
     * and is repairable. A record, even malformed or incomplete, is history
     * and is never overwritten by replaying its UUID. */
    reservation_lock();
    {
        struct stat existing;
        if (session_lstat(session->path, &existing) == 0) {
            (void)remove_and_sync(reservation, sessions);
            (void)remove_and_sync(temporary, sessions);
            reservation_unlock();
            set_error(error, error_size, "session id already exists");
            goto fail;
        }
        if (errno != ENOENT) {
            reservation_unlock();
            set_error(error, error_size, "cannot inspect session id");
            goto fail;
        }
        if (session_lstat(reservation, &existing) == 0) {
            if (!S_ISREG(existing.st_mode) || S_ISLNK(existing.st_mode) ||
                remove_and_sync(reservation, sessions) != 0) {
                reservation_unlock();
                set_error(error, error_size, "cannot repair session reservation");
                goto fail;
            }
        } else if (errno != ENOENT) {
            reservation_unlock();
            set_error(error, error_size, "cannot inspect session reservation");
            goto fail;
        }
    }
    reservation_fd = open(reservation, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
                          O_NOFOLLOW, 0600);
    if (reservation_fd < 0) {
        reservation_unlock();
        set_error(error, error_size, "cannot reserve session id");
        goto fail;
    }
    if (!private_file_fd(reservation_fd)) {
        close(reservation_fd);
        reservation_fd = -1;
        (void)remove_and_sync(reservation, sessions);
        reservation_unlock();
        set_error(error, error_size, "session reservation is not private or app-owned");
        goto fail;
    }
    sync_status = fsync(reservation_fd);
    close_status = close(reservation_fd);
    reservation_fd = -1;
    if (sync_status != 0 || close_status != 0 || sync_directory(sessions) != 0) {
        (void)remove_and_sync(reservation, sessions);
        reservation_unlock();
        set_error(error, error_size, "cannot persist session reservation");
        goto fail;
    }
    if (write_phase_unlocked(session, "starting", 0, 0) == 0) {
        if (remove_and_sync(reservation, sessions) == 0) {
            reservation_unlock();
            return 0;
        }
        set_error(error, error_size, "cannot persist session reservation cleanup");
    } else {
        set_error(error, error_size, "cannot write starting session: %s", strerror(errno));
    }
    (void)remove_and_sync(reservation, sessions);
    reservation_unlock();
    pw_title_session_close(session);
    return -1;

fail:
    if (reservation_fd >= 0) close(reservation_fd);
    session->active = 0;
    return -1;
}

int pw_title_session_resume(PwTitleSession *session, const PwTitleLaunch *launch,
                            const char *session_argument, char *error, size_t error_size)
{
    static const char prefix[] = "session=";
    char sessions[PW_TITLE_SESSION_PATH_SIZE], text[1024], expected[256];
    ssize_t used;
    int fd;

    if (!session || !launch || !session_argument ||
        strncmp(session_argument, prefix, sizeof(prefix) - 1) != 0 ||
        !pw_title_session_id_valid(session_argument + sizeof(prefix) - 1)) {
        set_error(error, error_size, "session argument is invalid");
        return -1;
    }
    memset(session, 0, sizeof(*session));
    strcpy(session->origin, "playden");
    strcpy(session->session_id, session_argument + sizeof(prefix) - 1);
    session->launch = *launch;
    if (snprintf(sessions, sizeof(sessions), "%s/logs/sessions", launch->data_root) >= (int)sizeof(sessions) ||
        snprintf(session->path, sizeof(session->path), "%s/%s.json", sessions,
                 session->session_id) >= (int)sizeof(session->path) ||
        !private_directory_safe(sessions)) {
        set_error(error, error_size, "session directory is unavailable or unsafe");
        return -1;
    }
    fd = open(session->path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0 || !private_file_fd(fd)) {
        if (fd >= 0) close(fd);
        set_error(error, error_size, "session record is missing or unsafe");
        return -1;
    }
    used = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (used <= 0) {
        set_error(error, error_size, "cannot read session record");
        return -1;
    }
    text[used] = 0;
    snprintf(expected, sizeof(expected), "\"session_id\":\"%s\",\"install_id\":\"%s\",\"title_id\":\"%s\",\"origin\":\"playden\",",
             session->session_id, launch->install_id, launch->title_id);
    if (!strstr(text, expected)) {
        set_error(error, error_size, "session record belongs to another launch");
        return -1;
    }
    if (strstr(text, "\"phase\":\"exited\"") || strstr(text, "\"phase\":\"failed\"")) {
        set_error(error, error_size, "session already ended");
        return -1;
    }
    strcpy(session->directory, sessions);
    session->active = 1;
    if (write_phase_unlocked(session, "starting", 0, 0) != 0) {
        set_error(error, error_size, "cannot write starting session: %s", strerror(errno));
        session->active = 0;
        return -1;
    }
    return 0;
}

int pw_title_session_running(PwTitleSession *session)
{
    int status;

    if (!session) return -1;
    phase_lock(session);
    status = write_phase_unlocked(session, "running", 0, 0);
    phase_unlock(session);
    return status;
}

static int terminal(PwTitleSession *session, const char *phase, int has_exit_code, int exit_code)
{
    int status;

    if (!session) return -1;
    phase_lock(session);
    status = write_phase_unlocked(session, phase, has_exit_code, exit_code);
    if (!status) session->active = 0;
    phase_unlock(session);
    return status;
}

int pw_title_session_exited(PwTitleSession *session, int has_exit_code, int exit_code)
{
    return terminal(session, "exited", has_exit_code, exit_code);
}

int pw_title_session_failed(PwTitleSession *session, int has_exit_code, int exit_code)
{
    return terminal(session, "failed", has_exit_code, exit_code);
}

void pw_title_session_close(PwTitleSession *session)
{
    if (!session) return;
    phase_lock(session);
    session->active = 0;
    phase_unlock(session);
}
