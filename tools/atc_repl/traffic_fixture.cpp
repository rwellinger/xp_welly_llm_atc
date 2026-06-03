/*
 * xp_wellys_atc - headless CLI shared with Catch2 tests.
 */

#include "traffic_fixture.hpp"

#include "data/traffic_geometry.hpp"
#include "data/traffic_phase_classifier.hpp"

#include <json.hpp>

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <string>

namespace traffic_fixture {

using json = nlohmann::json;

namespace {

traffic_context::WakeCategory parse_wake(const std::string &s) {
  using W = traffic_context::WakeCategory;
  if (s == "Light")
    return W::Light;
  if (s == "Medium")
    return W::Medium;
  if (s == "Heavy")
    return W::Heavy;
  if (s == "Super")
    return W::Super;
  return W::Unknown;
}

} // namespace

LoadedFixture load(const std::string &path) {
  std::ifstream in(path);
  if (!in.good()) {
    throw std::runtime_error("could not open traffic fixture: " + path);
  }
  json doc;
  try {
    in >> doc;
  } catch (const std::exception &e) {
    throw std::runtime_error("invalid JSON in traffic fixture " + path + ": " +
                             e.what());
  }

  if (!doc.contains("user") || !doc.contains("targets")) {
    throw std::runtime_error("traffic fixture " + path +
                             " must contain `user` and `targets`");
  }

  LoadedFixture out;
  const auto &u = doc["user"];
  out.user.lat = u.value("lat", 0.0);
  out.user.lon = u.value("lon", 0.0);
  out.user.alt_msl_ft = u.value("alt_msl_ft", 0.0);
  out.user.heading_true = u.value("heading_true", 0.0);
  out.user.nearest_airport_id =
      u.value("nearest_airport_id", std::string{});
  out.user.airport_elevation_ft = u.value("airport_elevation_ft", 0.0);

  constexpr double kMaxRangeNm = 40.0;

  for (const auto &t : doc["targets"]) {
    traffic_context::TrafficTarget target;
    target.callsign = t.value("callsign", std::string{});
    target.icao_type = t.value("icao_type", std::string{});
    target.modeS_id = t.value("modeS_id", static_cast<uint32_t>(0));
    target.lat = t.value("lat", 0.0);
    target.lon = t.value("lon", 0.0);
    target.alt_msl_ft = t.value("alt_msl_ft", 0.0);
    target.track_deg = t.value("track_deg", 0.0);
    target.groundspeed_kts = t.value("groundspeed_kts", 0.0);
    target.vertical_speed_fpm = t.value("vertical_speed_fpm", 0.0);
    target.wake = parse_wake(t.value("wake", std::string{"Unknown"}));

    target.distance_to_user_nm = traffic_geometry::distance_nm(
        out.user.lat, out.user.lon, target.lat, target.lon);
    if (target.distance_to_user_nm > kMaxRangeNm) {
      continue; // mirrors live runtime cutoff
    }
    target.bearing_from_user_deg = traffic_geometry::bearing_deg(
        out.user.lat, out.user.lon, target.lat, target.lon);
    target.clock_position = traffic_geometry::clock_position(
        out.user.heading_true, target.bearing_from_user_deg);
    target.altitude_diff_ft = target.alt_msl_ft - out.user.alt_msl_ft;
    target.alt_agl_ft = target.alt_msl_ft - out.user.airport_elevation_ft;
    if (target.alt_agl_ft < 0.0)
      target.alt_agl_ft = 0.0;
    // Fixture-based runs have no prior tick history, so prev_phase is
    // always Unknown — this matches a freshly-seen target in the live
    // runtime reader.
    target.phase = traffic_phase_classifier::classify(
        target, traffic_context::TrafficPhase::Unknown);

    out.snapshot.targets.push_back(std::move(target));
  }

  std::sort(out.snapshot.targets.begin(), out.snapshot.targets.end(),
            [](const traffic_context::TrafficTarget &a,
               const traffic_context::TrafficTarget &b) {
              return a.distance_to_user_nm < b.distance_to_user_nm;
            });

  // Deterministic timestamp — never time-of-day-dependent, since the
  // snapshot is dumped verbatim and tests compare byte-for-byte.
  out.snapshot.last_update_secs = 0.0;

  return out;
}

} // namespace traffic_fixture
