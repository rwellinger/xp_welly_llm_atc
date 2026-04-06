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

#ifndef INTENT_PARSER_HPP
#define INTENT_PARSER_HPP

#include "xplane_context.hpp"

#include <string>

namespace intent_parser {

enum class PilotIntent {
  UNKNOWN,
  RADIO_CHECK,
  INITIAL_CALL,
  INITIAL_CALL_GROUND,
  INITIAL_CALL_TOWER,
  INITIAL_CALL_INBOUND,
  REQUEST_TAXI,
  REQUEST_TAXI_PARKING,
  READY_FOR_DEPARTURE,
  REPORT_POSITION,
  REPORT_POSITION_DOWNWIND,
  REPORT_POSITION_BASE,
  REPORT_POSITION_FINAL,
  REQUEST_LANDING,
  RUNWAY_VACATED,
  READBACK,
  REQUEST_FREQUENCY,
  UNABLE,
  SELF_ANNOUNCE,
};

struct PilotMessage {
  std::string raw_transcript;
  PilotIntent intent = PilotIntent::UNKNOWN;
  float confidence = 0.0f;
  std::string callsign;
  std::string runway;
};

void init();
void stop();

PilotMessage parse(const std::string &transcript,
                   const xplane_context::XPlaneContext &ctx);

const char *intent_name(PilotIntent intent);
const char *intent_template_key(PilotIntent intent);

} // namespace intent_parser

#endif // INTENT_PARSER_HPP
