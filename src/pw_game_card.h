/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_GAME_CARD_H
#define PW_GAME_CARD_H
/* The launcher's game card: a player's settings for one game, kept in
 * <root>/settings/<profile id>.conf over the shipped profile
 * (pw_game_profile_apply_settings). Every row starts "As shipped" (empty:
 * the profile's own value); a row the player changes is written to the file,
 * and a file with no changed row is removed. */
#include <stddef.h>
#include <stdint.h>

enum {
    PW_GAME_CARD_BUTTONS = 17,
    PW_GAME_CARD_VALUE = 192,      /* one row's text, dll overrides the longest */
};

typedef enum PwGameCardRow {
    PW_GAME_CARD_PROGRAM = 0,      /* [application] executable: another program in the game's folder */
    PW_GAME_CARD_CONTROLLER,       /* [input] mode */
    PW_GAME_CARD_PRESET,           /* [input] preset: replaces the profile's whole [input] */
    PW_GAME_CARD_POINTER,          /* [input] mouse */
    PW_GAME_CARD_MOVEMENT,         /* [input] keyboard_stick */
    PW_GAME_CARD_SPEED,            /* [input] mouse_speed */
    PW_GAME_CARD_RESOLUTION,       /* [display] desktop */
    PW_GAME_CARD_LOG,              /* [debug] winedebug */
    PW_GAME_CARD_OVERRIDES,        /* [application] dll_overrides_add */
    PW_GAME_CARD_BUTTON_FIRST,     /* [input] cross ... touchpad, in PW_GAME_BUTTON order */
    PW_GAME_CARD_RESET = PW_GAME_CARD_BUTTON_FIRST + PW_GAME_CARD_BUTTONS,
    PW_GAME_CARD_ROW_COUNT
} PwGameCardRow;

typedef struct PwGameCard {
    char values[PW_GAME_CARD_RESET][PW_GAME_CARD_VALUE];  /* "" = as shipped */
} PwGameCard;

/* The row's name. */
const char *pw_game_card_label(PwGameCardRow row);
/* What the row shows for value, into out. */
void pw_game_card_show(const PwGameCard *card, PwGameCardRow row, char *out, size_t capacity);
/* A one-line explanation for the row. */
const char *pw_game_card_help(PwGameCardRow row);
/* Steps a choice row by direction (wrapping; "As shipped" is one of the choices). 0 for a row that is
 * not a choice (overrides: typed; reset: an action). */
int pw_game_card_step(PwGameCard *card, PwGameCardRow row, int direction);
/* Sets a typed row: library overrides (a WINEDLLOVERRIDES list) or the program (a path relative to the
 * game's program, ending .exe, no ".." or drive; "" for the profile's own). 0 when value is refused. */
int pw_game_card_set_text(PwGameCard *card, PwGameCardRow row, const char *value);
/* Every row back to as shipped. */
void pw_game_card_clear(PwGameCard *card);
/* 1 when any row differs from the profile. */
int pw_game_card_changed(const PwGameCard *card);

/* The settings file for card, into out; its length, or 0 when it does not fit. */
size_t pw_game_card_format(const PwGameCard *card, char *out, size_t capacity);
/* Reads a settings file the card wrote (or a player edited) back into card. Lines the card has no row
 * for are dropped; PW_OK, or PW_ERR_MALFORMED for a line that is not key = value in a section. */
int pw_game_card_parse(const uint8_t *bytes, size_t length, PwGameCard *card);

/* Library overrides by DLL name (lower case, no .dll): whether the card's list loads it from the game
 * first, and turning that on ("name=n,b" added) or off (the name taken out of its entry). */
int pw_game_card_override_on(const PwGameCard *card, const char *name);
int pw_game_card_override_toggle(PwGameCard *card, const char *name);
/* DLL names Wine has its own copy of that games often bring a version of: the card looks for each in
 * the game's folder (the title cannot list a folder). */
size_t pw_game_card_dll_count(void);
const char *pw_game_card_dll_name(size_t index);
/* Overrides offered whether or not the game's folder has the DLL. */
size_t pw_game_card_common_count(void);
const char *pw_game_card_common_name(size_t index);
/* Program names looked for next to the game's own (setup and launcher tools), in several spellings. */
size_t pw_game_card_program_count(void);
const char *pw_game_card_program_name(size_t index);

/* The controller presets the card offers, by name (input/<name>.input under the data root). */
size_t pw_game_card_preset_count(void);
const char *pw_game_card_preset_name(size_t index);
#endif
