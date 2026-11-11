/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_WINE_LAUNCH_H
#define PW_WINE_LAUNCH_H
/*
 * What the wine64 title runs, from its arguments.
 *
 * The title runs one game per process: the launcher restarts the title with
 * sceSystemServiceLoadExec and the chosen game's arguments, and closing the
 * game restarts it into the launcher. Arguments are "key=value" words:
 *   profile=<id>   a game from the title's catalog;
 *   path=<exe>     an absolute Windows path, run directly (overrides the
 *                  profile's executable);
 *   cycle=<n>      open/close cycles completed, for unattended validation;
 *   chain=<n>      the n-th program in a row that the game started (a
 *                  launcher's Play): run the program Wine wrote to the
 *                  prefix's exec-chain request, not the profile's.
 * With neither profile nor path the title shows the launcher, as when the
 * system starts it (argc 1, empty argv[0]).
 */
#include <stddef.h>
#include <stdint.h>

enum { PW_WINE_LAUNCH_PATH_MAX = 260, PW_WINE_LAUNCH_ARGS = 5, PW_WINE_LAUNCH_WORDS = 32,
       PW_WINE_CHAIN_ARGS_MAX = 512 };

typedef struct PwWineApp {
    const char *id;           /* profile=<id> */
    const char *name;         /* shown in the launcher */
    const char *detail;       /* second line in the launcher */
    const char *executable;   /* absolute Windows path */
} PwWineApp;

typedef enum PwWineLaunchMode { PW_WINE_LAUNCH_LAUNCHER = 1, PW_WINE_LAUNCH_GAME } PwWineLaunchMode;

typedef struct PwWineLaunch {
    PwWineLaunchMode mode;
    const PwWineApp *app;                         /* NULL for a bare path= */
    char executable[PW_WINE_LAUNCH_PATH_MAX];     /* GAME: what Wine runs */
    uint32_t cycle;
    uint32_t chain;     /* chain=<n>: 0 for the profile's own program */
    unsigned refused;   /* an unknown profile or an invalid path: shown in the launcher */
} PwWineLaunch;

/* Decide from argv. Always fills out; a refused game falls back to the
 * launcher. 0, or -1 when out or the catalog is invalid. */
int pw_wine_launch_parse(int argc, char *const *argv, const PwWineApp *apps, size_t count,
                         PwWineLaunch *out);

/* The argv for LoadExec: app's profile, path and cycle, or, with app NULL,
 * the launcher with the cycle. Words are built in storage; argv is
 * NULL-terminated. Returns the number of words, or 0 when storage or argv
 * is too small. */
size_t pw_wine_launch_argv(const PwWineApp *app, uint32_t cycle, char *storage, size_t size,
                           char **argv, size_t max);
/* The same with chain=<chain> after the cycle when chain is not 0 (app must
 * not be NULL then): one more word, so argv needs room for 5 entries. */
size_t pw_wine_launch_argv_chain(const PwWineApp *app, uint32_t cycle, uint32_t chain, char *storage,
                                 size_t size, char **argv, size_t max);

/* The program a game started, as ntdll writes it to WINE_PS5_EXEC_CHAIN
 * (wine/patches 1083): "pw-exec-chain 1", then image=, args= and cwd= lines. */
typedef struct PwWineChain {
    char image[PW_WINE_LAUNCH_PATH_MAX];          /* absolute Windows path of an .exe */
    char arguments[PW_WINE_CHAIN_ARGS_MAX + 1];   /* the command line after the program */
    char cwd[PW_WINE_LAUNCH_PATH_MAX];            /* no trailing '\' except a drive root */
} PwWineChain;
/* Parse a request. The image must be an absolute .exe path without ".."; a
 * missing, relative or ".." cwd becomes the image's folder. 0, or -1 for a
 * request that is not version 1, has no valid image, or a field too long. */
int pw_wine_launch_chain_parse(const char *text, size_t length, PwWineChain *out);
/* A profile's arguments line as words for Wine's argv, which Wine quotes
 * back into the Windows command line: words split at spaces and tabs, and
 * double quotes group a word ("a b" is one word; the quotes are dropped).
 * Words are copied into storage. Returns the number of words (0 for an
 * empty line), or -1 when text is NULL, a quote is unclosed, or the words
 * do not fit storage or max. */
int pw_wine_launch_split(const char *text, char *storage, size_t size, const char **words,
                         size_t max);

/* A profile's working directory (a Windows path pw_app_profile validated:
 * drive, backslashes, no "..") as the host directory it names in prefix:
 * C: is the prefix's drive_c and Z: the host root, as in a default prefix.
 * Returns 0, or -1 for another drive or when out is too small. */
int pw_wine_launch_host_dir(const char *prefix, const char *windows_path, char *out, size_t size);
#endif
