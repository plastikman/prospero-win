/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_title.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* A minimal PSF with the given keys and string values. */
static size_t sfo(uint8_t *out, const char *const *keys, const char *const *values, size_t count)
{
    size_t key_table = 20 + count * 16, key_size = 0, data_size = 0;
    for (size_t i = 0; i < count; i++) key_size += strlen(keys[i]) + 1;
    size_t data_table = (key_table + key_size + 3) & ~(size_t)3;
    memset(out, 0, 4096);
    memcpy(out, "\0PSF", 4);
    out[4] = 1; out[5] = 1;
    uint32_t v[3] = { (uint32_t)key_table, (uint32_t)data_table, (uint32_t)count };
    memcpy(out + 8, v, 12);
    size_t kpos = 0, dpos = 0;
    for (size_t i = 0; i < count; i++) {
        uint8_t *e = out + 20 + i * 16;
        uint16_t ko = (uint16_t)kpos; uint32_t len = (uint32_t)strlen(values[i]) + 1, max = 16, doff = (uint32_t)dpos;
        memcpy(e, &ko, 2); e[2] = 4; e[3] = 2;
        memcpy(e + 4, &len, 4); memcpy(e + 8, &max, 4); memcpy(e + 12, &doff, 4);
        strcpy((char *)out + key_table + kpos, keys[i]); kpos += strlen(keys[i]) + 1;
        strcpy((char *)out + data_table + dpos, values[i]); dpos += max;
    }
    data_size = dpos;
    return data_table + data_size;
}

static int dirs_playden(const char *path) { return !strcmp(path, "/data/PLDN10001"); }

int main(void)
{
    static uint8_t image[4096];
    char id[PW_TITLE_ID_CAPACITY], root[PW_TITLE_ROOT_CAPACITY];
    const char *keys[] = { "APP_VER", "CATEGORY", "TITLE", "TITLE_ID" };
    const char *values[] = { "01.59", "gd", "Orbis Wine ABI-v4", "GLDS10043" };
    size_t n = sfo(image, keys, values, 4);

    assert(pw_title_id_from_sfo(image, n, id) == 0 && !strcmp(id, "GLDS10043"));
    /* Cut short, wrong magic, no TITLE_ID, junk in it: refused. */
    assert(pw_title_id_from_sfo(image, 30, id) == -1 && !id[0]);
    image[1] = 'X';
    assert(pw_title_id_from_sfo(image, n, id) == -1);
    image[1] = 'P';
    const char *k2[] = { "APP_VER" }, *v2[] = { "01.59" };
    size_t n2 = sfo(image, k2, v2, 1);
    assert(pw_title_id_from_sfo(image, n2, id) == -1);
    const char *v3[] = { "01.59", "gd", "x", "../evil" };
    size_t n3 = sfo(image, keys, v3, 4);
    assert(pw_title_id_from_sfo(image, n3, id) == -1 && !id[0]);
    assert(pw_title_id_from_sfo(NULL, 100, id) == -1);

    /* The data folder: the title's own when it exists, else the fallback. */
    assert(pw_title_data_root("PLDN10001", "/data/prospero-win", dirs_playden, root, sizeof(root)) == 0 &&
           !strcmp(root, "/data/PLDN10001"));
    assert(pw_title_data_root("GLDS10043", "/data/prospero-win", dirs_playden, root, sizeof(root)) == 0 &&
           !strcmp(root, "/data/prospero-win"));
    assert(pw_title_data_root("", "/data/prospero-win", dirs_playden, root, sizeof(root)) == 0 &&
           !strcmp(root, "/data/prospero-win"));
    assert(pw_title_data_root(NULL, "/data/prospero-win", NULL, root, sizeof(root)) == 0 &&
           !strcmp(root, "/data/prospero-win"));
    assert(pw_title_data_root("PLDN10001", "/data/prospero-win", dirs_playden, root, 8) == -1);
    puts("test_pw_title: ok");
    return 0;
}
