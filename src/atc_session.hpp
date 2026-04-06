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

#include "intent_parser.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace atc_session {

enum class PTTState { IDLE, RECORDING, PROCESSING, PLAYING };

struct TranscriptEntry {
  double sim_time;
  bool is_pilot;
  std::string text;
  std::string frequency;
};

void init();
void stop();

void on_ptt_pressed();
void on_ptt_released();

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

} // namespace atc_session

#endif // ATC_SESSION_HPP
