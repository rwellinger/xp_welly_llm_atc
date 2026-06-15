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

// Returns the SID designator whose last waypoint (highest sequence number)
// matches fpl_first_fix — the first fix in the filed flight plan.
// When active_runway is non-empty, only SIDs for that runway are considered.
// When active_runway is empty, all runways at the airport are searched.
// Example: fpl_first_fix="ODIKI" → finds ODIK2A in the CIFP for the runway.
// Returns empty string when no matching SID is found or CIFP is absent.
std::string sid_name_for_last_fix(const std::string &cifp_dir,
                                   const std::string &icao,
                                   const std::string &active_runway,
                                   const std::string &fpl_first_fix);

// Returns the SID designator whose name starts with the given 3-letter prefix
// (the first 3 characters of the FPL first fix, per ICAO SID naming convention).
// Searches all runways. Used as a secondary fallback when the exact last-fix
// match fails (e.g. fpl_first_fix="LTP" → finds SID "LTP2A").
// Returns empty string when no matching SID is found or CIFP is absent.
std::string sid_name_for_fix_prefix(const std::string &cifp_dir,
                                     const std::string &icao,
                                     const std::string &prefix);

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

// ── Approach queries ───────────────────────────────────────────────────────

// Best available approach type and designator for the given destination runway.
// Used by en-route ATC when issuing the descent/STAR clearance:
// "expect ILS runway 04L" / "expect RNAV approach runway 22R"
// type_str is empty when no approach is found in the CIFP file.
struct ApproachInfo {
  std::string type_str;   // "ILS", "RNAV", "VOR DME", "VOR", "NDB", or ""
  std::string runway;     // e.g. "04L"
  std::string designator; // e.g. "I04LY"
};

// Returns the preferred approach for dest_runway given current visibility.
// Normal conditions (>=1500 m): RNAV/RNP preferred over ILS.
// Low visibility  (< 800 m):   ILS preferred (LVP operations).
// Intermediate    (800–1500 m): RNAV preferred, ILS as fallback.
// Below RNAV/ILS: Localizer > VOR/DME > VOR > NDB.
// dest_runway must be without "RW" prefix (e.g. "04L", "22R").
// visibility_m defaults to 5000 (normal VMC) when not supplied.
ApproachInfo best_approach(const std::string &cifp_dir,
                            const std::string &icao,
                            const std::string &dest_runway,
                            float visibility_m = 5000.0f);

// ── STAR queries ──────────────────────────────────────────────────────────

// Entry fix of a named STAR: the first waypoint (lowest sequence number).
// This is the fix the aircraft must cross to begin the arrival — the point
// used to compute TOD (top-of-descent) distance.
// alt.feet == 0 means no altitude constraint at the entry fix.
// is_ceiling == true means at-or-below (ATC clears descent to this FL).
struct StarEntryFix {
  std::string ident;       // e.g. "ABDIL"
  CifpAlt     alt;         // altitude at entry (0 = no constraint)
  bool        is_ceiling;  // true = at-or-below ("-"), false = at-or-above ("+") or exact
  std::string star_name;   // STAR designator, e.g. "ABDI8R"
};

// Returns the entry fix of the named STAR procedure in the CIFP file for
// icao.  Returns an empty StarEntryFix (ident="") when not found.
StarEntryFix star_entry_fix(const std::string &cifp_dir,
                             const std::string &icao,
                             const std::string &star_name);

// Finds the STAR name whose entry fix (lowest sequence number) matches
// entry_fix_ident.  Used when the SimBrief OFP does not supply a STAR name
// but the navlog's first STAR fix is known.
// dest_runway filters results (e.g. "04R"); pass empty to match ALL runways.
// Returns empty string when no matching STAR is found.
std::string star_name_for_entry_fix(const std::string &cifp_dir,
                                     const std::string &icao,
                                     const std::string &dest_runway,
                                     const std::string &entry_fix_ident);

// Clears the per-airport+runway result cache.  Call on airport change so a
// new airport's CIFP data is read fresh rather than returning stale results.
void clear_cache();

} // namespace cifp_reader

#endif // DATA_CIFP_READER_HPP
