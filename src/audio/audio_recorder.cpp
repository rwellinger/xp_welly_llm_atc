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

// Thin façade over audio::IAudioInput. The concrete capture
// implementation (CoreAudio on macOS, PortAudio on Linux) is picked by
// audio::make_audio_input() at link time — engine code stays platform-
// neutral.

#include "audio/audio_recorder.hpp"
#include "audio/i_audio_input.hpp"

#include <XPLMUtilities.h>

#include <memory>
#include <vector>

namespace audio_recorder {

namespace {

constexpr unsigned kBitsPerSample = 16;
constexpr unsigned kNumChannels = 1;

std::unique_ptr<audio::IAudioInput> input_;

std::vector<uint8_t> build_wav(const std::vector<int16_t> &pcm,
                               unsigned sample_rate) {
  uint32_t data_size = static_cast<uint32_t>(pcm.size() * sizeof(int16_t));
  uint32_t file_size = 36 + data_size;

  std::vector<uint8_t> wav;
  wav.reserve(44 + data_size);

  auto write_u32 = [&](uint32_t v) {
    wav.push_back(static_cast<uint8_t>(v & 0xFF));
    wav.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    wav.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    wav.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
  };
  auto write_u16 = [&](uint16_t v) {
    wav.push_back(static_cast<uint8_t>(v & 0xFF));
    wav.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  };
  auto write_str = [&](const char *s) {
    while (*s)
      wav.push_back(static_cast<uint8_t>(*s++));
  };

  write_str("RIFF");
  write_u32(file_size);
  write_str("WAVE");

  write_str("fmt ");
  write_u32(16);
  write_u16(1);
  write_u16(kNumChannels);
  write_u32(sample_rate);
  write_u32(size_t{sample_rate} * kNumChannels * sizeof(int16_t));
  write_u16(size_t{kNumChannels} * sizeof(int16_t));
  write_u16(kBitsPerSample);

  write_str("data");
  write_u32(data_size);

  const auto *raw = reinterpret_cast<const uint8_t *>(pcm.data());
  wav.insert(wav.end(), raw, raw + data_size);

  return wav;
}

} // namespace

void init() {
  input_ = audio::make_audio_input();
  if (!input_) {
    XPLMDebugString("[xp_wellys_atc] Warning: audio recorder not supported on "
                    "this platform\n");
    return;
  }
  input_->open();
}

void stop() {
  if (input_) {
    input_->close();
    input_.reset();
  }
}

void start_recording() {
  if (input_)
    input_->start_recording();
}

void stop_recording() {
  if (input_)
    input_->stop_recording();
}

std::vector<int16_t> take_pcm() {
  if (!input_)
    return {};
  return input_->take_pcm();
}

unsigned sample_rate_hz() { return input_ ? input_->sample_rate_hz() : 16000; }

// Consumes the captured PCM. After encode_wav(), the internal buffer is
// empty — same destructive semantics as take_pcm(). The two are
// alternatives, never both for the same recording.
std::vector<uint8_t> encode_wav() {
  if (!input_)
    return {};
  unsigned rate = input_->sample_rate_hz();
  std::vector<int16_t> pcm = input_->take_pcm();
  return build_wav(pcm, rate);
}

float duration_seconds() { return input_ ? input_->duration_seconds() : 0.0f; }

size_t buffer_samples() { return input_ ? input_->buffer_samples() : 0; }

} // namespace audio_recorder
