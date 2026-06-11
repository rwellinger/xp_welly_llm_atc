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

// Most restrictive "at or above" minimum altitude found on any waypoint
// across all SIDs for a given runway. ATC must not assign a climb altitude
// below this value while the aircraft is still on the SID.
struct CifpBindingAlt {
  CifpAlt     alt;      // highest minimum altitude (0 feet = no constraint)
  std::string waypoint; // waypoint identifier where this constraint occurs
  std::string sid;      // SID procedure designator where this occurs
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

// Returns the last waypoint identifier on the named SID procedure
// (the fix with the highest sequence number). Used for ATC-initiated
// "direct {last_fix}" shortcuts during the SID climb.
// Returns empty string when the SID is not found in the CIFP file.
std::string sid_last_fix(const std::string &cifp_dir,
                          const std::string &icao,
                          const std::string &sid_name);

// Returns the SID designator for the active runway whose last waypoint
// (highest sequence number) matches fpl_first_fix — the first fix in the
// filed flight plan, which is always the exit fix of the SID assigned by ATC.
// Example: fpl_first_fix="ODIKI" → finds ODIK2A in the CIFP for the runway.
// Returns empty string when no matching SID is found or CIFP is absent.
std::string sid_name_for_last_fix(const std::string &cifp_dir,
                                   const std::string &icao,
                                   const std::string &active_runway,
                                   const std::string &fpl_first_fix);

// Returns the SID designator assigned for the active departure runway.
// The first (alphabetically lowest) SID in the CIFP file for that runway
// is returned as the representative ATC-assigned SID name. Used as a fallback
// when the FPL first fix is not known. Returns empty string when no SID exists.
std::string sid_name_for_runway(const std::string &cifp_dir,
                                 const std::string &icao,
                                 const std::string &active_runway);

// Returns the most restrictive "at or above" minimum altitude across ALL SID
// waypoints for the given runway. Departure/Approach must not re-clear the
// aircraft below this altitude while still on the SID.
// Example: LFLP RW22 ODIK2A has FL130 at LP610 and FL150 at ODIKI — returns
// {FL150, "ODIKI", "ODIK2A"}. Returns {{0,false},"",""} when no constraint.
CifpBindingAlt sid_binding_altitude(const std::string &cifp_dir,
                                     const std::string &icao,
                                     const std::string &active_runway);

// Returns the runway designator (without "RW" prefix, e.g. "22") that has the
// most SID procedures in the CIFP file for the given airport.  Used by the
// runway selection logic so calm-wind departures prefer the procedural runway.
// Returns empty string if the CIFP file is absent or has no SIDs.
std::string preferred_departure_runway(const std::string &cifp_dir,
                                       const std::string &icao);

// Returns true if the named SID procedure exists in the CIFP file for the
// given runway (e.g. validates a SimBrief-supplied SID before use).
// Returns false when the CIFP file is absent — caller should keep the SID
// as-is (no validation possible).  When the file exists but the SID is not
// listed for active_runway, returns false (SID should be discarded).
bool is_sid_valid_for_runway(const std::string &cifp_dir,
                              const std::string &icao,
                              const std::string &sid_name,
                              const std::string &active_runway);

// Clears the per-airport+runway result cache.  Call on airport change so a
// new airport's CIFP data is read fresh rather than returning stale results.
void clear_cache();

} // namespace cifp_reader

#endif // DATA_CIFP_READER_HPP
