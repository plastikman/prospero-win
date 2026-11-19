/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_INPUT_SWITCH_H
#define PW_INPUT_SWITCH_H
#include <stdint.h>

enum { PW_INPUT_SWITCH_HOLD_US = 2000000, PW_INPUT_SWITCH_TAP_US = 96000 };
typedef struct PwInputSwitch {
    uint32_t button;
    int down, fired, left;
    uint32_t held_us, tap_us;
    int tap_left;
} PwInputSwitch;
typedef struct PwInputSwitchView {
    uint32_t held, pressed, released;
    int left, switch_now;
} PwInputSwitchView;
void pw_input_switch_init(PwInputSwitch *input_switch, uint32_t button);
void pw_input_switch_tick(PwInputSwitch *input_switch, uint32_t held, uint32_t pressed, uint32_t released,
                          int left, uint32_t tick_us, PwInputSwitchView *view);
int pw_input_switch_cancel(PwInputSwitch *input_switch);
#endif
