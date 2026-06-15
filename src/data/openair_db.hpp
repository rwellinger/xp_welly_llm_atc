/*
 * xp_wellys_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef OPENAIR_DB_HPP
#define OPENAIR_DB_HPP

#include <string>

namespace openair_db {

// Airspace class parsed from the OpenAir "AC" record.
enum class AirspaceClass {
    CTR,   // Control Zone          — AC CTR
    TMA,   // Terminal Maneuvering  — AC TMA
    CTA,   // Control Area          — AC CTA
    FIR,   // Flight Info Region    — AC FIR
    UIR,   // Upper Info Region     — AC UIR
    OTHER, // everything else (R, P, Q, D, E, F, G …)
};

// Result of find_enclosing(). When the position is outside all indexed
// airspaces, ac_class == OTHER and name is empty.
struct AirspaceEntry {
    std::string  name;
    AirspaceClass ac_class  = AirspaceClass::OTHER;
    int          floor_ft   = 0;
    int          ceiling_ft = 0;
};

// Parse CTR / TMA / CTA / FIR / UIR entries from an OpenAir-format airspace
// file (e.g. X-Plane "Custom Data/airspaces/airspace.txt").
// Pass an empty path to disable (headless tools, no Custom Data).
void init(std::string path);
void stop();

// Returns true once init() has finished (success or file-absent).
bool ready();

// Returns the innermost (smallest bounding-box area) airspace that contains
// (lat, lon, alt_ft) in 3-D: 2-D point-in-polygon AND floor_ft <= alt_ft
// <= ceiling_ft.  Returns an OTHER entry when the position is outside all
// indexed airspaces.
AirspaceEntry find_enclosing(double lat, double lon, int alt_ft);

// Backward-compat wrapper: returns ceiling of the CTR at (lat, lon)
// ignoring altitude. Returns 0 if not inside any CTR.
int ctr_ceiling_ft(double lat, double lon);

} // namespace openair_db

#endif // OPENAIR_DB_HPP
