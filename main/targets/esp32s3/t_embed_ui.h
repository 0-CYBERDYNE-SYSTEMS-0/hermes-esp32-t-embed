#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    T_EMBED_INPUT_LAUNCHER = 1,
    T_EMBED_INPUT_HERMES_READY,
    T_EMBED_INPUT_HERMES_ACTIVE,
    T_EMBED_INPUT_HERMES_LISTENING,
    T_EMBED_INPUT_HERMES_PROMPT,
    T_EMBED_INPUT_HERMES_MENU,
    T_EMBED_INPUT_HERMES_OFFLINE
} t_embed_input_context_t;

typedef struct {
    int detents;
    bool press_started;
    bool hold;
    bool released;
    bool short_click;
    bool down;
    t_embed_input_context_t press_context;
    int64_t pressed_at_us;
} t_embed_input_event_t;

/* The standard T-Embed ESP32-S3 board: ST7789V (170x320) in landscape,
 * rotary GPIO 2/1 and the GPIO0 encoder/BOOT switch. */
bool t_embed_ui_init(void);
void t_embed_ui_render_launcher(unsigned selection);
bool t_embed_ui_poll_launcher(unsigned *selection, bool *activate);
void t_embed_ui_poll_input(t_embed_input_context_t context, uint32_t hold_ms,
                           t_embed_input_event_t *event);
void t_embed_ui_render_menu_overlay(const char *title, const char *const *items,
                                    unsigned count, unsigned selected,
                                    const char *hint);
void t_embed_ui_render_prompt_overlay(const char *title, const char *text,
                                      unsigned scroll, bool answer_view,
                                      unsigned selected, unsigned outcome);
unsigned t_embed_ui_prompt_max_scroll(const char *text);
void t_embed_ui_invalidate_overlays(void);
void t_embed_ui_set_frame_flush_suppressed(bool suppressed);
void t_embed_ui_display_flush(const uint16_t *pixels, unsigned y0, unsigned y1);
void t_embed_ui_flush_framebuffer(const uint16_t *pixels, unsigned y0, unsigned y1);

#ifdef __cplusplus
}
#endif
