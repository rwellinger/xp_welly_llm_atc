/*
 * xp_wellys_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef ENGINE_ENGINE_HPP
#define ENGINE_ENGINE_HPP

#include "intent_parser.hpp"
#include "xplane_context.hpp"

#include <functional>
#include <string>

namespace engine {

struct Input {
  std::string transcript;
  float quality = 1.0f; // Whisper quality; 1.0 for text-only tests
  // Non-owning pointer to the current XPlaneContext. The plugin passes
  // &xplane_context::get(); the CLI will pass a scenario-built context.
  // Lifetime must cover the duration of process_transcript + any async
  // callbacks it spawns.
  const xplane_context::XPlaneContext *ctx = nullptr;
  std::string pilot_callsign;
  bool gpt_fallback_enabled = true;
};

struct Output {
  // Empty = silent transition (state changed but no response to speak).
  std::string response_text;
  intent_parser::PilotMessage parsed;
  // True for radio-discipline warnings — caller uses this if it needs to
  // distinguish "ATC clearance" from "ATC correction". State is unchanged
  // when is_warning is true.
  bool is_warning = false;
};

using Done = std::function<void(Output)>;

// Reset internal counters (profanity warnings, GPT call count). Call from
// plugin init / re-enable. Separate from the per-call flow so engine has
// no "stop" phase.
void reset();

// Number of GPT API calls made since last reset(). Callers that maintain
// their own aggregate API-call counter (Whisper + TTS + GPT) read this to
// include engine-initiated calls in the total.
int gpt_api_calls();

// Process a pilot transcript end-to-end:
//   - quality check (low quality -> say again)
//   - rule-based intent parse
//   - INAPPROPRIATE_LANGUAGE interception (escalating warnings)
//   - departure sub-variant disambiguation via GPT (if enabled)
//   - state machine invocation with two-stage (direct vs. GPT) routing
//
// `done` is always called exactly once. On the sync path it runs before
// process_transcript returns; on the GPT-async path it runs later on the
// thread that the GPT callback is dispatched on (main thread, via the
// plugin's callback drain).
void process_transcript(Input in, Done done);

} // namespace engine

#endif
