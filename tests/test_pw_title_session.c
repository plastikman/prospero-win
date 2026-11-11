/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "../src/pw_title_session.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void format_path(char *out, size_t capacity, const char *format, ...)
{
    va_list args;
    int length;
    va_start(args, format);
    length = vsnprintf(out, capacity, format, args);
    va_end(args);
    assert(length >= 0 && (size_t)length < capacity);
}

static PwTitleLaunch launch_for(const char *root)
{
    PwTitleLaunch launch;
    memset(&launch, 0, sizeof(launch));
    launch.schema_version = 1;
    launch.app_id = 220;
    launch.settings_revision = 42;
    strcpy(launch.title_id, "FAKE78001");
    strcpy(launch.install_id, "install-220");
    strcpy(launch.data_root, root);
    strcpy(launch.default_profile_id, "game");
    strcpy(launch.runtime_bundle_id, "wine-e16ec8f3");
    strcpy(launch.recipe_id, "steam-220-hl2-v1");
    strcpy(launch.recipe_version, "1");
    strcpy(launch.exit_policy, "exit-title");
    return launch;
}

static void make_temp_dir(char *path)
{
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(close(fd) == 0);
    assert(unlink(path) == 0);
    assert(mkdir(path, 0700) == 0);
}

static void read_text(const char *path, char *text, size_t capacity)
{
    int fd = open(path, O_RDONLY);
    ssize_t got;
    assert(fd >= 0);
    got = read(fd, text, capacity - 1);
    assert(got >= 0);
    text[got] = 0;
    assert(close(fd) == 0);
}

static void assert_contains(const char *text, const char *part)
{
    if (!strstr(text, part)) {
        fprintf(stderr, "missing %s in %s\n", part, text);
        abort();
    }
}

static void test_uuid_validation(void)
{
    assert(pw_title_session_id_valid("123e4567-e89b-12d3-a456-426614174000"));
    assert(!pw_title_session_id_valid("123E4567-E89B-12D3-A456-426614174000"));
    assert(!pw_title_session_id_valid("123e4567e89b12d3a456426614174000"));
    assert(!pw_title_session_id_valid("../../123e4567-e89b-12d3-a456-426614174000"));
    assert(!pw_title_session_id_valid("123e4567-e89b-12d3-a456-42661417400g"));
}

static void test_playden_lifecycle_is_atomic(void)
{
    char root[] = "/tmp/pw-title-session-XXXXXX";
    const char *id = "123e4567-e89b-12d3-a456-426614174000";
    char argument[64], text[2048];
    PwTitleLaunch launch;
    PwTitleSession session;
    DIR *dir;
    struct dirent *entry;

    make_temp_dir(root);
    launch = launch_for(root);
    format_path(argument, sizeof(argument), "session=%s", id);
    assert(pw_title_session_start(&session, &launch, argument, NULL, 0) == 0);
    assert(!strcmp(session.session_id, id));
    assert(!strcmp(session.origin, "playden"));
    {
        struct stat info;
        assert(lstat(session.path, &info) == 0);
        assert(info.st_uid == geteuid() && (info.st_mode & 07777) == 0600);
        assert(lstat(session.directory, &info) == 0);
        assert(info.st_uid == geteuid() && (info.st_mode & 07777) == 0700);
    }
    read_text(session.path, text, sizeof(text));
    assert_contains(text, "\"session_id\":\"123e4567-e89b-12d3-a456-426614174000\"");
    assert_contains(text, "\"install_id\":\"install-220\"");
    assert_contains(text, "\"title_id\":\"FAKE78001\"");
    assert_contains(text, "\"origin\":\"playden\"");
    assert_contains(text, "\"settings_revision\":42");
    assert_contains(text, "\"runtime_bundle_id\":\"wine-e16ec8f3\"");
    assert_contains(text, "\"phase\":\"starting\"");
    assert(!strstr(text, "exit_code"));

    assert(pw_title_session_running(&session) == 0);
    read_text(session.path, text, sizeof(text));
    assert_contains(text, "\"phase\":\"running\"");
    assert(pw_title_session_exited(&session, 1, 7) == 0);
    read_text(session.path, text, sizeof(text));
    assert_contains(text, "\"phase\":\"exited\"");
    {
        struct stat info;
        assert(lstat(session.path, &info) == 0);
        assert(info.st_uid == geteuid() && (info.st_mode & 07777) == 0600);
    }
    assert_contains(text, "\"exit_code\":7");

    {
        char sessions[512];
        format_path(sessions, sizeof(sessions), "%s/logs/sessions", root);
        dir = opendir(sessions);
    }
    assert(dir);
    while ((entry = readdir(dir))) {
        assert(!strstr(entry->d_name, ".tmp"));
        assert(!strstr(entry->d_name, ".lock"));
    }
    closedir(dir);
    pw_title_session_close(&session);
}

static void test_home_launch_generates_fresh_id(void)
{
    char root[] = "/tmp/pw-title-home-XXXXXX";
    char text[2048];
    PwTitleLaunch launch;
    PwTitleSession first, second;

    make_temp_dir(root);
    launch = launch_for(root);
    assert(pw_title_session_start(&first, &launch, NULL, NULL, 0) == 0);
    assert(pw_title_session_id_valid(first.session_id));
    assert(!strcmp(first.origin, "home"));
    read_text(first.path, text, sizeof(text));
    assert_contains(text, "\"origin\":\"home\"");
    assert(pw_title_session_start(&second, &launch, NULL, NULL, 0) == 0);
    assert(strcmp(first.session_id, second.session_id));
    pw_title_session_close(&first);
    pw_title_session_close(&second);
}

static void test_malformed_argument_cannot_escape(void)
{
    char root[] = "/tmp/pw-title-bad-XXXXXX";
    char error[128];
    PwTitleLaunch launch;
    PwTitleSession session;

    make_temp_dir(root);
    launch = launch_for(root);
    assert(pw_title_session_start(&session, &launch, "session=../../outside", error,
                                  sizeof(error)) != 0);
    assert_contains(error, "session");
    assert(access("/tmp/outside.json", F_OK) != 0);
    assert(pw_title_session_start(&session, &launch, "profile=other", error,
                                  sizeof(error)) != 0);
}

static void test_symlinked_log_directory_is_rejected(void)
{
    char root[] = "/tmp/pw-title-link-XXXXXX";
    char outside[] = "/tmp/pw-title-outside-XXXXXX";
    char link_path[512], error[128];
    PwTitleLaunch launch;
    PwTitleSession session;

    make_temp_dir(root);
    make_temp_dir(outside);
    format_path(link_path, sizeof(link_path), "%s/logs", root);
    assert(symlink(outside, link_path) == 0);
    launch = launch_for(root);
    assert(pw_title_session_start(&session, &launch,
                                  "session=123e4567-e89b-12d3-a456-426614174000",
                                  error, sizeof(error)) != 0);
    assert_contains(error, "logs");
}

static void test_failed_record_may_omit_exit_code(void)
{
    char root[] = "/tmp/pw-title-failed-XXXXXX";
    char text[2048];
    PwTitleLaunch launch;
    PwTitleSession session;

    make_temp_dir(root);
    launch = launch_for(root);
    assert(pw_title_session_start(&session, &launch, NULL, NULL, 0) == 0);
    assert(pw_title_session_failed(&session, 0, 0) == 0);
    read_text(session.path, text, sizeof(text));
    assert_contains(text, "\"phase\":\"failed\"");
    assert(!strstr(text, "exit_code"));
    pw_title_session_close(&session);
}

static void test_loader_failure_never_records_running_and_uuid_cannot_replay(void)
{
    char root[] = "/tmp/pw-title-loader-XXXXXX";
    const char *argument = "session=123e4567-e89b-12d3-a456-426614174000";
    char text[2048], preserved[2048], lock[768], temporary[768], error[128];
    PwTitleLaunch launch;
    PwTitleSession session, replay;

    make_temp_dir(root);
    launch = launch_for(root);
    assert(pw_title_session_start(&session, &launch, argument, NULL, 0) == 0);
    read_text(session.path, text, sizeof(text));
    assert_contains(text, "\"phase\":\"starting\"");
    assert(!strstr(text, "\"phase\":\"running\""));
    assert(pw_title_session_failed(&session, 1, -2) == 0);
    read_text(session.path, text, sizeof(text));
    assert_contains(text, "\"phase\":\"failed\"");
    assert_contains(text, "\"exit_code\":-2");
    assert(!strstr(text, "\"phase\":\"running\""));
    strcpy(preserved, text);
    format_path(lock, sizeof(lock), "%s.lock", session.path);
    format_path(temporary, sizeof(temporary), "%s.tmp", session.path);
    {
        int fd = open(lock, O_WRONLY | O_CREAT | O_EXCL, 0600);
        assert(fd >= 0);
        assert(close(fd) == 0);
        fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0600);
        assert(fd >= 0);
        assert(close(fd) == 0);
    }
    assert(pw_title_session_start(&replay, &launch, argument, error, sizeof(error)) != 0);
    assert_contains(error, "already exists");
    assert(access(lock, F_OK) != 0);
    assert(access(temporary, F_OK) != 0);
    read_text(session.path, text, sizeof(text));
    assert(!strcmp(text, preserved));
    assert(pw_title_session_start(&replay, &launch,
                                  "session=123E4567-E89B-12D3-A456-426614174000",
                                  error, sizeof(error)) != 0);
    assert_contains(error, "invalid");
    pw_title_session_close(&session);
}

static void test_orphan_reservation_is_repaired(void)
{
    char root[] = "/tmp/pw-title-orphan-XXXXXX";
    const char *argument = "session=123e4567-e89b-12d3-a456-426614174000";
    char logs[512], sessions[512], lock[768], temporary[768], text[2048];
    PwTitleLaunch launch;
    PwTitleSession session;
    int fd;

    make_temp_dir(root);
    format_path(logs, sizeof(logs), "%s/logs", root);
    format_path(sessions, sizeof(sessions), "%s/sessions", logs);
    assert(mkdir(logs, 0700) == 0);
    assert(mkdir(sessions, 0700) == 0);
    format_path(lock, sizeof(lock), "%s/123e4567-e89b-12d3-a456-426614174000.json.lock", sessions);
    fd = open(lock, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0);
    assert(close(fd) == 0);
    format_path(temporary, sizeof(temporary),
             "%s/123e4567-e89b-12d3-a456-426614174000.json.tmp", sessions);
    fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0);
    assert(close(fd) == 0);

    launch = launch_for(root);
    assert(pw_title_session_start(&session, &launch, argument, NULL, 0) == 0);
    assert(access(lock, F_OK) != 0);
    assert(access(temporary, F_OK) != 0);
    read_text(session.path, text, sizeof(text));
    assert_contains(text, "\"phase\":\"starting\"");
    pw_title_session_close(&session);
}

static void test_persistence_error_does_not_claim_next_phase(void)
{
    char root[] = "/tmp/pw-title-persist-XXXXXX";
    char moved[512], text[2048];
    PwTitleLaunch launch;
    PwTitleSession session;

    make_temp_dir(root);
    launch = launch_for(root);
    assert(pw_title_session_start(&session, &launch, NULL, NULL, 0) == 0);
    format_path(moved, sizeof(moved), "%s-moved", session.directory);
    assert(rename(session.directory, moved) == 0);
    assert(pw_title_session_running(&session) != 0);
    {
        char preserved[768];
        format_path(preserved, sizeof(preserved), "%s/%s.json", moved, session.session_id);
        read_text(preserved, text, sizeof(text));
    }
    assert_contains(text, "\"phase\":\"starting\"");
    assert(!strstr(text, "\"phase\":\"running\""));
    pw_title_session_close(&session);
}

static void test_path_walk_rejects_intermediate_symlink(void)
{
    char root[] = "/tmp/pw-title-walk-XXXXXX";
    char outside[] = "/tmp/pw-title-walk-out-XXXXXX";
    char real_root[PATH_MAX], real_outside[PATH_MAX]; /* realpath() writes up to PATH_MAX */
    char library[512], prefixes[512], link[512], file[512];
    int fd;

    make_temp_dir(root);
    make_temp_dir(outside);
    assert(realpath(root, real_root));
    assert(realpath(outside, real_outside));
    format_path(library, sizeof(library), "%s/library", real_root);
    format_path(prefixes, sizeof(prefixes), "%s/prefixes", library);
    assert(mkdir(library, 0700) == 0);
    assert(mkdir(prefixes, 0700) == 0);
    format_path(file, sizeof(file), "%s/game.exe", real_outside);
    fd = open(file, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0);
    assert(close(fd) == 0);
    assert(pw_title_path_is_directory(prefixes));
    assert(pw_title_path_is_regular_file(file));
    format_path(link, sizeof(link), "%s/game", prefixes);
    assert(symlink(real_outside, link) == 0);
    format_path(file, sizeof(file), "%s/game/game.exe", prefixes);
    assert(!pw_title_path_is_regular_file(file));
    assert(!pw_title_path_is_directory(link));
    assert(!pw_title_path_is_regular_file("relative/game.exe"));
}

typedef struct PhaseRace {
    PwTitleSession *session;
    volatile int go;
    int status;
} PhaseRace;

static void *race_running(void *opaque)
{
    PhaseRace *race = opaque;
    while (!__atomic_load_n(&race->go, __ATOMIC_ACQUIRE)) {}
    race->status = pw_title_session_running(race->session);
    return NULL;
}

static void *race_exited(void *opaque)
{
    PhaseRace *race = opaque;
    while (!__atomic_load_n(&race->go, __ATOMIC_ACQUIRE)) {}
    race->status = pw_title_session_exited(race->session, 0, 0);
    return NULL;
}

static void test_terminal_phase_never_regresses_under_concurrency(void)
{
    char root[] = "/tmp/pw-title-race-XXXXXX";
    char text[2048];
    PwTitleLaunch launch;

    make_temp_dir(root);
    launch = launch_for(root);
    for (int i = 0; i < 20; i++) {
        PwTitleSession session;
        PhaseRace running = { &session, 0, -99 }, exited = { &session, 0, -99 };
        pthread_t running_thread, exited_thread;

        assert(pw_title_session_start(&session, &launch, NULL, NULL, 0) == 0);
        assert(pthread_create(&running_thread, NULL, race_running, &running) == 0);
        assert(pthread_create(&exited_thread, NULL, race_exited, &exited) == 0);
        __atomic_store_n(&running.go, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&exited.go, 1, __ATOMIC_RELEASE);
        assert(pthread_join(running_thread, NULL) == 0);
        assert(pthread_join(exited_thread, NULL) == 0);
        assert(exited.status == 0);
        assert(running.status == 0 || running.status == -1);
        read_text(session.path, text, sizeof(text));
        assert_contains(text, "\"phase\":\"exited\"");
        {
            struct stat info;
            assert(lstat(session.path, &info) == 0);
            assert(info.st_uid == geteuid() && (info.st_mode & 07777) == 0600);
        }
        assert(!strstr(text, "\"phase\":\"running\""));
        pw_title_session_close(&session);
    }
}

static void test_chained_start_keeps_the_session(void)
{
    char root[] = "/tmp/pw-title-session-XXXXXX";
    PwTitleLaunch launch, other;
    PwTitleSession first, next, refused;
    char error[160], text[1024], path[PW_TITLE_SESSION_PATH_SIZE];
    const char *argument = "session=123e4567-e89b-12d3-a456-426614174000";
    const char *unknown = "session=123e4567-e89b-12d3-a456-426614174001";

    make_temp_dir(root);
    launch = launch_for(root);
    format_path(path, sizeof(path), "%s/logs/sessions/123e4567-e89b-12d3-a456-426614174000.json", launch.data_root);
    assert(pw_title_session_resume(&refused, &launch, argument, error, sizeof(error)) != 0);
    assert(pw_title_session_start(&first, &launch, argument, error, sizeof(error)) == 0);
    assert(pw_title_session_running(&first) == 0);
    pw_title_session_close(&first);

    /* Only this launch's own unfinished record continues. */
    assert(pw_title_session_resume(&refused, &launch, unknown, error, sizeof(error)) != 0);
    assert(pw_title_session_resume(&refused, &launch, "session=../../outside", error, sizeof(error)) != 0);
    other = launch;
    snprintf(other.install_id, sizeof(other.install_id), "%s", "another-install");
    assert(pw_title_session_resume(&refused, &other, argument, error, sizeof(error)) != 0);

    assert(pw_title_session_resume(&next, &launch, argument, error, sizeof(error)) == 0);
    read_text(path, text, sizeof(text));
    assert_contains(text, "\"phase\":\"starting\"");
    assert_contains(text, "\"origin\":\"playden\"");
    assert(pw_title_session_failed(&next, 0, 0) == 0);
    pw_title_session_close(&next);
    read_text(path, text, sizeof(text));
    assert_contains(text, "\"phase\":\"failed\"");

    /* An ended session is history. */
    assert(pw_title_session_resume(&refused, &launch, argument, error, sizeof(error)) != 0);
}

static void test_public_session_parents_are_rejected(void)
{
    char root[] = "/tmp/pw-title-private-XXXXXX";
    char logs[512], text[2048], error[512];
    PwTitleLaunch launch;
    PwTitleSession session;
    struct stat info;
    make_temp_dir(root);
    launch = launch_for(root);
    format_path(logs, sizeof(logs), "%s/logs", root);
    assert(mkdir(logs, 0755) == 0);
    assert(pw_title_session_start(&session, &launch, NULL, error, sizeof(error)) == -1);
    assert(lstat(logs, &info) == 0 && (info.st_mode & 07777) == 0755);
    assert(chmod(logs, 0700) == 0);
    assert(pw_title_session_start(&session, &launch, NULL, error, sizeof(error)) == 0);
    assert(chmod(session.directory, 0755) == 0);
    assert(pw_title_session_running(&session) == -1);
    read_text(session.path, text, sizeof(text));
    assert_contains(text, "\"phase\":\"starting\"");
    assert(chmod(session.directory, 0700) == 0);
    assert(pw_title_session_failed(&session, 0, 0) == 0);
    pw_title_session_close(&session);
}

int main(void)
{
    test_public_session_parents_are_rejected();
    test_chained_start_keeps_the_session();
    test_uuid_validation();
    test_playden_lifecycle_is_atomic();
    test_home_launch_generates_fresh_id();
    test_malformed_argument_cannot_escape();
    test_symlinked_log_directory_is_rejected();
    test_failed_record_may_omit_exit_code();
    test_loader_failure_never_records_running_and_uuid_cannot_replay();
    test_orphan_reservation_is_repaired();
    test_persistence_error_does_not_claim_next_phase();
    test_path_walk_rejects_intermediate_symlink();
    test_terminal_phase_never_regresses_under_concurrency();
    puts("pw_title_session: ok");
    return 0;
}
