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

#ifndef DATA_CIFP_READER_HPP
#define DATA_CIFP_READER_HPP

#include <string>

namespace cifp_reader {

// Altitude result from CIFP lookup.
// feet == 0 means not found; caller falls back to ifr_defaults.
// is_fl == true means the CIFP expressed the altitude as "FL130" —
// format as "FL130" in the clearance. When false, express as "X feet".
struct CifpAlt {
  int  feet  = 0;
  bool is_fl = false;
};

// Returns the initial climb altitude from the first SID waypoint for
// the given runway, parsed from <cifp_dir>/<icao>.dat.
//
// Returns {0, false} when:
//   - cifp_dir is empty (headless / atc_repl)
//   - the CIFP file doesn't exist for this airport
//   - the airport has no SIDs (general aviation, uncontrolled)
//   - no SID with an altitude constraint is found for the active runway
CifpAlt initial_altitude(const std::string &cifp_dir,
                         const std::string &icao,
                         const std::string &active_runway);

} // namespace cifp_reader

#endif // DATA_CIFP_READER_HPP
