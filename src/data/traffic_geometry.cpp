/*
 * xp_wellys_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "data/traffic_geometry.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace traffic_geometry {

namespace {
constexpr double kDeg2Rad = M_PI / 180.0;
constexpr double kEarthRadiusM = 6371000.0;
constexpr double kMetersPerNm = 1852.0;

// Round a double to the nearest 100 ft and return as int.
int round_to_100(double v) {
  double r = std::round(v / 100.0) * 100.0;
  return static_cast<int>(r);
}
} // namespace

double bearing_deg(double lat1, double lon1, double lat2, double lon2) {
  double lat1r = lat1 * kDeg2Rad;
  double lat2r = lat2 * kDeg2Rad;
  double dlon = (lon2 - lon1) * kDeg2Rad;
  double y = std::sin(dlon) * std::cos(lat2r);
  double x = std::cos(lat1r) * std::sin(lat2r) -
             std::sin(lat1r) * std::cos(lat2r) * std::cos(dlon);
  double bearing = std::atan2(y, x) / kDeg2Rad;
  return std::fmod(bearing + 360.0, 360.0);
}

double distance_nm(double lat1, double lon1, double lat2, double lon2) {
  double dlat = (lat2 - lat1) * kDeg2Rad;
  double dlon = (lon2 - lon1) * kDeg2Rad;
  double a = std::sin(dlat / 2.0) * std::sin(dlat / 2.0) +
             std::cos(lat1 * kDeg2Rad) * std::cos(lat2 * kDeg2Rad) *
                 std::sin(dlon / 2.0) * std::sin(dlon / 2.0);
  double dist_m =
      kEarthRadiusM * 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
  return dist_m / kMetersPerNm;
}

double clock_position(double user_heading_deg, double target_bearing_deg) {
  double rel = std::fmod(target_bearing_deg - user_heading_deg + 720.0, 360.0);
  double hours = std::round(rel / 30.0);
  // 0 and 12 both refer to dead-ahead; collapse onto 12 so the value
  // stays in (0, 12] as documented.
  if (hours <= 0.0 || hours >= 12.0)
    return 12.0;
  return hours;
}

std::string classify_relative_track(double user_track_deg,
                                    double target_track_deg, double clock_pos) {
  // Track diff in [0, 360): 0 = same heading, 180 = opposite.
  double diff = std::fmod(target_track_deg - user_track_deg + 720.0, 360.0);

  // Opposite direction: diff in [150°, 210°].
  if (diff >= 150.0 && diff <= 210.0)
    return "opposite direction";

  // Same direction: diff <= 30° or diff >= 330°.
  if (diff <= 30.0 || diff >= 330.0)
    return "same direction";

  // Crossing left to right: diff in [60°, 120°] AND target on left side
  // (clock 9, 10, 11 — i.e. clock_pos in (6, 12)).
  if (diff >= 60.0 && diff <= 120.0 && clock_pos > 6.0 && clock_pos < 12.0)
    return "crossing left to right";

  // Crossing right to left: diff in [240°, 300°] AND target on right
  // side (clock 1, 2, 3 — i.e. clock_pos in (0, 6)).
  if (diff >= 240.0 && diff <= 300.0 && clock_pos > 0.0 && clock_pos < 6.0)
    return "crossing right to left";

  // Fallback: closure exists but the geometry doesn't fit a clean
  // category.
  return "converging";
}

std::string format_altitude_info(double target_alt_msl_ft,
                                 double user_alt_msl_ft, bool has_mode_c) {
  if (has_mode_c) {
    int alt = round_to_100(target_alt_msl_ft);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "indicating %d feet", alt);
    return buf;
  }

  double diff = target_alt_msl_ft - user_alt_msl_ft;
  if (std::fabs(diff) < 2000.0) {
    int n = round_to_100(std::fabs(diff));
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%d feet %s", n,
                  diff >= 0.0 ? "above" : "below");
    return buf;
  }

  return "altitude unknown";
}

} // namespace traffic_geometry
