#include "t_embed_hermes_audio.h"

#include <atomic>
#include <cstring>
#include <limits>
#include <new>

#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "t_embed_audio_input.h"

namespace {

constexpr uint32_t kSampleRate = 16000;
constexpr size_t kMicReadFrames = 80;
constexpr size_t kMicChunkSamples = 320;
constexpr UBaseType_t kMicQueueDepth = 4;
constexpr size_t kSpeakerBufferBytes = 48000;
constexpr size_t kSpeakerChunkSamples = 160;
constexpr int64_t kSpeakerDmaTailUs = 20000;
constexpr uint32_t kSpeakerAbortWaitMs = 100;
constexpr char kTag[] = "t_embed_audio";

}  // namespace

struct TEmbedHermesAudio::Impl {
  struct MicChunk {
    uint32_t epoch;
    uint16_t count;
    int16_t samples[kMicChunkSamples];
  };

  std::atomic<bool> running{true};
  std::atomic<bool> capturing{false};
  std::atomic<bool> mic_armed{false};
  std::atomic<uint32_t> mic_epoch{0};
  std::atomic<bool> speaker_open{false};
  std::atomic<bool> speaker_draining{false};
  std::atomic<bool> speaker_playing{false};
  std::atomic<uint32_t> speaker_abort_generation{0};
  std::atomic<uint32_t> speaker_abort_handled{0};
  std::atomic<uint8_t> volume{100};
  std::atomic<TaskHandle_t> mic_task{nullptr};
  std::atomic<TaskHandle_t> speaker_task{nullptr};

  esp_err_t mic_status = ESP_ERR_INVALID_STATE;
  esp_err_t speaker_status = ESP_ERR_INVALID_STATE;
  t_embed_audio_input_t* input = nullptr;
  QueueHandle_t mic_queue = nullptr;
  i2s_chan_handle_t speaker_channel = nullptr;
  StreamBufferHandle_t speaker_buffer = nullptr;
  StaticStreamBuffer_t speaker_stream_control{};
  uint8_t* speaker_storage = nullptr;
  SemaphoreHandle_t speaker_lock = nullptr;
  int64_t speaker_tail_until_us = 0;
  bool initialized = false;

  static void mic_task_entry(void* arg) {
    static_cast<Impl*>(arg)->mic_task_loop();
  }

  static void speaker_task_entry(void* arg) {
    static_cast<Impl*>(arg)->speaker_task_loop();
  }

  void mic_task_loop() {
    int16_t read_buffer[kMicReadFrames];
    MicChunk chunk{};
    size_t chunk_count = 0;
    uint32_t chunk_epoch = mic_epoch.load();

    while (running.load()) {
      const uint32_t current_epoch = mic_epoch.load();
      if (current_epoch != chunk_epoch) {
        chunk_count = 0;
        chunk_epoch = current_epoch;
      }
      if (!capturing.load()) {
        chunk_count = 0;
        vTaskDelay(pdMS_TO_TICKS(5));
        continue;
      }

      const uint32_t epoch = current_epoch;
      size_t frames_read = 0;
      esp_err_t result = t_embed_audio_input_read_mono(
          input, read_buffer, kMicReadFrames, &frames_read, 20);
      if (result != ESP_OK || frames_read == 0) {
        if (result != ESP_OK) vTaskDelay(pdMS_TO_TICKS(2));
        continue;
      }
      if (!capturing.load() || epoch != mic_epoch.load()) {
        chunk_count = 0;
        continue;
      }

      size_t copied = 0;
      while (copied < frames_read) {
        size_t part = kMicChunkSamples - chunk_count;
        if (part > frames_read - copied) part = frames_read - copied;
        std::memcpy(chunk.samples + chunk_count, read_buffer + copied,
                    part * sizeof(int16_t));
        chunk_count += part;
        copied += part;
        if (chunk_count == kMicChunkSamples) {
          chunk.epoch = epoch;
          chunk.count = static_cast<uint16_t>(chunk_count);
          if (xQueueSend(mic_queue, &chunk, 0) != pdTRUE) {
            MicChunk dropped{};
            (void)xQueueReceive(mic_queue, &dropped, 0);
            (void)xQueueSend(mic_queue, &chunk, 0);
          }
          chunk_count = 0;
        }
      }
    }
    mic_task.store(nullptr);
    vTaskDelete(nullptr);
  }

  void speaker_task_loop() {
    int16_t samples[kSpeakerChunkSamples];

    for (;;) {
      const uint32_t generation = speaker_abort_generation.load();
      if (generation != speaker_abort_handled.load()) {
        if (xSemaphoreTake(speaker_lock, portMAX_DELAY) == pdTRUE) {
          if (speaker_abort_generation.load() == generation) {
            (void)xStreamBufferReset(speaker_buffer);
            speaker_tail_until_us = 0;
            esp_err_t result = i2s_channel_disable(speaker_channel);
            if (running.load() && result == ESP_OK)
              result = i2s_channel_enable(speaker_channel);
            if (result != ESP_OK)
              ESP_LOGW(kTag, "speaker abort could not restart I2S: %s",
                       esp_err_to_name(result));
            speaker_playing.store(false);
            speaker_draining.store(false);
            speaker_abort_handled.store(generation);
          }
          xSemaphoreGive(speaker_lock);
        }
        continue;
      }
      if (!running.load()) break;

      if (xSemaphoreTake(speaker_lock, portMAX_DELAY) != pdTRUE) continue;
      if (speaker_abort_generation.load() != generation) {
        xSemaphoreGive(speaker_lock);
        continue;
      }
      size_t bytes = xStreamBufferReceive(speaker_buffer, samples,
                                           sizeof(samples),
                                           pdMS_TO_TICKS(10));
      if (speaker_abort_generation.load() != generation) {
        xSemaphoreGive(speaker_lock);
        continue;
      }
      if (bytes > 0) {
        size_t count = bytes / sizeof(int16_t);
        uint8_t gain = volume.load();
        if (gain < 100) {
          for (size_t i = 0; i < count; ++i)
            samples[i] = static_cast<int16_t>(
                (static_cast<int32_t>(samples[i]) * gain) / 100);
        }

        speaker_playing.store(true);
        size_t offset = 0;
        while (running.load() && offset < count) {
          if (speaker_abort_generation.load() != generation) break;
          size_t written = 0;
          esp_err_t result = i2s_channel_write(
              speaker_channel, samples + offset,
              (count - offset) * sizeof(int16_t), &written,
              pdMS_TO_TICKS(25));
          if (result != ESP_OK || written == 0) {
            ESP_LOGW(kTag, "speaker write failed: %s", esp_err_to_name(result));
            break;
          }
          offset += written / sizeof(int16_t);
        }
        if (speaker_abort_generation.load() != generation) {
          xSemaphoreGive(speaker_lock);
          continue;
        }
        speaker_tail_until_us = esp_timer_get_time() + kSpeakerDmaTailUs;
        speaker_playing.store(false);
      } else if (!speaker_open.load() && speaker_draining.load()) {
        if (speaker_tail_until_us == 0 ||
            esp_timer_get_time() >= speaker_tail_until_us)
          speaker_draining.store(false);
      }
      xSemaphoreGive(speaker_lock);
    }

    speaker_task.store(nullptr);
    vTaskDelete(nullptr);
  }

  esp_err_t initialize_microphone() {
    mic_status = t_embed_audio_input_create(&input);
    if (mic_status != ESP_OK) return mic_status;
    mic_queue = xQueueCreate(kMicQueueDepth, sizeof(MicChunk));
    if (!mic_queue) {
      t_embed_audio_input_destroy(input);
      input = nullptr;
      return mic_status = ESP_ERR_NO_MEM;
    }

    TaskHandle_t task = nullptr;
    if (xTaskCreate(mic_task_entry, "t-embed-mic", 4096, this, 6, &task) != pdPASS) {
      vQueueDelete(mic_queue);
      mic_queue = nullptr;
      t_embed_audio_input_destroy(input);
      input = nullptr;
      return mic_status = ESP_ERR_NO_MEM;
    }
    mic_task.store(task);
    return mic_status = ESP_OK;
  }

  esp_err_t initialize_speaker() {
    speaker_storage = static_cast<uint8_t*>(heap_caps_malloc(
        kSpeakerBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!speaker_storage) return speaker_status = ESP_ERR_NO_MEM;
    speaker_buffer = xStreamBufferCreateStatic(
        kSpeakerBufferBytes, 1, speaker_storage, &speaker_stream_control);
    if (!speaker_buffer) {
      heap_caps_free(speaker_storage);
      speaker_storage = nullptr;
      return speaker_status = ESP_ERR_NO_MEM;
    }
    speaker_lock = xSemaphoreCreateMutex();
    if (!speaker_lock) {
      vStreamBufferDelete(speaker_buffer);
      speaker_buffer = nullptr;
      heap_caps_free(speaker_storage);
      speaker_storage = nullptr;
      return speaker_status = ESP_ERR_NO_MEM;
    }

    i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    channel_config.auto_clear = true;
    channel_config.dma_desc_num = 2;
    channel_config.dma_frame_num = kSpeakerChunkSamples;
    esp_err_t result = i2s_new_channel(&channel_config, &speaker_channel, NULL);
    if (result != ESP_OK) return speaker_status = result;

    i2s_std_config_t standard_config = {};
    standard_config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);
    standard_config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
    standard_config.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    standard_config.gpio_cfg.bclk = GPIO_NUM_7;
    standard_config.gpio_cfg.ws = GPIO_NUM_5;
    standard_config.gpio_cfg.dout = GPIO_NUM_6;
    standard_config.gpio_cfg.din = I2S_GPIO_UNUSED;
    result = i2s_channel_init_std_mode(speaker_channel, &standard_config);
    if (result == ESP_OK) result = i2s_channel_enable(speaker_channel);
    if (result != ESP_OK) return speaker_status = result;

    TaskHandle_t task = nullptr;
    if (xTaskCreate(speaker_task_entry, "t-embed-spk", 4096, this, 7, &task) != pdPASS)
      return speaker_status = ESP_ERR_NO_MEM;
    speaker_task.store(task);
    return speaker_status = ESP_OK;
  }

  void release() {
    running.store(false);
    capturing.store(false);
    if (speaker_status == ESP_OK) abort_speaker();

    for (unsigned i = 0; i < 100 &&
         (mic_task.load() || speaker_task.load()); ++i)
      vTaskDelay(1);
    if (mic_task.load() || speaker_task.load()) {
      ESP_LOGE(kTag, "audio tasks did not stop; retaining their state");
      return;
    }

    if (input) {
      t_embed_audio_input_destroy(input);
      input = nullptr;
    }
    if (mic_queue) {
      vQueueDelete(mic_queue);
      mic_queue = nullptr;
    }
    release_speaker();
  }

  void release_speaker() {
    if (speaker_channel) {
      (void)i2s_channel_disable(speaker_channel);
      (void)i2s_del_channel(speaker_channel);
      speaker_channel = nullptr;
    }
    if (speaker_buffer) {
      vStreamBufferDelete(speaker_buffer);
      speaker_buffer = nullptr;
    }
    if (speaker_storage) {
      heap_caps_free(speaker_storage);
      speaker_storage = nullptr;
    }
    if (speaker_lock) {
      vSemaphoreDelete(speaker_lock);
      speaker_lock = nullptr;
    }
  }

  void abort_speaker() {
    speaker_open.store(false);
    speaker_draining.store(false);
    speaker_abort_generation.fetch_add(1);
  }
};

TEmbedHermesAudio::TEmbedHermesAudio() : impl_(new (std::nothrow) Impl) {}

TEmbedHermesAudio::~TEmbedHermesAudio() {
  if (!impl_) return;
  impl_->release();
  if (!impl_->mic_task.load() && !impl_->speaker_task.load()) delete impl_;
  impl_ = nullptr;
}

esp_err_t TEmbedHermesAudio::initialize() {
  if (!impl_) return ESP_ERR_NO_MEM;
  if (impl_->initialized) return impl_->mic_status;
  impl_->initialized = true;
  impl_->initialize_microphone();
  impl_->initialize_speaker();
  if (impl_->speaker_status != ESP_OK) impl_->release_speaker();
  return impl_->mic_status;
}

esp_err_t TEmbedHermesAudio::microphone_status() const {
  return impl_ ? impl_->mic_status : ESP_ERR_NO_MEM;
}

esp_err_t TEmbedHermesAudio::speaker_status() const {
  return impl_ ? impl_->speaker_status : ESP_ERR_NO_MEM;
}

bool TEmbedHermesAudio::speaker_ready() const {
  return impl_ && impl_->speaker_status == ESP_OK;
}

size_t TEmbedHermesAudio::read_microphone(int16_t* samples, size_t capacity) {
  if (!impl_ || !samples || !capacity || !impl_->mic_queue ||
      !impl_->capturing.load())
    return 0;

  Impl::MicChunk chunk{};
  for (;;) {
    if (xQueuePeek(impl_->mic_queue, &chunk, 0) != pdTRUE) return 0;
    if (chunk.epoch != impl_->mic_epoch.load()) {
      (void)xQueueReceive(impl_->mic_queue, &chunk, 0);
      continue;
    }
    if (capacity < chunk.count) return 0;
    if (xQueueReceive(impl_->mic_queue, &chunk, 0) != pdTRUE) continue;
    std::memcpy(samples, chunk.samples, chunk.count * sizeof(int16_t));
    return chunk.count;
  }
}

bool TEmbedHermesAudio::start(uint32_t sample_rate) {
  if (!impl_) return false;
  impl_->capturing.store(false);
  impl_->mic_armed.store(false);
  impl_->mic_epoch.fetch_add(1);
  if (impl_->mic_queue) (void)xQueueReset(impl_->mic_queue);
  if (impl_->mic_status != ESP_OK || !impl_->mic_queue ||
      sample_rate != kSampleRate)
    return false;
  impl_->mic_armed.store(true);
  return true;
}

void TEmbedHermesAudio::activate_microphone_capture() {
  if (!impl_ || impl_->mic_status != ESP_OK || !impl_->mic_queue) return;
  bool armed = true;
  if (impl_->mic_armed.compare_exchange_strong(armed, false))
    impl_->capturing.store(true);
}

void TEmbedHermesAudio::stop() {
  if (!impl_) return;
  impl_->capturing.store(false);
  impl_->mic_armed.store(false);
  impl_->mic_epoch.fetch_add(1);
  if (impl_->mic_queue) (void)xQueueReset(impl_->mic_queue);
}

bool TEmbedHermesAudio::begin(uint32_t sample_rate) {
  if (!speaker_ready() || sample_rate != kSampleRate) return false;
  abort();
  const TickType_t start = xTaskGetTickCount();
  TickType_t timeout = pdMS_TO_TICKS(kSpeakerAbortWaitMs);
  if (timeout == 0) timeout = 1;
  const uint32_t generation = impl_->speaker_abort_generation.load();
  while (impl_->speaker_abort_handled.load() != generation) {
    if (static_cast<TickType_t>(xTaskGetTickCount() - start) >= timeout) return false;
    vTaskDelay(1);
  }
  if (impl_->speaker_abort_generation.load() != generation) return false;
  impl_->speaker_draining.store(false);
  impl_->speaker_open.store(true);
  return true;
}

void TEmbedHermesAudio::write(const int16_t* samples, size_t count) {
  if (!impl_ || !speaker_ready() || !samples || !impl_->speaker_open.load() ||
      count == 0 || count > std::numeric_limits<size_t>::max() / sizeof(int16_t))
    return;
  size_t bytes = count * sizeof(int16_t);
  size_t sent = xStreamBufferSend(impl_->speaker_buffer, samples, bytes, 0);
  if (sent < bytes)
    ESP_LOGW(kTag, "speaker queue full; dropped %u bytes",
             static_cast<unsigned>(bytes - sent));
}

void TEmbedHermesAudio::end() {
  if (!impl_) return;
  impl_->speaker_open.store(false);
  impl_->speaker_draining.store(true);
}

void TEmbedHermesAudio::abort() {
  if (impl_ && impl_->speaker_status == ESP_OK) impl_->abort_speaker();
}

bool TEmbedHermesAudio::busy() const {
  if (!impl_ || impl_->speaker_status != ESP_OK) return false;
  return impl_->speaker_open.load() || impl_->speaker_draining.load() ||
         impl_->speaker_playing.load() ||
         xStreamBufferBytesAvailable(impl_->speaker_buffer) > 0;
}

void TEmbedHermesAudio::set_volume(uint8_t percent) {
  if (impl_) impl_->volume.store(percent > 100 ? 100 : percent);
}
