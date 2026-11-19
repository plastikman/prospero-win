/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_input_switch.h"
#include <string.h>

void pw_input_switch_init(PwInputSwitch *input_switch, uint32_t button)
{
    memset(input_switch, 0, sizeof(*input_switch));
    input_switch->button = button;
}

int pw_input_switch_cancel(PwInputSwitch *s)
{
    const int pending = s->tap_us != 0;
    s->down = 0;
    s->fired = 0;
    s->tap_us = 0;
    return pending;
}

void pw_input_switch_tick(PwInputSwitch *s, uint32_t held, uint32_t pressed, uint32_t released,
                          int left, uint32_t tick_us, PwInputSwitchView *view)
{
    const uint32_t button = s->button;
    view->held = held & ~button;
    view->pressed = pressed & ~button;
    view->released = released & ~button;
    view->left = left;
    view->switch_now = 0;
    if (s->tap_us) {
        if ((pressed & button) || s->tap_us <= tick_us) {
            s->tap_us = 0;
            view->released |= button;
        } else {
            s->tap_us -= tick_us;
            view->held |= button;
            view->left = s->tap_left;
        }
    }
    if (pressed & button) {
        s->down = 1;
        s->fired = 0;
        s->held_us = 0;
        s->left = left;
    } else if (s->down) {
        s->held_us = s->held_us > PW_INPUT_SWITCH_HOLD_US - tick_us ? PW_INPUT_SWITCH_HOLD_US : s->held_us + tick_us;
    }
    if (!s->down) return;
    if (released & button) {
        s->down = 0;
        if (!s->fired) {
            s->tap_us = PW_INPUT_SWITCH_TAP_US;
            s->tap_left = s->left;
            view->pressed |= button;
            view->held |= button;
            view->left = s->left;
        }
        return;
    }
    if (!(held & button)) {
        s->down = 0;
        return;
    }
    if (!s->fired && s->held_us >= PW_INPUT_SWITCH_HOLD_US) {
        s->fired = 1;
        view->switch_now = 1;
    }
}
