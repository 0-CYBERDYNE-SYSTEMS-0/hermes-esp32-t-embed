#include "t_embed_apps.h"
#include "t_embed_audio_input.h"
#include "t_embed_ui.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MIC_FRAMES_PER_PACKET 80
#define MIC_PACKET_HEADER_SIZE 8
#define MIC_PACKET_DATA_SIZE (MIC_PACKET_HEADER_SIZE + MIC_FRAMES_PER_PACKET * sizeof(int16_t))
#define MIC_PACKET_SIZE (MIC_PACKET_DATA_SIZE + sizeof(uint16_t))
#define MIC_BRIDGE_TIMEOUT_US 2500000
#define MIC_UI_UPDATE_US 100000

typedef struct {
    char line[8];
    unsigned length;
    bool active;
    int64_t last_ping_us;
    uint32_t sequence;
} mic_bridge_t;

static void microphone_serial_init(void) {
    if (usb_serial_jtag_is_driver_installed()) return;
    usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&config));
}

static void mic_bridge_read_commands(mic_bridge_t *bridge, int64_t now_us) {
    uint8_t input[16];
    int count = usb_serial_jtag_read_bytes(input, sizeof(input), 0);
    for (int i = 0; i < count; i++) {
        char value = (char)input[i];
        if (value == '\r') continue;
        if (value == '\n') {
            bridge->line[bridge->length] = '\0';
            if (strcmp(bridge->line, "START") == 0 ||
                strcmp(bridge->line, "PING") == 0) {
                bridge->active = true;
                bridge->last_ping_us = now_us;
            } else if (strcmp(bridge->line, "STOP") == 0) {
                bridge->active = false;
            }
            bridge->length = 0;
        } else if (bridge->length < sizeof(bridge->line) - 1u) {
            bridge->line[bridge->length++] = value;
        } else {
            bridge->length = 0;
        }
    }
    if (bridge->active && now_us - bridge->last_ping_us > MIC_BRIDGE_TIMEOUT_US)
        bridge->active = false;
}

static void mic_bridge_send(mic_bridge_t *bridge, const int16_t *samples, bool muted) {
    uint8_t packet[MIC_PACKET_SIZE];
    packet[0] = 'T';
    packet[1] = 'M';
    packet[2] = 'I';
    packet[3] = 'C';
    packet[4] = (uint8_t)bridge->sequence;
    packet[5] = (uint8_t)(bridge->sequence >> 8);
    packet[6] = (uint8_t)(bridge->sequence >> 16);
    packet[7] = (uint8_t)(bridge->sequence >> 24);
    if (muted) {
        memset(packet + MIC_PACKET_HEADER_SIZE, 0,
               MIC_FRAMES_PER_PACKET * sizeof(int16_t));
    } else {
        memcpy(packet + MIC_PACKET_HEADER_SIZE, samples,
               MIC_FRAMES_PER_PACKET * sizeof(int16_t));
    }
    uint16_t crc = 0xffffu;
    for (unsigned i = 0; i < MIC_PACKET_DATA_SIZE; i++) {
        crc ^= (uint16_t)packet[i] << 8;
        for (unsigned bit = 0; bit < 8u; bit++)
            crc = (uint16_t)((crc & 0x8000u) ? (crc << 1) ^ 0x1021u : crc << 1);
    }
    packet[MIC_PACKET_DATA_SIZE] = (uint8_t)crc;
    packet[MIC_PACKET_DATA_SIZE + 1u] = (uint8_t)(crc >> 8);
    bridge->sequence++;
    (void)usb_serial_jtag_write_bytes(packet, sizeof(packet), pdMS_TO_TICKS(1));
}

void t_embed_microphone_run(void) {
    esp_log_level_set("*", ESP_LOG_NONE);
    bool muted = false;
    t_embed_ui_render_microphone(0, false, muted);
    microphone_serial_init();
    t_embed_audio_input_t *audio_input = NULL;
    ESP_ERROR_CHECK(t_embed_audio_input_create(&audio_input));
    mic_bridge_t bridge = {0};
    int16_t mono[MIC_FRAMES_PER_PACKET];
    unsigned peak = 0;
    int64_t last_ui_update = esp_timer_get_time();

    for (;;) {
        int64_t now_us = esp_timer_get_time();
        mic_bridge_read_commands(&bridge, now_us);
        if (t_embed_ui_poll_microphone_mute()) {
            muted = !muted;
            t_embed_ui_set_microphone_leds(true, muted);
        }
        if (t_embed_ui_home_requested()) {
            t_embed_ui_set_microphone_leds(false, false);
            esp_restart();
        }

        size_t frames_read = 0;
        esp_err_t result = t_embed_audio_input_read_mono(
            audio_input, mono, MIC_FRAMES_PER_PACKET, &frames_read, 20);
        if (result != ESP_OK || frames_read != MIC_FRAMES_PER_PACKET) continue;

        for (unsigned frame = 0; frame < MIC_FRAMES_PER_PACKET; frame++) {
            unsigned magnitude = mono[frame] < 0 ? (unsigned)-mono[frame] : (unsigned)mono[frame];
            if (magnitude > peak) peak = magnitude;
        }

        now_us = esp_timer_get_time();
        mic_bridge_read_commands(&bridge, now_us);
        if (bridge.active) mic_bridge_send(&bridge, mono, muted);
        if (now_us - last_ui_update >= MIC_UI_UPDATE_US) {
            t_embed_ui_render_microphone(muted ? 0u :
                                         (peak / 128u > 255u ? 255u : peak / 128u),
                                         bridge.active, muted);
            peak = 0;
            last_ui_update = now_us;
        }
    }
}
