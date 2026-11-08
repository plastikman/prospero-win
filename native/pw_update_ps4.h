/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_UPDATE_PS4_H
#define PW_UPDATE_PS4_H
/*
 * The launcher's check for a newer stable release, on the console's own HTTPS
 * stack (libSceHttp over libSceSsl). It runs once, on a thread of its own, so
 * the launcher never waits for the network; pw_update_ps4_notice() answers
 * NULL until a newer release has been found, and then the line to show.
 *
 * Nothing is checked when the build does not know its own release
 * (PW_RELEASE_VERSION, from orbis-win's VERSION file), or when
 * <root>/no-update-check exists. Nothing is downloaded or installed.
 */

/* Starts the check (once). root is the library root, e.g. /data/prospero-win;
 * log, if given, receives one line per step worth knowing about. */
void pw_update_ps4_start(const char *root, void (*log)(const char *line));

/* The notice to show, or NULL. Safe to call every frame. */
const char *pw_update_ps4_notice(void);

#endif
