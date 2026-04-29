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

#ifndef AUDIO_RECORDER_HPP
#define AUDIO_RECORDER_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

namespace audio_recorder {

void init();
void stop();

void start_recording();
void stop_recording();

std::vector<uint8_t> encode_wav();
float duration_seconds();
size_t buffer_samples();

} // namespace audio_recorder

#endif // AUDIO_RECORDER_HPP
