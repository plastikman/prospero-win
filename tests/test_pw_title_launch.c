/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_title_launch.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static const char valid[] =
    "{\"schema_version\":1,\"install_id\":\"install-220\",\"title_id\":\"FAKE78001\","
    "\"app_id\":220,\"data_root\":\"/data/playden/games/install-220\","
    "\"default_profile_id\":\"playden-steam-220\",\"settings_revision\":0,"
    "\"runtime_bundle_id\":\"wine-hl2-v1\",\"recipe_id\":\"steam-220-hl2-v1\","
    "\"recipe_version\":\"1\",\"exit_policy\":\"exit-title\"}";
static int parse(const char *text, PwTitleLaunch *out, char *error) {
    return pw_title_launch_parse(text, strlen(text), "FAKE78001", out, error, 128);
}
static void rejects_replacement(const char *old, const char *replacement) {
    char text[1024], error[128];
    PwTitleLaunch out, untouched;
    const char *at = strstr(valid, old);
    assert(at);
    snprintf(text, sizeof(text), "%.*s%s%s", (int)(at-valid), valid, replacement, at+strlen(old));
    memset(&out, 0xa5, sizeof(out)); untouched = out;
    assert(parse(text, &out, error) != PW_OK);
    assert(!memcmp(&out, &untouched, sizeof(out)));
    assert(error[0] != '\0');
}
static void explicit_game_root_contract(void) {
    char text[2048], error[128]; PwTitleLaunch out;
    snprintf(text, sizeof(text), "{\"game_root\":\"/data/homebrew/FAKE77031/games/220\",%s", valid + 1);
    char *schema = strstr(text, "\"schema_version\":1"); assert(schema); schema[strlen("\"schema_version\":")] = '2';
    assert(parse(text, &out, error) == PW_OK);
    assert(out.schema_version == 2 && !strcmp(out.game_root, "/data/homebrew/FAKE77031/games/220"));
    schema[strlen("\"schema_version\":")] = '1';
    assert(parse(text, &out, error) != PW_OK); // Schema 1 cannot smuggle a different root.
    const char *roots[] = {"/mnt/usb0/playden/games/220", "/data/games/220", "/data/playdens/220", NULL};
    for (unsigned i = 0; roots[i]; ++i) {
        snprintf(text, sizeof(text), "{\"game_root\":\"%s\",%s", roots[i], valid + 1);
        schema = strstr(text, "\"schema_version\":1"); schema[strlen("\"schema_version\":")] = '2';
        assert(parse(text, &out, error) == PW_OK && !strcmp(out.game_root, roots[i]));
    }
    const char *bad[] = {"", "/", "data/games/220", "/data/games/220/", "/data//220", "/data/games/../220", "/mnt/./220", "/other/games/220",
                         "/data/playden", "/data/playden/games/install-220", "/data/homebrew/x\\\\y", NULL};
    for (unsigned i = 0; bad[i]; ++i) {
        snprintf(text, sizeof(text), "{\"game_root\":\"%s\",%s", bad[i], valid + 1);
        schema = strstr(text, "\"schema_version\":1"); schema[strlen("\"schema_version\":")] = '2';
        assert(parse(text, &out, error) != PW_OK);
    }
}
static void usb_search_root_contract(void) {
    char text[2048], error[128]; PwTitleLaunch out;
    const char *good[] = {"/mnt/usb/playden/games/220", "/mnt/usb/220", NULL};
    const char *bad[] = {"/mnt/usb", "/mnt/usb/", "/mnt/usb0/playden/games/220", "/mnt/usbs/220", "/mnt/ext0/220",
                         "/data/games/220", "/mnt/usb//220", "/mnt/usb/../220", "/mnt/usb/./220", NULL};
    for (unsigned schema = 2; schema <= 3; ++schema) {
        for (unsigned i = 0; good[i]; ++i) {
            snprintf(text, sizeof(text), "{\"game_root\":\"%s\",%s", good[i], valid + 1);
            char *field = strstr(text, "\"schema_version\":1"); field[strlen("\"schema_version\":")] = (char)('0' + schema);
            memset(&out, 0, sizeof(out));
            if (schema == 3) assert(parse(text, &out, error) == PW_OK && out.schema_version == 3 && !strcmp(out.game_root, good[i]));
            else assert(parse(text, &out, error) != PW_OK); /* Schema 2 names a real place. */
        }
    }
    snprintf(text, sizeof(text), "{\"game_root\":\"/mnt/usb\",%s", valid + 1);
    char *field = strstr(text, "\"schema_version\":1"); field[strlen("\"schema_version\":")] = '2';
    assert(parse(text, &out, error) != PW_OK);
    for (unsigned i = 0; bad[i]; ++i) {
        snprintf(text, sizeof(text), "{\"game_root\":\"%s\",%s", bad[i], valid + 1);
        field = strstr(text, "\"schema_version\":1"); field[strlen("\"schema_version\":")] = '3';
        assert(parse(text, &out, error) != PW_OK);
    }
}
int main(void) {
    explicit_game_root_contract();
    usb_search_root_contract();
    PwTitleLaunch out;
    char error[128], text[1024];
    assert(parse(valid, &out, error) == PW_OK);
    assert(!strcmp(out.title_id, "FAKE78001"));
    assert(!strcmp(out.install_id, "install-220"));
    assert(!strcmp(out.data_root, "/data/playden/games/install-220"));
    assert(!strcmp(out.default_profile_id, "playden-steam-220"));
    assert(out.schema_version == 1 && out.app_id == 220 && out.settings_revision == 0);
    assert(!strcmp(out.recipe_version, "1") && !strcmp(out.exit_policy, "exit-title"));
    rejects_replacement("\"schema_version\":1", "\"schema_version\":2"); // Schema 2 requires its explicit root.
    rejects_replacement("\"schema_version\":1", "\"schema_version\":3");
    rejects_replacement("FAKE78001", "FAKE78002");
    rejects_replacement("FAKE78001", "fake78001");
    rejects_replacement("\"app_id\":220", "\"app_id\":0");
    rejects_replacement("\"app_id\":220", "\"app_id\":4294967296");
    rejects_replacement("\"app_id\":220", "\"app_id\":0220");
    rejects_replacement("\"app_id\":220", "\"app_id\":220.0");
    rejects_replacement("\"app_id\":220", "\"app_id\":220e0");
    rejects_replacement("\"settings_revision\":0", "\"settings_revision\":18446744073709551616");
    rejects_replacement("\"install_id\":\"install-220\"", "\"install_id\":\"../220\"");
    rejects_replacement("/data/playden/games/install-220", "/data/playden/games/other");
    rejects_replacement("/data/playden/games/install-220", "/data/playden/games/install-220/../other");
    rejects_replacement("/data/playden/games/install-220", "/data/playden/games/install-220/");
    rejects_replacement("\"exit_policy\":\"exit-title\"", "\"exit_policy\":\"catalog\"");
    rejects_replacement("\"default_profile_id\":\"playden-steam-220\"", "\"default_profile_id\":\"../other\"");
    rejects_replacement("\"recipe_version\":\"1\"", "\"recipe_version\":1");
    rejects_replacement("\"recipe_version\":\"1\"", "\"recipe_version\":\"\"");
    rejects_replacement("\"schema_version\":1,", "\"schema_version\":1,\"schema_version\":1,");
    rejects_replacement("\"schema_version\":1,", "\"unknown\":1,\"schema_version\":1,");
    rejects_replacement("\"schema_version\":1,", "");
    rejects_replacement("\"install_id\":\"install-220\"", "\"install_id\":\"install-220\\u0000\"");
    rejects_replacement("\"install_id\":\"install-220\"", "\"install_id\":\"install-220\\n\"");
    assert(pw_title_launch_parse(valid, sizeof(valid)-2, "FAKE78001", &out, error, sizeof(error)) != PW_OK);
    snprintf(text, sizeof(text), "%s true", valid);
    assert(parse(text, &out, error) != PW_OK);
    snprintf(text, sizeof(text), " \n%s\t ", valid);
    assert(parse(text, &out, error) == PW_OK);
    assert(pw_title_launch_parse(valid, sizeof(valid)-1, "bad", &out, error, sizeof(error)) != PW_OK);
    assert(pw_title_launch_parse(NULL, 0, "FAKE78001", &out, error, sizeof(error)) != PW_OK);
    assert(pw_title_launch_parse(valid, PW_TITLE_LAUNCH_MAX_BYTES+1, "FAKE78001", &out, error, sizeof(error)) != PW_OK);
    puts("test_pw_title_launch: passed");
    return 0;
}
