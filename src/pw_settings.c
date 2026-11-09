/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include "pw_settings.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *const wine_log_names[PW_SETTINGS_WINE_LOG_COUNT] = { "profile", "quiet", "detailed" };
/* quiet: errors only, which keeps this port's own PW_ records (err
 * channels) and drops the rest; detailed: what a crash report needs. */
static const char *const wine_log_values[PW_SETTINGS_WINE_LOG_COUNT] = {
    NULL, "-all,err+all", "+tid,err+all,+loaddll,+process,+seh" };

void pw_settings_defaults(PwSettings *settings)
{
    memset(settings, 0, sizeof(*settings));
    settings->log = -1;
    settings->log_port = PW_SETTINGS_DEFAULT_PORT;
    settings->wine_log = PW_SETTINGS_WINE_LOG_PROFILE;
    settings->update_check = 1;
}

int pw_settings_valid_ipv4(const char *text)
{
    if (!text) return 0;
    for (int part = 0; part < 4; part++) {
        unsigned value = 0, digits = 0;
        while (*text >= '0' && *text <= '9') {
            value = value * 10u + (unsigned)(*text++ - '0');
            if (++digits > 3 || value > 255) return 0;
        }
        if (!digits) return 0;
        if (part < 3 && *text++ != '.') return 0;
    }
    return *text == '\0';
}

void pw_settings_ipv4_step(char address[PW_SETTINGS_SERVER], unsigned part, int delta)
{
    unsigned parts[4] = { 0, 0, 0, 0 };

    if (pw_settings_valid_ipv4(address))
        sscanf(address, "%u.%u.%u.%u", &parts[0], &parts[1], &parts[2], &parts[3]);
    if (part < 4) parts[part] = (unsigned)(((int)parts[part] + delta % 256 + 256) % 256);
    snprintf(address, PW_SETTINGS_SERVER, "%u.%u.%u.%u", parts[0], parts[1], parts[2], parts[3]);
}

int pw_settings_set_address(PwSettings *settings, const char *typed)
{
    char text[40], *colon, *end;
    unsigned long port = settings->log_port;
    size_t length;

    if (!typed) return 0;
    while (*typed == ' ' || *typed == '\t') typed++;
    length = strlen(typed);
    while (length && (typed[length - 1] == ' ' || typed[length - 1] == '\t' ||
                      typed[length - 1] == '\r' || typed[length - 1] == '\n')) length--;
    if (!length || length >= sizeof(text)) return 0;
    memcpy(text, typed, length);
    text[length] = '\0';
    if ((colon = strchr(text, ':'))) {
        *colon = '\0';
        port = strtoul(colon + 1, &end, 10);
        if (!colon[1] || *end || port == 0 || port > 65535) return 0;
    }
    length = strlen(text);
    if (!pw_settings_valid_ipv4(text) || length >= sizeof(settings->log_server)) return 0;
    memcpy(settings->log_server, text, length + 1);
    settings->log_port = (uint16_t)port;
    return 1;
}

/* 1/0 and the usual words for them; -1 for anything else. */
static int parse_switch(const char *value)
{
    static const char *const on[] = { "1", "on", "yes", "true" }, *const off[] = { "0", "off", "no", "false" };
    for (size_t i = 0; i < 4; i++) {
        if (!strcmp(value, on[i])) return 1;
        if (!strcmp(value, off[i])) return 0;
    }
    return -1;
}

static int apply(PwSettings *settings, const char *key, const char *value)
{
    int on;

    if (!strcmp(key, "log_server")) {
        if (!pw_settings_valid_ipv4(value)) return 0;
        snprintf(settings->log_server, sizeof(settings->log_server), "%s", value);
        return 1;
    }
    if (!strcmp(key, "log_port")) {
        char *end;
        unsigned long port = strtoul(value, &end, 10);
        if (!*value || *end || port == 0 || port > 65535) return 0;
        settings->log_port = (uint16_t)port;
        return 1;
    }
    if (!strcmp(key, "wine_log")) {
        for (int i = 0; i < PW_SETTINGS_WINE_LOG_COUNT; i++)
            if (!strcmp(value, wine_log_names[i])) { settings->wine_log = (PwSettingsWineLog)i; return 1; }
        return 0;
    }
    if ((on = parse_switch(value)) < 0) return 0;
    if (!strcmp(key, "log")) settings->log = on;
    else if (!strcmp(key, "wait_watchdog")) settings->wait_watchdog = on;
    else if (!strcmp(key, "gpu_hang_report")) settings->gpu_hang_report = on;
    else if (!strcmp(key, "update_check")) settings->update_check = on;
    else return 0;
    return 1;
}

int pw_settings_parse(const char *text, size_t length, PwSettings *settings)
{
    int applied = 0;
    size_t at = 0;

    while (at < length) {
        char line[160];
        size_t end = at, used = 0;
        while (end < length && text[end] != '\n') end++;
        for (size_t i = at; i < end && used + 1 < sizeof(line); i++)
            if (text[i] != '\r') line[used++] = text[i];
        line[used] = '\0';
        at = end + 1;
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char *equals = strchr(line, '=');
        if (!equals) continue;
        *equals = '\0';
        /* Trim spaces around the key and the value. */
        char *key = line, *value = equals + 1;
        while (*key == ' ' || *key == '\t') key++;
        for (char *e = equals; e > key && (e[-1] == ' ' || e[-1] == '\t');) *--e = '\0';
        while (*value == ' ' || *value == '\t') value++;
        for (char *e = value + strlen(value); e > value && (e[-1] == ' ' || e[-1] == '\t');) *--e = '\0';
        applied += apply(settings, key, value);
    }
    return applied;
}

int pw_settings_format(const PwSettings *settings, char *out, size_t cap)
{
    char log_line[16] = "";
    if (settings->log >= 0) snprintf(log_line, sizeof(log_line), "log=%d\n", settings->log ? 1 : 0);
    int n = snprintf(out, cap,
                     "# orbis-win settings, written by the launcher's Settings card.\n"
                     "%s"
                     "log_server=%s\n"
                     "log_port=%u\n"
                     "wine_log=%s\n"
                     "wait_watchdog=%d\n"
                     "gpu_hang_report=%d\n"
                     "update_check=%d\n",
                     log_line, settings->log_server, (unsigned)settings->log_port,
                     wine_log_names[settings->wine_log < PW_SETTINGS_WINE_LOG_COUNT ? settings->wine_log : 0],
                     settings->wait_watchdog ? 1 : 0, settings->gpu_hang_report ? 1 : 0,
                     settings->update_check ? 1 : 0);
    return n < 0 || (size_t)n >= cap ? -1 : n;
}

static int settings_path(const char *root, const char *name, char *out, size_t cap)
{
    int n = snprintf(out, cap, "%s/%s", root ? root : "", name);
    return root && *root && n > 0 && (size_t)n < cap ? 0 : -1;
}

int pw_settings_load(const char *root, PwSettings *settings)
{
    char path[512], text[PW_SETTINGS_FILE_MAX + 1];
    FILE *file;
    size_t length;

    pw_settings_defaults(settings);
    if (settings_path(root, "settings.conf", path, sizeof(path))) return -1;
    if (!(file = fopen(path, "rb"))) return errno == ENOENT ? 1 : -1;
    length = fread(text, 1, sizeof(text), file);
    int failed = ferror(file) || length > PW_SETTINGS_FILE_MAX;
    fclose(file);
    if (failed) return -1;
    pw_settings_parse(text, length, settings);
    return 0;
}

int pw_settings_save(const char *root, const PwSettings *settings)
{
    char path[512], temporary[520], text[PW_SETTINGS_TEXT_MAX];
    int length = pw_settings_format(settings, text, sizeof(text)), fd, failed;

    if (length < 0 || settings_path(root, "settings.conf", path, sizeof(path)) ||
        settings_path(root, "settings.conf.tmp", temporary, sizeof(temporary))) return -1;
    if ((fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0666)) < 0) return -1;
    failed = write(fd, text, (size_t)length) != length || fsync(fd) != 0;
    if (close(fd) != 0) failed = 1;
    if (failed || rename(temporary, path) != 0) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

const char *pw_settings_row_label(PwSettingsRow row)
{
    switch (row) {
    case PW_SETTINGS_ROW_LOG: return "Send log to PC";
    case PW_SETTINGS_ROW_LOG_SERVER: return "PC address";
    case PW_SETTINGS_ROW_WINE_LOG: return "Wine log";
    case PW_SETTINGS_ROW_WAIT_WATCHDOG: return "Wait watchdog";
    case PW_SETTINGS_ROW_GPU_HANG_REPORT: return "GPU hang report";
    case PW_SETTINGS_ROW_UPDATE_CHECK: return "Check for updates";
    default: return "";
    }
}

const char *pw_settings_row_help(PwSettingsRow row)
{
    switch (row) {
    case PW_SETTINGS_ROW_LOG: return "Streams the app log to ps5logd on the PC below.";
    case PW_SETTINGS_ROW_LOG_SERVER: return "Cross: type the PC's address, as 192.168.1.20 or 192.168.1.20:9300";
    case PW_SETTINGS_ROW_WINE_LOG: return "Profile: each game's own. Quiet: errors. Detailed: crash reports.";
    case PW_SETTINGS_ROW_WAIT_WATCHDOG: return "Logs threads stuck waiting. For hangs: leave off to play.";
    case PW_SETTINGS_ROW_GPU_HANG_REPORT: return "Full report when the GPU falls behind. Heavy: causes stutters.";
    case PW_SETTINGS_ROW_UPDATE_CHECK: return "Asks GitHub for a newer stable release when the launcher opens.";
    default: return "";
    }
}

void pw_settings_row_value(const PwSettings *settings, PwSettingsRow row, char *out, size_t cap)
{
    const char *text = "";
    switch (row) {
    case PW_SETTINGS_ROW_LOG:
        text = settings->log == 1 ? "On" : "Off";
        break;
    case PW_SETTINGS_ROW_LOG_SERVER:
        snprintf(out, cap, "%s:%u", settings->log_server[0] ? settings->log_server : "not set",
                 (unsigned)settings->log_port);
        return;
    case PW_SETTINGS_ROW_WINE_LOG:
        text = settings->wine_log == PW_SETTINGS_WINE_LOG_QUIET ? "Quiet" :
               settings->wine_log == PW_SETTINGS_WINE_LOG_DETAILED ? "Detailed" : "Profile";
        break;
    case PW_SETTINGS_ROW_WAIT_WATCHDOG: text = settings->wait_watchdog ? "On" : "Off"; break;
    case PW_SETTINGS_ROW_GPU_HANG_REPORT: text = settings->gpu_hang_report ? "On" : "Off"; break;
    case PW_SETTINGS_ROW_UPDATE_CHECK: text = settings->update_check ? "On" : "Off"; break;
    default: break;
    }
    snprintf(out, cap, "%s", text);
}

int pw_settings_row_activate(PwSettings *settings, PwSettingsRow row)
{
    switch (row) {
    case PW_SETTINGS_ROW_LOG:
        settings->log = settings->log != 1;
        return 1;
    case PW_SETTINGS_ROW_WINE_LOG:
        settings->wine_log = (PwSettingsWineLog)((settings->wine_log + 1) % PW_SETTINGS_WINE_LOG_COUNT);
        return 1;
    case PW_SETTINGS_ROW_WAIT_WATCHDOG: settings->wait_watchdog = !settings->wait_watchdog; return 1;
    case PW_SETTINGS_ROW_GPU_HANG_REPORT: settings->gpu_hang_report = !settings->gpu_hang_report; return 1;
    case PW_SETTINGS_ROW_UPDATE_CHECK: settings->update_check = !settings->update_check; return 1;
    default: return 0;
    }
}

const char *pw_settings_winedebug(const PwSettings *settings)
{
    return settings->wine_log < PW_SETTINGS_WINE_LOG_COUNT ? wine_log_values[settings->wine_log] : NULL;
}
