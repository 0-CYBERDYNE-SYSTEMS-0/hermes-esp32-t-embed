#include "t_embed_audio_input.h"

#include <stdbool.h>
#include <stdlib.h>

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "audio_codec_ctrl_if.h"
#include "audio_codec_data_if.h"
#include "audio_codec_if.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es7210_adc.h"
#include "freertos/FreeRTOS.h"

#define T_EMBED_AUDIO_SAMPLE_RATE 16000
#define T_EMBED_AUDIO_CHANNELS 2
#define T_EMBED_AUDIO_MAX_FRAMES 80
#define T_EMBED_AUDIO_I2C_PORT 0
#define T_EMBED_AUDIO_I2S_PORT I2S_NUM_0
#define T_EMBED_AUDIO_I2C_SDA GPIO_NUM_18
#define T_EMBED_AUDIO_I2C_SCL GPIO_NUM_8
#define T_EMBED_AUDIO_I2S_MCLK GPIO_NUM_48
#define T_EMBED_AUDIO_I2S_BCLK GPIO_NUM_47
#define T_EMBED_AUDIO_I2S_WS GPIO_NUM_21
#define T_EMBED_AUDIO_I2S_DIN GPIO_NUM_14

struct t_embed_audio_input {
    i2s_chan_handle_t rx_channel;
    i2c_master_bus_handle_t i2c_bus;
    esp_codec_dev_handle_t codec;
    const audio_codec_ctrl_if_t *control;
    const audio_codec_data_if_t *data;
    const audio_codec_if_t *adc;
    bool codec_open;
};

static void t_embed_audio_input_release(t_embed_audio_input_t *input) {
    if (!input) return;
    if (input->codec_open) esp_codec_dev_close(input->codec);
    if (input->codec) esp_codec_dev_delete(input->codec);
    if (input->adc) audio_codec_delete_codec_if(input->adc);
    if (input->data) audio_codec_delete_data_if(input->data);
    if (input->control) audio_codec_delete_ctrl_if(input->control);
    if (input->rx_channel) {
        (void)i2s_channel_disable(input->rx_channel);
        (void)i2s_del_channel(input->rx_channel);
    }
    if (input->i2c_bus) (void)i2c_del_master_bus(input->i2c_bus);
    free(input);
}

esp_err_t t_embed_audio_input_create(t_embed_audio_input_t **out) {
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;

    t_embed_audio_input_t *input = calloc(1, sizeof(*input));
    if (!input) return ESP_ERR_NO_MEM;

    i2c_master_bus_config_t bus_config = {
        .i2c_port = T_EMBED_AUDIO_I2C_PORT,
        .sda_io_num = T_EMBED_AUDIO_I2C_SDA,
        .scl_io_num = T_EMBED_AUDIO_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t result = i2c_new_master_bus(&bus_config, &input->i2c_bus);
    if (result != ESP_OK) goto fail;

    i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(T_EMBED_AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    result = i2s_new_channel(&channel_config, NULL, &input->rx_channel);
    if (result != ESP_OK) goto fail;

    i2s_std_config_t standard_config = {
        .clk_cfg = {
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .sample_rate_hz = T_EMBED_AUDIO_SAMPLE_RATE,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = T_EMBED_AUDIO_I2S_MCLK,
            .bclk = T_EMBED_AUDIO_I2S_BCLK,
            .ws = T_EMBED_AUDIO_I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = T_EMBED_AUDIO_I2S_DIN,
        },
    };
    result = i2s_channel_init_std_mode(input->rx_channel, &standard_config);
    if (result != ESP_OK) goto fail;

    audio_codec_i2c_cfg_t i2c_config = {
        .port = T_EMBED_AUDIO_I2C_PORT,
        .addr = ES7210_CODEC_DEFAULT_ADDR,
        .bus_handle = input->i2c_bus,
    };
    input->control = audio_codec_new_i2c_ctrl(&i2c_config);
    if (!input->control) {
        result = ESP_ERR_NO_MEM;
        goto fail;
    }

    audio_codec_i2s_cfg_t data_config = {
        .port = T_EMBED_AUDIO_I2S_PORT,
        .rx_handle = input->rx_channel,
        .tx_handle = NULL,
    };
    input->data = audio_codec_new_i2s_data(&data_config);
    if (!input->data) {
        result = ESP_ERR_NO_MEM;
        goto fail;
    }

    es7210_codec_cfg_t adc_config = {
        .ctrl_if = input->control,
        .master_mode = false,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
        .mclk_src = ES7210_MCLK_FROM_PAD,
        .mclk_div = I2S_MCLK_MULTIPLE_256,
    };
    input->adc = es7210_codec_new(&adc_config);
    if (!input->adc) {
        result = ESP_ERR_NO_MEM;
        goto fail;
    }

    esp_codec_dev_cfg_t device_config = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = input->adc,
        .data_if = input->data,
    };
    input->codec = esp_codec_dev_new(&device_config);
    if (!input->codec) {
        result = ESP_ERR_NO_MEM;
        goto fail;
    }

    esp_codec_dev_sample_info_t sample_config = {
        .bits_per_sample = I2S_DATA_BIT_WIDTH_16BIT,
        .channel = T_EMBED_AUDIO_CHANNELS,
        .channel_mask = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
        .sample_rate = T_EMBED_AUDIO_SAMPLE_RATE,
    };
    input->codec_open = true;
    result = esp_codec_dev_open(input->codec, &sample_config);
    if (result != ESP_OK) goto fail;

    result = esp_codec_dev_set_in_gain(input->codec, 24);
    if (result != ESP_OK) goto fail;

    *out = input;
    return ESP_OK;

fail:
    t_embed_audio_input_release(input);
    return result;
}

void t_embed_audio_input_destroy(t_embed_audio_input_t *input) {
    t_embed_audio_input_release(input);
}

esp_err_t t_embed_audio_input_read_mono(t_embed_audio_input_t *input,
                                         int16_t *samples,
                                         size_t frame_capacity,
                                         size_t *frames_read,
                                         uint32_t timeout_ms) {
    if (!input || !samples || !frame_capacity || !frames_read)
        return ESP_ERR_INVALID_ARG;
    *frames_read = 0;

    size_t frames = frame_capacity;
    if (frames > T_EMBED_AUDIO_MAX_FRAMES) frames = T_EMBED_AUDIO_MAX_FRAMES;
    int16_t stereo[T_EMBED_AUDIO_MAX_FRAMES * T_EMBED_AUDIO_CHANNELS];
    size_t bytes_read = 0;
    esp_err_t result = i2s_channel_read(input->rx_channel, stereo,
                                         frames * T_EMBED_AUDIO_CHANNELS * sizeof(int16_t),
                                         &bytes_read, pdMS_TO_TICKS(timeout_ms));
    if (result != ESP_OK) return result;

    size_t frames_captured = bytes_read /
        (T_EMBED_AUDIO_CHANNELS * sizeof(int16_t));
    for (size_t frame = 0; frame < frames_captured; ++frame) {
        int32_t mixed = (int32_t)stereo[frame * 2] + stereo[frame * 2 + 1];
        samples[frame] = (int16_t)(mixed / T_EMBED_AUDIO_CHANNELS);
    }
    *frames_read = frames_captured;
    return ESP_OK;
}
