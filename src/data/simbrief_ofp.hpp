/*
 * xp_wellys_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#ifndef DATA_SIMBRIEF_OFP_HPP
#define DATA_SIMBRIEF_OFP_HPP

#include <string>

// SDK-free shared cache for the last successfully fetched SimBrief OFP.
// Written by simbrief_client (plugin-only) after a successful fetch;
// read by xplane_context_runtime to populate XPlaneContext IFR fields.
namespace simbrief_ofp {

struct OfpData {
  std::string origin_icao;       // e.g. "LFLP"
  std::string destination_icao;  // e.g. "LFMN"
  std::string sid_name;          // e.g. "MOBE2D" (empty if none filed)
  std::string fpl_first_fix;    // first waypoint after departure = last fix of SID, e.g. "AMIKI"
  int         cruise_alt_ft  = 0; // cruise altitude in feet (display only)
  std::string aircraft_reg;      // e.g. "N900SB"
  std::string aircraft_type;     // ICAO type code, e.g. "TBM9"
  long long   sched_off      = 0; // scheduled takeoff Unix timestamp (0 = unknown)
  bool        valid          = false;
};

void         set(const OfpData &ofp); // called from simbrief_client after fetch
OfpData      get();                   // called from xplane_context_runtime
void         clear();                 // call on new flight / user request

} // namespace simbrief_ofp

#endif // DATA_SIMBRIEF_OFP_HPP
