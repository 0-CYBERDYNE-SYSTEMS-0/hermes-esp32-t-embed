#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct t_embed_audio_input t_embed_audio_input_t;

// Creates the T-Embed's 16 kHz, two-channel ES7210 capture path on I2S0.
esp_err_t t_embed_audio_input_create(t_embed_audio_input_t **out);
void t_embed_audio_input_destroy(t_embed_audio_input_t *input);

// Reads and averages up to 80 interleaved stereo frames into mono PCM16.
// frames_read is set to the number of mono samples returned.
esp_err_t t_embed_audio_input_read_mono(t_embed_audio_input_t *input,
                                         int16_t *samples,
                                         size_t frame_capacity,
                                         size_t *frames_read,
                                         uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
