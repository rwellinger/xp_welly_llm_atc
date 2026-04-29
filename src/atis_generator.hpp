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

#ifndef ATIS_GENERATOR_HPP
#define ATIS_GENERATOR_HPP

#include "core/xplane_context.hpp"

#include <string>

namespace atis_generator {

void init();
void stop();

// Current ATIS letter ('A' through 'Z', wraps around)
char current_letter();

// Called every ~1s from flight loop. Increments letter on significant changes.
void check_for_update(const xplane_context::XPlaneContext &ctx);

// Generate full ATIS text for the nearest airport
std::string generate_atis_text(const xplane_context::XPlaneContext &ctx);

// Returns true if the active COM frequency matches the ATIS freq of nearest
// airport
bool is_tuned_to_atis(const xplane_context::XPlaneContext &ctx);

} // namespace atis_generator

#endif // ATIS_GENERATOR_HPP
