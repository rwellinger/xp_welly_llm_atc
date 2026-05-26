/*
 * xp_wellys_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#include "backends/openai_common.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace backends::openai_common {

std::string last4(const std::string &api_key) {
  if (api_key.size() <= 4)
    return api_key;
  return api_key.substr(api_key.size() - 4);
}

namespace {

// Write little-endian primitives into a byte vector.
void write_u32_le(std::vector<uint8_t> &out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xff));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xff));
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xff));
}
void write_u16_le(std::vector<uint8_t> &out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xff));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
}

uint32_t read_u32_le(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t read_u16_le(const uint8_t *p) {
  return static_cast<uint16_t>(p[0]) |
         static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}

} // namespace

std::vector<uint8_t>
pcm_float32_to_wav(const std::vector<float> &pcm_16k_mono) {
  constexpr uint32_t sample_rate = 16000;
  constexpr uint16_t channels = 1;
  constexpr uint16_t bits_per_sample = 16;
  const uint32_t data_bytes =
      static_cast<uint32_t>(pcm_16k_mono.size() * sizeof(int16_t));

  std::vector<uint8_t> wav;
  wav.reserve(44 + data_bytes);

  // RIFF header
  wav.insert(wav.end(), {'R', 'I', 'F', 'F'});
  write_u32_le(wav, 36 + data_bytes); // file size - 8
  wav.insert(wav.end(), {'W', 'A', 'V', 'E'});

  // fmt chunk
  wav.insert(wav.end(), {'f', 'm', 't', ' '});
  write_u32_le(wav, 16); // fmt chunk size
  write_u16_le(wav, 1);  // PCM format
  write_u16_le(wav, channels);
  write_u32_le(wav, sample_rate);
  write_u32_le(wav, sample_rate * channels * bits_per_sample / 8); // byte rate
  write_u16_le(wav, channels * bits_per_sample / 8); // block align
  write_u16_le(wav, bits_per_sample);

  // data chunk
  wav.insert(wav.end(), {'d', 'a', 't', 'a'});
  write_u32_le(wav, data_bytes);

  // float [-1, 1] -> int16 with clipping
  for (float f : pcm_16k_mono) {
    const float clamped = std::max(-1.0f, std::min(1.0f, f));
    const int32_t s = static_cast<int32_t>(std::lround(clamped * 32767.0f));
    const int16_t i16 = static_cast<int16_t>(
        std::max<int32_t>(-32768, std::min<int32_t>(32767, s)));
    wav.push_back(static_cast<uint8_t>(i16 & 0xff));
    wav.push_back(static_cast<uint8_t>((i16 >> 8) & 0xff));
  }
  return wav;
}

std::vector<int16_t> wav_to_pcm_int16(const std::vector<uint8_t> &wav,
                                      uint32_t &sample_rate_hz) {
  sample_rate_hz = 0;
  if (wav.size() < 44)
    return {};
  if (std::memcmp(wav.data(), "RIFF", 4) != 0 ||
      std::memcmp(wav.data() + 8, "WAVE", 4) != 0)
    return {};

  // Walk sub-chunks until we find fmt + data. OpenAI's WAV reply is
  // canonical PCM but the spec allows additional chunks before data,
  // so we don't assume the 44-byte canonical layout.
  size_t pos = 12;
  uint16_t format = 0;
  uint16_t channels = 0;
  uint32_t sample_rate = 0;
  uint16_t bits_per_sample = 0;
  bool fmt_ok = false;

  while (pos + 8 <= wav.size()) {
    char id[5] = {static_cast<char>(wav[pos]), static_cast<char>(wav[pos + 1]),
                  static_cast<char>(wav[pos + 2]),
                  static_cast<char>(wav[pos + 3]), 0};
    uint32_t chunk_size = read_u32_le(wav.data() + pos + 4);
    size_t chunk_start = pos + 8;

    if (std::memcmp(id, "fmt ", 4) == 0 && chunk_size >= 16 &&
        chunk_start + 16 <= wav.size()) {
      format = read_u16_le(wav.data() + chunk_start);
      channels = read_u16_le(wav.data() + chunk_start + 2);
      sample_rate = read_u32_le(wav.data() + chunk_start + 4);
      bits_per_sample = read_u16_le(wav.data() + chunk_start + 14);
      fmt_ok = true;
    } else if (std::memcmp(id, "data", 4) == 0 && fmt_ok) {
      if (format != 1 || bits_per_sample != 16 || channels != 1)
        return {}; // only 16-bit mono PCM supported
      if (chunk_start + chunk_size > wav.size())
        chunk_size = static_cast<uint32_t>(wav.size() - chunk_start);
      const size_t sample_count = chunk_size / sizeof(int16_t);
      std::vector<int16_t> out(sample_count);
      for (size_t i = 0; i < sample_count; ++i) {
        out[i] = static_cast<int16_t>(
            read_u16_le(wav.data() + chunk_start + i * sizeof(int16_t)));
      }
      sample_rate_hz = sample_rate;
      return out;
    }
    pos = chunk_start + chunk_size;
    // Chunks are word-aligned in the spec.
    if (chunk_size & 1u)
      ++pos;
  }
  return {};
}

} // namespace backends::openai_common
