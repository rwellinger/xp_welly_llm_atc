#include "data/traffic_context.hpp"
#include "data/traffic_phase_classifier.hpp"

#include <catch2/catch_amalgamated.hpp>

using traffic_context::TrafficPhase;
using traffic_context::TrafficTarget;
using traffic_phase_classifier::classify;

namespace {

// Minimal builder — the classifier only reads alt_agl / groundspeed /
// vertical_speed, so everything else stays at its struct default.
TrafficTarget make(double agl, double gs, double vs) {
  TrafficTarget t;
  t.alt_agl_ft = agl;
  t.groundspeed_kts = gs;
  t.vertical_speed_fpm = vs;
  return t;
}

} // namespace

TEST_CASE("classifier: OnGround at idle thresholds", "[traffic][classifier]") {
  REQUIRE(classify(make(0.0, 0.0, 0.0), TrafficPhase::Unknown) ==
          TrafficPhase::OnGround);
  REQUIRE(classify(make(20.0, 4.9, 0.0), TrafficPhase::Unknown) ==
          TrafficPhase::OnGround);
  // 5 kts is the OnGround/Taxi boundary, classified as Taxi.
  REQUIRE(classify(make(20.0, 5.0, 0.0), TrafficPhase::Unknown) ==
          TrafficPhase::Taxi);
}

TEST_CASE("classifier: Taxi between 5 and 40 kts", "[traffic][classifier]") {
  REQUIRE(classify(make(20.0, 5.0, 0.0), TrafficPhase::Unknown) ==
          TrafficPhase::Taxi);
  REQUIRE(classify(make(20.0, 25.0, 0.0), TrafficPhase::Unknown) ==
          TrafficPhase::Taxi);
  REQUIRE(classify(make(20.0, 39.0, 0.0), TrafficPhase::Unknown) ==
          TrafficPhase::Taxi);
  // 40 kts on ground without climb -> Unknown (not Takeoff: no vs > 200)
  REQUIRE(classify(make(20.0, 40.0, 0.0), TrafficPhase::Unknown) ==
          TrafficPhase::Unknown);
}

TEST_CASE("classifier: Takeoff requires speed + climb",
          "[traffic][classifier]") {
  // 50 kts + climbing at 500 fpm under 200 ft AGL -> Takeoff.
  REQUIRE(classify(make(100.0, 50.0, 500.0), TrafficPhase::Unknown) ==
          TrafficPhase::Takeoff);
  REQUIRE(classify(make(180.0, 80.0, 300.0), TrafficPhase::Unknown) ==
          TrafficPhase::Takeoff);
  // Same speed but level -> Unknown (vs not > 200)
  REQUIRE(classify(make(100.0, 50.0, 0.0), TrafficPhase::Unknown) ==
          TrafficPhase::Unknown);
  // Same speed but already above 200 ft AGL -> Unknown (Phase 4 will refine)
  REQUIRE(classify(make(200.0, 50.0, 500.0), TrafficPhase::Unknown) ==
          TrafficPhase::Unknown);
}

TEST_CASE("classifier: Landed needs prior airborne phase",
          "[traffic][classifier]") {
  // Came from Final, now on the rollout under 80 kts -> Landed.
  REQUIRE(classify(make(20.0, 60.0, 0.0), TrafficPhase::Final) ==
          TrafficPhase::Landed);
  REQUIRE(classify(make(20.0, 60.0, 0.0), TrafficPhase::Pattern) ==
          TrafficPhase::Landed);
  REQUIRE(classify(make(20.0, 60.0, 0.0), TrafficPhase::Cruise) ==
          TrafficPhase::Landed);
  REQUIRE(classify(make(20.0, 60.0, 0.0), TrafficPhase::Descend) ==
          TrafficPhase::Landed);
  REQUIRE(classify(make(20.0, 60.0, 0.0), TrafficPhase::Climb) ==
          TrafficPhase::Landed);
  // Came from OnGround (just sitting), now Taxi-speed -> Taxi, not Landed.
  REQUIRE(classify(make(20.0, 10.0, 0.0), TrafficPhase::OnGround) ==
          TrafficPhase::Taxi);
  // Came from Taxi at slow speed -> still Taxi.
  REQUIRE(classify(make(20.0, 10.0, 0.0), TrafficPhase::Taxi) ==
          TrafficPhase::Taxi);
}

TEST_CASE("classifier: Landed bounded by 80 kts ceiling",
          "[traffic][classifier]") {
  REQUIRE(classify(make(20.0, 79.0, 0.0), TrafficPhase::Final) ==
          TrafficPhase::Landed);
  // 80 kts on the rollout would still be a roll, but the rule's
  // strict-less-than makes 80 fall through to Unknown until Phase 4
  // refines the airborne-state classifiers.
  REQUIRE(classify(make(20.0, 80.0, 0.0), TrafficPhase::Final) ==
          TrafficPhase::Unknown);
}

TEST_CASE("classifier: AGL ceiling at 50 ft for ground phases",
          "[traffic][classifier]") {
  // 49 AGL + 10 kts -> Taxi
  REQUIRE(classify(make(49.0, 10.0, 0.0), TrafficPhase::Unknown) ==
          TrafficPhase::Taxi);
  // 50 AGL + 10 kts -> Unknown (no longer a ground phase)
  REQUIRE(classify(make(50.0, 10.0, 0.0), TrafficPhase::Unknown) ==
          TrafficPhase::Unknown);
}

TEST_CASE("classifier: airborne-pending phases stay Unknown for now",
          "[traffic][classifier]") {
  // Cruise altitude, level, fast — Phase 4 refines this to Cruise.
  // For Phase 3 the classifier returns Unknown.
  REQUIRE(classify(make(5000.0, 120.0, 0.0), TrafficPhase::Unknown) ==
          TrafficPhase::Unknown);
  // Descending below pattern but still above 200 ft -> Unknown.
  REQUIRE(classify(make(800.0, 90.0, -500.0), TrafficPhase::Unknown) ==
          TrafficPhase::Unknown);
}
