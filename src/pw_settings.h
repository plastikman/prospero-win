/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_SETTINGS_H
#define PW_SETTINGS_H
#include <stddef.h>
#include <stdint.h>

/* The launcher's Settings card and the file it keeps, <root>/settings.conf:
 * KEY=VALUE lines, '#' starts a comment, unknown keys are kept out of the
 * way (ignored, and dropped on the next save).
 *
 *   log=1                  send the app log to a PC (ps5logd); 0 = don't.
 *                          Absent: off. (An older <root>/dev.conf is carried
 *                          over once by the title, then ignored.)
 *   log_server=10.0.0.2    the PC's IPv4 address
 *   log_port=9300
 *   wine_log=profile       profile | quiet | detailed: Wine's channels.
 *                          profile = each game's [debug] winedebug, or the
 *                          title's default; the others replace them.
 *   wait_watchdog=0        Wine's wait watchdog (WINE_PS5_WAIT_WATCHDOG)
 *   gpu_hang_report=0      the GPU driver's full hung-submit report
 *                          (ORBIS_DUMP_HUNG; read by the driver itself)
 *   update_check=1         ask GitHub for a newer stable release
 *
 * This is the console's one switch file: each title keeps its own in its
 * data folder (orbis-win /data/prospero-win, Playden /data/PLDN10001), with
 * the same keys. The game reads it when it starts, so a change applies to
 * the next game; the GPU driver reads gpu_hang_report from it itself.
 * Nothing here touches a profile. */

enum {
    PW_SETTINGS_SERVER = 16,          /* "255.255.255.255" + NUL */
    PW_SETTINGS_DEFAULT_PORT = 9300,
    PW_SETTINGS_FILE_MAX = 4096,      /* a larger file is refused */
    PW_SETTINGS_TEXT_MAX = 512,       /* what pw_settings_format writes, at most */
};

typedef enum PwSettingsWineLog {
    PW_SETTINGS_WINE_LOG_PROFILE, PW_SETTINGS_WINE_LOG_QUIET, PW_SETTINGS_WINE_LOG_DETAILED,
    PW_SETTINGS_WINE_LOG_COUNT
} PwSettingsWineLog;

typedef struct PwSettings {
    int log;                          /* -1 not set (dev.conf decides), 0 off, 1 on */
    char log_server[PW_SETTINGS_SERVER];
    uint16_t log_port;
    PwSettingsWineLog wine_log;
    int wait_watchdog;
    int gpu_hang_report;
    int update_check;
} PwSettings;

/* The card's rows, top to bottom. */
typedef enum PwSettingsRow {
    PW_SETTINGS_ROW_LOG, PW_SETTINGS_ROW_LOG_SERVER, PW_SETTINGS_ROW_WINE_LOG,
    PW_SETTINGS_ROW_WAIT_WATCHDOG, PW_SETTINGS_ROW_GPU_HANG_REPORT, PW_SETTINGS_ROW_UPDATE_CHECK,
    PW_SETTINGS_ROW_COUNT
} PwSettingsRow;

void pw_settings_defaults(PwSettings *settings);
/* Applies text[0..length) over *settings. A known key with a bad value is
 * skipped and the rest still apply. Returns the number of keys applied. */
int pw_settings_parse(const char *text, size_t length, PwSettings *settings);
/* Writes every setting (log only when it is set) into out, NUL-terminated.
 * Returns the length, or -1 when cap is too small. */
int pw_settings_format(const PwSettings *settings, char *out, size_t cap);
/* <root>/settings.conf: load leaves the defaults when the file is absent
 * (returns 1), applies it (0), or refuses one that is unreadable or too large
 * (-1, defaults kept). save writes a temporary file, syncs it and renames it
 * over the old one; 0 on success. */
int pw_settings_load(const char *root, PwSettings *settings);
int pw_settings_save(const char *root, const PwSettings *settings);

/* A dotted IPv4 address, four parts of 0-255 with no leading junk. */
int pw_settings_valid_ipv4(const char *text);
/* Moves part `part` (0-3) of a valid address by delta, wrapping 0..255.
 * An invalid address starts from 0.0.0.0. */
void pw_settings_ipv4_step(char address[PW_SETTINGS_SERVER], unsigned part, int delta);
/* What was typed for the PC: "a.b.c.d" or "a.b.c.d:port", spaces around it
 * ignored. 1 when it was valid and applied, 0 (nothing changed) otherwise. */
int pw_settings_set_address(PwSettings *settings, const char *typed);

/* The card. label/value are short strings the launcher's 5x7 font draws. */
const char *pw_settings_row_label(PwSettingsRow row);
void pw_settings_row_value(const PwSettings *settings, PwSettingsRow row, char *out, size_t cap);
/* One line about the selected row, for under the card: at most
 * PW_SETTINGS_HELP_MAX characters, which is what the card's width shows. */
enum { PW_SETTINGS_HELP_MAX = 68 };
const char *pw_settings_row_help(PwSettingsRow row);
/* Cross on a row: flips an on/off row, steps wine_log to the next level.
 * The address row is edited, not toggled (returns 0). 1 when changed. */
int pw_settings_row_activate(PwSettings *settings, PwSettingsRow row);

/* WINEDEBUG for wine_log, or NULL for "profile" (leave it as it is). */
const char *pw_settings_winedebug(const PwSettings *settings);

#endif
