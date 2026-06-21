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
#include <unordered_map>
#include <utility>
#include <vector>

namespace cifp_reader {

// Altitude result from CIFP lookup.
// feet == 0 means not found; caller falls back to ifr_defaults.
// is_fl == true means the CIFP expressed the altitude as "FL130" —
// format as "FL130" in the clearance. When false, express as "X feet".
struct CifpAlt {
  int feet = 0;
  bool is_fl = false;
};

// Most restrictive "at or above" minimum altitude found on any waypoint
// across all SIDs for a given runway. ATC must not assign a climb altitude
// below this value while the aircraft is still on the SID.
struct CifpBindingAlt {
  CifpAlt alt;          // highest minimum altitude (0 feet = no constraint)
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
CifpAlt initial_altitude(const std::string &cifp_dir, const std::string &icao,
                         const std::string &active_runway);

// Returns the last waypoint identifier on the named SID procedure
// (the fix with the highest sequence number). Used for ATC-initiated
// "direct {last_fix}" shortcuts during the SID climb.
// Returns empty string when the SID is not found in the CIFP file.
std::string sid_last_fix(const std::string &cifp_dir, const std::string &icao,
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
// (the first 3 characters of the FPL first fix, per ICAO SID naming
// convention). Searches all runways. Used as a secondary fallback when the
// exact last-fix match fails (e.g. fpl_first_fix="LTP" → finds SID "LTP2A").
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

// When the STAR serves ALL runways, pick the best approach runway.
// Scores each runway by: approach type priority > headwind alignment > L over R.
// wind_dir_true: meteorological wind direction in degrees true (-1 = unknown).
std::string best_runway_for_approach(const std::string &cifp_dir,
                                      const std::string &icao,
                                      float wind_dir_true,
                                      float visibility_m = 5000.0f);

// ── STAR waypoint constraints ─────────────────────────────────────────────

// One constrained waypoint along a STAR or approach transition.
// Only waypoints with at least one altitude or speed constraint are returned
// by star_waypoints() and approach_procedure_waypoints().
struct StarWaypoint {
  std::string ident;            // e.g. "TIPIK"
  CifpAlt     alt;              // altitude constraint (feet == 0 = none)
  bool        is_ceiling;       // true = at-or-below ("-")
  bool        is_floor;         // true = at-or-above ("+")
  int         speed_kt;         // max speed in kt (0 = no restriction)
  int         seq;              // CIFP sequence number (for ordering)
  bool        is_approach_proc; // true = from APPCH transition, not STAR
};

// Returns all waypoints in the named STAR that have an altitude or speed
// constraint, ordered by sequence number (entry fix first).
// Returns empty vector when the STAR is not found or CIFP is unavailable.
std::vector<StarWaypoint> star_waypoints(const std::string &cifp_dir,
                                          const std::string &icao,
                                          const std::string &star_name);

// Returns the last fix (highest sequence number) of the named STAR,
// regardless of whether it carries an altitude constraint.  This is
// the IAF that the STAR terminates at and the approach transition begins
// from (e.g. "MUS" for ABDI8R at LFMN).
// Returns empty string when the STAR is not found or CIFP is unavailable.
std::string star_last_fix(const std::string &cifp_dir,
                           const std::string &icao,
                           const std::string &star_name);

// Returns constrained waypoints from the approach procedure transition
// for approach_designator (e.g. "R04LZ"), matching the given
// transition_ident (e.g. "MUS" — the last STAR fix / IAF).
// FM (Fix-to-Manual = radar vectoring) and IF (Initial Fix / IAF entry)
// legs are excluded: FM has no fixed endpoint, IF duplicates the STAR end.
// Only waypoints with altitude constraints are returned, ordered by seq.
// Returns empty when the approach or transition is not found.
std::vector<StarWaypoint> approach_procedure_waypoints(
    const std::string &cifp_dir,
    const std::string &icao,
    const std::string &approach_designator,
    const std::string &transition_ident);

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

// Final Approach Fix (FAF) info for a specific approach procedure.
// ident: fix name (e.g. "FI04L").  lat/lon: from earth_fix.dat (0.0 if not
// found there).  alt_ft: altitude constraint from the CIFP APPCH record (0 if
// none).  All fields empty/0 when the approach or FAF is not found.
struct FafFix {
  std::string ident;
  double lat    = 0.0;
  double lon    = 0.0;
  int    alt_ft = 0;
};

// Finds the FAF for the given approach designator (e.g. "I04LY") by:
//   1. Reading the final-approach segment (route_type "I") of the CIFP file
//      for icao and finding the waypoint whose description 4th char is 'F'.
//   2. Looking up that ident + icao in earth_fix.dat (one directory above
//      cifp_dir) to get exact lat/lon.
// Returns an empty FafFix when approach or FAF is not found.
// Result is cached per (icao, designator) — call clear_cache() on airport change.
FafFix approach_faf(const std::string &cifp_dir,
                    const std::string &icao,
                    const std::string &approach_designator);

// Returns the alphabetically first STAR that serves dest_runway at icao.
// Falls back to any STAR if no runway-specific one is found.
// Used when the pilot's FPL does not contain a STAR entry fix so no navlog
// match is possible; this gives a plausible STAR name from CIFP alone.
// Returns empty when CIFP is unavailable or no STAR exists.
std::string first_star_for_runway(const std::string &cifp_dir,
                                   const std::string &icao,
                                   const std::string &dest_runway);

// Returns the landing runway served by a named STAR (e.g. "BORDI3L" → "04L").
// Returns empty when the STAR serves ALL runways, is not found, or CIFP is
// unavailable.  Used to obtain a runway for cifp_reader::best_approach().
std::string runway_for_star(const std::string &cifp_dir,
                             const std::string &icao,
                             const std::string &star_name);

// Clears the per-airport+runway result cache.  Call on airport change so a
// new airport's CIFP data is read fresh rather than returning stale results.
void clear_cache();

// Looks up lat/lon for a list of fix idents from earth_fix.dat (one directory
// above cifp_dir). When multiple entries share the same ident, the one whose
// airport field matches preferred_icao is preferred; otherwise the first match
// is returned.  Fixes not found in earth_fix.dat are absent from the result.
// The scan reads the file once and fills all requested idents in one pass.
std::unordered_map<std::string, std::pair<double, double>>
lookup_fix_positions(const std::string &cifp_dir,
                     const std::vector<std::string> &idents,
                     const std::string &preferred_icao);

} // namespace cifp_reader

#endif // DATA_CIFP_READER_HPP
