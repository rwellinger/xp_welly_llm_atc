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

#ifndef ATC_SESSION_HPP
#define ATC_SESSION_HPP

#include "atc/intent_parser.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace atc_session {

enum class PTTState { IDLE, RECORDING, PROCESSING, PLAYING };

// Transcript row classification.
//   Pilot  — the pilot's spoken transmission (transcribed by STT).
//   Tower  — the controller's audible reply (synthesized by TTS).
//   System — a plugin-side notice that did NOT go through the radio
//            (e.g. "Funkstoerung — bitte wiederholen" emitted by the
//            TTS revert guard when synthesis failed). System rows are
//            UI-only and MUST NOT feed back into the LM context or
//            into last_tower_response_text_ for REQUEST_REPEAT — those
//            consumers should ignore them.
enum class TranscriptKind { Pilot, Tower, System };

struct TranscriptEntry {
  double sim_time;
  TranscriptKind kind;
  std::string text;
  std::string frequency;
  // Controller label at the time this message was spoken (e.g. "Lyon", "Annecy ATC").
  // Stored at push_back so historical entries don't change when the active controller does.
  std::string label;
};

void init();
void stop();

void on_ptt_pressed();
void on_ptt_released();

// Inject a pre-transcribed pilot transcript directly into the engine,
// skipping STT. Used by the in-plugin Debug-Texteingabe (Settings ->
// debug_text_input). Same downstream path as the STT callback: pushes a
// Pilot row to the transcript, runs engine::process_transcript, and on
// a non-empty engine reply pushes a Tower row + speaks it via the live
// TTS strategy. No-op when state_ != PTTState::IDLE or when LM/TTS
// backends are not ready.
void submit_text(const std::string &text);

// Called every flight loop frame — checks playback completion
void update();

PTTState ptt_state();
std::string ptt_state_label();

// Last recording info (populated after stop_recording)
float last_recording_duration();
size_t last_recording_samples();
size_t last_wav_bytes();

// Last parsed intent
const intent_parser::PilotMessage &last_pilot_message();

// Session stats
int total_transcriptions();
int total_api_calls();

// Transcript access
const std::vector<TranscriptEntry> &transcript_entries();
void clear_transcript();

// Last ATC (non-pilot) response text
std::string last_atc_response();

// Clear the ATIS playback cooldown. Used when the user toggles the
// backend mode in Settings — a fresh test of the new pipeline should
// not be silently swallowed by the prior pipeline's 120 s cooldown.
void reset_atis_cooldown();

} // namespace atc_session

#endif // ATC_SESSION_HPP
