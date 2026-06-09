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
// If no SID is found for active_runway, the reciprocal runway is tried as a
// fallback (e.g. calm-wind active="04" but SIDs only coded for "22").
//
// Returns {0, false} when:
//   - cifp_dir is empty (headless / atc_repl)
//   - the CIFP file doesn't exist for this airport
//   - the airport has no SIDs (general aviation, uncontrolled)
//   - no SID with an altitude constraint is found for any runway
CifpAlt initial_altitude(const std::string &cifp_dir,
                         const std::string &icao,
                         const std::string &active_runway);

// Returns the runway designator (without "RW" prefix, e.g. "22") that has the
// most SID procedures in the CIFP file for the given airport.  Used by the
// runway selection logic so calm-wind departures prefer the procedural runway.
// Returns empty string if the CIFP file is absent or has no SIDs.
std::string preferred_departure_runway(const std::string &cifp_dir,
                                       const std::string &icao);

// Clears the per-airport+runway result cache.  Call on airport change so a
// new airport's CIFP data is read fresh rather than returning stale results.
void clear_cache();

} // namespace cifp_reader

#endif // DATA_CIFP_READER_HPP
