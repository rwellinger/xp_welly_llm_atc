/*
 * xp_wellys_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

// PulseAudio (pa_simple) implementation of IAudioInput. Selected via
// make_audio_input() at the bottom of this TU. Plugin-only — the CMake
// build gates this file behind `if(UNIX AND NOT APPLE)`.
//
// Uses the pa_simple synchronous API: a background thread reads 512-frame
// chunks from the PulseAudio server in a tight loop and appends them to the
// capture buffer while recording_ is set. pa_simple performs all sample-rate
// conversion server-side, so we always get 16 kHz mono 16-bit PCM regardless
// of the hardware device's native rate.
//
// This implementation works transparently on both pure PulseAudio and PipeWire
// (which exposes a full PulseAudio compatibility layer) — no code change needed
// on modern Ubuntu / Zorin / Fedora desktops where PipeWire is the default.

#include "audio/i_audio_input.hpp"
#include "audio/mic_permission.hpp"
#include "persistence/settings.hpp"

#include <XPLMUtilities.h>

#include <pulse/error.h>
#include <pulse/simple.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace audio {

namespace {

constexpr unsigned kSampleRate = 16000;
constexpr unsigned kNumChannels = 1;
// 512 frames ≈ 32 ms at 16 kHz. Short enough that close() returns promptly
// after setting capture_running_ = false.
constexpr size_t kCaptureFrames = 512;

class PulseAudioInput : public IAudioInput {
public:
  PulseAudioInput() = default;
  ~PulseAudioInput() override { close(); }

  bool open() override;
  void close() override;

  void start_recording() override;
  void stop_recording() override;

  std::vector<int16_t> take_pcm() override;
  unsigned sample_rate_hz() const override { return kSampleRate; }
  std::size_t buffer_samples() const override;
  float duration_seconds() const override;

private:
  void capture_loop();

  pa_simple *pa_stream_ = nullptr;
  std::thread capture_thread_;
  std::atomic<bool> capture_running_{false};
  std::atomic<bool> recording_{false};
  std::vector<int16_t> buffer_;
  mutable std::mutex buffer_mutex_;
};

void PulseAudioInput::capture_loop() {
  int16_t chunk[kCaptureFrames];
  constexpr int kMaxConsecutiveErrors = 5;
  int consecutive_errors = 0;
  while (capture_running_.load()) {
    int error = 0;
    if (pa_simple_read(pa_stream_, chunk, sizeof(chunk), &error) < 0) {
      if (!capture_running_.load())
        break;
      ++consecutive_errors;
      char log[160];
      std::snprintf(log, sizeof(log),
                    "[xp_wellys_atc] PulseAudio read error (%d/%d): %s\n",
                    consecutive_errors, kMaxConsecutiveErrors,
                    pa_strerror(error));
      XPLMDebugString(log);
      if (consecutive_errors >= kMaxConsecutiveErrors) {
        XPLMDebugString("[xp_wellys_atc] PulseAudio: too many consecutive "
                        "errors, stopping capture thread\n");
        capture_running_ = false;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }
    consecutive_errors = 0;
    if (recording_.load()) {
      std::lock_guard<std::mutex> lock(buffer_mutex_);
      buffer_.insert(buffer_.end(), chunk, chunk + kCaptureFrames);
    }
  }
}

bool PulseAudioInput::open() {
  mic_permission::check_and_request(); // no-op on Linux, kept for symmetry

  static const pa_sample_spec ss = {PA_SAMPLE_S16LE, kSampleRate, kNumChannels};
  int error = 0;
  pa_stream_ = pa_simple_new(nullptr, "xp_wellys_atc", PA_STREAM_RECORD,
                             nullptr, "mic", &ss, nullptr, nullptr, &error);
  if (!pa_stream_) {
    char log[128];
    std::snprintf(log, sizeof(log),
                  "[xp_wellys_atc] PulseAudio open failed: %s\n",
                  pa_strerror(error));
    XPLMDebugString(log);
    return false;
  }

  capture_running_ = true;
  capture_thread_ = std::thread(&PulseAudioInput::capture_loop, this);

  XPLMDebugString("[xp_wellys_atc] Audio recorder initialized "
                  "(PulseAudio 16kHz mono 16-bit)\n");
  return true;
}

void PulseAudioInput::close() {
  capture_running_ = false;
  if (capture_thread_.joinable())
    capture_thread_.join();
  if (pa_stream_) {
    pa_simple_free(pa_stream_);
    pa_stream_ = nullptr;
  }
  recording_ = false;
  std::lock_guard<std::mutex> lock(buffer_mutex_);
  buffer_.clear();
}

void PulseAudioInput::start_recording() {
  if (!pa_stream_) {
    XPLMDebugString("[xp_wellys_atc] Warning: audio recorder not "
                    "initialized\n");
    return;
  }
  {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    buffer_.clear();
  }
  recording_ = true;
}

void PulseAudioInput::stop_recording() {
  recording_ = false;
  if (settings::debug_logging()) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    int16_t peak = 0;
    for (auto s : buffer_) {
      int16_t abs_s = s < 0 ? static_cast<int16_t>(-s) : s;
      if (abs_s > peak)
        peak = abs_s;
    }
    float peak_pct = (static_cast<float>(peak) / 32767.0f) * 100.0f;
    char log[256];
    std::snprintf(log, sizeof(log),
                  "[xp_wellys_atc][DEBUG] Recording stopped: %zu samples, "
                  "peak: %d (%.1f%%)\n",
                  buffer_.size(), static_cast<int>(peak), peak_pct);
    XPLMDebugString(log);
  }
}

std::vector<int16_t> PulseAudioInput::take_pcm() {
  std::lock_guard<std::mutex> lock(buffer_mutex_);
  return std::move(buffer_);
}

std::size_t PulseAudioInput::buffer_samples() const {
  std::lock_guard<std::mutex> lock(buffer_mutex_);
  return buffer_.size();
}

float PulseAudioInput::duration_seconds() const {
  std::lock_guard<std::mutex> lock(buffer_mutex_);
  if (buffer_.empty())
    return 0.0f;
  return static_cast<float>(buffer_.size()) / static_cast<float>(kSampleRate);
}

} // namespace

std::unique_ptr<IAudioInput> make_audio_input() {
  return std::make_unique<PulseAudioInput>();
}

} // namespace audio
