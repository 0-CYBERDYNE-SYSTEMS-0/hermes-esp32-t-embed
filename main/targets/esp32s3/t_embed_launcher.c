#include "t_embed_apps.h"
#include "t_embed_ui.h"

#include "driver/usb_serial_jtag.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#ifndef HUNT_BUILD_TAG
#define HUNT_BUILD_TAG "local"
#endif

/* Machine interface for the launcher. The knob stays the primary control, but
 * a bench or CI rig must be able to select an app without a human finger:
 * every app then exposes its own serial CLI, so the whole device is drivable
 * from the host. Commands are newline-terminated ASCII, replies are one line.
 *
 *   PING            -> PONG
 *   INFO            -> TEMBED launcher <build-tag>
 *   APP SDR|1|0     -> launches the ESP-SDR app  (reply: APP SDR)
 *   APP MIC|2       -> launches the microphone app
 *   APP HERMES|3    -> launches the Hermes app
 *
 * Launching never returns (the apps own the device), so the reply is written
 * before the USB driver is handed over to the app.
 */
static bool console_ready;

static bool console_init(void) {
    if (usb_serial_jtag_is_driver_installed()) return true;
    usb_serial_jtag_driver_config_t config = {
        .tx_buffer_size = 512,
        .rx_buffer_size = 256,
    };
    return usb_serial_jtag_driver_install(&config) == ESP_OK;
}

static void console_reply(const char *text) {
    (void)usb_serial_jtag_write_bytes(text, strlen(text), pdMS_TO_TICKS(50));
}

/* Returns the app index to launch, or -1 when the line is not a launch. */
static int console_parse(const char *line) {
    if (!strcmp(line, "PING")) {
        console_reply("PONG\n");
        return -1;
    }
    if (!strcmp(line, "INFO") || !strcmp(line, "BOOT?")) {
        char reply[64];
        snprintf(reply, sizeof(reply), "TEMBED launcher %s\n", HUNT_BUILD_TAG);
        console_reply(reply);
        return -1;
    }
    static const char *const names[3] = {"SDR", "MIC", "HERMES"};
    unsigned index;
    if (!strncmp(line, "APP ", 4)) {
        const char *arg = line + 4;
        for (index = 0; index < 3; index++) {
            if (!strcasecmp(arg, names[index])) break;
        }
        if (index == 3) {
            if (!strcmp(arg, "0")) index = 0;
            else if (!strcmp(arg, "1")) index = 1;
            else if (!strcmp(arg, "2")) index = 2;
            else {
                console_reply("ERR app\n");
                return -1;
            }
        }
        char reply[24];
        snprintf(reply, sizeof(reply), "APP %s\n", names[index]);
        console_reply(reply);
        return (int)index;
    }
    console_reply("ERR command\n");
    return -1;
}

static int console_poll(void) {
    static char line[40];
    static size_t used;
    uint8_t chunk[32];
    int count = usb_serial_jtag_read_bytes(chunk, sizeof(chunk), 0);
    for (int i = 0; i < count; i++) {
        char c = (char)chunk[i];
        if (c == '\r') continue;
        if (c == '\n') {
            line[used] = '\0';
            used = 0;
            if (!line[0]) continue;
            int launch = console_parse(line);
            if (launch >= 0) return launch;
            continue;
        }
        if (used + 1u < sizeof(line)) line[used++] = c;
        else used = 0; /* overlong line: drop it rather than truncate a command */
    }
    return -1;
}

static void console_hand_over(void) {
    if (!console_ready) return;
    console_ready = false;
    (void)usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(100));
    /* Every app installs its own USB Serial/JTAG driver; leaving ours in place
     * makes the app's install fail. */
    (void)usb_serial_jtag_driver_uninstall();
}

void app_main(void) {
    ESP_ERROR_CHECK(t_embed_ui_init(2437u) ? ESP_OK : ESP_FAIL);
    console_ready = console_init();
    unsigned selection = 0;
    t_embed_ui_render_launcher(selection);
    for (;;) {
        bool activate = false;
        if (console_ready) {
            int launch = console_poll();
            if (launch >= 0) {
                selection = (unsigned)launch;
                activate = true;
                t_embed_ui_render_launcher(selection);
            }
        }
        if (t_embed_ui_poll_launcher(&selection, &activate)) {
            t_embed_ui_render_launcher(selection);
        }
        if (activate) {
            console_hand_over();
            if (selection == 0u) t_embed_sdr_run();
            else if (selection == 1u) t_embed_microphone_run();
            else t_embed_hermes_run();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
