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

#ifndef ATC_STATE_MACHINE_HPP
#define ATC_STATE_MACHINE_HPP

#include "flight_phase.hpp"
#include "intent_parser.hpp"
#include "xplane_context.hpp"

#include <map>
#include <string>

namespace atc_state_machine {

enum class ATCState {
  IDLE,
  GROUND_CONTACT,
  TAXI_CLEARED,
  TOWER_CONTACT,
  DEPARTURE_CLEARED,
  PATTERN_ENTRY,
  LANDING_CLEARED,
  TOUCH_AND_GO_CLEARED,
  UNICOM_ACTIVE,
  EN_ROUTE,
  APPROACH_CONTACT,
};

struct ATCResponse {
  std::string text;
  ATCState next_state = ATCState::IDLE;
  bool requires_readback = false;
};

void init();
void stop();
void reset();

ATCState get_state();
const char *state_name(ATCState state);
bool is_readback_pending();

// Departure intent (set when entering DEPARTURE_CLEARED).
// Returns "PATTERN" or "CROSS_COUNTRY".
const char *get_departure_type_name();

ATCState state_from_name(const std::string &name);
void set_state(ATCState state);

std::map<std::string, std::string>
build_vars(const intent_parser::PilotMessage &msg,
           const xplane_context::XPlaneContext &ctx);

ATCResponse process(const intent_parser::PilotMessage &msg,
                    const xplane_context::XPlaneContext &ctx);

// Check and apply auto-corrections based on flight phase mismatches.
// Call every frame from atc_session::update(). Uses dt for delay timers.
void check_auto_correction(flight_phase::FlightPhase phase, float dt);

} // namespace atc_state_machine

#endif // ATC_STATE_MACHINE_HPP
