/*
 * xp_wellys_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#ifndef BACKENDS_MANAGER_HPP
#define BACKENDS_MANAGER_HPP

#include "backends/i_language_model.hpp"
#include "backends/i_speech_to_text.hpp"
#include "backends/i_text_to_speech.hpp"
#include "persistence/model_manifest.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace backends {

// Lifecycle. init() is idempotent; stop() joins all worker threads and
// drops registered backends.
void init();
void stop();

// Plugin code registers concrete backends (whisper.cpp, llama.cpp,
// Piper) at startup once their model files have been verified. Pass
// nullptr to unregister. The async dispatchers below short-circuit to
// `success=false` when no backend is registered, so callers do not need
// to special-case "models not loaded yet".
void register_stt(std::unique_ptr<ISpeechToText> stt);
void register_lm(std::unique_ptr<ILanguageModel> lm);
void register_tts(std::unique_ptr<ITextToSpeech> tts);

bool stt_ready();
bool lm_ready();
bool tts_ready();

// Last observed end-to-end inference time per stage, in
// milliseconds. 0 means "no inference completed yet". Read by the UI
// for live tuning; updated by the worker threads under no lock so a
// torn read is theoretically possible but harmless (a millisecond
// counter is at most one digit off for a frame).
uint32_t last_stt_ms();
uint32_t last_lm_ms();
uint32_t last_tts_ms();

// Drain pending async callbacks on the X-Plane main thread. Call from
// the flight loop. One call drains all three queues.
void drain_callback_queue();

namespace stt {

struct TranscriptResult {
  std::string text;
  // 0.0 = noise/garbage, 1.0 = confident transcription. The plugin's
  // existing engine path uses < 0.3 as a hard "say again" gate.
  float quality = 1.0f;
  bool success = false;
};

// Schedules transcription on a worker thread and dispatches the
// callback on the main thread via drain_callback_queue(). The PCM is
// expected to be 16-bit mono; the requested sample rate is converted
// to 16 kHz float internally if needed.
void transcribe_async(std::vector<int16_t> pcm16, uint32_t sample_rate_hz,
                      std::function<void(TranscriptResult)> callback,
                      std::string airport_context = {});

} // namespace stt

namespace lm {

// Full ATC reply generation: returns the assistant text. Used by the
// engine's low-confidence fallback path.
void respond_async(
    std::string system_prompt, std::string user_text,
    std::function<void(std::string text, bool success)> callback);

// Single-token-ish intent classification. Identical signature/contract
// to the old gpt_client::classify_intent_async — engine code can swap
// 1:1.
void classify_intent_async(
    std::string transcript, std::string system_prompt,
    std::function<void(std::string intent_key, bool success)> callback);

} // namespace lm

namespace tts {

struct Audio {
  std::vector<int16_t> pcm16;
  uint32_t sample_rate_hz = 0;
  int channels = 1;
};

// Synthesize using the voice currently assigned to `role` (resolved
// via settings::voice_for_role). `length_scale` > 1.0 = slower (ATIS
// uses ~1.18); < 1.0 = faster.
void synthesize_async(std::string text, model_manifest::VoiceRole role,
                      float length_scale,
                      std::function<void(Audio audio, bool success)> callback);

} // namespace tts

} // namespace backends

#endif // BACKENDS_MANAGER_HPP
