/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_wine_launch.h"
#include <string.h>

static const char *value_of(const char *word, const char *key)
{
    size_t length = strlen(key);

    return !strncmp(word, key, length) && word[length] == '=' ? word + length + 1 : NULL;
}

/* An absolute Windows path: a drive letter, ':' and '\'. */
static int valid_path(const char *path)
{
    size_t length = strlen(path);

    return length >= 3 && length < PW_WINE_LAUNCH_PATH_MAX &&
           ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
           path[1] == ':' && path[2] == '\\';
}

static int parse_count(const char *text, uint32_t *out)
{
    uint64_t value = 0;

    if (!*text) return -1;
    for (; *text; text++) {
        if (*text < '0' || *text > '9') return -1;
        if ((value = value * 10u + (uint64_t)(*text - '0')) > UINT32_MAX) return -1;
    }
    *out = (uint32_t)value;
    return 0;
}

int pw_wine_launch_parse(int argc, char *const *argv, const PwWineApp *apps, size_t count,
                         PwWineLaunch *out)
{
    const char *profile = NULL, *path = NULL;

    if (!out || (!apps && count)) return -1;
    memset(out, 0, sizeof(*out));
    out->mode = PW_WINE_LAUNCH_LAUNCHER;
    for (int i = 0; argv && i < argc; i++) {
        const char *word = argv[i], *value;
        if (!word) continue;
        if ((value = value_of(word, "profile"))) profile = value;
        else if ((value = value_of(word, "path"))) path = value;
        else if ((value = value_of(word, "cycle")) && parse_count(value, &out->cycle)) out->cycle = 0;
        else if ((value = value_of(word, "chain")) && parse_count(value, &out->chain)) out->chain = 0;
    }
    if (profile) {
        for (size_t i = 0; i < count && !out->app; i++)
            if (apps[i].id && !strcmp(apps[i].id, profile)) out->app = &apps[i];
        if (!out->app) { out->refused = 1; return 0; }
        if (!path) path = out->app->executable;
    }
    if (!path) return 0;
    if (!valid_path(path)) { out->app = NULL; out->refused = 1; return 0; }
    memcpy(out->executable, path, strlen(path) + 1);
    out->mode = PW_WINE_LAUNCH_GAME;
    return 0;
}

/* Append "key=value" to storage at *used; 0, or -1 when it does not fit. */
static int add_word(char *storage, size_t size, size_t *used, const char *key, const char *value)
{
    size_t key_length = strlen(key), value_length = strlen(value);

    if (*used + key_length + 1 + value_length + 1 > size) return -1;
    memcpy(storage + *used, key, key_length);
    storage[*used + key_length] = '=';
    memcpy(storage + *used + key_length + 1, value, value_length + 1);
    *used += key_length + 1 + value_length + 1;
    return 0;
}

static void decimal(uint32_t value, char number[11])
{
    char digits[11];
    size_t n = 0;

    do digits[n++] = (char)('0' + value % 10u); while ((value /= 10u) && n < sizeof(digits));
    for (size_t i = 0; i < n; i++) number[i] = digits[n - 1 - i];
    number[n] = 0;
}

size_t pw_wine_launch_argv(const PwWineApp *app, uint32_t cycle, char *storage, size_t size,
                           char **argv, size_t max)
{
    return pw_wine_launch_argv_chain(app, cycle, 0, storage, size, argv, max);
}

size_t pw_wine_launch_argv_chain(const PwWineApp *app, uint32_t cycle, uint32_t chain, char *storage,
                                 size_t size, char **argv, size_t max)
{
    char number[11];
    size_t used = 0, words = 0;

    /* profile and path (or launcher), cycle, chain, NULL; the launcher has no chain. */
    if (!storage || !argv || (chain && !app) || max < 4u + (chain ? 1u : 0u)) return 0;
    decimal(cycle, number);
    if (app) {
        if (!app->id || !app->executable) return 0;
        argv[words++] = storage + used;
        if (add_word(storage, size, &used, "profile", app->id)) return 0;
        argv[words++] = storage + used;
        if (add_word(storage, size, &used, "path", app->executable)) return 0;
    } else {
        argv[words++] = storage + used;
        if (add_word(storage, size, &used, "launcher", "1")) return 0;
    }
    argv[words++] = storage + used;
    if (add_word(storage, size, &used, "cycle", number)) return 0;
    if (chain) {
        decimal(chain, number);
        argv[words++] = storage + used;
        if (add_word(storage, size, &used, "chain", number)) return 0;
    }
    argv[words] = NULL;
    return words;
}

/* A path with a ".." component. */
static int has_parent(const char *path)
{
    for (const char *p = path; (p = strstr(p, "..")); p += 2)
        if ((p == path || p[-1] == '\\') && (p[2] == 0 || p[2] == '\\')) return 1;
    return 0;
}

static int ends_exe(const char *path)
{
    size_t length = strlen(path);
    const char *tail = path + length - 4;

    return length > 4 && tail[0] == '.' && (tail[1] | 0x20) == 'e' && (tail[2] | 0x20) == 'x' &&
           (tail[3] | 0x20) == 'e';
}

int pw_wine_launch_chain_parse(const char *text, size_t length, PwWineChain *out)
{
    int version = 0, image = 0;
    size_t start = 0;

    if (!text || !out) return -1;
    memset(out, 0, sizeof(*out));
    while (start < length) {
        size_t end = start, field;
        const char *line = text + start, *value = NULL;
        char *target = NULL;
        size_t capacity = 0;

        while (end < length && text[end] != '\n') end++;
        field = end - start;
        if (field == 15 && !memcmp(line, "pw-exec-chain 1", 15)) version = 1;
        else if (field >= 6 && !memcmp(line, "image=", 6)) { value = line + 6; target = out->image; capacity = sizeof(out->image); image = 1; }
        else if (field >= 5 && !memcmp(line, "args=", 5)) { value = line + 5; target = out->arguments; capacity = sizeof(out->arguments); }
        else if (field >= 4 && !memcmp(line, "cwd=", 4)) { value = line + 4; target = out->cwd; capacity = sizeof(out->cwd); }
        if (target) {
            size_t value_length = field - (size_t)(value - line);
            if (value_length >= capacity || memchr(value, 0, value_length)) return -1;
            memcpy(target, value, value_length);
            target[value_length] = 0;
        }
        start = end + 1;
    }
    if (!version || !image || !valid_path(out->image) || !ends_exe(out->image) || has_parent(out->image))
        return -1;
    {
        size_t cwd_length = strlen(out->cwd);
        while (cwd_length > 3 && out->cwd[cwd_length - 1] == '\\') out->cwd[--cwd_length] = 0;
    }
    if (!valid_path(out->cwd) || has_parent(out->cwd)) {
        /* The image's own folder; a program at a drive's root keeps the root. */
        const char *slash = strrchr(out->image, '\\');
        size_t folder = (size_t)(slash - out->image);
        if (folder < 3) folder = 3;
        memcpy(out->cwd, out->image, folder);
        out->cwd[folder] = 0;
    }
    return 0;
}

int pw_wine_launch_split(const char *text, char *storage, size_t size, const char **words,
                         size_t max)
{
    size_t used = 0;
    int count = 0;

    if (!text || !storage || !words) return -1;
    for (;;) {
        int quoted = 0;

        while (*text == ' ' || *text == '\t') text++;
        if (!*text) return count;
        if ((size_t)count >= max) return -1;
        words[count++] = storage + used;
        for (; *text && (quoted || (*text != ' ' && *text != '\t')); text++) {
            if (*text == '"') { quoted = !quoted; continue; }
            if (used + 1 >= size) return -1;
            storage[used++] = *text;
        }
        if (quoted || used >= size) return -1;
        storage[used++] = 0;
    }
}

int pw_wine_launch_host_dir(const char *prefix, const char *windows_path, char *out, size_t size)
{
    const char *base;
    size_t used;

    if (!prefix || !windows_path || !out || !size || strlen(windows_path) < 3 ||
        windows_path[1] != ':' || windows_path[2] != '\\')
        return -1;
    if (windows_path[0] == 'C' || windows_path[0] == 'c') base = "/drive_c";
    else if (windows_path[0] == 'Z' || windows_path[0] == 'z') base = prefix = "";
    else return -1;
    if (strlen(prefix) + strlen(base) >= size) return -1;
    memcpy(out, prefix, strlen(prefix));
    memcpy(out + strlen(prefix), base, strlen(base));
    used = strlen(prefix) + strlen(base);
    for (const char *p = windows_path + 2; *p; p++) {
        if (used + 1 >= size) return -1;
        out[used++] = *p == '\\' ? '/' : *p;
    }
    /* No trailing separator, except for the root itself. */
    while (used > 1 && out[used - 1] == '/') used--;
    out[used] = 0;
    return 0;
}
