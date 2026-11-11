/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PROSPERO_WIN_PW_TITLE_SESSION_H
#define PROSPERO_WIN_PW_TITLE_SESSION_H

#include "pw_title_launch.h"

enum {
    PW_TITLE_SESSION_ID_SIZE = 37,
    PW_TITLE_SESSION_ORIGIN_SIZE = 8,
    PW_TITLE_SESSION_PATH_SIZE = 640,
};

typedef struct PwTitleSession {
    char session_id[PW_TITLE_SESSION_ID_SIZE];
    char origin[PW_TITLE_SESSION_ORIGIN_SIZE];
    char directory[PW_TITLE_SESSION_PATH_SIZE];
    char path[PW_TITLE_SESSION_PATH_SIZE];
    PwTitleLaunch launch;
    volatile int phase_lock;
    int active;
} PwTitleSession;

/* A session argument is either NULL (a Home launch) or "session=<UUID>"
 * (a Playden handoff). The UUID is canonical lowercase 8-4-4-4-12 hexadecimal form. */
int pw_title_session_id_valid(const char *id);
/* Existing absolute paths with no symlink in any component. These are
 * ownership checks for managed trees, not a race-proof security boundary. */
int pw_title_path_is_directory(const char *path);
int pw_title_path_is_regular_file(const char *path);
int pw_title_session_start(PwTitleSession *session, const PwTitleLaunch *launch,
                           const char *session_argument, char *error, size_t error_size);
/* Continue a handed-off session after the title restarted itself to run the next
 * program. The record must exist, belong to this launch and not have ended. */
int pw_title_session_resume(PwTitleSession *session, const PwTitleLaunch *launch,
                            const char *session_argument, char *error, size_t error_size);

/* Call running only after the Wine entry thread has actually begun. Exited
 * may omit an exit status when Wine's exit callback does not provide one. */
int pw_title_session_running(PwTitleSession *session);
int pw_title_session_exited(PwTitleSession *session, int has_exit_code, int exit_code);
int pw_title_session_failed(PwTitleSession *session, int has_exit_code, int exit_code);
void pw_title_session_close(PwTitleSession *session);

#endif
