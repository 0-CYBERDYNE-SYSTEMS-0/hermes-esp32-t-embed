#include "t_embed_apps.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hg/app.hpp"
#include "hg/protocol.hpp"
#include "nvs.h"
#include "nvs_flash.h"
#include "t_embed_hermes_audio.h"
#include "t_embed_hermes_transport.h"
#include "t_embed_gesture.h"
#include "t_embed_ui.h"

namespace {

constexpr char kTag[] = "t_embed_hermes";
constexpr size_t kUsbLineBytes = 512;
constexpr int64_t kPromptArmUs = 600000;
constexpr int64_t kNoExpiry = 0;
constexpr int kNetworkBackoffMs[] = {1000, 2000, 4000, 8000, 15000, 30000};

void secure_zero(void *memory, size_t size) {
    volatile uint8_t *bytes = static_cast<volatile uint8_t *>(memory);
    while (size--) *bytes++ = 0;
}

void wipe(std::string &value) {
    secure_zero(value.data(), value.size());
    value.clear();
}

class HermesSystem final : public hg::System {
public:
    uint32_t now_ms() override {
        return static_cast<uint32_t>(esp_timer_get_time() / 1000);
    }

    void random_bytes(uint8_t *out, size_t len) override {
        esp_fill_random(out, len);
    }

    void log(hg::LogLevel level, std::string_view message) override {
        std::string line(message);
        switch (level) {
            case hg::LogLevel::Debug: ESP_LOGD(kTag, "%s", line.c_str()); break;
            case hg::LogLevel::Info: ESP_LOGI(kTag, "%s", line.c_str()); break;
            case hg::LogLevel::Warn: ESP_LOGW(kTag, "%s", line.c_str()); break;
            case hg::LogLevel::Error: ESP_LOGE(kTag, "%s", line.c_str()); break;
        }
    }
};

class HermesStorage final : public hg::Storage {
public:
    bool initialize() {
        last_error_ = nvs_flash_init();
        ready_ = last_error_ == ESP_OK;
        if (!ready_) ESP_LOGE(kTag, "NVS unavailable: %s; storage was not erased", esp_err_to_name(last_error_));
        return ready_;
    }

    bool ready() const { return ready_; }
    esp_err_t last_error() const { return last_error_; }

    std::optional<std::string> get(std::string_view key) override {
        nvs_handle_t handle;
        std::string name(key);
        esp_err_t error = open(name, NVS_READONLY, &handle);
        if (error != ESP_OK) return std::nullopt;

        size_t size = 0;
        error = nvs_get_str(handle, name.c_str(), nullptr, &size);
        if (error == ESP_ERR_NVS_NOT_FOUND) {
            last_error_ = ESP_OK;
            nvs_close(handle);
            return std::nullopt;
        }
        if (error != ESP_OK || size == 0 || size > 1024) {
            if (error == ESP_OK) error = ESP_ERR_INVALID_SIZE;
            last_error_ = error;
            nvs_close(handle);
            return std::nullopt;
        }

        std::string value(size, '\0');
        error = nvs_get_str(handle, name.c_str(), value.data(), &size);
        nvs_close(handle);
        if (error != ESP_OK) {
            last_error_ = error;
            return std::nullopt;
        }
        value.resize(size ? size - 1 : 0);
        last_error_ = ESP_OK;
        return value;
    }

    void set(std::string_view key, std::string_view value) override {
        nvs_handle_t handle;
        std::string name(key);
        std::string stored(value);
        esp_err_t error = open(name, NVS_READWRITE, &handle);
        if (error != ESP_OK) {
            wipe(stored);
            return;
        }
        error = nvs_set_str(handle, name.c_str(), stored.c_str());
        if (error == ESP_OK) error = nvs_commit(handle);
        nvs_close(handle);
        wipe(stored);
        last_error_ = error;
    }

    bool set_wifi(std::string_view ssid, std::string_view password) {
        std::string ssid_copy(ssid);
        std::string password_copy(password);
        nvs_handle_t handle;
        esp_err_t error = open("wifi_ssid", NVS_READWRITE, &handle);
        if (error == ESP_OK) {
            error = nvs_set_str(handle, "wifi_ssid", ssid_copy.c_str());
            if (error == ESP_OK) error = nvs_set_str(handle, "wifi_pass", password_copy.c_str());
            if (error == ESP_OK) error = nvs_commit(handle);
            nvs_close(handle);
        }
        wipe(ssid_copy);
        wipe(password_copy);
        last_error_ = error;
        return error == ESP_OK;
    }

    void erase(std::string_view key) override {
        nvs_handle_t handle;
        std::string name(key);
        esp_err_t error = open(name, NVS_READWRITE, &handle);
        if (error != ESP_OK) return;
        error = nvs_erase_key(handle, name.c_str());
        if (error == ESP_ERR_NVS_NOT_FOUND) error = ESP_OK;
        if (error == ESP_OK) error = nvs_commit(handle);
        nvs_close(handle);
        last_error_ = error;
    }

private:
    esp_err_t open(const std::string &name, nvs_open_mode_t mode, nvs_handle_t *handle) {
        if (!ready_) {
            last_error_ = ESP_ERR_INVALID_STATE;
            return last_error_;
        }
        if (name.empty() || name.size() > 15) {
            last_error_ = ESP_ERR_NVS_INVALID_NAME;
            return last_error_;
        }
        esp_err_t error = nvs_open("hgadget", mode, handle);
        if (error != ESP_OK) last_error_ = error;
        return error;
    }

    bool ready_ = false;
    esp_err_t last_error_ = ESP_ERR_INVALID_STATE;
};

class HermesDisplay final : public hg::Display {
public:
    HermesDisplay() {
        pixels_ = static_cast<uint16_t *>(heap_caps_calloc(
            320u * 170u, sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }

    ~HermesDisplay() override { heap_caps_free(pixels_); }

    bool ready() const { return pixels_ != nullptr; }
    hg::DisplayInfo info() const override {
        return {320, 170, true, false, false};
    }
    uint16_t *framebuffer() override { return pixels_; }
    void flush(uint16_t y0, uint16_t y1) override {
        t_embed_ui_display_flush(pixels_, y0, y1);
    }

private:
    uint16_t *pixels_ = nullptr;
};

struct NetworkEvent {
    bool up;
    uint16_t reason;
    int8_t rssi;
    char detail[48];
};

class HermesWifi {
public:
    explicit HermesWifi(QueueHandle_t events) : events_(events) {}

    bool start(std::string_view ssid, std::string_view password) {
        if (ssid.empty() || ssid.size() > 32 || password.size() > 63) {
            last_error_ = ESP_ERR_INVALID_ARG;
            return false;
        }

        esp_err_t error = esp_netif_init();
        if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return fail(error);
        error = esp_event_loop_create_default();
        if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return fail(error);
        netif_ = esp_netif_create_default_wifi_sta();
        if (!netif_) return fail(ESP_ERR_NO_MEM);

        wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
        error = esp_wifi_init(&init);
        if (error != ESP_OK) return fail(error);
        error = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
            &HermesWifi::event_handler, this, &wifi_handler_);
        if (error != ESP_OK) return fail(error);
        error = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
            &HermesWifi::event_handler, this, &ip_handler_);
        if (error != ESP_OK) return fail(error);

        wifi_config_t config{};
        std::memcpy(config.sta.ssid, ssid.data(), ssid.size());
        std::memcpy(config.sta.password, password.data(), password.size());
        error = esp_wifi_set_storage(WIFI_STORAGE_RAM);
        if (error == ESP_OK) error = esp_wifi_set_mode(WIFI_MODE_STA);
        if (error == ESP_OK) error = esp_wifi_set_config(WIFI_IF_STA, &config);
        secure_zero(config.sta.password, sizeof(config.sta.password));
        if (error == ESP_OK) error = esp_wifi_start();
        if (error == ESP_OK) error = esp_wifi_set_ps(WIFI_PS_NONE);
        if (error == ESP_OK) error = esp_wifi_connect();
        if (error != ESP_OK) return fail(error);

        initialized_ = true;
        return true;
    }

    bool initialized() const { return initialized_; }
    esp_err_t last_error() const { return last_error_; }

    bool connect_retry_due(uint32_t now) const {
        return initialized_ && retry_at_ms_ != 0 &&
               static_cast<int32_t>(now - retry_at_ms_) >= 0;
    }

    void retry(uint32_t now) {
        if (!initialized_) return;
        esp_err_t error = esp_wifi_connect();
        if (error != ESP_OK) ESP_LOGW(kTag, "Wi-Fi reconnect request failed: %s", esp_err_to_name(error));
        schedule_retry(now);
    }

    void connected() {
        backoff_index_ = 0;
        retry_at_ms_ = 0;
    }

    void disconnected(uint32_t now) { schedule_retry(now); }

private:
    static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
        auto *self = static_cast<HermesWifi *>(arg);
        if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
            auto *event = static_cast<wifi_event_sta_disconnected_t *>(data);
            uint16_t reason = event ? event->reason : 0;
            char detail[48];
            int8_t rssi = event ? event->rssi : 0;
            std::snprintf(detail, sizeof(detail), "Wi-Fi disconnected (reason %u, %d dBm)",
                          static_cast<unsigned>(reason), static_cast<int>(rssi));
            self->enqueue(false, detail, reason, rssi);
        } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
            self->enqueue(true, "Wi-Fi connected");
        }
    }

    void enqueue(bool up, const char *detail, uint16_t reason = 0, int8_t rssi = 0) {
        if (!events_) return;
        NetworkEvent event{};
        event.up = up;
        event.reason = reason;
        event.rssi = rssi;
        std::snprintf(event.detail, sizeof(event.detail), "%s", detail);
        (void)xQueueSend(events_, &event, 0);
    }

    bool fail(esp_err_t error) {
        last_error_ = error;
        ESP_LOGE(kTag, "Wi-Fi initialization failed: %s", esp_err_to_name(error));
        return false;
    }

    void schedule_retry(uint32_t now) {
        const size_t last = sizeof(kNetworkBackoffMs) / sizeof(kNetworkBackoffMs[0]) - 1;
        size_t index = std::min(backoff_index_, last);
        retry_at_ms_ = now + static_cast<uint32_t>(kNetworkBackoffMs[index]);
        if (backoff_index_ < last) backoff_index_++;
    }

    QueueHandle_t events_ = nullptr;
    esp_netif_t *netif_ = nullptr;
    esp_event_handler_instance_t wifi_handler_ = nullptr;
    esp_event_handler_instance_t ip_handler_ = nullptr;
    uint32_t retry_at_ms_ = 0;
    size_t backoff_index_ = 0;
    esp_err_t last_error_ = ESP_OK;
    bool initialized_ = false;
};

enum class LocalView : uint8_t { None, ActionMenu, NewSessionConfirm, Volume, Prompt };

struct PromptGuard {
    bool active = false;
    bool expired = false;
    bool answered = false;
    bool send_failed = false;
    int64_t opened_at_us = 0;
    int64_t expires_at_us = kNoExpiry;
    std::string id;
    std::string last_answered_id;
};

bool parse_quoted(std::string_view line, size_t &cursor, size_t max_bytes,
                  std::string &value) {
    while (cursor < line.size() && (line[cursor] == ' ' || line[cursor] == '\t')) cursor++;
    if (cursor >= line.size() || line[cursor++] != '"') return false;
    value.clear();
    bool closed = false;
    while (cursor < line.size()) {
        char ch = line[cursor++];
        if (ch == '"') { closed = true; break; }
        if (ch == '\\') {
            if (cursor >= line.size()) return false;
            ch = line[cursor++];
            if (ch != '"' && ch != '\\') return false;
        }
        if (static_cast<unsigned char>(ch) < 0x20u || value.size() == max_bytes) return false;
        value.push_back(ch);
    }
    while (cursor < line.size() && (line[cursor] == ' ' || line[cursor] == '\t')) cursor++;
    return closed;
}

bool has_prefix(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool valid_server_url(std::string_view url) {
    size_t authority = 0;
    if (has_prefix(url, "wss://")) authority = 6;
    else if (has_prefix(url, "ws://")) authority = 5;
    else return false;
    if (url.size() <= authority || url[authority] == '/') return false;
    for (unsigned char ch : url) {
        if (ch <= 0x20u || ch == '"' || ch == '\\') return false;
    }
    return url.size() <= 255;
}

class HermesRunner {
public:
    void run() {
        nvs_ready_ = storage_.initialize();
        usb_ready_ = init_usb();
        if (!usb_ready_) ESP_LOGE(kTag, "native USB Serial/JTAG unavailable");

        audio_.initialize();
        if (audio_.microphone_status() != ESP_OK)
            ESP_LOGE(kTag, "microphone unavailable: %s", esp_err_to_name(audio_.microphone_status()));
        if (audio_.speaker_status() != ESP_OK)
            ESP_LOGW(kTag, "speaker unavailable: %s", esp_err_to_name(audio_.speaker_status()));
        if (!display_.ready()) {
            ESP_LOGE(kTag, "Hermes framebuffer allocation failed");
            return_to_launcher_overlay("DISPLAY MEMORY ERROR");
            process_launcher_error();
            return;
        }

        net_events_ = xQueueCreate(8, sizeof(NetworkEvent));
        if (!net_events_) ESP_LOGE(kTag, "network event queue allocation failed");
        wifi_ = std::make_unique<HermesWifi>(net_events_);

        transport_ready_ = transport_.begin();
        if (!transport_ready_) ESP_LOGE(kTag, "WebSocket transport initialization failed");

        hg::DeviceProfile profile;
        profile.board = "T-Embed ESP32-S3R8";
        profile.firmware = "local";
        profile.default_name = "T-Embed";
        profile.has_cancel_button = false;
        profile.has_scroll_buttons = true;
        profile.touch_screen = false;
        profile.talk_label = "HOLD KNOB";
        profile.cancel_label = "MENU";
        profile.mic_rate = 16000;
        profile.speaker_rate = 16000;

        hg::Hal hal;
        hal.system = &system_;
        hal.transport = &transport_;
        hal.storage = &storage_;
        hal.display = display_.ready() ? &display_ : nullptr;
        hal.mic = audio_.microphone_status() == ESP_OK ? &audio_ : nullptr;
        hal.speaker = audio_.speaker_ready() ? &audio_ : nullptr;
        hal.updater = nullptr;

        hg::App app(hal, std::move(profile));
        app.on_diag = [this](hg::json::Value &report) {
            report.set("nvs_ready", nvs_ready_)
                  .set("usb_ready", usb_ready_)
                  .set("websocket_ready", transport_ready_)
                  .set("mic_ready", audio_.microphone_status() == ESP_OK)
                  .set("mic_error", esp_err_to_name(audio_.microphone_status()))
                  .set("speaker_ready", audio_.speaker_status() == ESP_OK)
                  .set("speaker_error", esp_err_to_name(audio_.speaker_status()))
                  .set("internal_free", static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)))
                  .set("internal_largest", static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)))
                  .set("psram_free", static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        };
        app.begin();
        transport_.set_text_observer(&HermesRunner::observe_text, this);

        auto ssid_value = storage_.get("wifi_ssid");
        auto password_value = storage_.get("wifi_pass");
        std::string ssid = ssid_value.value_or("");
        std::string password = password_value.value_or("");
        bool wifi_started = nvs_ready_ && net_events_ && wifi_->start(ssid, password);
        wipe(ssid);
        wipe(password);
        if (ssid_value) wipe(*ssid_value);
        if (password_value) wipe(*password_value);
        if (!wifi_started) {
            const char *detail = nvs_ready_ ? "Configure Wi-Fi with USB" : "NVS unavailable; configuration cannot be saved";
            app.on_network(false, detail);
        }

        for (;;) {
            transport_.poll(app);
            poll_network(app);
            update_prompt_expiry();
            app.tick();
            synchronize_view(app);

            bool restart_requested = false;
            if (usb_ready_) poll_usb(app, restart_requested);
            if (restart_requested) restart_to_launcher(app);

            // Apply messages already queued before sampling a fresh physical press.
            if (transport_.poll(app) != 0) {
                update_prompt_expiry();
                app.tick();
                synchronize_view(app);
            }

            t_embed_input_event_t input{};
            t_embed_input_context_t context = input_context(app);
            uint32_t hold_ms = input_hold_ms(context, app);
            t_embed_ui_poll_input(context, hold_ms, &input);
            handle_input(app, input);

            app.tick();
            update_prompt_expiry();
            synchronize_view(app);
            render_local_view(app);
            pump_microphone(app);
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }

private:
    static void observe_text(void *context, std::string_view text) {
        static_cast<HermesRunner *>(context)->inspect_text(text);
    }

    void inspect_text(std::string_view text) {
        hg::json::Value message;
        if (!hg::json::parse(text, message) || !message.is_object()) return;
        const std::string &type = message["type"].as_string();
        if (type == "prompt") {
            const std::string &id = message["id"].as_string();
            if (id.empty()) return;
            if (prompt_.active && prompt_.id == id) return;

            prompt_.active = true;
            prompt_.expired = false;
            prompt_.answered = id == prompt_.last_answered_id;
            prompt_.send_failed = false;
            prompt_.id = id;
            prompt_.opened_at_us = esp_timer_get_time();
            prompt_.expires_at_us = kNoExpiry;
            double ttl_s = message["ttl_s"].as_number(0);
            if (std::isfinite(ttl_s) && ttl_s > 0) {
                const double max_ttl_us = 2147483647000.0;
                double ttl_us = std::min(ttl_s * 1000000.0, max_ttl_us);
                prompt_.expires_at_us = prompt_.opened_at_us + static_cast<int64_t>(ttl_us);
            }
            view_prompt_id_.clear();
        } else if (type == "prompt.close") {
            const std::string &id = message["id"].as_string();
            if (prompt_.active && (id.empty() || id == prompt_.id)) clear_prompt_guard();
        }
    }

    void clear_prompt_guard() {
        prompt_.active = false;
        prompt_.expired = false;
        prompt_.answered = false;
        prompt_.send_failed = false;
        prompt_.opened_at_us = 0;
        prompt_.expires_at_us = kNoExpiry;
        prompt_.id.clear();
        view_prompt_id_.clear();
        answer_view_ = false;
        prompt_arming_notice_ = false;
        prompt_scroll_ = 0;
        prompt_selection_ = 0;
    }

    void update_prompt_expiry() {
        int64_t now = esp_timer_get_time();
        if (prompt_.active && prompt_.expires_at_us != kNoExpiry &&
            now >= prompt_.expires_at_us)
            prompt_.expired = true;
        if (prompt_arming_notice_ && now >= prompt_.opened_at_us + kPromptArmUs)
            prompt_arming_notice_ = false;
    }

    void synchronize_view(hg::App &app) {
        hg::Screen screen = app.screen();
        if (prompt_.active && !app.online() && screen != hg::Screen::Listening) {
            clear_prompt_guard();
        }
        if (prompt_.active && screen == hg::Screen::Prompt) {
            if (local_view_ != LocalView::Prompt || view_prompt_id_ != prompt_.id) {
                local_view_ = LocalView::Prompt;
                view_prompt_id_ = prompt_.id;
                prompt_scroll_ = 0;
                prompt_selection_ = 0;
                answer_view_ = false;
            }
        } else if (local_view_ == LocalView::Prompt &&
                   (!prompt_.active || screen != hg::Screen::Prompt)) {
            local_view_ = LocalView::None;
            answer_view_ = false;
        }

        if (local_view_ == LocalView::ActionMenu && (!app.online() || !app.paired())) {
            if (selection_ > 1u) selection_ = 1u;
        }
    }

    t_embed_input_context_t input_context(const hg::App &app) const {
        if (local_view_ == LocalView::ActionMenu ||
            local_view_ == LocalView::NewSessionConfirm ||
            local_view_ == LocalView::Volume)
            return T_EMBED_INPUT_HERMES_MENU;
        if (local_view_ == LocalView::Prompt && app.screen() == hg::Screen::Prompt)
            return T_EMBED_INPUT_HERMES_PROMPT;
        switch (app.screen()) {
            case hg::Screen::Listening: return T_EMBED_INPUT_HERMES_LISTENING;
            case hg::Screen::Ready:
            case hg::Screen::Thinking:
            case hg::Screen::Responding:
            case hg::Screen::Card:
            case hg::Screen::Image:
                return app.online() && app.paired() ? T_EMBED_INPUT_HERMES_ACTIVE
                                                    : T_EMBED_INPUT_HERMES_OFFLINE;
            default: return T_EMBED_INPUT_HERMES_OFFLINE;
        }
    }

    uint32_t input_hold_ms(t_embed_input_context_t context, const hg::App &) const {
        if (context == T_EMBED_INPUT_HERMES_ACTIVE) return 300;
        if (context == T_EMBED_INPUT_HERMES_PROMPT) return 300;
        if (context == T_EMBED_INPUT_HERMES_LISTENING) return 0;
        if (context == T_EMBED_INPUT_HERMES_MENU) return 800;
        return 800;
    }

    void handle_input(hg::App &app, const t_embed_input_event_t &input) {
        if (local_view_ == LocalView::Prompt && app.screen() == hg::Screen::Prompt) {
            handle_prompt_input(app, input);
            return;
        }
        if (local_view_ == LocalView::ActionMenu ||
            local_view_ == LocalView::NewSessionConfirm ||
            local_view_ == LocalView::Volume) {
            handle_menu_input(app, input);
            return;
        }

        if (input.hold && input.press_context == T_EMBED_INPUT_HERMES_ACTIVE &&
            app.online() && app.paired() && !prompt_.active) {
            if (app.screen() == hg::Screen::Thinking || app.screen() == hg::Screen::Responding) {
                audio_.abort();
                app.on_button(hg::Button::Cancel, true);
                app.on_button(hg::Button::Cancel, false);
            }
            app.on_button(hg::Button::Talk, true);
            ptt_active_ = app.screen() == hg::Screen::Listening;
        }

        if (input.detents != 0 && app.screen() == hg::Screen::Listening) {
            audio_.stop();
            audio_.abort();
            app.on_button(hg::Button::Cancel, true);
            app.on_button(hg::Button::Cancel, false);
            ptt_active_ = false;
            return;
        }

        if (input.released && ptt_active_ &&
            input.press_context == T_EMBED_INPUT_HERMES_ACTIVE) {
            app.on_button(hg::Button::Talk, false);
            ptt_active_ = false;
        }

        if (input.detents != 0 && app.screen() != hg::Screen::Listening) {
            send_scroll(app, input.detents);
        }

        if (input.short_click &&
            (input.press_context == T_EMBED_INPUT_HERMES_ACTIVE ||
             input.press_context == T_EMBED_INPUT_HERMES_OFFLINE) &&
            !prompt_.active) {
            open_action_menu(app);
        }
    }

    void handle_prompt_input(hg::App &app, const t_embed_input_event_t &input) {
        if (input.press_context != T_EMBED_INPUT_HERMES_PROMPT ||
            (input.detents == 0 && !input.short_click) ||
            prompt_.answered || prompt_.expired)
            return;

        if (!answer_view_) {
            if (input.detents != 0) {
                unsigned max_scroll = t_embed_ui_prompt_max_scroll(app.model().detail.c_str());
                int64_t next = static_cast<int64_t>(prompt_scroll_) + input.detents;
                prompt_scroll_ = static_cast<unsigned>(std::max<int64_t>(0,
                    std::min<int64_t>(max_scroll, next)));
            }
            if (input.short_click) {
                answer_view_ = true;
                prompt_selection_ = 0;  // Deny is always the initial choice.
                if (esp_timer_get_time() < prompt_.opened_at_us + kPromptArmUs)
                    prompt_arming_notice_ = true;
            }
            return;
        }

        if (input.detents != 0)
            prompt_selection_ = t_embed_menu_move(prompt_selection_, input.detents, 2u);
        if (!input.short_click) return;

        const size_t polled = transport_.poll(app);
        if (polled >= t_embed::HermesTransport::kEventQueueDepth) return;
        int64_t now = esp_timer_get_time();
        bool armed = input.pressed_at_us >= prompt_.opened_at_us + kPromptArmUs;
        bool not_expired = prompt_.expires_at_us == kNoExpiry || now < prompt_.expires_at_us;
        if (!app.online() || !app.paired() || !prompt_.active ||
            prompt_.id != view_prompt_id_ || !armed || !not_expired) {
            if (!not_expired) prompt_.expired = true;
            return;
        }

        prompt_.last_answered_id = prompt_.id;
        prompt_.answered = true;
        hg::json::Value reply = hg::proto::message("prompt.reply");
        reply.set("id", prompt_.id).set("answer", prompt_selection_ == 1u ? "yes" : "no");
        std::string payload = reply.dump();
        prompt_.send_failed = !transport_.send_text(payload);
    }

    void open_action_menu(const hg::App &app) {
        local_view_ = LocalView::ActionMenu;
        selection_ = app.online() && app.paired() ? 4u : 1u;
    }

    void handle_menu_input(hg::App &app, const t_embed_input_event_t &input) {
        if (local_view_ == LocalView::Volume) {
            int64_t next = static_cast<int64_t>(volume_draft_) +
                           static_cast<int64_t>(input.detents) * 5;
            unsigned previous = volume_draft_;
            volume_draft_ = static_cast<unsigned>(std::max<int64_t>(0,
                std::min<int64_t>(100, next)));
            std::snprintf(volume_label_, sizeof(volume_label_), "VOLUME %u PCT", volume_draft_);
            // The overlay cache keys on pointers; volume_label_ is rewritten in
            // place, so force a redraw when its text changes.
            if (volume_draft_ != previous) t_embed_ui_invalidate_overlays();
            if (input.short_click && input.press_context == T_EMBED_INPUT_HERMES_MENU) {
                char command[32];
                std::snprintf(command, sizeof(command), "set volume %u", volume_draft_);
                (void)app.console(command);
                local_view_ = LocalView::ActionMenu;
                selection_ = app.online() && app.paired() ? 4u : 1u;
            }
            return;
        }

        if (local_view_ == LocalView::NewSessionConfirm) {
            selection_ = t_embed_menu_move(selection_, input.detents, 2u);
            if (input.short_click && input.press_context == T_EMBED_INPUT_HERMES_MENU) {
                if (selection_ == 0u && !prompt_.active) {
                    audio_.abort();
                    (void)app.console("new-session");
                    local_view_ = LocalView::None;
                } else {
                    local_view_ = LocalView::ActionMenu;
                    selection_ = 4u;
                }
            }
            return;
        }

        size_t count = app.online() && app.paired() ? 5u : 2u;
        if (input.detents != 0) selection_ = t_embed_menu_move(selection_, input.detents,
                                                                 static_cast<unsigned>(count));
        if (!input.short_click || input.press_context != T_EMBED_INPUT_HERMES_MENU) return;

        if (count == 2u) {
            if (selection_ == 0u) restart_to_launcher(app);
            local_view_ = LocalView::None;
            return;
        }

        switch (selection_) {
            case 0:
                if (!prompt_.active) {
                    audio_.abort();
                    app.on_button(hg::Button::Cancel, true);
                    app.on_button(hg::Button::Cancel, false);
                }
                local_view_ = LocalView::None;
                break;
            case 1:
                local_view_ = LocalView::NewSessionConfirm;
                selection_ = 1u;  // No is the safe default.
                break;
            case 2:
                volume_draft_ = read_volume();
                std::snprintf(volume_label_, sizeof(volume_label_), "VOLUME %u PCT", volume_draft_);
                local_view_ = LocalView::Volume;
                break;
            case 3:
                restart_to_launcher(app);
                break;
            default:
                local_view_ = LocalView::None;
                break;
        }
    }

    unsigned read_volume() {
        auto value = storage_.get("volume");
        if (!value) return 70;
        char *end = nullptr;
        long parsed = std::strtol(value->c_str(), &end, 10);
        if (end == value->c_str() || *end != '\0') return 70;
        return static_cast<unsigned>(std::max<long>(0, std::min<long>(100, parsed)));
    }

    void send_scroll(hg::App &app, int detents) {
        int count = std::min(32, std::abs(detents));
        hg::Button button = detents > 0 ? hg::Button::Down : hg::Button::Up;
        for (int i = 0; i < count; i++) app.on_button(button, true);
    }

    void render_local_view(const hg::App &app) {
        bool visible = local_view_ != LocalView::None;
        if (visible && !display_suppressed_) {
            t_embed_ui_set_frame_flush_suppressed(true);
            display_suppressed_ = true;
        }
        if (local_view_ == LocalView::Prompt) {
            unsigned outcome = prompt_.expired ? 2u : prompt_.answered
                ? (prompt_.send_failed ? 3u : 1u) : prompt_arming_notice_ ? 4u : 0u;
            t_embed_ui_render_prompt_overlay(app.model().headline.c_str(),
                app.model().detail.c_str(), prompt_scroll_, answer_view_,
                prompt_selection_, outcome);
        } else if (local_view_ == LocalView::ActionMenu) {
            static const char *const active_items[] = {
                "CANCEL TURN", "NEW CONVERSATION", "VOLUME", "HOME", "BACK"};
            static const char *const offline_items[] = {"HOME", "BACK"};
            if (app.online() && app.paired()) {
                t_embed_ui_render_menu_overlay("HERMES MENU", active_items, 5u,
                    selection_, "TURN SELECT PRESS OPEN");
            } else {
                t_embed_ui_render_menu_overlay("HERMES", offline_items, 2u,
                    selection_ % 2u, "TURN SELECT PRESS OPEN");
            }
        } else if (local_view_ == LocalView::NewSessionConfirm) {
            static const char *const items[] = {"YES", "NO"};
            t_embed_ui_render_menu_overlay("START NEW CONVERSATION?", items, 2u,
                selection_, "TURN SELECT PRESS CONFIRM");
        } else if (local_view_ == LocalView::Volume) {
            const char *const items[] = {volume_label_};
            t_embed_ui_render_menu_overlay("SPEAKER VOLUME", items, 1u, 0u,
                "TURN ADJUST PRESS SAVE");
        } else if (display_suppressed_) {
            display_suppressed_ = false;
            t_embed_ui_set_frame_flush_suppressed(false);
            t_embed_ui_invalidate_overlays();
            if (display_.ready()) t_embed_ui_flush_framebuffer(display_.framebuffer(), 0, 170);
        }
    }

    void pump_microphone(hg::App &app) {
        if (app.screen() != hg::Screen::Listening || audio_.microphone_status() != ESP_OK) {
            mic_capture_active_ = false;
            return;
        }
        if (!mic_capture_active_) {
            audio_.activate_microphone_capture();
            mic_capture_active_ = true;
        }
        int16_t samples[320];
        size_t count = audio_.read_microphone(samples, sizeof(samples) / sizeof(samples[0]));
        if (count) app.on_mic_samples(samples, count);
    }

    void poll_network(hg::App &app) {
        if (!net_events_) return;
        NetworkEvent event{};
        while (xQueueReceive(net_events_, &event, 0) == pdTRUE) {
            wifi_disconnect_reason_ = event.reason;
            wifi_disconnect_rssi_ = event.rssi;
            if (event.up) {
                wifi_up_ = true;
                wifi_->connected();
                app.on_network(true, event.detail);
            } else {
                wifi_up_ = false;
                audio_.abort();
                app.on_network(false, event.detail);
                wifi_->disconnected(system_.now_ms());
            }
        }
        if (!wifi_up_ && wifi_->connect_retry_due(system_.now_ms()))
            wifi_->retry(system_.now_ms());
    }

    static bool parse_wifi(std::string_view line, std::string &ssid,
                           std::string &password) {
        size_t cursor = 4;
        if (!parse_quoted(line, cursor, 32, ssid) || ssid.empty() ||
            !parse_quoted(line, cursor, 63, password) || cursor != line.size())
            return false;
        return true;
    }

    static bool parse_server(std::string_view line, std::string &url) {
        size_t cursor = 6;
        return parse_quoted(line, cursor, 255, url) && cursor == line.size() &&
               valid_server_url(url);
    }

    bool init_usb() {
        if (usb_serial_jtag_is_driver_installed()) return true;
        usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        config.tx_buffer_size = 2048;
        config.rx_buffer_size = 1024;
        return usb_serial_jtag_driver_install(&config) == ESP_OK;
    }

    void write_usb(std::string_view line) {
        if (!usb_ready_) return;
        std::string output(line);
        output.push_back('\n');
        (void)usb_serial_jtag_write_bytes(output.data(), output.size(), pdMS_TO_TICKS(20));
    }

    void poll_usb(hg::App &app, bool &restart_requested) {
        uint8_t input[64];
        int count = usb_serial_jtag_read_bytes(input, sizeof(input), 0);
        for (int i = 0; i < count; i++) {
            char value = static_cast<char>(input[i]);
            if (value == '\r') continue;
            if (value == '\n') {
                if (usb_discard_line_) {
                    write_usb("ERR LINE TOO LONG");
                } else if (usb_line_size_ != 0) {
                    usb_line_[usb_line_size_] = '\0';
                    restart_requested = process_usb_line(app,
                        std::string_view(usb_line_, usb_line_size_));
                }
                secure_zero(usb_line_, sizeof(usb_line_));
                usb_line_size_ = 0;
                usb_discard_line_ = false;
                if (restart_requested) break;
            } else if (!usb_discard_line_) {
                if (usb_line_size_ + 1 < sizeof(usb_line_)) {
                    usb_line_[usb_line_size_++] = value;
                } else {
                    secure_zero(usb_line_, sizeof(usb_line_));
                    usb_line_size_ = 0;
                    usb_discard_line_ = true;
                }
            }
        }
        secure_zero(input, sizeof(input));
    }

    bool process_usb_line(hg::App &app, std::string_view line) {
        if (line == "HELP") {
            write_usb("Commands: WIFI \"ssid\" \"password\" | SERVER \"ws[s]://host/path\" | STATUS | DIAG");
        } else if (line == "STATUS") {
            bool wifi_configured = configured("wifi_ssid");
            bool server_configured = configured("server");
            char result[256];
            std::snprintf(result, sizeof(result),
                "OK STATUS WIFI_CONFIGURED=%s SERVER_CONFIGURED=%s WIFI=%s HERMES=%s PAIRED=%s MIC=%s SPEAKER=%s NVS=%s",
                wifi_configured ? "YES" : "NO", server_configured ? "YES" : "NO",
                wifi_up_ ? "UP" : "DOWN", hg::screen_name(app.screen()),
                app.paired() ? "YES" : "NO",
                audio_.microphone_status() == ESP_OK ? "READY" : "ERROR",
                audio_.speaker_status() == ESP_OK ? "READY" : "ERROR",
                nvs_ready_ ? "READY" : "ERROR");
            write_usb(result);
        } else if (line == "DIAG") {
            char result[320];
            std::snprintf(result, sizeof(result),
                "OK DIAG INTERNAL_FREE=%u INTERNAL_LARGEST=%u PSRAM_FREE=%u NVS_ERROR=%s WIFI_ERROR=%s WIFI_REASON=%u WIFI_RSSI=%d MIC_ERROR=%s SPEAKER_ERROR=%s",
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                esp_err_to_name(storage_.last_error()),
                esp_err_to_name(wifi_ ? wifi_->last_error() : ESP_ERR_INVALID_STATE),
                static_cast<unsigned>(wifi_disconnect_reason_),
                static_cast<int>(wifi_disconnect_rssi_),
                esp_err_to_name(audio_.microphone_status()),
                esp_err_to_name(audio_.speaker_status()));
            write_usb(result);
        } else if (has_prefix(line, "WIFI ")) {
            std::string ssid, password;
            bool valid = parse_wifi(line, ssid, password);
            if (!valid) {
                wipe(ssid);
                wipe(password);
                write_usb("ERR WIFI SYNTAX; USE QUOTED VALUES");
                return false;
            }
            if (!nvs_ready_) {
                wipe(ssid);
                wipe(password);
                write_usb("ERR NVS UNAVAILABLE; SETTINGS WERE NOT ERASED");
                return false;
            }
            bool saved = storage_.set_wifi(ssid, password);
            wipe(ssid);
            wipe(password);
            if (!saved) {
                write_usb("ERR WIFI SAVE FAILED");
                return false;
            }
            write_usb("OK WIFI SAVED; RESTARTING");
            return true;
        } else if (has_prefix(line, "SERVER ")) {
            std::string url;
            if (!parse_server(line, url)) {
                wipe(url);
                write_usb("ERR SERVER SYNTAX; USE A QUOTED WS OR WSS URL");
                return false;
            }
            if (!nvs_ready_) {
                wipe(url);
                write_usb("ERR NVS UNAVAILABLE; SETTINGS WERE NOT ERASED");
                return false;
            }
            storage_.set("server", url);
            wipe(url);
            if (storage_.last_error() != ESP_OK) {
                write_usb("ERR SERVER SAVE FAILED");
                return false;
            }
            write_usb("OK SERVER SAVED; RESTARTING");
            return true;
        } else if (!line.empty()) {
            write_usb("ERR UNKNOWN COMMAND; SEND HELP");
        }
        return false;
    }

    bool configured(std::string_view key) {
        auto value = storage_.get(key);
        bool result = value && !value->empty();
        if (value && key == "wifi_pass") wipe(*value);
        return result;
    }

    void restart_to_launcher(hg::App &app) {
        if (app.screen() != hg::Screen::Prompt && !prompt_.active) {
            if (app.screen() == hg::Screen::Listening) {
                app.on_button(hg::Button::Cancel, true);
                app.on_button(hg::Button::Cancel, false);
            } else if (app.screen() == hg::Screen::Thinking ||
                       app.screen() == hg::Screen::Responding) {
                app.on_button(hg::Button::Cancel, true);
                app.on_button(hg::Button::Cancel, false);
            }
        }
        audio_.stop();
        audio_.abort();
        while (gpio_get_level(GPIO_NUM_0) == 0) vTaskDelay(pdMS_TO_TICKS(5));
        if (usb_ready_) (void)usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(100));
        esp_restart();
    }

    void return_to_launcher_overlay(const char *message) {
        static const char *const items[] = {"RESTART DEVICE"};
        t_embed_ui_set_frame_flush_suppressed(false);
        t_embed_ui_render_menu_overlay("HERMES ERROR", items, 1u, 0u, message);
    }

    void process_launcher_error() {
        for (;;) {
            t_embed_input_event_t input{};
            t_embed_ui_poll_input(T_EMBED_INPUT_HERMES_OFFLINE, 800u, &input);
            if (input.short_click) esp_restart();
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    HermesSystem system_;
    HermesStorage storage_;
    HermesDisplay display_;
    TEmbedHermesAudio audio_;
    t_embed::HermesTransport transport_;
    std::unique_ptr<HermesWifi> wifi_;
    QueueHandle_t net_events_ = nullptr;
    PromptGuard prompt_;
    LocalView local_view_ = LocalView::None;
    std::string view_prompt_id_;
    unsigned prompt_scroll_ = 0;
    unsigned prompt_selection_ = 0;
    unsigned selection_ = 0;
    unsigned volume_draft_ = 70;
    char volume_label_[24] = "VOLUME 70 PCT";
    char usb_line_[kUsbLineBytes]{};
    size_t usb_line_size_ = 0;
    bool usb_discard_line_ = false;
    bool answer_view_ = false;
    bool prompt_arming_notice_ = false;
    bool display_suppressed_ = false;
    bool mic_capture_active_ = false;
    bool ptt_active_ = false;
    bool wifi_up_ = false;
    uint16_t wifi_disconnect_reason_ = 0;
    int8_t wifi_disconnect_rssi_ = 0;
    bool nvs_ready_ = false;
    bool usb_ready_ = false;
    bool transport_ready_ = false;
};

}  // namespace

extern "C" void t_embed_hermes_run(void) {
    HermesRunner runner;
    runner.run();
}
