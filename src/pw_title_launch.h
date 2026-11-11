/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PROSPERO_WIN_PW_TITLE_LAUNCH_H
#define PROSPERO_WIN_PW_TITLE_LAUNCH_H
#include "../include/prospero_win.h"
#include <stddef.h>
#include <stdint.h>
enum { PW_TITLE_LAUNCH_MAX_BYTES = 8192 };
typedef struct PwTitleLaunch {
    unsigned schema_version;
    uint32_t app_id;
    uint64_t settings_revision;
    char title_id[10];
    char install_id[65];
    char data_root[512];
    char default_profile_id[65];
    char runtime_bundle_id[65];
    char recipe_id[65];
    char recipe_version[65];
    char exit_policy[16];
    /* Schema 2 binds the recorded Steam download root independently of title.
     * Schema 3 is the same, for a USB game: /mnt/usb/<folder> names no drive,
     * and the title finds the drive that holds the program. /mnt/usb is never a
     * mount, so stale use fails instead of reaching the wrong drive.
     * Schema 1 retains its legacy per-title inferred root. */
    char game_root[1024];
} PwTitleLaunch;
/* Bounded flat JSON schema. Output is written only on success. Filesystem
 * symlink/ownership checks are performed by the native caller after elevation. */
int pw_title_launch_parse(const char *bytes, size_t length,
                          const char *expected_title_id, PwTitleLaunch *out,
                          char *error, size_t error_size);
#endif
