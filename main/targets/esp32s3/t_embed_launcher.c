#include "t_embed_apps.h"
#include "t_embed_ui.h"

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void app_main(void) {
    ESP_ERROR_CHECK(t_embed_ui_init(2437u) ? ESP_OK : ESP_FAIL);
    unsigned selection = 0;
    t_embed_ui_render_launcher(selection);
    for (;;) {
        bool activate = false;
        if (t_embed_ui_poll_launcher(&selection, &activate))
            t_embed_ui_render_launcher(selection);
        if (activate) {
            if (selection == 0u) t_embed_sdr_run();
            else if (selection == 1u) t_embed_microphone_run();
            else t_embed_hermes_run();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
