/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_update_ps4.h"
#include "../src/pw_update_check.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef PW_RELEASE_VERSION
#define PW_RELEASE_VERSION ""
#endif
#define PW_UPDATE_URL "https://api.github.com/repos/plastikman/orbis-win/releases/latest"
#define PW_UPDATE_SITE "github.com/plastikman/orbis-win"
#define PW_UPDATE_BODY_MAX (64u * 1024u)    /* tag_name is near the top; more is not needed */
#define PW_UPDATE_TIMEOUT_US (8u * 1000u * 1000u)

int sceSysmoduleLoadModuleInternal(uint32_t id);
int sceSysmoduleGetModuleHandleInternal(uint32_t id, int32_t *handle);
int sceKernelDlsym(int32_t handle, const char *symbol, void **address);
int sceNetPoolCreate(const char *name, int size, int flags);
int sceNetPoolDestroy(int id);

/* libSceHttp and libSceSsl are system modules a title loads at run time; their
 * functions are looked up by name in the loaded module (as pw_hid_ps5.c does
 * for the keyboard), so the launcher imports nothing from them and still
 * starts on a console where loading them fails. The server certificate is
 * always verified (libSceHttp's default); if this console's root store cannot
 * verify GitHub's chain, the check fails, says so in the log, and the launcher
 * shows nothing. */
enum { MODULE_HTTP = 0x8000000Au, MODULE_SSL = 0x8000000Bu };
enum { HTTP_VERSION_1_1 = 2, HTTP_METHOD_GET = 0, HTTP_HEADER_OVERWRITE = 0 };

static struct {
    int (*ssl_init)(size_t pool);
    int (*ssl_term)(int ctx);
    int (*init)(int net_pool, int ssl_ctx, size_t pool);
    int (*term)(int ctx);
    int (*create_template)(int ctx, const char *agent, int version, int auto_proxy);
    int (*delete_template)(int id);
    int (*resolve_timeout)(int id, uint32_t usec);
    int (*connect_timeout)(int id, uint32_t usec);
    int (*recv_timeout)(int id, uint32_t usec);
    int (*create_connection)(int template_id, const char *url, int keep_alive);
    int (*delete_connection)(int id);
    int (*create_request)(int connection, int method, const char *url, uint64_t content_length);
    int (*delete_request)(int id);
    int (*add_header)(int id, const char *name, const char *value, int mode);
    int (*send)(int id, const void *data, size_t size);
    int (*status)(int id, int *code);
    int (*read)(int id, void *data, uint32_t size);
} http;

static char notice[128];
static volatile int notice_ready;
static void (*log_line)(const char *line);

static void note(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void note(const char *format, ...)
{
    char line[192];
    __builtin_va_list args;

    if (!log_line) return;
    __builtin_va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    __builtin_va_end(args);
    log_line(line);
}

static int module(uint32_t id)
{
    int32_t handle = -1;
    int status = sceSysmoduleLoadModuleInternal(id);

    if (sceSysmoduleGetModuleHandleInternal(id, &handle) || handle < 0) {
        note("update check: module %#x load=%#x, no handle", (unsigned)id, (unsigned)status);
        return -1;
    }
    return handle;
}

static int resolve(int handle, const char *name, void *slot)
{
    void *address = NULL;

    if (sceKernelDlsym(handle, name, &address) || !address) {
        note("update check: %s not found", name);
        return -1;
    }
    memcpy(slot, &address, sizeof(address));
    return 0;
}

static int bind_http(void)
{
    int ssl = module(MODULE_SSL), lib = module(MODULE_HTTP);

    if (ssl < 0 || lib < 0) return -1;
    return resolve(ssl, "sceSslInit", &http.ssl_init) | resolve(ssl, "sceSslTerm", &http.ssl_term) |
           resolve(lib, "sceHttpInit", &http.init) | resolve(lib, "sceHttpTerm", &http.term) |
           resolve(lib, "sceHttpCreateTemplate", &http.create_template) |
           resolve(lib, "sceHttpDeleteTemplate", &http.delete_template) |
           resolve(lib, "sceHttpSetResolveTimeOut", &http.resolve_timeout) |
           resolve(lib, "sceHttpSetConnectTimeOut", &http.connect_timeout) |
           resolve(lib, "sceHttpSetRecvTimeOut", &http.recv_timeout) |
           resolve(lib, "sceHttpCreateConnectionWithURL", &http.create_connection) |
           resolve(lib, "sceHttpDeleteConnection", &http.delete_connection) |
           resolve(lib, "sceHttpCreateRequestWithURL", &http.create_request) |
           resolve(lib, "sceHttpDeleteRequest", &http.delete_request) |
           resolve(lib, "sceHttpAddRequestHeader", &http.add_header) |
           resolve(lib, "sceHttpSendRequest", &http.send) |
           resolve(lib, "sceHttpGetStatusCode", &http.status) |
           resolve(lib, "sceHttpReadData", &http.read) ? -1 : 0;
}

/* One GET of the latest release. The body length, or -1 (logged). */
static long get_release(int ctx, char *body, size_t capacity)
{
    int template_id, connection = -1, request = -1, code = 0, status;
    long length = -1;
    size_t used = 0;

    template_id = http.create_template(ctx, "orbis-win-update-check/" PW_RELEASE_VERSION, HTTP_VERSION_1_1, 1);
    if (template_id < 0) { note("update check: template %#x", (unsigned)template_id); return -1; }
    http.resolve_timeout(template_id, PW_UPDATE_TIMEOUT_US);
    http.connect_timeout(template_id, PW_UPDATE_TIMEOUT_US);
    http.recv_timeout(template_id, PW_UPDATE_TIMEOUT_US);
    if ((connection = http.create_connection(template_id, PW_UPDATE_URL, 0)) < 0) {
        note("update check: connection %#x", (unsigned)connection);
        goto done;
    }
    if ((request = http.create_request(connection, HTTP_METHOD_GET, PW_UPDATE_URL, 0)) < 0) {
        note("update check: request %#x", (unsigned)request);
        goto done;
    }
    http.add_header(request, "Accept", "application/vnd.github+json", HTTP_HEADER_OVERWRITE);
    if ((status = http.send(request, NULL, 0)) < 0) {
        /* No network, DNS, a timeout, or a certificate this console cannot
         * verify (libSceHttp's SSL errors are 0x80435xxx). */
        note("update check: send %#x", (unsigned)status);
        goto done;
    }
    if (http.status(request, &code) < 0 || code != 200) {
        note("update check: HTTP %d", code);
        goto done;
    }
    for (;;) {
        int n = http.read(request, body + used, (uint32_t)(capacity - 1 - used));
        if (n < 0) { note("update check: read %#x", (unsigned)n); break; }
        if (n == 0 || (used += (size_t)n) >= capacity - 1) break;
    }
    body[used] = 0;
    length = (long)used;
done:
    if (request >= 0) http.delete_request(request);
    if (connection >= 0) http.delete_connection(connection);
    http.delete_template(template_id);
    return length;
}

static void *fetch(void *unused)
{
    int net_pool = -1, ssl_ctx = -1, ctx = -1;
    char *body = MAP_FAILED;
    long length;

    (void)unused;
    if (bind_http()) return NULL;
    if ((net_pool = sceNetPoolCreate("pw-update-check", 16 * 1024, 0)) < 0) {
        note("update check: net pool %#x", (unsigned)net_pool);
        goto done;
    }
    if ((ssl_ctx = http.ssl_init(256 * 1024)) < 0) { note("update check: ssl %#x", (unsigned)ssl_ctx); goto done; }
    if ((ctx = http.init(net_pool, ssl_ctx, 64 * 1024)) < 0) { note("update check: http %#x", (unsigned)ctx); goto done; }
    body = mmap(NULL, PW_UPDATE_BODY_MAX, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (body == MAP_FAILED) goto done;
    if ((length = get_release(ctx, body, PW_UPDATE_BODY_MAX)) >= 0) {
        char line[sizeof(notice)];
        int result = pw_update_notice(PW_RELEASE_VERSION, body, (size_t)length, PW_UPDATE_SITE, line, sizeof(line));
        if (result == 1) {
            memcpy(notice, line, sizeof(notice));
            __atomic_store_n(&notice_ready, 1, __ATOMIC_RELEASE);
        }
        note("update check: running %s, %s", PW_RELEASE_VERSION,
             result == 1 ? line : result == 0 ? "up to date" : "answer not understood");
    }
done:
    if (body != MAP_FAILED) munmap(body, PW_UPDATE_BODY_MAX);
    if (ctx >= 0) http.term(ctx);
    if (ssl_ctx >= 0) http.ssl_term(ssl_ctx);
    if (net_pool >= 0) sceNetPoolDestroy(net_pool);
    return NULL;
}

void pw_update_ps4_start(const char *root, void (*log)(const char *line))
{
    static int started;
    unsigned version[3];
    char marker[160];
    pthread_t thread;
    pthread_attr_t attr;

    if (started++) return;
    log_line = log;
    if (pw_update_parse_version(PW_RELEASE_VERSION, version)) {
        note("update check: off, this build has no release version");
        return;
    }
    snprintf(marker, sizeof(marker), "%s/no-update-check", root ? root : "");
    if (root && !access(marker, F_OK)) {
        note("update check: off (%s)", marker);
        return;
    }
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread, &attr, fetch, NULL)) note("update check: no thread");
    pthread_attr_destroy(&attr);
}

const char *pw_update_ps4_notice(void)
{
    return __atomic_load_n(&notice_ready, __ATOMIC_ACQUIRE) ? notice : NULL;
}
