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

#ifndef TTS_CLIENT_HPP
#define TTS_CLIENT_HPP

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tts_client {

void init();
void stop();

void speak_async(
    const std::string &text,
    std::function<void(std::vector<uint8_t> mp3_data, bool success)> callback,
    float speed = 1.0f);

// Called from flight loop to drain pending callbacks on main thread
void drain_callback_queue();

} // namespace tts_client

#endif // TTS_CLIENT_HPP
