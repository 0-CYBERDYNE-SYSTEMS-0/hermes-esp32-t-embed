#include "t_embed_gesture.h"

#include <string.h>

void t_embed_gesture_init(t_embed_gesture_state_t *state, bool initially_down,
                          int64_t now_us) {
    if (!state) return;
    memset(state, 0, sizeof(*state));
    state->initialized = true;
    state->raw_down = initially_down;
    state->stable_down = initially_down;
    state->suppress_release = initially_down;
    state->raw_changed_at_us = now_us;
}

void t_embed_gesture_update(t_embed_gesture_state_t *state, bool raw_down,
                            int detents, int64_t now_us, uint32_t debounce_ms,
                            uint32_t hold_ms, uint8_t context,
                            t_embed_gesture_event_t *event) {
    if (!state || !event) return;
    memset(event, 0, sizeof(*event));
    event->detents = detents;
    if (!state->initialized) t_embed_gesture_init(state, raw_down, now_us);
    if (raw_down != state->raw_down) {
        state->raw_down = raw_down;
        state->raw_changed_at_us = now_us;
        if (raw_down) {
            state->pressed_at_us = now_us;
            state->press_context = context;
            state->hold_ms = hold_ms;
            state->hold_fired = false;
        }
    }

    int64_t debounce_us = (int64_t)debounce_ms * 1000;
    if (state->raw_down != state->stable_down &&
        now_us - state->raw_changed_at_us >= debounce_us) {
        state->stable_down = state->raw_down;
        event->press_context = state->press_context;
        event->pressed_at_us = state->pressed_at_us;
        if (state->stable_down) {
            event->press_started = true;
        } else {
            event->released = true;
            bool below_hold_threshold = state->hold_ms == 0u ||
                state->raw_changed_at_us - state->pressed_at_us <
                    (int64_t)state->hold_ms * 1000;
            event->short_click = !state->hold_fired &&
                !state->suppress_release && below_hold_threshold;
            state->suppress_release = false;
            state->hold_fired = false;
        }
    }

    if (state->stable_down && state->raw_down && !state->hold_fired &&
        state->hold_ms > 0u &&
        now_us - state->pressed_at_us >= (int64_t)state->hold_ms * 1000) {
        state->hold_fired = true;
        event->hold = true;
        event->press_context = state->press_context;
        event->pressed_at_us = state->pressed_at_us;
    }
    event->down = state->stable_down;
    if (!event->press_context) event->press_context = state->press_context;
    if (!event->pressed_at_us) event->pressed_at_us = state->pressed_at_us;
}

void t_embed_gesture_suppress_until_release(t_embed_gesture_state_t *state) {
    if (state && (state->raw_down || state->stable_down)) state->suppress_release = true;
}

unsigned t_embed_menu_move(unsigned selected, int detents, unsigned item_count) {
    if (!item_count) return 0;
    int64_t position = (int64_t)(selected % item_count) + detents;
    int64_t count = item_count;
    position %= count;
    if (position < 0) position += count;
    return (unsigned)position;
}
