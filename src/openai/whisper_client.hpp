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

#ifndef WHISPER_CLIENT_HPP
#define WHISPER_CLIENT_HPP

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace whisper_client {

void init();
void stop();

struct TranscriptResult {
  std::string text;
  float quality = 1.0f; // 0.0 = noise/garbage, 1.0 = confident transcription
  bool success = false;
};

void transcribe_async(std::vector<uint8_t> wav_data,
                      std::function<void(TranscriptResult result)> callback,
                      const std::string &airport_context = {});

// Called from flight loop to drain pending callbacks on main thread
void drain_callback_queue();

} // namespace whisper_client

#endif // WHISPER_CLIENT_HPP
