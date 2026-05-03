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

#include "atc/flight_phase.hpp"
#include "atc/intent_parser.hpp"
#include "core/xplane_context.hpp"

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

// Pilot-driven "Disregard" — drops the current ATC dialog and lands on
// a flow-appropriate state instead of blind IDLE: airborne pilots near
// their last airport keep PATTERN_ENTRY; airborne pilots away from any
// airport return to EN_ROUTE; pilots on the ground go all the way to
// IDLE. Always preserves the runway lock when staying airborne so the
// pilot doesn't have to re-negotiate it.
void disregard(const xplane_context::XPlaneContext &ctx,
               flight_phase::FlightPhase phase);

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

// Per-frame airport-change reset. When the pilot is EN_ROUTE and the
// nearest airport changes (e.g. crossing into a new control zone), drop
// to IDLE so the UI hint pipeline reflects the new airport's options
// (INITIAL_CALL_INBOUND etc.) instead of remaining silent on EN_ROUTE.
// Call from atc_session::update() after check_auto_correction().
void check_airport_change(const xplane_context::XPlaneContext &ctx);

// Render a controller-issued traffic advisory through the standard
// template path WITHOUT changing ATCState. The traffic dialog runs
// parallel to the main flow (see traffic_dialog.hpp). Returns the
// rendered text.
std::string
render_traffic_advisory(const std::map<std::string, std::string> &advisory_vars,
                        const xplane_context::XPlaneContext &ctx);

} // namespace atc_state_machine

#endif // ATC_STATE_MACHINE_HPP
