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

#include "xplane_context.hpp"

#include <cmath>

namespace xplane_context {

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

FrequencyType AirportFrequencies::lookup(float freq_mhz) const {
  auto to_khz = [](float mhz) -> uint32_t {
    return static_cast<uint32_t>(std::round(mhz * 1000.0f));
  };
  uint32_t target = to_khz(freq_mhz);
  for (const auto &f : all) {
    uint32_t diff =
        (target > f.freq_khz) ? target - f.freq_khz : f.freq_khz - target;
    if (diff <= 1)
      return f.type;
  }
  return FrequencyType::UNKNOWN;
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
