/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_update_check.h"

#include <stdio.h>
#include <string.h>

int pw_update_parse_version(const char *text, unsigned v[3])
{
    if (!text || !v) return -1;
    if (*text == 'v' || *text == 'V') text++;
    for (int part = 0; part < 3; part++) {
        unsigned long value = 0;
        int digits = 0;

        while (*text >= '0' && *text <= '9') {
            value = value * 10u + (unsigned long)(*text++ - '0');
            if (++digits > 6) return -1;
        }
        if (!digits) return -1;
        v[part] = (unsigned)value;
        if (part < 2 && *text++ != '.') return -1;
    }
    return *text ? -1 : 0;
}

int pw_update_is_newer(const char *current, const char *latest)
{
    unsigned have[3], offer[3];

    if (pw_update_parse_version(current, have) || pw_update_parse_version(latest, offer)) return -1;
    for (int i = 0; i < 3; i++)
        if (offer[i] != have[i]) return offer[i] > have[i];
    return 0;
}

/* The value after "key": within json, skipping whitespace; NULL if absent. */
static const char *value_of(const char *json, size_t length, const char *key)
{
    const size_t key_length = strlen(key);

    for (size_t i = 0; i + key_length + 2 <= length; i++) {
        if (json[i] != '"' || memcmp(json + i + 1, key, key_length) || json[i + 1 + key_length] != '"')
            continue;
        size_t j = i + key_length + 2;
        while (j < length && (json[j] == ' ' || json[j] == '\t' || json[j] == '\r' || json[j] == '\n')) j++;
        if (j >= length || json[j] != ':') continue;
        j++;
        while (j < length && (json[j] == ' ' || json[j] == '\t' || json[j] == '\r' || json[j] == '\n')) j++;
        return j < length ? json + j : NULL;
    }
    return NULL;
}

static int is_true(const char *json, size_t length, const char *key)
{
    const char *value = value_of(json, length, key);
    return value && (size_t)(json + length - value) >= 4 && !memcmp(value, "true", 4);
}

int pw_update_release_tag(const char *json, size_t length, char *tag, size_t capacity)
{
    const char *value;
    size_t n = 0;

    if (!json || !tag || !capacity) return -1;
    tag[0] = 0;
    if (is_true(json, length, "draft") || is_true(json, length, "prerelease")) return -1;
    if (!(value = value_of(json, length, "tag_name")) || *value != '"') return -1;
    for (value++; value < json + length && *value != '"'; value++) {
        if (*value == '\\' || n + 1 >= capacity) return -1;
        tag[n++] = *value;
    }
    if (value >= json + length) { tag[0] = 0; return -1; }
    tag[n] = 0;
    return n ? 0 : -1;
}

int pw_update_notice(const char *current, const char *json, size_t length, const char *site,
                     char *out, size_t capacity)
{
    char tag[32];
    int newer;

    if (!out || !capacity) return -1;
    out[0] = 0;
    if (pw_update_release_tag(json, length, tag, sizeof(tag))) return -1;
    if ((newer = pw_update_is_newer(current, tag)) <= 0) return newer;
    if (snprintf(out, capacity, "%s available: %s", tag, site ? site : "") >= (int)capacity) {
        out[0] = 0;
        return -1;
    }
    return 1;
}
