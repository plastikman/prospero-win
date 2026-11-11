/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_game_card.h"
#include "pw_app_profile.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

typedef struct Choice { const char *value, *label; } Choice;
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static const char *const button_keys[PW_GAME_CARD_BUTTONS] = {
    "cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2", "l3", "r3",
    "up", "down", "left", "right", "options", "create", "touchpad" };
static const char *const button_labels[PW_GAME_CARD_BUTTONS] = {
    "Cross", "Circle", "Square", "Triangle", "L1", "R1", "L2", "R2", "L3", "R3",
    "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Options", "Create", "Touchpad click" };

static const Choice controllers[] = { { "", "As shipped" }, { "keyboard", "Keyboard and mouse" },
                                      { "xinput", "Xbox controller" } };
static const Choice presets[] = { { "", "As shipped" }, { "gamepad", "Gamepad" }, { "mouse", "Mouse" },
                                  { "arrows", "Arrow keys" }, { "mouselook", "Mouse look (3D games)" },
                                  { "goldsrc", "GoldSrc (Half-Life)" }, { "mdk", "MDK" } };
static const Choice sticks[] = { { "", "As shipped" }, { "none", "None" }, { "left_stick", "Left stick" },
                                 { "right_stick", "Right stick" }, { "touchpad", "Touchpad" } };
static const Choice movements[] = { { "", "As shipped" }, { "none", "None" },
                                    { "left_stick", "Left stick (W A S D)" },
                                    { "right_stick", "Right stick (W A S D)" } };
static const Choice speeds[] = { { "", "As shipped" }, { "400", "400" }, { "600", "600" }, { "900", "900" },
                                 { "1200", "1200" }, { "1800", "1800" }, { "2400", "2400" } };
static const Choice resolutions[] = { { "", "As shipped" }, { "1024x768", "1024x768 (4:3)" },
                                      { "1152x864", "1152x864 (4:3)" }, { "1280x960", "1280x960 (4:3)" },
                                      { "1280x720", "1280x720" }, { "1600x900", "1600x900" },
                                      { "1920x1080", "1920x1080" }, { "2560x1440", "2560x1440" } };
static const Choice logs[] = {
    { "", "As shipped" },
    { "-all", "Quiet" },
    { "err+all,+loaddll,+process,+seh", "Normal" },
    { "err+all,warn+all,fixme-all,+loaddll,+process,+seh", "Verbose" },
    { "err+all,warn+all,+key,+keyboard,+dinput,+xinput,+hid,+rawinput,+cursor", "Input" } };
/* What a button can send: the runtime's binding names (src/pw_game_profile.c). */
static const char *const button_values[] = {
    "", "none", "mouse_left", "mouse_right", "mouse_middle", "enter", "escape", "space", "tab",
    "backspace", "shift", "ctrl", "alt", "up", "down", "left", "right", "pageup", "pagedown", "home",
    "end", "insert", "delete", "pause", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m",
    "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z", "0", "1", "2", "3", "4", "5", "6",
    "7", "8", "9", "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12", "comma",
    "period", "slash", "semicolon", "quote", "minus", "equals", "lbracket", "rbracket", "backslash",
    "backquote" };

static const Choice *choices_of(PwGameCardRow row, size_t *count)
{
    switch (row) {
    case PW_GAME_CARD_CONTROLLER: *count = COUNT(controllers); return controllers;
    case PW_GAME_CARD_PRESET: *count = COUNT(presets); return presets;
    case PW_GAME_CARD_POINTER: *count = COUNT(sticks); return sticks;
    case PW_GAME_CARD_MOVEMENT: *count = COUNT(movements); return movements;
    case PW_GAME_CARD_SPEED: *count = COUNT(speeds); return speeds;
    case PW_GAME_CARD_RESOLUTION: *count = COUNT(resolutions); return resolutions;
    case PW_GAME_CARD_LOG: *count = COUNT(logs); return logs;
    default: *count = 0; return NULL;
    }
}

static int is_button(PwGameCardRow row)
{
    return row >= PW_GAME_CARD_BUTTON_FIRST && row < PW_GAME_CARD_RESET;
}

const char *pw_game_card_label(PwGameCardRow row)
{
    static const char *const labels[] = { "Program", "Controller", "Controller preset", "Pointer stick",
                                          "Movement stick", "Pointer speed", "Resolution",
                                          "Wine log", "Library overrides" };
    if (row < PW_GAME_CARD_BUTTON_FIRST) return labels[row];
    if (is_button(row)) return button_labels[row - PW_GAME_CARD_BUTTON_FIRST];
    if (row == PW_GAME_CARD_RESET) return "Reset this game";
    return "";
}

const char *pw_game_card_help(PwGameCardRow row)
{
    switch (row) {
    case PW_GAME_CARD_PROGRAM: return "Left/Right: other programs next to the game. Cross: type a name.";
    case PW_GAME_CARD_CONTROLLER: return "How the game sees the controller. Left/Right: change.";
    case PW_GAME_CARD_PRESET: return "A ready-made layout. It replaces the profile's own controls.";
    case PW_GAME_CARD_POINTER: return "Which stick (or the touchpad) moves the mouse pointer.";
    case PW_GAME_CARD_MOVEMENT: return "A stick that presses W A S D, for games that walk with keys.";
    case PW_GAME_CARD_SPEED: return "How fast the pointer moves, pixels per second.";
    case PW_GAME_CARD_RESOLUTION: return "The game's screen size. 4:3 for older games.";
    case PW_GAME_CARD_LOG: return "Wine's log for this game. Settings > Wine log must be Profile.";
    case PW_GAME_CARD_OVERRIDES: return "Cross: the game's own DLLs and common ones, or type your own.";
    case PW_GAME_CARD_RESET: return "Cross: every row back to the profile's own.";
    default: return "The key or mouse button this button sends. Left/Right: change.";
    }
}

void pw_game_card_show(const PwGameCard *card, PwGameCardRow row, char *out, size_t capacity)
{
    size_t count;
    const Choice *choices = choices_of(row, &count);
    const char *value;

    if (!out || !capacity) return;
    out[0] = 0;
    if (!card || row >= PW_GAME_CARD_ROW_COUNT) return;
    if (row == PW_GAME_CARD_RESET) { snprintf(out, capacity, "%s", pw_game_card_changed(card) ? "Cross" : "-"); return; }
    value = card->values[row];
    if (!value[0]) { snprintf(out, capacity, "As shipped"); return; }
    for (size_t i = 0; choices && i < count; i++)
        if (!strcmp(choices[i].value, value)) { snprintf(out, capacity, "%s", choices[i].label); return; }
    snprintf(out, capacity, "%s", value);
}

int pw_game_card_step(PwGameCard *card, PwGameCardRow row, int direction)
{
    size_t count, index = 0;
    const Choice *choices;
    const char *next;

    if (!card || !direction || row >= PW_GAME_CARD_RESET) return 0;
    if (is_button(row)) {
        count = COUNT(button_values);
        for (size_t i = 0; i < count; i++)
            if (!strcmp(button_values[i], card->values[row])) index = i;
        index = (size_t)(((long)index + direction) % (long)count + (long)count) % count;
        next = button_values[index];
    } else {
        choices = choices_of(row, &count);
        if (!choices) return 0;
        for (size_t i = 0; i < count; i++)
            if (!strcmp(choices[i].value, card->values[row])) index = i;
        index = (size_t)(((long)index + direction) % (long)count + (long)count) % count;
        next = choices[index].value;
    }
    snprintf(card->values[row], sizeof(card->values[row]), "%s", next);
    return 1;
}

int pw_game_card_set_text(PwGameCard *card, PwGameCardRow row, const char *value)
{
    if (!card || !value || (row != PW_GAME_CARD_OVERRIDES && row != PW_GAME_CARD_PROGRAM)) return 0;
    size_t length = strlen(value);
    if (length >= PW_GAME_CARD_VALUE) return 0;
    if (row == PW_GAME_CARD_PROGRAM) {
        /* The runtime checks it again (pw_game_profile_apply_settings); this keeps the card honest. */
        size_t segment = 0;
        if (length && (length < 5 || strncasecmp(value + length - 4, ".exe", 4))) return 0;
        for (size_t i = 0; i <= length; i++) {
            char c = i < length ? value[i] : '\\';
            if (i < length && ((unsigned char)c < 0x20 || strchr(":*?\"<>|", c))) return 0;
            if (c == '\\' || c == '/') {
                size_t n = i - segment;
                if (length && (!n || (n == 2 && !strncmp(value + segment, "..", 2)) || (n == 1 && value[segment] == '.')))
                    return 0;
                segment = i + 1;
            }
        }
        memcpy(card->values[row], value, length + 1);
        return 1;
    }
    for (size_t i = 0; i < length; i++) {
        char c = value[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
              c == '-' || c == '.' || c == ',' || c == ';' || c == '=' || c == '*' || c == '+'))
            return 0;
    }
    memcpy(card->values[row], value, length + 1);
    return 1;
}

void pw_game_card_clear(PwGameCard *card)
{
    if (card) memset(card, 0, sizeof(*card));
}

int pw_game_card_changed(const PwGameCard *card)
{
    for (size_t i = 0; card && i < PW_GAME_CARD_RESET; i++)
        if (card->values[i][0]) return 1;
    return 0;
}

size_t pw_game_card_format(const PwGameCard *card, char *out, size_t capacity)
{
    static const struct { PwGameCardRow row; const char *key; } input_keys[] = {
        { PW_GAME_CARD_PRESET, "preset" }, { PW_GAME_CARD_CONTROLLER, "mode" },
        { PW_GAME_CARD_POINTER, "mouse" }, { PW_GAME_CARD_MOVEMENT, "keyboard_stick" },
        { PW_GAME_CARD_SPEED, "mouse_speed" } };
    size_t used = 0;
    int input = 0;
#define ADD(...) do { \
        int n = snprintf(out + used, capacity - used, __VA_ARGS__); \
        if (n < 0 || (size_t)n >= capacity - used) return 0; \
        used += (size_t)n; \
    } while (0)
    if (!card || !out || !capacity) return 0;
    out[0] = 0;
    ADD("; This game's settings from the launcher's game card, over its profile.\n");
    for (size_t i = 0; i < COUNT(input_keys); i++) {
        if (!card->values[input_keys[i].row][0]) continue;
        if (!input++) ADD("[input]\n");
        ADD("%s = %s\n", input_keys[i].key, card->values[input_keys[i].row]);
    }
    for (size_t b = 0; b < PW_GAME_CARD_BUTTONS; b++) {
        const char *value = card->values[PW_GAME_CARD_BUTTON_FIRST + b];
        if (!value[0]) continue;
        if (!input++) ADD("[input]\n");
        ADD("%s = %s\n", button_keys[b], value);
    }
    if (card->values[PW_GAME_CARD_RESOLUTION][0])
        ADD("[display]\ndesktop = %s\n", card->values[PW_GAME_CARD_RESOLUTION]);
    if (card->values[PW_GAME_CARD_LOG][0]) ADD("[debug]\nwinedebug = %s\n", card->values[PW_GAME_CARD_LOG]);
    if (card->values[PW_GAME_CARD_OVERRIDES][0] || card->values[PW_GAME_CARD_PROGRAM][0]) ADD("[application]\n");
    if (card->values[PW_GAME_CARD_PROGRAM][0]) ADD("executable = %s\n", card->values[PW_GAME_CARD_PROGRAM]);
    if (card->values[PW_GAME_CARD_OVERRIDES][0])
        ADD("dll_overrides_add = %s\n", card->values[PW_GAME_CARD_OVERRIDES]);
#undef ADD
    return used;
}

static int same(const uint8_t *text, size_t length, const char *word)
{
    return strlen(word) == length && !memcmp(text, word, length);
}

int pw_game_card_parse(const uint8_t *bytes, size_t length, PwGameCard *card)
{
    enum { NONE, INPUT, DISPLAY, DEBUG_, APPLICATION, OTHER } section = NONE;
    size_t cursor = 0;

    if (!card || (!bytes && length)) return PW_ERR_PRECONDITION;
    pw_game_card_clear(card);
    while (cursor < length) {
        size_t end = cursor;
        while (end < length && bytes[end] != '\n') end++;
        const uint8_t *begin = bytes + cursor, *stop = bytes + end;
        cursor = end + 1;
        while (begin < stop && (*begin == ' ' || *begin == '\t')) begin++;
        while (stop > begin && (stop[-1] == ' ' || stop[-1] == '\t' || stop[-1] == '\r')) stop--;
        if (begin == stop || *begin == ';' || *begin == '#') continue;
        if (*begin == '[') {
            size_t n = (size_t)(stop - begin);
            section = same(begin, n, "[input]") ? INPUT : same(begin, n, "[display]") ? DISPLAY :
                      same(begin, n, "[debug]") ? DEBUG_ : same(begin, n, "[application]") ? APPLICATION : OTHER;
            continue;
        }
        const uint8_t *equals = memchr(begin, '=', (size_t)(stop - begin));
        if (!equals || section == NONE) return PW_ERR_MALFORMED;
        const uint8_t *key_end = equals, *value = equals + 1;
        while (key_end > begin && (key_end[-1] == ' ' || key_end[-1] == '\t')) key_end--;
        while (value < stop && (*value == ' ' || *value == '\t')) value++;
        size_t key_length = (size_t)(key_end - begin), value_length = (size_t)(stop - value);
        int row = -1;
        if (section == INPUT) {
            static const struct { const char *key; PwGameCardRow row; } keys[] = {
                { "preset", PW_GAME_CARD_PRESET }, { "mode", PW_GAME_CARD_CONTROLLER },
                { "mouse", PW_GAME_CARD_POINTER }, { "keyboard_stick", PW_GAME_CARD_MOVEMENT },
                { "mouse_speed", PW_GAME_CARD_SPEED } };
            for (size_t i = 0; i < COUNT(keys); i++)
                if (same(begin, key_length, keys[i].key)) row = (int)keys[i].row;
            for (size_t b = 0; row < 0 && b < PW_GAME_CARD_BUTTONS; b++)
                if (same(begin, key_length, button_keys[b])) row = (int)(PW_GAME_CARD_BUTTON_FIRST + b);
        } else if (section == DISPLAY && same(begin, key_length, "desktop")) {
            row = PW_GAME_CARD_RESOLUTION;
        } else if (section == DEBUG_ && same(begin, key_length, "winedebug")) {
            row = PW_GAME_CARD_LOG;
        } else if (section == APPLICATION && same(begin, key_length, "dll_overrides_add")) {
            row = PW_GAME_CARD_OVERRIDES;
        } else if (section == APPLICATION && same(begin, key_length, "executable")) {
            row = PW_GAME_CARD_PROGRAM;
        }
        if (row < 0 || !value_length || value_length >= PW_GAME_CARD_VALUE) continue;
        memcpy(card->values[row], value, value_length);
        card->values[row][value_length] = 0;
    }
    return PW_OK;
}

/* The names in an override list ("a,b=n;c=n,b": a, b, c), lower case, into names; how many. */
static size_t override_names(const char *list, char names[][32], size_t capacity)
{
    size_t count = 0;
    const char *entry = list;
    while (*entry && count < capacity) {
        const char *end = strchr(entry, ';'), *equals;
        size_t length = end ? (size_t)(end - entry) : strlen(entry);
        equals = memchr(entry, '=', length);
        size_t left = equals ? (size_t)(equals - entry) : length;
        for (size_t start = 0; start < left && count < capacity;) {
            const char *comma = memchr(entry + start, ',', left - start);
            size_t n = comma ? (size_t)(comma - entry) - start : left - start;
            if (n && n < 32) {
                for (size_t k = 0; k < n; k++) {
                    char c = entry[start + k];
                    names[count][k] = c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
                }
                names[count++][n] = 0;
            }
            start += n + 1;
        }
        entry += length + (end ? 1 : 0);
    }
    return count;
}

int pw_game_card_override_on(const PwGameCard *card, const char *name)
{
    char names[48][32];
    if (!card || !name) return 0;
    size_t count = override_names(card->values[PW_GAME_CARD_OVERRIDES], names, 48);
    for (size_t i = 0; i < count; i++)
        if (!strcmp(names[i], name)) return 1;
    return 0;
}

int pw_game_card_override_toggle(PwGameCard *card, const char *name)
{
    char *list, out[PW_GAME_CARD_VALUE] = "";
    size_t used = 0;
    if (!card || !name || !*name) return 0;
    list = card->values[PW_GAME_CARD_OVERRIDES];
    if (!pw_game_card_override_on(card, name)) {
        int n = snprintf(out, sizeof(out), "%s%s%s=n,b", list, list[0] ? ";" : "", name);
        if (n < 0 || (size_t)n >= sizeof(out)) return 0;
        memcpy(list, out, (size_t)n + 1);
        return 1;
    }
    /* Off: take the name out of whichever entry names it; an entry left with no name goes. */
    for (const char *entry = list; *entry;) {
        const char *end = strchr(entry, ';');
        size_t length = end ? (size_t)(end - entry) : strlen(entry);
        const char *equals = memchr(entry, '=', length);
        size_t left = equals ? (size_t)(equals - entry) : length;
        char kept[PW_GAME_CARD_VALUE] = "";
        size_t kept_used = 0;
        for (size_t start = 0; start < left;) {
            const char *comma = memchr(entry + start, ',', left - start);
            size_t n = comma ? (size_t)(comma - entry) - start : left - start;
            int match = n == strlen(name) && !strncasecmp(entry + start, name, n);
            if (n && !match && kept_used + n + 1 < sizeof(kept)) {
                if (kept_used) kept[kept_used++] = ',';
                memcpy(kept + kept_used, entry + start, n);
                kept_used += n;
                kept[kept_used] = 0;
            }
            start += n + 1;
        }
        if (kept_used) {
            int n = snprintf(out + used, sizeof(out) - used, "%s%s%.*s", used ? ";" : "", kept,
                             (int)(length - left), entry + left);
            if (n < 0 || (size_t)n >= sizeof(out) - used) return 0;
            used += (size_t)n;
        }
        entry += length + (end ? 1 : 0);
    }
    memcpy(list, out, used + 1);
    return 1;
}

static const char *const dll_names[] = {
    "d3dx9_24", "d3dx9_25", "d3dx9_26", "d3dx9_27", "d3dx9_28", "d3dx9_29", "d3dx9_30", "d3dx9_31",
    "d3dx9_32", "d3dx9_33", "d3dx9_34", "d3dx9_35", "d3dx9_36", "d3dx9_37", "d3dx9_38", "d3dx9_39",
    "d3dx9_40", "d3dx9_41", "d3dx9_42", "d3dx9_43", "d3dx10_43", "d3dx11_43", "d3dcompiler_33",
    "d3dcompiler_43", "d3dcompiler_47", "xinput1_1", "xinput1_2", "xinput1_3", "xinput1_4",
    "xinput9_1_0", "xaudio2_7", "xaudio2_8", "xaudio2_9", "x3daudio1_7", "xapofx1_5", "dinput",
    "dinput8", "dsound", "ddraw", "d3dim", "d3dxof", "msvcp60", "msvcp70", "msvcp71", "msvcp80",
    "msvcp90", "msvcp100", "msvcp110", "msvcp120", "msvcp140", "msvcr70", "msvcr71", "msvcr80",
    "msvcr90", "msvcr100", "msvcr110", "msvcr120", "vcruntime140", "mfc42", "msvbvm60" };
static const char *const common_names[] = { "dinput8", "dsound", "xinput1_3", "d3dcompiler_43", "d3dx9_43" };
static const char *const program_names[] = {
    "setup.exe", "Setup.exe", "SETUP.EXE", "config.exe", "Config.exe", "configure.exe", "Configure.exe",
    "launcher.exe", "Launcher.exe", "LAUNCHER.EXE", "settings.exe", "Settings.exe", "options.exe",
    "Options.exe", "setupreg.exe", "GameConfig.exe", "Configurator.exe" };

size_t pw_game_card_dll_count(void) { return COUNT(dll_names); }
const char *pw_game_card_dll_name(size_t index) { return index < COUNT(dll_names) ? dll_names[index] : NULL; }
size_t pw_game_card_common_count(void) { return COUNT(common_names); }
const char *pw_game_card_common_name(size_t index)
{
    return index < COUNT(common_names) ? common_names[index] : NULL;
}
size_t pw_game_card_program_count(void) { return COUNT(program_names); }
const char *pw_game_card_program_name(size_t index)
{
    return index < COUNT(program_names) ? program_names[index] : NULL;
}

size_t pw_game_card_preset_count(void) { return COUNT(presets) - 1; }

const char *pw_game_card_preset_name(size_t index)
{
    return index + 1 < COUNT(presets) ? presets[index + 1].value : NULL;
}
