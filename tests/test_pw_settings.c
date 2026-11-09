/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include "../src/pw_settings.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void test_defaults(void)
{
    PwSettings s;
    pw_settings_defaults(&s);
    assert(s.log == -1 && !s.log_server[0] && s.log_port == 9300);
    assert(s.wine_log == PW_SETTINGS_WINE_LOG_PROFILE && !pw_settings_winedebug(&s));
    assert(!s.wait_watchdog && !s.gpu_hang_report && s.update_check == 1);
}

static void test_parse(void)
{
    PwSettings s;
    const char text[] =
        "# a comment\r\n"
        "  log = on  \r\n"
        "log_server=10.168.2.47 # the PC\n"
        "log_port=9301\n"
        "wine_log=detailed\n"
        "wait_watchdog=yes\n"
        "gpu_hang_report=1\n"
        "update_check=false\n"
        "unknown=1\n"
        "no equals sign\n";
    pw_settings_defaults(&s);
    assert(pw_settings_parse(text, sizeof(text) - 1, &s) == 7);
    assert(s.log == 1 && !strcmp(s.log_server, "10.168.2.47") && s.log_port == 9301);
    assert(s.wine_log == PW_SETTINGS_WINE_LOG_DETAILED);
    assert(!strcmp(pw_settings_winedebug(&s), "+tid,err+all,+loaddll,+process,+seh"));
    assert(s.wait_watchdog == 1 && s.gpu_hang_report == 1 && s.update_check == 0);

    /* Bad values are skipped; the rest still apply. */
    const char bad[] = "log=maybe\nlog_server=10.0.0.256\nlog_port=0\nlog_port=70000\n"
                       "wine_log=loud\nwait_watchdog=0\n";
    assert(pw_settings_parse(bad, sizeof(bad) - 1, &s) == 1);
    assert(s.log == 1 && !strcmp(s.log_server, "10.168.2.47") && s.log_port == 9301);
    assert(s.wine_log == PW_SETTINGS_WINE_LOG_DETAILED && s.wait_watchdog == 0);
    /* No trailing newline. */
    assert(pw_settings_parse("wine_log=quiet", 14, &s) == 1 &&
           !strcmp(pw_settings_winedebug(&s), "-all,err+all"));
}

static void test_ipv4(void)
{
    assert(pw_settings_valid_ipv4("0.0.0.0") && pw_settings_valid_ipv4("255.255.255.255"));
    assert(!pw_settings_valid_ipv4("256.0.0.1") && !pw_settings_valid_ipv4("1.2.3"));
    assert(!pw_settings_valid_ipv4("1.2.3.4.") && !pw_settings_valid_ipv4("1.2.3.4 "));
    assert(!pw_settings_valid_ipv4("1..3.4") && !pw_settings_valid_ipv4("0001.2.3.4"));
    assert(!pw_settings_valid_ipv4("") && !pw_settings_valid_ipv4(NULL));

    char a[PW_SETTINGS_SERVER] = "10.168.2.47";
    pw_settings_ipv4_step(a, 3, 1);
    assert(!strcmp(a, "10.168.2.48"));
    pw_settings_ipv4_step(a, 0, -11);
    assert(!strcmp(a, "255.168.2.48"));
    pw_settings_ipv4_step(a, 0, 1);
    assert(!strcmp(a, "0.168.2.48"));
    pw_settings_ipv4_step(a, 2, 300);         /* wraps by 300 mod 256 = 44 */
    assert(!strcmp(a, "0.168.46.48"));
    char empty[PW_SETTINGS_SERVER] = "";
    pw_settings_ipv4_step(empty, 0, 192);
    assert(!strcmp(empty, "192.0.0.0"));
}

static void test_address(void)
{
    PwSettings s;
    pw_settings_defaults(&s);
    assert(pw_settings_set_address(&s, " 192.168.1.20 \n") && !strcmp(s.log_server, "192.168.1.20") &&
           s.log_port == 9300);
    assert(pw_settings_set_address(&s, "10.0.0.5:9400") && !strcmp(s.log_server, "10.0.0.5") && s.log_port == 9400);
    /* Anything else leaves both as they were. */
    const char *bad[] = { "", "   ", "10.0.0", "10.0.0.5:", "10.0.0.5:0", "10.0.0.5:70000", "10.0.0.5:93a",
                          "pc.local", "10.0.0.256", "1.2.3.4.5", NULL };
    for (int i = 0; bad[i]; i++) assert(!pw_settings_set_address(&s, bad[i]));
    assert(!pw_settings_set_address(&s, NULL));
    assert(!strcmp(s.log_server, "10.0.0.5") && s.log_port == 9400);
}

static void test_rows(void)
{
    PwSettings s;
    char value[48];
    pw_settings_defaults(&s);

    /* An unset log is off, and Cross turns it on, then off. */
    pw_settings_row_value(&s, PW_SETTINGS_ROW_LOG, value, sizeof(value));
    assert(!strcmp(value, "Off"));
    assert(pw_settings_row_activate(&s, PW_SETTINGS_ROW_LOG) && s.log == 1);
    pw_settings_row_value(&s, PW_SETTINGS_ROW_LOG, value, sizeof(value));
    assert(!strcmp(value, "On"));
    assert(pw_settings_row_activate(&s, PW_SETTINGS_ROW_LOG) && s.log == 0);
    assert(pw_settings_row_activate(&s, PW_SETTINGS_ROW_LOG) && s.log == 1);

    pw_settings_row_value(&s, PW_SETTINGS_ROW_LOG_SERVER, value, sizeof(value));
    assert(!strcmp(value, "not set:9300"));
    assert(!pw_settings_row_activate(&s, PW_SETTINGS_ROW_LOG_SERVER));

    /* Wine log cycles profile -> quiet -> detailed -> profile. */
    assert(pw_settings_row_activate(&s, PW_SETTINGS_ROW_WINE_LOG) && s.wine_log == PW_SETTINGS_WINE_LOG_QUIET);
    assert(pw_settings_row_activate(&s, PW_SETTINGS_ROW_WINE_LOG) && s.wine_log == PW_SETTINGS_WINE_LOG_DETAILED);
    pw_settings_row_value(&s, PW_SETTINGS_ROW_WINE_LOG, value, sizeof(value));
    assert(!strcmp(value, "Detailed"));
    assert(pw_settings_row_activate(&s, PW_SETTINGS_ROW_WINE_LOG) && s.wine_log == PW_SETTINGS_WINE_LOG_PROFILE);

    assert(pw_settings_row_activate(&s, PW_SETTINGS_ROW_WAIT_WATCHDOG) && s.wait_watchdog);
    assert(pw_settings_row_activate(&s, PW_SETTINGS_ROW_GPU_HANG_REPORT) && s.gpu_hang_report);
    assert(pw_settings_row_activate(&s, PW_SETTINGS_ROW_UPDATE_CHECK) && !s.update_check);
    for (int r = 0; r < PW_SETTINGS_ROW_COUNT; r++) {
        assert(*pw_settings_row_label((PwSettingsRow)r) && *pw_settings_row_help((PwSettingsRow)r));
        assert(strlen(pw_settings_row_help((PwSettingsRow)r)) <= PW_SETTINGS_HELP_MAX);
        /* The launcher's font has upper case, digits and a little
         * punctuation; everything the card shows must be in it. */
        const char *strings[] = { pw_settings_row_label((PwSettingsRow)r), pw_settings_row_help((PwSettingsRow)r) };
        for (int k = 0; k < 2; k++)
            for (const char *c = strings[k]; *c; c++)
                assert(strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 !'()+,-./:=>?_", *c));
    }
    assert(!pw_settings_row_activate(&s, PW_SETTINGS_ROW_COUNT));
}

static void test_round_trip(void)
{
    char dir[] = "build/host/settings-XXXXXX", text[PW_SETTINGS_TEXT_MAX];
    PwSettings s, back;

    assert(mkdtemp(dir));
    assert(pw_settings_load(dir, &back) == 1 && back.log == -1);   /* no file: defaults */
    pw_settings_defaults(&s);
    assert(pw_settings_format(&s, text, sizeof(text)) > 0);
    assert(!strstr(text, "\nlog="));                                  /* unset log is not written */
    s.log = 1;
    snprintf(s.log_server, sizeof(s.log_server), "192.168.1.20");
    s.log_port = 9400;
    s.wine_log = PW_SETTINGS_WINE_LOG_QUIET;
    s.wait_watchdog = s.gpu_hang_report = 1;
    s.update_check = 0;
    assert(pw_settings_save(dir, &s) == 0);
    assert(pw_settings_load(dir, &back) == 0);
    assert(!memcmp(&s, &back, sizeof(s)));
    assert(pw_settings_format(&s, text, 16) == -1);

    char path[128];
    snprintf(path, sizeof(path), "%s/settings.conf.tmp", dir);
    assert(access(path, F_OK) != 0);                                  /* renamed, not left */
    /* A file too large to be settings is refused, and the defaults stay. */
    snprintf(path, sizeof(path), "%s/settings.conf", dir);
    FILE *f = fopen(path, "w");
    for (int i = 0; i < PW_SETTINGS_FILE_MAX; i++) fputc('#', f);
    fputs("\nlog=1\n", f);
    fclose(f);
    assert(pw_settings_load(dir, &back) == -1 && back.log == -1);
    unlink(path);
    rmdir(dir);
    assert(pw_settings_load(NULL, &back) == -1 && pw_settings_save("", &s) == -1);
}

int main(void)
{
    test_defaults();
    test_parse();
    test_ipv4();
    test_address();
    test_rows();
    test_round_trip();
    puts("test_pw_settings: ok");
    return 0;
}
