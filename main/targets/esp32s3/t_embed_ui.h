#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ring_capture.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned frequency_mhz;
    unsigned rate_code; /* 0 = 80, 1 = 40, 6 = 16 Msps */
    unsigned step_mhz;
    bool paused;
    bool usb_transport;
} t_embed_ui_view_t;

typedef struct {
    bool ready;
    bool paused;
    unsigned frequency_mhz;
    unsigned rate_code;
    unsigned step_mhz;
    unsigned nfft;
    uint32_t screen_frames;
    uint32_t rf_frames;
    uint32_t ffts;
    uint32_t bin_checksum;
    uint32_t frame_age_ms;
    uint32_t capture_us;
    uint32_t late_max;
    uint32_t drops;
    uint32_t failures;
    uint8_t bin_min;
    uint8_t bin_max;
} t_embed_ui_stats_t;

typedef enum {
    T_EMBED_INPUT_LAUNCHER = 1,
    T_EMBED_INPUT_SDR,
    T_EMBED_INPUT_SDR_MENU,
    T_EMBED_INPUT_MICROPHONE,
    T_EMBED_INPUT_MICROPHONE_MENU,
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
bool t_embed_ui_init(unsigned initial_frequency_mhz);
void t_embed_ui_render_launcher(unsigned selection);
bool t_embed_ui_poll_launcher(unsigned *selection, bool *activate);
void t_embed_ui_poll_input(t_embed_input_context_t context, uint32_t hold_ms,
                           t_embed_input_event_t *event);
bool t_embed_ui_poll_microphone_mute(void);
void t_embed_ui_set_microphone_leds(bool enabled, bool muted);
void t_embed_ui_render_microphone(unsigned level, bool bridge_connected, bool muted);
bool t_embed_ui_home_requested(void);
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
/* Polls the quadrature encoder and keys. Returns true when view state changes. */
bool t_embed_ui_poll_controls(unsigned *frequency_mhz);
bool t_embed_ui_capture_due(int64_t now_us);
unsigned t_embed_ui_rate_code(void);
bool t_embed_ui_paused(void);
void t_embed_ui_note_capture(const ring_result_t *result, unsigned nfft,
                             unsigned frequency_mhz, unsigned rate_code);
void t_embed_ui_render_view(const t_embed_ui_view_t *view);
void t_embed_ui_render_spectrum(const uint8_t *bins, unsigned nfft);
void t_embed_ui_get_stats(t_embed_ui_stats_t *stats);

#ifdef __cplusplus
}
#endif
