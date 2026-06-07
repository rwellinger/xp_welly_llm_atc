/*
 * xp_wellys_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#ifndef BACKENDS_I_TEXT_TO_SPEECH_HPP
#define BACKENDS_I_TEXT_TO_SPEECH_HPP

#include "persistence/model_manifest.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace backends {

class ITextToSpeech {
public:
  virtual ~ITextToSpeech() = default;

  // Synthesize `text` to mono 16-bit PCM samples using the voice
  // identified by `voice_id` (must be a previously load_voice'd id).
  // `sample_rate_hz` is filled with the synthesizer's native sample
  // rate (no resampling). `length_scale` controls speech rate: 1.0 is
  // normal, >1.0 is slower (used for ATIS), <1.0 is faster. An empty
  // result indicates failure (e.g. unknown voice_id).
  virtual std::vector<int16_t> synthesize(const std::string &voice_id,
                                          const std::string &text,
                                          float length_scale,
                                          uint32_t &sample_rate_hz) = 0;

  // Load a voice into memory. Idempotent — calling twice with the
  // same voice_id is a no-op.
  virtual bool load_voice(const std::string &voice_id,
                          const std::string &voice_onnx_path,
                          const std::string &voice_json_path) = 0;

  // Drop a previously loaded voice. No-op if not loaded.
  virtual void unload_voice(const std::string &voice_id) = 0;

  // True when at least one voice with this id is loaded and ready.
  virtual bool has_voice(const std::string &voice_id) const = 0;

  // Backend-specific default voice id for the given role. Used as a
  // last-resort fallback when the user-configured voice is not
  // available. Each backend returns an id it knows how to synthesize:
  // Piper picks from the model manifest, OpenAI picks from its six
  // built-in voices. Keeps role-to-voice resolution out of engine
  // code per the Backend Adapter Rule.
  virtual std::string
  default_voice_for(model_manifest::VoiceRole role) const = 0;

  // Human-readable description of the most recent failure. Empty after
  // a successful call or if the backend never failed. Manager reads
  // this after a synthesize() that returned empty so the UI can show
  // why (timeout, HTTP 5xx, decode failure).
  virtual std::string last_error_message() const { return {}; }
};

} // namespace backends

#endif // BACKENDS_I_TEXT_TO_SPEECH_HPP
