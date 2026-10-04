#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "hg/hal.hpp"

// T-Embed split-bus audio adapter. The owner task drains read_microphone()
// and forwards samples to hg::App; driver tasks never call the core directly.
class TEmbedHermesAudio final : public hg::AudioIn, public hg::AudioOut {
 public:
  TEmbedHermesAudio();
  ~TEmbedHermesAudio() override;

  // Initializes I2S0/ES7210 input and independently attempts I2S1 speaker
  // output. The return value is the microphone status; speaker_status() lets
  // the caller omit Hal::speaker when speaker initialization fails.
  esp_err_t initialize();
  esp_err_t microphone_status() const;
  esp_err_t speaker_status() const;
  bool speaker_ready() const;

  // Nonblocking: returns one queued mono PCM16 chunk, up to 320 samples.
  size_t read_microphone(int16_t* samples, size_t capacity);
  // Call on the owner task only after Talk handling returns and Listening is flushed.
  void activate_microphone_capture();

  bool start(uint32_t sample_rate) override;
  void stop() override;
  bool begin(uint32_t sample_rate) override;
  void write(const int16_t* samples, size_t count) override;
  void end() override;
  void abort() override;
  bool busy() const override;
  void set_volume(uint8_t percent) override;

 private:
  struct Impl;
  Impl* impl_;
};
