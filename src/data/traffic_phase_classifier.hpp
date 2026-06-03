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

#ifndef TRAFFIC_PHASE_CLASSIFIER_HPP
#define TRAFFIC_PHASE_CLASSIFIER_HPP

#include "data/traffic_context.hpp"

// Phase 3 traffic-phase classifier. SDK-free, provider-agnostic,
// table-driven heuristic. Independent of the user-aircraft
// flight_phase detector: traffic targets see only their own
// alt_agl / groundspeed / vertical_speed plus the phase we
// classified them as on the previous tick.
//
// Phases left as Unknown here (Climb, Cruise, Descend, Final, Pattern)
// are refined in Phase 4 once we add the geometry against the user's
// runway.
namespace traffic_phase_classifier {

// Pure function. `prev_phase` is the phase we classified the same
// target as on the previous update tick, or Unknown if this is the
// first time we have seen the modeS_id. The Landed rule is the only
// branch that consults prev_phase — every other branch is a pure
// function of the target's current dynamics.
traffic_context::TrafficPhase
classify(const traffic_context::TrafficTarget &target,
         traffic_context::TrafficPhase prev_phase);

} // namespace traffic_phase_classifier

#endif // TRAFFIC_PHASE_CLASSIFIER_HPP
