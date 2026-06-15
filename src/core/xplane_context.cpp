/*
 * xp_wellys_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * ── POD helpers only. All X-Plane SDK / apt.dat runtime code lives in
 *    xplane_context_runtime.cpp, which is only linked into the plugin.
 */

#include "core/xplane_context.hpp"

#include <cmath>

namespace xplane_context {

namespace {
// Priority ranking for resolving frequency type when multiple apt.dat entries
// share the same frequency (part-time towers like KVRB list 126.300 as both
// TOWER and UNICOM). Higher rank wins.
int priority_rank(FrequencyType t) {
  switch (t) {
  case FrequencyType::TOWER:
    return 7;
  case FrequencyType::GROUND:
    return 6;
  case FrequencyType::DELIVERY:
    return 5;
  case FrequencyType::APPROACH:
    return 4;
  case FrequencyType::DEPARTURE:
    return 4;
  case FrequencyType::ATIS:
    return 3;
  case FrequencyType::CTAF:
    return 2;
  case FrequencyType::UNICOM:
    return 1;
  case FrequencyType::UNKNOWN:
    return 0;
  }
  return 0;
}
} // namespace

bool AirportFrequencies::has(FrequencyType ft) const {
  for (const auto &f : all)
    if (f.type == ft)
      return true;
  return false;
}

float AirportFrequencies::first_mhz(FrequencyType ft) const {
  for (const auto &f : all)
    if (f.type == ft)
      return static_cast<float>(f.freq_khz) / 1000.0f;
  return 0.0f;
}

std::string AirportFrequencies::first_name(FrequencyType ft) const {
  for (const auto &f : all)
    if (f.type == ft)
      return f.name;
  return {};
}

FrequencyType AirportFrequencies::lookup(float freq_mhz) const {
  const uint32_t target = static_cast<uint32_t>(std::round(freq_mhz * 1000.0f));
  FrequencyType best = FrequencyType::UNKNOWN;
  int best_rank = -1;
  for (const auto &f : all) {
    const uint32_t diff =
        (target > f.freq_khz) ? target - f.freq_khz : f.freq_khz - target;
    if (diff > 1)
      continue;
    const int rank = priority_rank(f.type);
    if (rank > best_rank) {
      best_rank = rank;
      best = f.type;
    }
  }
  return best;
}

bool AirportFrequencies::has_ground() const {
  return has(FrequencyType::GROUND);
}

const char *frequency_type_name(FrequencyType ft) {
  switch (ft) {
  case FrequencyType::UNKNOWN:
    return "Unknown";
  case FrequencyType::DELIVERY:
    return "Delivery";
  case FrequencyType::GROUND:
    return "Ground";
  case FrequencyType::TOWER:
    return "Tower";
  case FrequencyType::APPROACH:
    return "Approach";
  case FrequencyType::DEPARTURE:
    return "Departure";
  case FrequencyType::UNICOM:
    return "Unicom";
  case FrequencyType::CTAF:
    return "CTAF";
  case FrequencyType::ATIS:
    return "ATIS";
  }
  return "Unknown";
}

} // namespace xplane_context
