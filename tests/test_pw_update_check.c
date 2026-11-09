/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_update_check.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define SITE "github.com/plastikman/orbis-win"

static int notice(const char *current, const char *json, char *out, size_t capacity)
{
    return pw_update_notice(current, json, strlen(json), SITE, out, capacity);
}


static void test_release_asset(void)
{
    /* GitHub's answer, trimmed: the uploader object inside an asset has
     * names of its own, and "body" comes after the assets. */
    const char json[] =
        "{\"url\":\"https://api.github.com/x\",\"tag_name\":\"v0.0.3\",\"draft\":false,\"prerelease\":false,"
        "\"name\":\"v0.0.3\",\"assets\":["
        "{\"url\":\"https://api.github.com/a/1\",\"name\":\"BUILD-MANIFEST-01.49.txt\","
        "\"uploader\":{\"login\":\"someone\",\"name\":\"IV0000-GLDS10043_00-GLDS100430000000.pkg\"},"
        "\"size\":1483,\"digest\":\"sha256:35d2c628db6ca05a62c1ef17e7cfd7462f3058113935b8f64504b0f94b026efb\","
        "\"browser_download_url\":\"https://github.com/o/r/releases/download/v0.0.3/BUILD-MANIFEST-01.49.txt\"},"
        "{\"url\":\"https://api.github.com/a/2\",\"name\":\"IV0000-GLDS10043_00-GLDS100430000000.pkg\","
        "\"label\":\"\",\"uploader\":{\"login\":\"someone\",\"id\":1},\"content_type\":\"application/octet-stream\","
        "\"size\": 505741312, \"digest\": \"sha256:5DAFF912c9466073684fcc33e0c71bca166cdb38ae7d969f95a23d56c5a8605d\","
        "\"browser_download_url\":\"https://github.com/o/r/releases/download/v0.0.3/IV0000-GLDS10043_00-GLDS100430000000.pkg\"}"
        "],\"tarball_url\":\"https://x\",\"body\":\"notes with \\\"quotes\\\" and {braces}\"}";
    PwUpdateAsset asset;

    assert(pw_update_release_asset(json, sizeof(json) - 1, "IV0000-GLDS10043_00-GLDS100430000000.pkg", &asset) == 0);
    assert(!strcmp(asset.name, "IV0000-GLDS10043_00-GLDS100430000000.pkg"));
    assert(asset.size == 505741312ull);
    assert(!strcmp(asset.api_url, "https://api.github.com/a/2"));
    assert(!strcmp(asset.sha256, "5daff912c9466073684fcc33e0c71bca166cdb38ae7d969f95a23d56c5a8605d"));
    assert(!strcmp(asset.url, "https://github.com/o/r/releases/download/v0.0.3/IV0000-GLDS10043_00-GLDS100430000000.pkg"));
    assert(pw_update_release_asset(json, sizeof(json) - 1, "BUILD-MANIFEST-01.49.txt", &asset) == 0 && asset.size == 1483);
    assert(pw_update_release_asset(json, sizeof(json) - 1, "missing.pkg", &asset) == -1);
    /* Cut short, or without a digest: refused. */
    assert(pw_update_release_asset(json, 300, "IV0000-GLDS10043_00-GLDS100430000000.pkg", &asset) == -1);
    const char no_digest[] = "{\"assets\":[{\"name\":\"a.pkg\",\"size\":5,\"browser_download_url\":\"https://x/a.pkg\"}]}";
    assert(pw_update_release_asset(no_digest, sizeof(no_digest) - 1, "a.pkg", &asset) == -1);
    const char http[] = "{\"assets\":[{\"name\":\"a.pkg\",\"size\":5,\"browser_download_url\":\"http://x/a.pkg\","
                        "\"digest\":\"sha256:5daff912c9466073684fcc33e0c71bca166cdb38ae7d969f95a23d56c5a8605d\"}]}";
    assert(pw_update_release_asset(http, sizeof(http) - 1, "a.pkg", &asset) == -1);
    assert(pw_update_release_asset("[]", 2, "a.pkg", &asset) == -1);
    /* An "url" that is not GitHub's API is not used. */
    const char other[] = "{\"assets\":[{\"url\":\"https://example.com/a\",\"name\":\"a.pkg\",\"size\":5,"
                         "\"browser_download_url\":\"https://x/a.pkg\","
                         "\"digest\":\"sha256:5daff912c9466073684fcc33e0c71bca166cdb38ae7d969f95a23d56c5a8605d\"}]}";
    assert(pw_update_release_asset(other, sizeof(other) - 1, "a.pkg", &asset) == 0 && !asset.api_url[0]);
}

int main(void)
{
    test_release_asset();
    unsigned v[3];
    char out[96], tag[32];

    /* Versions: MAJOR.MINOR.PATCH, an optional v, nothing else. */
    assert(pw_update_parse_version("0.0.2", v) == 0 && v[0] == 0 && v[1] == 0 && v[2] == 2);
    assert(pw_update_parse_version("v1.12.300", v) == 0 && v[0] == 1 && v[1] == 12 && v[2] == 300);
    assert(pw_update_parse_version("V2.0.0", v) == 0 && v[0] == 2);
    static const char *bad[] = { "", "v", "1", "1.2", "1.2.", "1..3", "1.2.3.4", "1.2.3-beta.1",
                                 "0.0.1-alpha.6", " 1.2.3", "1.2.3 ", "a.b.c", "1234567.0.0" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        assert(pw_update_parse_version(bad[i], v) == -1);
    assert(pw_update_parse_version(NULL, v) == -1);

    /* Ordering is numeric per part, not textual. */
    assert(pw_update_is_newer("0.0.2", "v0.0.3") == 1);
    assert(pw_update_is_newer("0.0.9", "v0.0.10") == 1);
    assert(pw_update_is_newer("0.9.9", "v1.0.0") == 1);
    assert(pw_update_is_newer("0.0.2", "v0.0.2") == 0);
    assert(pw_update_is_newer("0.0.3", "v0.0.2") == 0);
    assert(pw_update_is_newer("1.0.0", "v0.99.99") == 0);
    assert(pw_update_is_newer("unknown", "v0.0.3") == -1);
    assert(pw_update_is_newer("0.0.2", "v0.0.3-beta.1") == -1);

    /* The release JSON: tag_name wherever it is, with GitHub's spacing. */
    static const char release[] =
        "{\n  \"url\": \"https://api.github.com/repos/plastikman/orbis-win/releases/1\",\n"
        "  \"html_url\": \"https://github.com/plastikman/orbis-win/releases/tag/v0.0.3\",\n"
        "  \"tag_name\": \"v0.0.3\",\n  \"name\": \"v0.0.3 \\\"tag_name\\\"\",\n"
        "  \"draft\": false,\n  \"prerelease\": false\n}\n";
    assert(pw_update_release_tag(release, strlen(release), tag, sizeof(tag)) == 0 && !strcmp(tag, "v0.0.3"));
    static const char compact[] = "{\"tag_name\":\"v1.2.3\",\"draft\":false,\"prerelease\":false}";
    assert(pw_update_release_tag(compact, strlen(compact), tag, sizeof(tag)) == 0 && !strcmp(tag, "v1.2.3"));
    /* A key that only ends in tag_name is not it. */
    static const char lookalike[] = "{\"not_tag_name\":\"v9.9.9\",\"tag_name\":\"v0.0.4\"}";
    assert(pw_update_release_tag(lookalike, strlen(lookalike), tag, sizeof(tag)) == 0 && !strcmp(tag, "v0.0.4"));
    /* Drafts and pre-releases are never offered, even if they were answered. */
    static const char pre[] = "{\"tag_name\":\"v0.0.4\",\"prerelease\": true}";
    assert(pw_update_release_tag(pre, strlen(pre), tag, sizeof(tag)) == -1);
    static const char draft[] = "{\"tag_name\":\"v0.0.4\",\"draft\":true}";
    assert(pw_update_release_tag(draft, strlen(draft), tag, sizeof(tag)) == -1);
    /* Missing, not a string, unterminated, too long, escaped, empty, or cut off. */
    static const char *broken[] = {
        "{}", "{\"message\":\"Not Found\"}", "{\"tag_name\":3}", "{\"tag_name\":\"v0.0.3",
        "{\"tag_name\":\"v0.0.3-with-a-tail-that-is-far-too-long-to-fit\"}", "{\"tag_name\":\"v0\\\".0.3\"}",
        "{\"tag_name\":\"\"}", "{\"tag_name\"", "{\"tag_name\":", "",
    };
    for (size_t i = 0; i < sizeof(broken) / sizeof(broken[0]); i++)
        assert(pw_update_release_tag(broken[i], strlen(broken[i]), tag, sizeof(tag)) == -1);
    /* The length bounds the scan: a tag past it is not read. */
    assert(pw_update_release_tag(compact, 12, tag, sizeof(tag)) == -1);

    /* The launcher's line. */
    assert(notice("0.0.2", release, out, sizeof(out)) == 1);
    assert(!strcmp(out, "v0.0.3 available: " SITE));
    assert(notice("0.0.3", release, out, sizeof(out)) == 0 && out[0] == 0);
    assert(notice("0.0.4", release, out, sizeof(out)) == 0 && out[0] == 0);
    assert(notice("0.0.2", pre, out, sizeof(out)) == -1 && out[0] == 0);
    assert(notice("0.0.2", "{\"message\":\"API rate limit exceeded\"}", out, sizeof(out)) == -1);
    assert(notice("unknown", release, out, sizeof(out)) == -1 && out[0] == 0);
    assert(notice("0.0.2", release, out, 20) == -1 && out[0] == 0);   /* does not fit */

    puts("pw_update_check tests passed");
    return 0;
}
