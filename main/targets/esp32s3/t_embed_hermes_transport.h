#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_websocket_client.h"
#include "hg/hal.hpp"

namespace hg {
class App;
}

namespace t_embed {

class HermesTransport final : public hg::Transport {
public:
    using TextObserver = void (*)(void *context, std::string_view text);

    static constexpr size_t kEventQueueDepth = 8;
    static constexpr size_t kMaxMessageBytes = 16 * 1024;

    HermesTransport() = default;
    ~HermesTransport() override;

    HermesTransport(const HermesTransport &) = delete;
    HermesTransport &operator=(const HermesTransport &) = delete;

    bool begin();
    size_t poll(hg::App &app);
    // Set on the owner task. The observer runs before App dispatch; its view is callback-scoped.
    void set_text_observer(TextObserver observer, void *context);

    void connect(const std::string &url, const std::string &subprotocol) override;
    // true means accepted into the bounded worker queue, not delivered. A later connect/close may discard an unsent
    // message if it is stale at the worker generation check; it cannot interrupt a write already underway.
    bool send_text(std::string_view text) override;
    bool send_binary(const uint8_t *data, size_t len) override;
    void close() override;

private:
    enum class EventType : uint8_t { Open, Closed, Text, Binary };
    enum class CommandType : uint8_t { Connect, Close, Stop };
    enum class SendType : uint8_t { Text, Binary };

    struct QueuedEvent {
        EventType type;
        uint32_t generation;
        uint8_t *data;
        size_t len;
    };

    struct CallbackContext {
        HermesTransport *transport;
        uint32_t generation;
    };

    struct ControlCommand {
        CommandType type;
        uint32_t generation;
        char *connection_data;
        size_t url_len;
    };

    struct SendCommand {
        SendType type;
        uint32_t generation;
        uint8_t *data;
        size_t len;
    };

    static void worker_task_entry(void *arg);
    static void on_event(void *arg, const char *base, int32_t id, void *event_data);
    void worker_loop();
    bool enqueue_control(const ControlCommand &command);
    bool enqueue_send(const SendCommand &command);
    void process_control(ControlCommand &command);
    void process_send(SendCommand &command);
    void destroy_client();
    void handle_event(uint32_t generation, int32_t id, void *event_data);
    void handle_data(uint32_t generation, const esp_websocket_event_data_t &data);
    bool enqueue(EventType type, uint32_t generation, const void *data, size_t len);
    void reset_rx();
    void clear_queue();
    void clear_control_queue();
    void clear_send_queue();

    QueueHandle_t queue_ = nullptr;
    QueueHandle_t control_queue_ = nullptr;
    QueueHandle_t send_queue_ = nullptr;
    TaskHandle_t worker_task_ = nullptr;
    SemaphoreHandle_t worker_done_ = nullptr;
    esp_websocket_client_handle_t client_ = nullptr;
    CallbackContext callback_context_{};
    std::atomic<uint32_t> generation_{0};
    uint32_t active_generation_ = 0;
    char *connection_data_ = nullptr;
    size_t url_len_ = 0;
    bool owner_connected_ = false;

    uint8_t *rx_buffer_ = nullptr;
    size_t rx_size_ = 0;
    size_t rx_expected_ = 0;
    uint8_t rx_opcode_ = 0;
    TextObserver text_observer_ = nullptr;
    void *text_observer_context_ = nullptr;
};

}  // namespace t_embed
