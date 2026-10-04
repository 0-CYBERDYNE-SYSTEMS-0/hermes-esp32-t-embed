#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool initialized;
    bool raw_down;
    bool stable_down;
    bool suppress_release;
    bool hold_fired;
    uint8_t press_context;
    uint32_t hold_ms;
    int64_t raw_changed_at_us;
    int64_t pressed_at_us;
} t_embed_gesture_state_t;

typedef struct {
    int detents;
    bool press_started;
    bool hold;
    bool released;
    bool short_click;
    bool down;
    uint8_t press_context;
    int64_t pressed_at_us;
} t_embed_gesture_event_t;

void t_embed_gesture_init(t_embed_gesture_state_t *state, bool initially_down,
                          int64_t now_us);
void t_embed_gesture_update(t_embed_gesture_state_t *state, bool raw_down,
                            int detents, int64_t now_us, uint32_t debounce_ms,
                            uint32_t hold_ms, uint8_t context,
                            t_embed_gesture_event_t *event);
unsigned t_embed_menu_move(unsigned selected, int detents, unsigned item_count);

#ifdef __cplusplus
}
#endif
