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

#ifndef I_AUDIO_INPUT_HPP
#define I_AUDIO_INPUT_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace audio {

// SDK-free strategy interface for microphone capture. Mirrors the
// i_speech_to_text / i_language_model / i_text_to_speech split in
// src/backends/ — engine code talks to the interface; the concrete
// implementation (CoreAudio on macOS, PortAudio on Linux) is chosen
// by make_audio_input() at link time.
class IAudioInput {
public:
  virtual ~IAudioInput() = default;

  // One-shot lifecycle. open() acquires the HAL device and configures
  // a 16-bit signed PCM mono stream at the device's native sample
  // rate. Returns true on success. close() releases everything; safe
  // to call repeatedly.
  virtual bool open() = 0;
  virtual void close() = 0;

  // Push-to-talk: start_recording() begins appending samples to the
  // internal buffer; stop_recording() halts capture but keeps the
  // buffer intact so take_pcm() / sample_rate_hz() / buffer_samples()
  // / duration_seconds() can be queried before the next session.
  virtual void start_recording() = 0;
  virtual void stop_recording() = 0;

  // Move the captured PCM out into the caller. After this call the
  // internal buffer is empty.
  virtual std::vector<int16_t> take_pcm() = 0;

  // Device-native sample rate selected at open() time.
  virtual unsigned sample_rate_hz() const = 0;

  // Current size of the capture buffer (number of int16 samples).
  virtual std::size_t buffer_samples() const = 0;

  // Current capture buffer duration in seconds.
  virtual float duration_seconds() const = 0;
};

// Factory — the one place that picks the concrete implementation.
// Linked against the CoreAudio TU on macOS, against the PortAudio TU
// on Linux. Engine code MUST go through this entry point.
std::unique_ptr<IAudioInput> make_audio_input();

} // namespace audio

#endif // I_AUDIO_INPUT_HPP
