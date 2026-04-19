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

#ifndef FLIGHT_PHASE_HPP
#define FLIGHT_PHASE_HPP

#include "xplane_context.hpp"

#include <map>
#include <string>
#include <vector>

namespace flight_phase {

enum class FlightPhase {
  PARKED,
  GROUND_READY,
  TAXI,
  TAKEOFF_ROLL,
  CLIMB,
  PATTERN,
  FINAL_APPROACH,
  LANDING_ROLL,
  CRUISE,
};

struct IntentPrecondition {
  std::vector<FlightPhase> allowed_phases;
  std::string rejection_parked;
  std::string rejection_airborne;
  std::string rejection_ground;
};

struct AutoCorrection {
  std::vector<FlightPhase> phases;
  std::string next_state;
  float delay_sec = 3.0f;
};

struct FrequencyRule {
  std::vector<std::string> allowed; // FrequencyType names: GROUND, TOWER, etc.
  std::string rejection;            // rendered when current freq not allowed
};

void init();
void stop();
void update(const xplane_context::XPlaneContext &ctx, float dt);
void reload();

FlightPhase get();
const char *phase_name(FlightPhase phase);
bool is_airborne(FlightPhase phase);
bool is_on_ground(FlightPhase phase);

// Precondition check: returns empty string if allowed, rejection message if not
std::string check_precondition(const std::string &intent_key,
                               FlightPhase phase);

// Auto-correction lookup for a given ATC state
const std::map<std::string, AutoCorrection> *
get_auto_corrections(const std::string &atc_state);

FlightPhase phase_from_name(const std::string &name);

// Check if intent is valid for the current COM frequency type
bool is_intent_valid_for_frequency(const std::string &intent_key,
                                   xplane_context::FrequencyType freq_type);

// Frequency-precondition check: returns empty string if allowed, rejection
// message template if the intent is not permitted on the current frequency.
std::string
check_frequency_precondition(const std::string &intent_key,
                             xplane_context::FrequencyType freq_type);

// Get pilot phraseology template for an intent (for UI helper text)
std::string get_pilot_phraseology(const std::string &intent_key);

} // namespace flight_phase

#endif // FLIGHT_PHASE_HPP
