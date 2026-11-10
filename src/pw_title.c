/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_title.h"
#include <stdio.h>
#include <string.h>

static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

int pw_title_id_from_sfo(const uint8_t *sfo, size_t length, char out[PW_TITLE_ID_CAPACITY])
{
    uint32_t keys, data, count;

    if (!out) return -1;
    out[0] = '\0';
    /* PSF: magic "\0PSF", version, key table offset, data table offset, entry
     * count, then 16-byte entries: key offset u16, format u16, length u32,
     * maximum u32, data offset u32. */
    if (!sfo || length < 20 || memcmp(sfo, "\0PSF", 4)) return -1;
    keys = u32(sfo + 8);
    data = u32(sfo + 12);
    count = u32(sfo + 16);
    if (keys > length || data > length || count > (length - 20) / 16) return -1;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *entry = sfo + 20 + (size_t)i * 16;
        size_t key = (size_t)keys + u16(entry), size = u32(entry + 4), offset = (size_t)data + u32(entry + 12);

        if (key + 9 > length || memcmp(sfo + key, "TITLE_ID", 9)) continue;
        if (size < 2 || size > PW_TITLE_ID_CAPACITY || offset > length || size > length - offset) return -1;
        memcpy(out, sfo + offset, size);
        out[size - 1] = '\0';
        if (!out[0]) return -1;
        for (const char *c = out; *c; c++)
            if (!((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9'))) { out[0] = '\0'; return -1; }
        return 0;
    }
    return -1;
}

int pw_title_data_root(const char *title, const char *fallback, int (*is_directory)(const char *path),
                       char *out, size_t capacity)
{
    char path[PW_TITLE_ROOT_CAPACITY];
    int n;

    if (!out || !capacity) return -1;
    if (title && *title && is_directory &&
        (n = snprintf(path, sizeof(path), "/data/%s", title)) > 0 && (size_t)n < sizeof(path) &&
        is_directory(path)) {
        n = snprintf(out, capacity, "%s", path);
    } else {
        n = snprintf(out, capacity, "%s", fallback ? fallback : "");
    }
    return n < 0 || (size_t)n >= capacity ? -1 : 0;
}
