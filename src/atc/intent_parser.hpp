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

#include "core/xplane_context.hpp"

#include <string>

namespace intent_parser {

enum class PilotIntent {
  UNKNOWN,
  RADIO_CHECK,
  INITIAL_CALL,
  INITIAL_CALL_GROUND,
  INITIAL_CALL_TOWER,
  INITIAL_CALL_INBOUND,
  INITIAL_CALL_INBOUND_VRP,
  INITIAL_CALL_APPROACH,
  REQUEST_TAXI,
  REQUEST_TAXI_PARKING,
  READY_FOR_DEPARTURE,
  READY_FOR_DEPARTURE_VFR,
  REPORT_POSITION,
  REPORT_POSITION_DOWNWIND,
  REPORT_POSITION_BASE,
  REPORT_POSITION_FINAL,
  REQUEST_LANDING,
  REQUEST_TOUCH_AND_GO,
  GO_AROUND,
  RUNWAY_VACATED,
  READBACK,
  REQUEST_FREQUENCY,
  LEAVING_FREQUENCY,
  UNABLE,
  SELF_ANNOUNCE,
  REQUEST_FLIGHT_FOLLOWING,
  INAPPROPRIATE_LANGUAGE,
  NEGATIVE_CORRECTION,
  TRAFFIC_IN_SIGHT,
  TRAFFIC_NEGATIVE_CONTACT,
  TRAFFIC_LOOKING,
  REQUEST_REPEAT, // NfL §18 c) Nr. 4 — "WIEDERHOLEN SIE / SAY AGAIN"
};

struct PilotMessage {
  std::string raw_transcript;
  PilotIntent intent = PilotIntent::UNKNOWN;
  float confidence = 0.0f;
  std::string callsign;
  std::string runway;
  std::string vrp_name;      // canonical VRP name if detected ("Whiskey")
  bool has_position = false; // pilot reported position (e.g. "on parking")
};

void init();
void stop();

PilotMessage parse(const std::string &transcript,
                   const xplane_context::XPlaneContext &ctx);

const char *intent_name(PilotIntent intent);
const char *intent_template_key(PilotIntent intent);
PilotIntent intent_from_key(const std::string &key);

} // namespace intent_parser

#endif // INTENT_PARSER_HPP
