#include "t_embed_hermes_transport.h"

#include <cstring>
#include <limits>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "hg/app.hpp"

namespace t_embed {
namespace {

constexpr char kTag[] = "hg.ws";
constexpr int kBufferSize = 4096;
constexpr size_t kMaxSendBytes = HermesTransport::kMaxMessageBytes;
// Limit internal fallback to 4 KiB per event; the inbound event queue plus one polled event stays under 36 KiB.
constexpr size_t kMaxInternalMessageBytes = 4096;
constexpr size_t kCommandQueueDepth = 1;
constexpr size_t kSendQueueDepth = 1;
constexpr uint32_t kWorkerStackSize = 6144;

void *allocate_message_buffer(size_t len) {
    void *buffer = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer && len <= kMaxInternalMessageBytes) buffer = heap_caps_malloc(len, MALLOC_CAP_8BIT);
    return buffer;
}

}  // namespace

HermesTransport::~HermesTransport() {
    if (worker_task_) {
        ControlCommand command{CommandType::Stop, generation_.load(std::memory_order_relaxed), nullptr, 0};
        enqueue_control(command);
        if (worker_done_) xSemaphoreTake(worker_done_, portMAX_DELAY);
        worker_task_ = nullptr;
    }

    clear_control_queue();
    clear_send_queue();
    clear_queue();
    if (queue_) {
        vQueueDelete(queue_);
        queue_ = nullptr;
    }
    if (control_queue_) {
        vQueueDelete(control_queue_);
        control_queue_ = nullptr;
    }
    if (send_queue_) {
        vQueueDelete(send_queue_);
        send_queue_ = nullptr;
    }
    if (worker_done_) {
        vSemaphoreDelete(worker_done_);
        worker_done_ = nullptr;
    }
    heap_caps_free(rx_buffer_);
    rx_buffer_ = nullptr;
}

bool HermesTransport::begin() {
    if (queue_ && control_queue_ && send_queue_ && worker_done_ && worker_task_ && rx_buffer_) return true;

    if (!queue_) queue_ = xQueueCreate(kEventQueueDepth, sizeof(QueuedEvent));
    if (!control_queue_) control_queue_ = xQueueCreate(kCommandQueueDepth, sizeof(ControlCommand));
    if (!send_queue_) send_queue_ = xQueueCreate(kSendQueueDepth, sizeof(SendCommand));
    if (!worker_done_) worker_done_ = xSemaphoreCreateBinary();
    if (!rx_buffer_) rx_buffer_ = static_cast<uint8_t *>(allocate_message_buffer(kMaxMessageBytes));
    if (!queue_ || !control_queue_ || !send_queue_ || !worker_done_ || !rx_buffer_) {
        if (queue_) {
            vQueueDelete(queue_);
            queue_ = nullptr;
        }
        if (control_queue_) {
            vQueueDelete(control_queue_);
            control_queue_ = nullptr;
        }
        if (send_queue_) {
            vQueueDelete(send_queue_);
            send_queue_ = nullptr;
        }
        if (worker_done_) {
            vSemaphoreDelete(worker_done_);
            worker_done_ = nullptr;
        }
        heap_caps_free(rx_buffer_);
        rx_buffer_ = nullptr;
        ESP_LOGE(kTag, "could not allocate bounded WebSocket buffers");
        return false;
    }

    if (!worker_task_ &&
        xTaskCreate(&HermesTransport::worker_task_entry, "hg.ws.owner", kWorkerStackSize, this,
                    tskIDLE_PRIORITY + 1, &worker_task_) != pdPASS) {
        vQueueDelete(queue_);
        queue_ = nullptr;
        vQueueDelete(control_queue_);
        control_queue_ = nullptr;
        vQueueDelete(send_queue_);
        send_queue_ = nullptr;
        vSemaphoreDelete(worker_done_);
        worker_done_ = nullptr;
        heap_caps_free(rx_buffer_);
        rx_buffer_ = nullptr;
        ESP_LOGE(kTag, "could not create WebSocket worker");
        return false;
    }
    return true;
}

void HermesTransport::connect(const std::string &url, const std::string &subprotocol) {
    if (!begin()) return;

    owner_connected_ = false;
    const uint32_t generation = generation_.fetch_add(1, std::memory_order_relaxed) + 1;
    clear_queue();

    const size_t max_size = std::numeric_limits<size_t>::max();
    if (subprotocol.size() > max_size - 2 || url.size() > max_size - subprotocol.size() - 2) {
        static constexpr char reason[] = "client configuration too large";
        enqueue(EventType::Closed, generation, reason, sizeof(reason) - 1);
        enqueue_control({CommandType::Close, generation, nullptr, 0});
        return;
    }

    const size_t bytes = url.size() + subprotocol.size() + 2;
    if (bytes > kMaxSendBytes) {
        static constexpr char reason[] = "client configuration too large";
        enqueue(EventType::Closed, generation, reason, sizeof(reason) - 1);
        enqueue_control({CommandType::Close, generation, nullptr, 0});
        return;
    }

    auto *connection_data = static_cast<char *>(allocate_message_buffer(bytes));
    if (!connection_data) {
        static constexpr char reason[] = "client configuration allocation failed";
        enqueue(EventType::Closed, generation, reason, sizeof(reason) - 1);
        enqueue_control({CommandType::Close, generation, nullptr, 0});
        return;
    }
    if (!url.empty()) std::memcpy(connection_data, url.data(), url.size());
    connection_data[url.size()] = '\0';
    if (!subprotocol.empty()) {
        std::memcpy(connection_data + url.size() + 1, subprotocol.data(), subprotocol.size());
    }
    connection_data[bytes - 1] = '\0';

    if (!enqueue_control({CommandType::Connect, generation, connection_data, url.size()})) {
        static constexpr char reason[] = "client command queue unavailable";
        enqueue(EventType::Closed, generation, reason, sizeof(reason) - 1);
        enqueue_control({CommandType::Close, generation, nullptr, 0});
    }
}

bool HermesTransport::send_text(std::string_view text) {
    if (!owner_connected_ || text.size() > kMaxSendBytes || !send_queue_) return false;

    uint8_t *copy = nullptr;
    if (!text.empty()) {
        copy = static_cast<uint8_t *>(allocate_message_buffer(text.size()));
        if (!copy) return false;
        std::memcpy(copy, text.data(), text.size());
    }
    return enqueue_send({SendType::Text, generation_.load(std::memory_order_relaxed), copy, text.size()});
}

bool HermesTransport::send_binary(const uint8_t *data, size_t len) {
    if ((!data && len != 0) || !owner_connected_ || len > kMaxSendBytes || !send_queue_) return false;

    uint8_t *copy = nullptr;
    if (len != 0) {
        copy = static_cast<uint8_t *>(allocate_message_buffer(len));
        if (!copy) return false;
        std::memcpy(copy, data, len);
    }
    return enqueue_send({SendType::Binary, generation_.load(std::memory_order_relaxed), copy, len});
}

void HermesTransport::close() {
    owner_connected_ = false;
    const uint32_t generation = generation_.fetch_add(1, std::memory_order_relaxed) + 1;
    clear_queue();
    enqueue_control({CommandType::Close, generation, nullptr, 0});
}

size_t HermesTransport::poll(hg::App &app) {
    if (!queue_) return 0;

    size_t handled = 0;
    QueuedEvent event{};
    while (handled < kEventQueueDepth && xQueueReceive(queue_, &event, 0) == pdTRUE) {
        if (event.generation == generation_.load(std::memory_order_relaxed)) {
            switch (event.type) {
                case EventType::Open:
                    owner_connected_ = true;
                    app.on_transport_open();
                    break;
                case EventType::Closed:
                    owner_connected_ = false;
                    app.on_transport_closed(std::string_view(
                        event.data ? reinterpret_cast<const char *>(event.data) : "", event.len));
                    break;
                case EventType::Text: {
                    const std::string_view text(
                        event.data ? reinterpret_cast<const char *>(event.data) : "", event.len);
                    if (text_observer_) text_observer_(text_observer_context_, text);
                    app.on_transport_text(text);
                    break;
                }
                case EventType::Binary:
                    app.on_transport_binary(event.data, event.len);
                    break;
            }
        }
        heap_caps_free(event.data);
        ++handled;
    }
    return handled;
}

void HermesTransport::set_text_observer(TextObserver observer, void *context) {
    text_observer_ = observer;
    text_observer_context_ = context;
}

void HermesTransport::worker_task_entry(void *arg) {
    static_cast<HermesTransport *>(arg)->worker_loop();
    vTaskDelete(nullptr);
}

void HermesTransport::worker_loop() {
    for (;;) {
        ControlCommand control{};
        if (xQueueReceive(control_queue_, &control, 0) == pdTRUE) {
            if (control.type == CommandType::Stop) {
                heap_caps_free(control.connection_data);
                destroy_client();
                reset_rx();
                clear_send_queue();
                break;
            }
            process_control(control);
            heap_caps_free(control.connection_data);
            continue;
        }

        SendCommand send{};
        if (xQueuePeek(send_queue_, &send, 0) == pdTRUE) {
            process_send(send);
            SendCommand completed{};
            if (xQueueReceive(send_queue_, &completed, 0) == pdTRUE) heap_caps_free(completed.data);
            continue;
        }

        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }

    if (worker_done_) xSemaphoreGive(worker_done_);
}

bool HermesTransport::enqueue_control(const ControlCommand &command) {
    if (!control_queue_ || !worker_task_) {
        heap_caps_free(command.connection_data);
        return false;
    }

    // Lifecycle requests are a one-slot latest-request mailbox; an in-flight operation is not interrupted.
    ControlCommand replaced{};
    if (xQueueReceive(control_queue_, &replaced, 0) == pdTRUE) heap_caps_free(replaced.connection_data);
    if (xQueueSend(control_queue_, &command, 0) != pdTRUE) {
        heap_caps_free(command.connection_data);
        return false;
    }
    xTaskNotifyGive(worker_task_);
    return true;
}

bool HermesTransport::enqueue_send(const SendCommand &command) {
    if (!send_queue_ || !worker_task_ || xQueueSend(send_queue_, &command, 0) != pdTRUE) {
        heap_caps_free(command.data);
        return false;
    }
    xTaskNotifyGive(worker_task_);
    return true;
}

void HermesTransport::process_control(ControlCommand &command) {
    destroy_client();
    reset_rx();

    active_generation_ = command.generation;
    callback_context_ = {this, active_generation_};
    if (command.type == CommandType::Close) return;

    connection_data_ = command.connection_data;
    command.connection_data = nullptr;
    url_len_ = command.url_len;

    esp_websocket_client_config_t config = {};
    config.uri = connection_data_;
    config.subprotocol = connection_data_ + url_len_ + 1;
    config.buffer_size = kBufferSize;
    config.task_stack = 6144;
    config.disable_auto_reconnect = true;
    config.network_timeout_ms = 10000;
    config.ping_interval_sec = 0;
    if (std::strncmp(connection_data_, "wss://", 5) == 0) config.crt_bundle_attach = esp_crt_bundle_attach;

    client_ = esp_websocket_client_init(&config);
    if (!client_) {
        static constexpr char reason[] = "client init failed";
        enqueue(EventType::Closed, active_generation_, reason, sizeof(reason) - 1);
        destroy_client();
        return;
    }

    if (esp_websocket_register_events(client_, WEBSOCKET_EVENT_ANY, &HermesTransport::on_event,
                                      &callback_context_) != ESP_OK) {
        static constexpr char reason[] = "event registration failed";
        enqueue(EventType::Closed, active_generation_, reason, sizeof(reason) - 1);
        destroy_client();
        return;
    }
    if (esp_websocket_client_start(client_) != ESP_OK) {
        static constexpr char reason[] = "client start failed";
        enqueue(EventType::Closed, active_generation_, reason, sizeof(reason) - 1);
        destroy_client();
    }
}

void HermesTransport::process_send(SendCommand &command) {
    if (command.generation != generation_.load(std::memory_order_relaxed) ||
        command.generation != active_generation_ || !client_ || !esp_websocket_client_is_connected(client_)) {
        return;
    }

    int sent = -1;
    if (command.type == SendType::Text) {
        sent = esp_websocket_client_send_text(client_, reinterpret_cast<const char *>(command.data),
                                              static_cast<int>(command.len), pdMS_TO_TICKS(2000));
    } else {
        sent = esp_websocket_client_send_bin(client_, reinterpret_cast<const char *>(command.data),
                                             static_cast<int>(command.len), pdMS_TO_TICKS(2000));
    }
    if (sent != static_cast<int>(command.len)) ESP_LOGW(kTag, "queued WebSocket send did not complete");
}

void HermesTransport::destroy_client() {
    if (client_) {
        if (esp_websocket_client_is_connected(client_)) {
            esp_websocket_client_close(client_, pdMS_TO_TICKS(1000));
        }
        esp_websocket_client_destroy(client_);
        client_ = nullptr;
    }

    heap_caps_free(connection_data_);
    connection_data_ = nullptr;
    url_len_ = 0;
}

void HermesTransport::on_event(void *arg, const char *, int32_t id, void *event_data) {
    auto *context = static_cast<CallbackContext *>(arg);
    context->transport->handle_event(context->generation, id, event_data);
}

void HermesTransport::handle_event(uint32_t generation, int32_t id, void *event_data) {
    switch (id) {
        case WEBSOCKET_EVENT_CONNECTED:
            enqueue(EventType::Open, generation, nullptr, 0);
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_CLOSED: {
            static constexpr char reason[] = "disconnected";
            enqueue(EventType::Closed, generation, reason, sizeof(reason) - 1);
            break;
        }
        case WEBSOCKET_EVENT_ERROR: {
            static constexpr char reason[] = "connection error";
            enqueue(EventType::Closed, generation, reason, sizeof(reason) - 1);
            break;
        }
        case WEBSOCKET_EVENT_DATA:
            if (event_data) {
                handle_data(generation, *static_cast<esp_websocket_event_data_t *>(event_data));
            }
            break;
        default:
            break;
    }
}

void HermesTransport::handle_data(uint32_t generation, const esp_websocket_event_data_t &data) {
    const uint8_t opcode = static_cast<uint8_t>(data.op_code);
    if (opcode == 0x8 || opcode == 0x9 || opcode == 0xA) return;

    const size_t payload_len = static_cast<size_t>(data.payload_len);
    const size_t payload_offset = static_cast<size_t>(data.payload_offset);
    const size_t chunk_len = static_cast<size_t>(data.data_len);
    if (payload_len > kMaxMessageBytes || payload_offset > payload_len || chunk_len > payload_len - payload_offset ||
        (chunk_len != 0 && !data.data_ptr)) {
        reset_rx();
        ESP_LOGW(kTag, "dropping invalid or oversized WebSocket message");
        return;
    }

    if (opcode == 0x1 || opcode == 0x2) {
        if (payload_offset == 0) {
            rx_size_ = 0;
            rx_expected_ = payload_len;
            rx_opcode_ = opcode;
        } else if (rx_opcode_ != opcode || rx_expected_ != payload_len) {
            reset_rx();
            ESP_LOGW(kTag, "dropping out-of-order WebSocket fragment");
            return;
        }
    } else if (opcode == 0x0) {
        if (rx_opcode_ == 0 || rx_expected_ != payload_len) {
            reset_rx();
            ESP_LOGW(kTag, "dropping unexpected WebSocket continuation");
            return;
        }
    } else {
        reset_rx();
        ESP_LOGW(kTag, "dropping unsupported WebSocket opcode");
        return;
    }

    if (payload_offset != rx_size_) {
        reset_rx();
        ESP_LOGW(kTag, "dropping WebSocket message with a fragment gap");
        return;
    }
    if (chunk_len != 0) std::memcpy(rx_buffer_ + rx_size_, data.data_ptr, chunk_len);
    rx_size_ += chunk_len;

    if (data.fin) {
        if (rx_size_ != rx_expected_) {
            reset_rx();
            ESP_LOGW(kTag, "dropping incomplete WebSocket message");
            return;
        }
        const EventType type = rx_opcode_ == 0x1 ? EventType::Text : EventType::Binary;
        enqueue(type, generation, rx_buffer_, rx_size_);
        reset_rx();
    } else if (rx_size_ >= rx_expected_) {
        reset_rx();
        ESP_LOGW(kTag, "dropping WebSocket message without a final fragment");
    }
}

bool HermesTransport::enqueue(EventType type, uint32_t generation, const void *data, size_t len) {
    if (!queue_ || len > kMaxMessageBytes || (len != 0 && !data)) return false;

    uint8_t *copy = nullptr;
    if (len != 0) {
        copy = static_cast<uint8_t *>(allocate_message_buffer(len));
        if (!copy) {
            ESP_LOGW(kTag, "dropping WebSocket event: no memory for %u bytes", static_cast<unsigned>(len));
            return false;
        }
        std::memcpy(copy, data, len);
    }

    QueuedEvent event{type, generation, copy, len};
    if (xQueueSend(queue_, &event, 0) != pdTRUE) {
        heap_caps_free(copy);
        ESP_LOGW(kTag, "dropping WebSocket event: event queue is full");
        return false;
    }
    return true;
}

void HermesTransport::reset_rx() {
    rx_size_ = 0;
    rx_expected_ = 0;
    rx_opcode_ = 0;
}

void HermesTransport::clear_queue() {
    if (!queue_) return;
    QueuedEvent event{};
    while (xQueueReceive(queue_, &event, 0) == pdTRUE) heap_caps_free(event.data);
}

void HermesTransport::clear_control_queue() {
    if (!control_queue_) return;
    ControlCommand command{};
    while (xQueueReceive(control_queue_, &command, 0) == pdTRUE) heap_caps_free(command.connection_data);
}

void HermesTransport::clear_send_queue() {
    if (!send_queue_) return;
    SendCommand command{};
    while (xQueueReceive(send_queue_, &command, 0) == pdTRUE) heap_caps_free(command.data);
}

}  // namespace t_embed
