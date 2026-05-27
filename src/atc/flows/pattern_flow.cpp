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

#include "atc/flows/pattern_flow.hpp"

namespace pattern_flow {

using atc_state_machine::ATCState;

const char *state_name(State s) {
  switch (s) {
  case State::DEPARTURE_CLEARED:
    return "Pattern/DEPARTURE_CLEARED";
  case State::PATTERN_ENTRY:
    return "Pattern/PATTERN_ENTRY";
  case State::LANDING_CLEARED:
    return "Pattern/LANDING_CLEARED";
  case State::TOUCH_AND_GO_CLEARED:
    return "Pattern/TOUCH_AND_GO_CLEARED";
  }
  return "Pattern/UNKNOWN";
}

State from_atc_state(ATCState s) {
  switch (s) {
  case ATCState::DEPARTURE_CLEARED:
    return State::DEPARTURE_CLEARED;
  case ATCState::PATTERN_ENTRY:
    return State::PATTERN_ENTRY;
  case ATCState::LANDING_CLEARED:
    return State::LANDING_CLEARED;
  case ATCState::TOUCH_AND_GO_CLEARED:
    return State::TOUCH_AND_GO_CLEARED;
  default:
    // Caller must gate with is_pattern_state(); fallback returns
    // DEPARTURE_CLEARED to keep the contract total.
    return State::DEPARTURE_CLEARED;
  }
}

ATCState to_atc_state(State s) {
  switch (s) {
  case State::DEPARTURE_CLEARED:
    return ATCState::DEPARTURE_CLEARED;
  case State::PATTERN_ENTRY:
    return ATCState::PATTERN_ENTRY;
  case State::LANDING_CLEARED:
    return ATCState::LANDING_CLEARED;
  case State::TOUCH_AND_GO_CLEARED:
    return ATCState::TOUCH_AND_GO_CLEARED;
  }
  return ATCState::DEPARTURE_CLEARED;
}

bool is_pattern_state(ATCState s) {
  switch (s) {
  case ATCState::DEPARTURE_CLEARED:
  case ATCState::PATTERN_ENTRY:
  case ATCState::LANDING_CLEARED:
  case ATCState::TOUCH_AND_GO_CLEARED:
    return true;
  default:
    return false;
  }
}

} // namespace pattern_flow
