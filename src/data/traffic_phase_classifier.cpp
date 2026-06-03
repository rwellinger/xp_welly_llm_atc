/*
 * xp_wellys_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "data/traffic_phase_classifier.hpp"

namespace traffic_phase_classifier {

using traffic_context::TrafficPhase;

namespace {

// "Was the target airborne the last time we classified it?" — the only
// trigger for the Landed branch. Phase 4 will refine Climb / Cruise /
// Descend / Final / Pattern; until then they stay Unknown but we still
// recognise them in this check so the Landed rule kicks in cleanly if
// they ever feed through.
bool was_airborne(TrafficPhase prev) {
  switch (prev) {
  case TrafficPhase::Climb:
  case TrafficPhase::Cruise:
  case TrafficPhase::Descend:
  case TrafficPhase::Final:
  case TrafficPhase::Pattern:
    return true;
  default:
    return false;
  }
}

} // namespace

TrafficPhase classify(const traffic_context::TrafficTarget &target,
                      TrafficPhase prev_phase) {
  const double agl = target.alt_agl_ft;
  const double gs = target.groundspeed_kts;
  const double vs = target.vertical_speed_fpm;

  // Landed is order-sensitive: a target that was airborne on the prior
  // tick and is now on the ground stays "Landed" rather than collapsing
  // to OnGround/Taxi, so downstream advisories can treat it as a fresh
  // arrival.
  if (was_airborne(prev_phase) && agl < 50.0 && gs < 80.0)
    return TrafficPhase::Landed;

  if (agl < 50.0 && gs < 5.0)
    return TrafficPhase::OnGround;

  if (agl < 50.0 && gs >= 5.0 && gs < 40.0)
    return TrafficPhase::Taxi;

  if (agl < 200.0 && gs >= 40.0 && vs > 200.0)
    return TrafficPhase::Takeoff;

  return TrafficPhase::Unknown;
}

} // namespace traffic_phase_classifier
