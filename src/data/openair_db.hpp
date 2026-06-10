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

// Parse CTR entries from an OpenAir-format airspace file (e.g. X-Plane
// "Custom Data/airspaces/airspace.txt"). Only AC CTR records are indexed.
// Pass an empty path to disable (headless tools, no Custom Data).
void init(std::string path);
void stop();

// Returns true once init() has finished (success or file-absent).
bool ready();

// Returns the ceiling of the CTR that contains (lat, lon) in feet MSL.
// Uses ray-casting point-in-polygon against the parsed DP polygon.
// Returns 0 if (lat, lon) is not inside any indexed CTR.
int ctr_ceiling_ft(double lat, double lon);

} // namespace openair_db

#endif // OPENAIR_DB_HPP
