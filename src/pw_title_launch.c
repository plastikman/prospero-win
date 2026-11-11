/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_title_launch.h"
#include <string.h>
#include <stddef.h>

typedef struct Reader {
    const unsigned char *next, *end;
} Reader;
static void whitespace(Reader *r) {
    while (r->next < r->end && (*r->next == ' ' || *r->next == '\t' ||
           *r->next == '\r' || *r->next == '\n')) ++r->next;
}
static int take(Reader *r, unsigned char c) {
    whitespace(r);
    if (r->next == r->end || *r->next != c) return 0;
    ++r->next; return 1;
}
static int hex(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
/* Identity and managed-path fields are ASCII. Decode JSON escapes before
 * validating them, so escaped traversal/duplicates cannot bypass the contract. */
static int string(Reader *r, char *out, size_t capacity) {
    size_t n = 0;
    if (!take(r, '"')) return PW_ERR_MALFORMED;
    while (r->next < r->end) {
        unsigned c = *r->next++;
        if (c == '"') { out[n] = 0; return PW_OK; }
        if (c == '\\') {
            if (r->next == r->end) return PW_ERR_TRUNCATED;
            c = *r->next++;
            if (c == 'u') {
                if ((size_t)(r->end - r->next) < 4) return PW_ERR_TRUNCATED;
                c = 0;
                for (unsigned i = 0; i < 4; ++i) {
                    int digit = hex(*r->next++);
                    if (digit < 0) return PW_ERR_MALFORMED;
                    c = (c << 4) | (unsigned)digit;
                }
            } else if (c != '"' && c != '\\' && c != '/') return PW_ERR_MALFORMED;
        }
        if (c < 0x20 || c > 0x7e) return PW_ERR_MALFORMED;
        if (n + 1 >= capacity) return PW_ERR_LIMIT;
        out[n++] = (char)c;
    }
    return PW_ERR_TRUNCATED;
}
static int number(Reader *r, uint64_t *out) {
    uint64_t n = 0;
    whitespace(r);
    if (r->next == r->end || *r->next < '0' || *r->next > '9') return PW_ERR_MALFORMED;
    if (*r->next == '0' && r->next + 1 < r->end && r->next[1] >= '0' && r->next[1] <= '9')
        return PW_ERR_MALFORMED;
    while (r->next < r->end && *r->next >= '0' && *r->next <= '9') {
        unsigned digit = *r->next++ - '0';
        if (n > (UINT64_MAX - digit) / 10) return PW_ERR_OVERFLOW;
        n = n * 10 + digit;
    }
    *out = n; return PW_OK;
}
static int identifier(const char *s) {
    if (!s[0] || !((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= '0' && s[0] <= '9'))) return 0;
    for (size_t i = 1; s[i]; ++i)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') ||
              s[i] == '_' || s[i] == '-')) return 0;
    return 1;
}
static int title(const char *s) {
    if (!s || strlen(s) != 9) return 0;
    for (unsigned i = 0; i < 9; ++i)
        if (i < 4 ? s[i] < 'A' || s[i] > 'Z' : s[i] < '0' || s[i] > '9') return 0;
    return 1;
}
static int game_root(const char *path, unsigned schema) {
    if (strncmp(path, "/data/", 6) && strncmp(path, "/mnt/", 5)) return 0;
    if (schema == 3 ? strncmp(path, "/mnt/usb/", 9) != 0
                    : !strcmp(path, "/mnt/usb") || !strncmp(path, "/mnt/usb/", 9)) return 0;
    /* Game files must stay outside Playden's private bottle/control tree. */
    if (!strncmp(path, "/data/playden", 13) && (!path[13] || path[13] == '/')) return 0;
    const char *part = path + 1;
    for (const char *p = part;; ++p) {
        if (*p == '\\') return 0;
        if (*p != '/' && *p) continue;
        size_t n = (size_t)(p - part);
        if (!n || (n == 1 && part[0] == '.') || (n == 2 && part[0] == '.' && part[1] == '.')) return 0;
        if (!*p) return 1;
        part = p + 1;
    }
}
/* "/data/homebrew/<title>/games/<app>" without stdio (portable core contract). */
static int legacy_game_root(char *out, size_t capacity, const char *title_id, uint32_t app_id) {
    char digits[11];
    size_t count = 0, used = 0;
    do { digits[count++] = (char)('0' + app_id % 10u); app_id /= 10u; } while (app_id);
    const char *parts[] = {"/data/homebrew/", title_id, "/games/"};
    for (unsigned i = 0; i < 3; ++i) {
        size_t n = strlen(parts[i]);
        if (n >= capacity - used) return 0;
        memcpy(out + used, parts[i], n); used += n;
    }
    if (count >= capacity - used) return 0;
    while (count) out[used++] = digits[--count];
    out[used] = 0;
    return 1;
}
static int fail(char *error, size_t size, int status, const char *message) {
    if (error && size) {
        size_t n = strlen(message);
        if (n >= size) n = size - 1;
        memcpy(error, message, n);
        error[n] = 0;
    }
    return status;
}
int pw_title_launch_parse(const char *bytes, size_t length, const char *expected_title_id,
                         PwTitleLaunch *out, char *error, size_t error_size) {
    static const char *const keys[] = {
        "schema_version", "app_id", "settings_revision", "title_id", "install_id", "data_root",
        "default_profile_id", "runtime_bundle_id", "recipe_id", "recipe_version", "exit_policy", "game_root"
    };
    PwTitleLaunch candidate = {0};
    Reader r;
    unsigned seen = 0;
    if (!bytes || !out || !title(expected_title_id))
        return fail(error, error_size, PW_ERR_PRECONDITION, "invalid descriptor arguments");
    if (!length || length > PW_TITLE_LAUNCH_MAX_BYTES)
        return fail(error, error_size, PW_ERR_LIMIT, "descriptor size outside bounds");
    r.next = (const unsigned char *)bytes; r.end = r.next + length;
    if (!take(&r, '{')) return fail(error, error_size, PW_ERR_MALFORMED, "descriptor must be a JSON object");
    for (;;) {
        char key[64], *destination = NULL;
        size_t capacity = 0;
        unsigned field = 0;
        int status = string(&r, key, sizeof(key));
        if (status != PW_OK) return fail(error, error_size, status, "invalid descriptor key");
        for (; field < sizeof(keys)/sizeof(keys[0]); ++field) if (!strcmp(key, keys[field])) break;
        if (field == sizeof(keys)/sizeof(keys[0]) || (seen & (1u << field)))
            return fail(error, error_size, PW_ERR_MALFORMED, "unknown or duplicate descriptor key");
        if (!take(&r, ':')) return fail(error, error_size, PW_ERR_MALFORMED, "missing descriptor colon");
        if (field < 3) {
            uint64_t value = 0;
            status = number(&r, &value);
            if (status != PW_OK) return fail(error, error_size, status, "invalid descriptor integer");
            if (field == 0) {
                if (value < 1 || value > 3) return fail(error, error_size, PW_ERR_UNSUPPORTED, "unsupported descriptor schema");
                candidate.schema_version = (unsigned)value;
            } else if (field == 1) {
                if (!value || value > UINT32_MAX) return fail(error, error_size, PW_ERR_MALFORMED, "invalid Steam app ID");
                candidate.app_id = (uint32_t)value;
            } else candidate.settings_revision = value;
        } else {
#define FIELD(name) destination = candidate.name; capacity = sizeof(candidate.name); break
            switch (field) {
                case 3: FIELD(title_id);
                case 4: FIELD(install_id);
                case 5: FIELD(data_root);
                case 6: FIELD(default_profile_id);
                case 7: FIELD(runtime_bundle_id);
                case 8: FIELD(recipe_id);
                case 9: FIELD(recipe_version);
                case 10: FIELD(exit_policy);
                case 11: FIELD(game_root);
            }
#undef FIELD
            status = string(&r, destination, capacity);
            if (status != PW_OK) return fail(error, error_size, status, "invalid descriptor string");
        }
        seen |= 1u << field;
        whitespace(&r);
        if (take(&r, '}')) break;
        if (!take(&r, ',')) return fail(error, error_size, PW_ERR_MALFORMED, "missing descriptor separator");
    }
    whitespace(&r);
    if (r.next != r.end || seen != (1u << (candidate.schema_version >= 2 ? 12 : 11)) - 1u)
        return fail(error, error_size, PW_ERR_MALFORMED, "missing fields or trailing descriptor data");
    if (strcmp(candidate.title_id, expected_title_id) || !title(candidate.title_id))
        return fail(error, error_size, PW_ERR_MALFORMED, "descriptor title does not match this executable");
    if (!identifier(candidate.install_id) || !identifier(candidate.default_profile_id) ||
        !identifier(candidate.runtime_bundle_id) || !identifier(candidate.recipe_id) ||
        !identifier(candidate.recipe_version))
        return fail(error, error_size, PW_ERR_MALFORMED, "invalid descriptor identifier");
    {
        static const char root[] = "/data/playden/games/";
        if (strncmp(candidate.data_root, root, sizeof(root) - 1) ||
            strcmp(candidate.data_root + sizeof(root) - 1, candidate.install_id))
            return fail(error, error_size, PW_ERR_MALFORMED, "descriptor data root is not its managed install root");
    }
    if (candidate.schema_version == 1) {
        if (!legacy_game_root(candidate.game_root, sizeof(candidate.game_root),
                              candidate.title_id, candidate.app_id))
            return fail(error, error_size, PW_ERR_LIMIT, "legacy game root outside bounds");
    } else if (!game_root(candidate.game_root, candidate.schema_version))
        return fail(error, error_size, PW_ERR_MALFORMED, "game root is not a canonical console storage path");
    if (strcmp(candidate.exit_policy, "exit-title"))
        return fail(error, error_size, PW_ERR_UNSUPPORTED, "unsupported title exit policy");
    *out = candidate;
    if (error && error_size) error[0] = 0;
    return PW_OK;
}
