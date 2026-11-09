/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_sha256.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void check(const void *data, size_t size, size_t chunk, const char *want)
{
    PwSha256 sha;
    uint8_t digest[32];
    char hex[65];

    pw_sha256_init(&sha);
    for (size_t at = 0; at < size; at += chunk)
        pw_sha256_update(&sha, (const char *)data + at, size - at < chunk ? size - at : chunk);
    pw_sha256_final(&sha, digest);
    pw_sha256_hex(digest, hex);
    if (strcmp(hex, want)) printf("got %s\nwant %s\n", hex, want);
    assert(!strcmp(hex, want));
}

int main(void)
{
    static char million[1000000];
    const char *two_blocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";

    check("", 0, 1, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check("abc", 3, 1, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    /* 56 bytes: the length no longer fits the first block. */
    for (size_t chunk = 1; chunk <= 64; chunk++)
        check(two_blocks, strlen(two_blocks), chunk,
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    memset(million, 'a', sizeof(million));
    check(million, sizeof(million), 4096, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    check(million, sizeof(million), 777, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    puts("test_pw_sha256: ok");
    return 0;
}
