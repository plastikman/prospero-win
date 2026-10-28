/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include "pw_diagnostics.h"
#include "ps5log/ps5log.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Two locks: `lock` guards the session file and its buffer and is only ever
 * held for memory copies and local writes; `net_lock` guards the TCP channel
 * (ps5log), whose sends and reconnects may wait for the network. No caller
 * holds both, so a slow network never stalls a caller that only writes the
 * file, such as the title's loop calling pw_diagnostics_tick(). */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t net_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t fd = -1;
static int disk_error;
static char current[512], previous[512], identity[256];
static char pending[65536];
static size_t used, bytes;
static uint64_t sequence, last_flush;
/* Set while the retry thread reconnects: records then skip the network (they
 * are still saved) instead of waiting for the connect timeout. */
static int reconnecting; /* __atomic: written by the retry thread */
static int retry_started;
static uint64_t retry_interval_ns = 5000000000ull;

/* Wine stderr and the title heartbeat can be frequent during startup. Keep
 * their producers independent of filesystem and TCP latency: one worker owns
 * the existing durable/network logger, while callers only copy into this
 * bounded queue. If the sink stops draining, dropping diagnostics is safer
 * than freezing the title. */
enum { ASYNC_LOG_CAPACITY = 512 };
static char async_log_queue[ASYNC_LOG_CAPACITY][PS5LOG_MAX_LINE];
static size_t async_log_head, async_log_count;
static pthread_mutex_t async_log_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t async_log_ready = PTHREAD_COND_INITIALIZER;
static int async_log_started;
static unsigned long async_log_dropped;

static void *async_log_worker(void *unused)
{
    (void)unused;
    for (;;) {
        char message[PS5LOG_MAX_LINE];
        pthread_mutex_lock(&async_log_lock);
        while (!async_log_count) pthread_cond_wait(&async_log_ready, &async_log_lock);
        memcpy(message, async_log_queue[async_log_head], sizeof(message));
        async_log_head = (async_log_head + 1u) % ASYNC_LOG_CAPACITY;
        async_log_count--;
        pthread_mutex_unlock(&async_log_lock);
        pw_diagnostics_log("%s", message);
    }
    return NULL;
}

static void start_async_log_worker(void)
{
    pthread_t thread;
    pthread_attr_t attributes;

    pthread_mutex_lock(&async_log_lock);
    int started = async_log_started;
    pthread_mutex_unlock(&async_log_lock);
    if (started || pthread_attr_init(&attributes) != 0) return;
    (void)pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread, &attributes, async_log_worker, NULL) == 0) {
        pthread_mutex_lock(&async_log_lock);
        async_log_started = 1;
        pthread_mutex_unlock(&async_log_lock);
    }
    pthread_attr_destroy(&attributes);
}

static int put_all(int target, const char *data, size_t length)
{
    while (length) {
        ssize_t n = write(target, data, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        data += n; length -= (size_t)n;
    }
    return 0;
}

static void flush(void)
{
    if (fd < 0 || !used) return;
    if (bytes + used > PW_DIAGNOSTICS_CHUNK - 128u) {
        close(fd); fd = -1;
        if (rename(current, previous) != 0) { disk_error = errno; used = 0; return; }
        fd = open(current, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        bytes = strlen(identity);
        if (fd < 0 || put_all(fd, identity, bytes) != 0) goto failed;
    }
    if (put_all(fd, pending, used) != 0) goto failed;
    bytes += used; used = 0;
    return;
failed:
    disk_error = errno ? errno : EIO;
    if (fd >= 0) close(fd);
    fd = -1; used = 0;
}

static void sleep_ns(uint64_t ns)
{
    struct timespec delay = { (time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull) };
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
}

/* Brings the TCP channel back after it drops. A reconnect waits up to the
 * channel's connect timeout, so it runs here and never in the title's loop. */
static void *retry_network(void *unused)
{
    (void)unused;
    for (;;) {
        sleep_ns(retry_interval_ns);
        pthread_mutex_lock(&net_lock);
        int up = ps5log_enabled();
        pthread_mutex_unlock(&net_lock);
        if (up) continue;
        __atomic_store_n(&reconnecting, 1, __ATOMIC_RELEASE);
        pthread_mutex_lock(&net_lock);
        (void)ps5log_reconnect();
        pthread_mutex_unlock(&net_lock);
        __atomic_store_n(&reconnecting, 0, __ATOMIC_RELEASE);
    }
    return NULL;
}

static void start_retry_thread(void)
{
    pthread_attr_t attributes;
    pthread_t thread;

    if (retry_started) return;
    if (pthread_attr_init(&attributes) != 0) return;
    (void)pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread, &attributes, retry_network, NULL) == 0) retry_started = 1;
    pthread_attr_destroy(&attributes);
}

int pw_diagnostics_open(const char *root, const char *build, const char *profile, uint32_t cycle)
{
    char directory[448], index[512], number[32];
    unsigned next = 0;
    int counter, result = -1;
    pthread_mutex_lock(&lock);
    if (fd >= 0) { flush(); close(fd); fd = -1; }
    disk_error = 0;
    if (!root || snprintf(directory, sizeof(directory), "%s/logs", root) >= (int)sizeof(directory)) {
        disk_error = ENAMETOOLONG; goto out;
    }
    /* ⚠ 0777, and chmod what already exists. These logs are read off the
     * console over FTP, and GoldHEN's server does not run as the title: a
     * directory the title made 0700 is one nobody can list, which is how the
     * session logs sat here unreadable. Same for the files below. */
    if (mkdir(root, 0777) != 0 && errno != EEXIST) { disk_error = errno; goto out; }
    if (mkdir(directory, 0777) != 0 && errno != EEXIST) { disk_error = errno; goto out; }
    (void)chmod(root, 0777);
    (void)chmod(directory, 0777);
    snprintf(index, sizeof(index), "%s/next.txt", directory);
    counter = open(index, O_RDONLY);
    if (counter >= 0) {
        ssize_t n = read(counter, number, sizeof(number) - 1); close(counter);
        if (n > 0) { number[n] = 0; (void)sscanf(number, "%u", &next); }
    }
    next %= PW_DIAGNOSTICS_SESSIONS;
    snprintf(current, sizeof(current), "%s/session-%u.log", directory, next);
    snprintf(previous, sizeof(previous), "%s/session-%u.previous.log", directory, next);
    (void)unlink(previous);
    fd = open(current, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) { disk_error = errno; goto out; }
    /* ⚠ The mode above is masked by the title's umask, which leaves these 0600
     * and unreadable over FTP however open-permissioned the directory is. */
    (void)fchmod(fd, 0666);
    /* Identity repeats on rotation so a retained tail can be identified. */
    snprintf(identity, sizeof(identity), "PW_REPORT/1 build=%s profile=%s cycle=%u pid=%ld time=%lld\n",
             build ? build : "unknown", profile ? profile : "launcher", cycle,
             (long)getpid(), (long long)time(NULL));
    bytes = strlen(identity);
    if (put_all(fd, identity, bytes) != 0) { disk_error = errno; close(fd); fd = -1; goto out; }
    counter = open(index, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (counter < 0) { disk_error = errno; close(fd); fd = -1; goto out; }
    int n = snprintf(number, sizeof(number), "%u\n", (next + 1) % PW_DIAGNOSTICS_SESSIONS);
    int saved = put_all(counter, number, (size_t)n);
    close(counter);
    if (saved != 0) { disk_error = errno; close(fd); fd = -1; goto out; }
    flush(); result = fd >= 0 ? 0 : -1;
out:
    start_retry_thread();
    start_async_log_worker();
    pthread_mutex_unlock(&lock);
    return result;
}

void pw_diagnostics_log_async(const char *format, ...)
{
    char message[PS5LOG_MAX_LINE];
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    if (length < 0) return;
    message[sizeof(message) - 1] = 0;

    pthread_mutex_lock(&async_log_lock);
    if (!async_log_started) {
        pthread_mutex_unlock(&async_log_lock);
        pw_diagnostics_log("%s", message);
        return;
    }
    if (async_log_count == ASYNC_LOG_CAPACITY) {
        async_log_dropped++;
        pthread_mutex_unlock(&async_log_lock);
        return;
    }
    size_t tail = (async_log_head + async_log_count) % ASYNC_LOG_CAPACITY;
    memcpy(async_log_queue[tail], message, sizeof(message));
    async_log_count++;
    pthread_cond_signal(&async_log_ready);
    pthread_mutex_unlock(&async_log_lock);
}

unsigned long pw_diagnostics_async_dropped(void)
{
    pthread_mutex_lock(&async_log_lock);
    unsigned long dropped = async_log_dropped;
    pthread_mutex_unlock(&async_log_lock);
    return dropped;
}

void pw_diagnostics_log(const char *format, ...)
{
    /* The same capacity as ps5log's own records; a longer message is cut and
     * says so, in the saved file and on the network alike. */
    static const char marker[] = " [truncated]";
    char message[PS5LOG_MAX_LINE], line[PS5LOG_MAX_LINE + 96];
    struct timespec clock;
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    if (length < 0) message[0] = 0;
    else if ((size_t)length >= sizeof(message))
        memcpy(message + sizeof(message) - sizeof(marker), marker, sizeof(marker));
    clock_gettime(CLOCK_MONOTONIC, &clock);
    pthread_mutex_lock(&lock);
    int n = snprintf(line, sizeof(line), "REC seq=%llu t=%lld.%09ld %s\n",
                     (unsigned long long)++sequence, (long long)clock.tv_sec, clock.tv_nsec, message);
    size_t size = n > 0 && n < (int)sizeof(line) ? (size_t)n : sizeof(line) - 1;
    if (used + size > sizeof(pending) || bytes + used + size > PW_DIAGNOSTICS_CHUNK - 128u) flush();
    if (used + size <= sizeof(pending)) { memcpy(pending + used, line, size); used += size; }
    /* ⚠ Write it now rather than on the next tick. Everything still in the
     * buffer is lost when the process does not come back through
     * pw_diagnostics_close, and the interesting runs are exactly those: Wine
     * calls NtTerminateProcess from inside the loader or the game, the title
     * never returns to its own loop, and the hundred milliseconds before the
     * next pw_diagnostics_tick never arrive.
     *
     * That is not a corner case here, it is every launch that ends badly. The
     * last session read as though the game went quiet after the activation
     * context, with the native alive ticks stopping at the same instant --
     * one buffer, two symptoms -- when in fact both were simply never
     * written. A syscall per record is nothing against another launch spent
     * learning nothing. */
    flush();
    pthread_mutex_unlock(&lock);
    if (!__atomic_load_n(&reconnecting, __ATOMIC_ACQUIRE)) {
        pthread_mutex_lock(&net_lock);
        (void)ps5log_line(PS5LOG_INFO, message);
        pthread_mutex_unlock(&net_lock);
    }
}

void pw_diagnostics_tick(uint64_t now)
{
    pthread_mutex_lock(&lock);
    if (now - last_flush >= 100000000ull) { flush(); last_flush = now; }
    pthread_mutex_unlock(&lock);
}

void pw_diagnostics_close(const char *reason)
{
    pw_diagnostics_log("PW_WINE64 session_end reason=%s", reason);
    pthread_mutex_lock(&lock);
    flush(); if (fd >= 0) { (void)fsync(fd); close(fd); fd = -1; }
    pthread_mutex_unlock(&lock);
    pthread_mutex_lock(&net_lock);
    ps5log_close(reason);
    pthread_mutex_unlock(&net_lock);
}

void pw_diagnostics_resume(void)
{
    pthread_mutex_lock(&lock);
    if (fd < 0 && current[0]) {
        fd = open(current, O_WRONLY | O_APPEND);
        if (fd < 0) disk_error = errno;
    }
    pthread_mutex_unlock(&lock);
    /* The title is still running after a failed restart: bring the channel
     * back now, as before closing it, rather than in the retry thread's time. */
    pthread_mutex_lock(&net_lock);
    (void)ps5log_reconnect();
    pthread_mutex_unlock(&net_lock);
}

void pw_diagnostics_status(char *buffer, size_t size)
{
    pthread_mutex_lock(&lock);
    snprintf(buffer, size, fd >= 0 ? "LOG SAVED - OPTIONS: REPORT HELP" : "LOG UNAVAILABLE (ERROR %d)", disk_error);
    pthread_mutex_unlock(&lock);
}

static size_t append_text(char *line, size_t n, const char *text)
{
    while (*text) line[n++] = *text++;
    return n;
}

static size_t append_hex(char *line, size_t n, uint64_t value)
{
    for (int shift = 60; shift >= 0; shift -= 4) line[n++] = "0123456789abcdef"[(value >> shift) & 15];
    return n;
}

void pw_diagnostics_emergency(int signal, uintptr_t address, uint64_t pc,
                              int segment, uint64_t segment_offset,
                              const char *module, uint64_t module_offset)
{
    char line[320];
    size_t n = 0;

    n = append_text(line, n, "PW_WINE64 fault signal=0x");
    n = append_hex(line, n, (unsigned)signal);
    n = append_text(line, n, " addr=0x");
    n = append_hex(line, n, address);
    n = append_text(line, n, " rip=0x");
    n = append_hex(line, n, pc);
    if (segment >= 0) {
        n = append_text(line, n, " ntdll_segment=0x");
        n = append_hex(line, n, (unsigned)segment);
        n = append_text(line, n, " offset=0x");
        n = append_hex(line, n, segment_offset);
    }
    /* ⚠ A bare rip names nothing once the fault is outside ntdll -- which is
     * where every interesting crash has been. pw_wine_dl tracks each module it
     * loaded, so report which one and the offset inside it. */
    if (module) {
        n = append_text(line, n, " module=");
        for (const char *p = module; *p && n < sizeof(line) - 24; p++) line[n++] = *p;
        n = append_text(line, n, " module_offset=0x");
        n = append_hex(line, n, module_offset);
    }
    line[n++] = '\n';
    /* write() and send() only: no lock, no allocation, no formatting library. */
    if (fd >= 0) { ssize_t result = write(fd, line, n); (void)result; }
    (void)ps5log_raw(line, n);
}

#ifdef PW_DIAGNOSTICS_TESTING
void pw_diagnostics_test_retry_interval(uint64_t ns) { retry_interval_ns = ns; }
#endif
