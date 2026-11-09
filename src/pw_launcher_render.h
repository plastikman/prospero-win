/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_LAUNCHER_RENDER_H
#define PW_LAUNCHER_RENDER_H
#include "pw_present.h"

/* Software-drawn launcher screen for the fixed 1920x1080 output.  The look
 * is inspired by a classic blue-and-green desktop theme; every shape, colour
 * ramp and the 5x7 glyph set are original to this file.  The scene holds
 * plain strings, so the renderer knows nothing about profiles or Win32. */
enum {
    PW_LAUNCHER_RENDER_WIDTH=1920,PW_LAUNCHER_RENDER_HEIGHT=1080,
    PW_LAUNCHER_RENDER_PAGE=6,   /* tiles visible at once (3 x 2) */
    PW_LAUNCHER_RENDER_NONE=UINT32_MAX
};
typedef struct PwLauncherItem {
    const char *title;           /* shown upper-cased; unknown glyphs as '?' */
    const char *detail;          /* optional second line */
    unsigned available;          /* 0: listed as not available yet */
    const char *badge;           /* optional: shown in place of "Ready" */
} PwLauncherItem;
/* The Settings page: rows of a label and its value, one selected. */
typedef struct PwLauncherSettingsView {
    const char *const *labels;
    const char *const *values;
    uint32_t count,selected;
    const char *help;            /* optional line under the rows */
    int editing;                 /* the selected value is being edited */
    int edit_part;               /* while editing: which dot-separated part (0-3) */
} PwLauncherSettingsView;
typedef struct PwLauncherScene {
    const PwLauncherItem *items;
    uint32_t count,selected;     /* selected==PW_LAUNCHER_RENDER_NONE: no highlight */
    const char *status;          /* optional taskbar status text */
    const char *notice;          /* optional title-bar notice (e.g. an update) */
    const PwLauncherSettingsView *settings;  /* non-NULL: draw the Settings page, not the tiles */
    const char *hints;           /* optional taskbar button hints in place of the default */
} PwLauncherScene;

/* What a pad button or a key does in the launcher. */
typedef enum PwLauncherAction {
    PW_LAUNCHER_ACTION_NONE, PW_LAUNCHER_ACTION_LEFT, PW_LAUNCHER_ACTION_RIGHT,
    PW_LAUNCHER_ACTION_UP, PW_LAUNCHER_ACTION_DOWN, PW_LAUNCHER_ACTION_CHOOSE,
    PW_LAUNCHER_ACTION_BACK
} PwLauncherAction;

/* A USB keyboard's key (a Windows virtual key) in the launcher: the arrow
 * keys move, Enter and Space choose, Escape and Backspace go back. */
PwLauncherAction pw_launcher_key_action(uint32_t vk);
/* Moves the selection over the three-wide grid of tiles. 1 when action
 * chooses the selected tile and it is one of the first choosable (the
 * games; refused profiles follow them), else 0. */
int pw_launcher_navigate(PwLauncherScene *scene, PwLauncherAction action, uint32_t choosable);

/* Draws the whole screen into target (exactly 1920x1080, BGRX, any valid
 * stride).  The page shown is the one containing the selection. */
int pw_launcher_render(const PwLauncherScene *scene,const PwPresentTarget *target);

#endif
