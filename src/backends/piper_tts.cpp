/*
 * xp_wellys_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 *
 * Lifted from spikes/spike_e2e/src/piper_tts.cpp; the spike validated
 * the chunked synthesis loop. Adds `length_scale` plumbing so ATIS can
 * speak slower than tower/ground, and a per-voice synth pool so
 * different roles (Atis/Tower/Ground/Center) can use different voices
 * without paying a swap penalty mid-flight.
 */

#include "backends/piper_tts.hpp"

#include "core/logging.hpp"
#include "piper.h"

#include <cstdint>

namespace backends {

namespace {

constexpr const char *kBackendTag = "TTS-LOCAL";

int16_t f32_to_i16(float x) {
  if (x > 1.0f)
    x = 1.0f;
  if (x < -1.0f)
    x = -1.0f;
  return static_cast<int16_t>(x * 32767.0f);
}

} // namespace

PiperTts::PiperTts() = default;

PiperTts::~PiperTts() {
  std::lock_guard<std::mutex> lk(mutex_);
  for (auto &kv : synths_) {
    if (kv.second)
      piper_free(kv.second);
  }
  synths_.clear();
}

bool PiperTts::init(const std::string &espeakng_data_dir) {
  espeakng_dir_ = espeakng_data_dir;
  return !espeakng_dir_.empty();
}

bool PiperTts::load_voice(const std::string &voice_id,
                          const std::string &voice_onnx_path,
                          const std::string &voice_json_path) {
  if (voice_id.empty() || espeakng_dir_.empty())
    return false;

  {
    std::lock_guard<std::mutex> lk(mutex_);
    if (synths_.count(voice_id) > 0)
      return true; // already loaded
  }

  // piper_create may take ~50–300 ms on M1 (ONNX session init + voice
  // config parse). Run it outside the mutex so concurrent load_voice
  // calls for *different* ids overlap.
  piper_synthesizer *s = piper_create(
      voice_onnx_path.c_str(), voice_json_path.c_str(), espeakng_dir_.c_str());
  if (!s)
    return false;

  std::lock_guard<std::mutex> lk(mutex_);
  // Race: another thread may have inserted while we created. Drop the
  // duplicate.
  auto [it, inserted] = synths_.emplace(voice_id, s);
  if (!inserted) {
    piper_free(s);
  }
  return true;
}

void PiperTts::unload_voice(const std::string &voice_id) {
  piper_synthesizer *to_free = nullptr;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = synths_.find(voice_id);
    if (it == synths_.end())
      return;
    to_free = it->second;
    synths_.erase(it);
  }
  if (to_free)
    piper_free(to_free);
}

bool PiperTts::has_voice(const std::string &voice_id) const {
  std::lock_guard<std::mutex> lk(mutex_);
  return synths_.count(voice_id) > 0;
}

std::string PiperTts::default_voice_for(model_manifest::VoiceRole role) const {
  return model_manifest::default_voice_for(role);
}

std::vector<int16_t> PiperTts::synthesize(const std::string &voice_id,
                                          const std::string &text,
                                          float length_scale,
                                          uint32_t &sample_rate_hz) {
  sample_rate_hz = 0;
  if (text.empty())
    return {};
  logging::info("[%s] synthesize voice %s, %zu chars, length_scale %.2f "
                "(Piper, espeak-ng)",
                kBackendTag, voice_id.c_str(), text.size(), length_scale);

  piper_synthesizer *synth = nullptr;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = synths_.find(voice_id);
    if (it == synths_.end())
      return {};
    synth = it->second;
  }
  if (!synth)
    return {};

  piper_synthesize_options opts = piper_default_synthesize_options(synth);
  // Piper's `length_scale` directly maps speech rate. Our public
  // contract uses the same convention (>1.0 = slower). Clamp to a
  // sensible band so a stray caller cannot wedge the synthesizer.
  if (length_scale < 0.5f)
    length_scale = 0.5f;
  if (length_scale > 2.0f)
    length_scale = 2.0f;
  opts.length_scale = length_scale;

  if (piper_synthesize_start(synth, text.c_str(), &opts) != PIPER_OK) {
    return {};
  }

  std::vector<int16_t> pcm;
  pcm.reserve(text.size() * 1024);

  piper_audio_chunk chunk{};
  int rc = 0;
  while ((rc = piper_synthesize_next(synth, &chunk)) != PIPER_DONE) {
    if (rc != PIPER_OK)
      return {};
    if (sample_rate_hz == 0)
      sample_rate_hz = static_cast<uint32_t>(chunk.sample_rate);
    const float *s = chunk.samples;
    for (size_t i = 0; i < chunk.num_samples; ++i) {
      pcm.push_back(f32_to_i16(s[i]));
    }
  }
  // The terminating PIPER_DONE chunk may still carry samples.
  if (chunk.num_samples > 0) {
    if (sample_rate_hz == 0)
      sample_rate_hz = static_cast<uint32_t>(chunk.sample_rate);
    const float *s = chunk.samples;
    for (size_t i = 0; i < chunk.num_samples; ++i) {
      pcm.push_back(f32_to_i16(s[i]));
    }
  }
  return pcm;
}

} // namespace backends
