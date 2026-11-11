/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_game_card.h"
#include "../src/pw_game_profile.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define APP "[application]\nid = mafia\nname = Mafia\nexecutable = C:\\Games\\Mafia\\Game.exe\n" \
            "working_directory = C:\\Games\\Mafia\nprefix = default\nruntime = wine-wow64\n" \
            "architecture = pe32\ngraphics = dxvk\ndll_overrides = d3d8,d3d9=n\n[input]\nmode = xinput\n"

static int applies(const PwGameCard *card)
{
    char text[4096];
    PwGameProfile profile;
    size_t length = pw_game_card_format(card, text, sizeof(text));
    assert(length);
    assert(pw_game_profile_parse((const uint8_t *)APP, strlen(APP), &profile) == PW_OK);
    return pw_game_profile_apply_settings(&profile, (const uint8_t *)text, length);
}

int main(void)
{
    PwGameCard card, back;
    char text[4096], shown[64];

    /* Untouched: nothing changed, and the file is only its comment. */
    pw_game_card_clear(&card);
    assert(!pw_game_card_changed(&card));
    pw_game_card_show(&card, PW_GAME_CARD_CONTROLLER, shown, sizeof(shown));
    assert(!strcmp(shown, "As shipped"));
    assert(applies(&card) == PW_OK);

    /* Every choice of every row is one the runtime takes. */
    for (int row = 0; row < PW_GAME_CARD_RESET; row++) {
        int steps = 0;
        pw_game_card_clear(&card);
        /* Typed rows (and the overrides picker) are not stepped. */
        if (row == PW_GAME_CARD_OVERRIDES || row == PW_GAME_CARD_PROGRAM) {
            assert(!pw_game_card_step(&card, (PwGameCardRow)row, 1));
            continue;
        }
        do {
            assert(pw_game_card_step(&card, (PwGameCardRow)row, 1));
            if (applies(&card) != PW_OK) {
                fprintf(stderr, "row %d value '%s' refused\n", row, card.values[row]);
                assert(0);
            }
        } while (card.values[row][0] && ++steps < 200);
        assert(steps < 200);           /* wrapped back to as shipped */
    }
    assert(!pw_game_card_step(&card, PW_GAME_CARD_RESET, 1));

    /* A mixed card: written, read back unchanged, and applied as the runtime would. */
    pw_game_card_clear(&card);
    pw_game_card_step(&card, PW_GAME_CARD_CONTROLLER, 1);                     /* keyboard */
    pw_game_card_step(&card, PW_GAME_CARD_PRESET, 4);                         /* mouselook */
    pw_game_card_step(&card, PW_GAME_CARD_POINTER, 4);                        /* touchpad */
    pw_game_card_step(&card, PW_GAME_CARD_RESOLUTION, 2);                     /* 1152x864 */
    pw_game_card_step(&card, PW_GAME_CARD_LOG, 3);                            /* verbose */
    assert(pw_game_card_set_text(&card, PW_GAME_CARD_OVERRIDES, "d3dx9_27=n,b"));
    assert(!pw_game_card_set_text(&card, PW_GAME_CARD_OVERRIDES, "bad value"));
    assert(!pw_game_card_set_text(&card, PW_GAME_CARD_CONTROLLER, "keyboard"));
    pw_game_card_step(&card, (PwGameCardRow)(PW_GAME_CARD_BUTTON_FIRST + 0), 7);   /* cross = space */
    assert(!strcmp(card.values[PW_GAME_CARD_PRESET], "mouselook"));
    assert(!strcmp(card.values[PW_GAME_CARD_RESOLUTION], "1152x864"));
    assert(!strcmp(card.values[PW_GAME_CARD_BUTTON_FIRST], "space"));
    size_t length = pw_game_card_format(&card, text, sizeof(text));
    assert(length && pw_game_card_parse((const uint8_t *)text, length, &back) == PW_OK);
    assert(!memcmp(&card, &back, sizeof(card)));
    {
        PwGameProfile profile;
        assert(pw_game_profile_parse((const uint8_t *)APP, strlen(APP), &profile) == PW_OK);
        assert(pw_game_profile_apply_settings(&profile, (const uint8_t *)text, length) == PW_OK);
        assert(!strcmp(profile.input.preset, "mouselook") && profile.input.mode == PW_GAME_INPUT_KEYBOARD);
        assert(profile.input.mouse == PW_GAME_STICK_TOUCHPAD);
        assert(profile.display.width == 1152 && profile.display.height == 864);
        assert(!strcmp(profile.app.dll_overrides, "d3d8,d3d9=n;d3dx9_27=n,b"));
    }
    /* A hand-edited file: unknown lines dropped, a line outside a section refused. */
    {
        const char *edited = "[input]\nmode = keyboard\nrumble = 50\n[runtime]\nfast_clock = true\n";
        assert(pw_game_card_parse((const uint8_t *)edited, strlen(edited), &back) == PW_OK);
        assert(!strcmp(back.values[PW_GAME_CARD_CONTROLLER], "keyboard") && pw_game_card_changed(&back));
        assert(pw_game_card_parse((const uint8_t *)"mode = keyboard\n", 16, &back) == PW_ERR_MALFORMED);
    }
    /* The program: a name next to the game's own, applied by the runtime. */
    pw_game_card_clear(&card);
    assert(pw_game_card_set_text(&card, PW_GAME_CARD_PROGRAM, "setup.exe") && applies(&card) == PW_OK);
    assert(pw_game_card_set_text(&card, PW_GAME_CARD_PROGRAM, "Tools/Config.EXE") && applies(&card) == PW_OK);
    assert(!pw_game_card_set_text(&card, PW_GAME_CARD_PROGRAM, "..\\x.exe"));
    assert(!pw_game_card_set_text(&card, PW_GAME_CARD_PROGRAM, "C:\\x.exe"));
    assert(!pw_game_card_set_text(&card, PW_GAME_CARD_PROGRAM, "readme.txt"));
    assert(pw_game_card_set_text(&card, PW_GAME_CARD_PROGRAM, "") && !pw_game_card_changed(&card));
    for (size_t i = 0; i < pw_game_card_program_count(); i++)
        assert(pw_game_card_set_text(&card, PW_GAME_CARD_PROGRAM, pw_game_card_program_name(i)) &&
               applies(&card) == PW_OK);
    /* The overrides picker: on adds name=n,b, off takes only that name out. */
    pw_game_card_clear(&card);
    assert(pw_game_card_override_toggle(&card, "d3dx9_27") && pw_game_card_override_on(&card, "d3dx9_27"));
    assert(pw_game_card_override_toggle(&card, "dinput8"));
    assert(!strcmp(card.values[PW_GAME_CARD_OVERRIDES], "d3dx9_27=n,b;dinput8=n,b") && applies(&card) == PW_OK);
    assert(pw_game_card_override_toggle(&card, "d3dx9_27") && !pw_game_card_override_on(&card, "d3dx9_27"));
    assert(!strcmp(card.values[PW_GAME_CARD_OVERRIDES], "dinput8=n,b"));
    assert(pw_game_card_set_text(&card, PW_GAME_CARD_OVERRIDES, "A,B=n;dinput8=n,b"));
    assert(pw_game_card_override_on(&card, "a") && pw_game_card_override_toggle(&card, "a"));
    assert(!strcmp(card.values[PW_GAME_CARD_OVERRIDES], "B=n;dinput8=n,b"));
    for (size_t i = 0; i < pw_game_card_dll_count(); i++) {
        pw_game_card_clear(&card);
        assert(pw_game_card_override_toggle(&card, pw_game_card_dll_name(i)) && applies(&card) == PW_OK);
    }
    assert(pw_game_card_common_count() == 5 && pw_game_card_dll_count() > 40);
    assert(pw_game_card_preset_count() == 6 && !strcmp(pw_game_card_preset_name(3), "mouselook") &&
           !pw_game_card_preset_name(6));
    printf("game card passed: every choice taken by the runtime, written and read back, a preset over the profile\n");
    return 0;
}
