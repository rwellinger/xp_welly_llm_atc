// Catch2 unit tests for the phraseology-hints matrix lookup.
//
// Drives the JSON-backed matrix in data/regions/<region>/
// phraseology_hints.json. Each TEST_CASE represents one row of the
// approved State x Phase x Facility x Frequency matrix and pins the
// expected hint set so future JSON edits surface as test failures.

#include "atc/phraseology_hints.hpp"
#include "core/xplane_context.hpp"

#include <catch2/catch_amalgamated.hpp>

#include <algorithm>

using atc_state_machine::ATCState;
using flight_phase::FlightPhase;
using xplane_context::FrequencyType;

namespace {

// settings::region_data_dir() in the test stub resolves XP_ATC_DATA_DIR
// or falls back to ./data — both point at the live region JSON files,
// so ::init() loads the same matrix the plugin uses.
struct LoadGuard {
  LoadGuard() { phraseology_hints::init(); }
  ~LoadGuard() { phraseology_hints::stop(); }
};

bool contains(const std::vector<std::string> &v, const std::string &needle) {
  return std::find(v.begin(), v.end(), needle) != v.end();
}

phraseology_hints::HintQuery make_query(ATCState state, FlightPhase phase,
                                        bool is_towered, FrequencyType freq,
                                        bool tower_only = false,
                                        bool post_landing = false) {
  phraseology_hints::HintQuery q{};
  q.state = state;
  q.phase = phase;
  q.is_towered = is_towered;
  q.frequency_type = freq;
  q.tower_only = tower_only;
  q.post_landing = post_landing;
  return q;
}

} // namespace

TEST_CASE("phraseology_hints: cold start IDLE+PARKED on Ground",
          "[phraseology_hints]") {
  LoadGuard g;
  auto q = make_query(ATCState::IDLE, FlightPhase::PARKED, /*is_towered=*/true,
                      FrequencyType::GROUND);
  auto hints = phraseology_hints::lookup(q);
  REQUIRE(contains(hints, "INITIAL_CALL_GROUND"));
  REQUIRE(contains(hints, "RADIO_CHECK"));
  REQUIRE_FALSE(contains(hints, "READY_FOR_DEPARTURE"));
  REQUIRE_FALSE(contains(hints, "REQUEST_TAXI_PARKING"));
}

TEST_CASE("phraseology_hints: tower-only IDLE+PARKED on Tower",
          "[phraseology_hints]") {
  LoadGuard g;
  auto q = make_query(ATCState::IDLE, FlightPhase::PARKED, /*is_towered=*/true,
                      FrequencyType::TOWER, /*tower_only=*/true);
  auto hints = phraseology_hints::lookup(q);
  REQUIRE(contains(hints, "INITIAL_CALL_GROUND"));
  REQUIRE_FALSE(contains(hints, "READY_FOR_DEPARTURE"));
}

TEST_CASE("phraseology_hints: 2-tier IDLE+PARKED on Tower yields no hints",
          "[phraseology_hints]") {
  LoadGuard g;
  auto q = make_query(ATCState::IDLE, FlightPhase::PARKED, /*is_towered=*/true,
                      FrequencyType::TOWER, /*tower_only=*/false);
  auto hints = phraseology_hints::lookup(q);
  REQUIRE(hints.empty());
}

TEST_CASE("phraseology_hints: post-landing on Ground -> taxi to parking",
          "[phraseology_hints]") {
  LoadGuard g;
  auto q = make_query(ATCState::IDLE, FlightPhase::TAXI, /*is_towered=*/true,
                      FrequencyType::GROUND, /*tower_only=*/false,
                      /*post_landing=*/true);
  auto hints = phraseology_hints::lookup(q);
  REQUIRE(contains(hints, "REQUEST_TAXI_PARKING"));
  REQUIRE_FALSE(contains(hints, "INITIAL_CALL_GROUND"));
  REQUIRE_FALSE(contains(hints, "REQUEST_TAXI"));
}

TEST_CASE("phraseology_hints: GROUND_CONTACT awaiting taxi clearance",
          "[phraseology_hints]") {
  LoadGuard g;
  auto q = make_query(ATCState::GROUND_CONTACT, FlightPhase::PARKED,
                      /*is_towered=*/true, FrequencyType::GROUND);
  auto hints = phraseology_hints::lookup(q);
  REQUIRE(contains(hints, "REQUEST_TAXI"));
}

TEST_CASE("phraseology_hints: TOWER_CONTACT at the holding point",
          "[phraseology_hints]") {
  LoadGuard g;
  auto q = make_query(ATCState::TOWER_CONTACT, FlightPhase::TAXI,
                      /*is_towered=*/true, FrequencyType::TOWER);
  auto hints = phraseology_hints::lookup(q);
  REQUIRE(contains(hints, "READY_FOR_DEPARTURE"));
  REQUIRE(contains(hints, "READY_FOR_DEPARTURE_VFR"));
}

TEST_CASE("phraseology_hints: PATTERN_ENTRY in the pattern",
          "[phraseology_hints]") {
  LoadGuard g;
  auto q = make_query(ATCState::PATTERN_ENTRY, FlightPhase::PATTERN,
                      /*is_towered=*/true, FrequencyType::TOWER);
  auto hints = phraseology_hints::lookup(q);
  REQUIRE(contains(hints, "REPORT_POSITION_DOWNWIND"));
  REQUIRE(contains(hints, "REQUEST_LANDING"));
  REQUIRE(contains(hints, "GO_AROUND"));
}

TEST_CASE("phraseology_hints: tower-only post-landing offers RUNWAY_VACATED + REQUEST_TAXI_PARKING",
          "[phraseology_hints]") {
  LoadGuard g;
  // LSZB-style: tower_only field, pilot just touched down and is
  // taxiing clear on the Tower freq (no Ground exists). The same
  // controller handles taxi-in, so REQUEST_TAXI_PARKING must stay
  // available alongside RUNWAY_VACATED.
  auto q = make_query(ATCState::IDLE, FlightPhase::TAXI, /*is_towered=*/true,
                      FrequencyType::TOWER, /*tower_only=*/true,
                      /*post_landing=*/true);
  auto hints = phraseology_hints::lookup(q);
  REQUIRE(contains(hints, "RUNWAY_VACATED"));
  REQUIRE(contains(hints, "REQUEST_TAXI_PARKING"));
}

TEST_CASE("phraseology_hints: 2-tier post-landing on Tower only offers RUNWAY_VACATED",
          "[phraseology_hints]") {
  LoadGuard g;
  // LSZG-style: 2-tier field, pilot still on Tower freq after touchdown.
  // Pilot must vacate, then retune to Ground for taxi-in — taxi
  // hints belong to the Ground rule, not here.
  auto q = make_query(ATCState::IDLE, FlightPhase::TAXI, /*is_towered=*/true,
                      FrequencyType::TOWER, /*tower_only=*/false,
                      /*post_landing=*/true);
  auto hints = phraseology_hints::lookup(q);
  REQUIRE(contains(hints, "RUNWAY_VACATED"));
  REQUIRE_FALSE(contains(hints, "REQUEST_TAXI_PARKING"));
}

TEST_CASE("phraseology_hints: LANDING_CLEARED rolling out -> RUNWAY_VACATED",
          "[phraseology_hints]") {
  LoadGuard g;
  auto q = make_query(ATCState::LANDING_CLEARED, FlightPhase::LANDING_ROLL,
                      /*is_towered=*/true, FrequencyType::TOWER);
  auto hints = phraseology_hints::lookup(q);
  REQUIRE(contains(hints, "RUNWAY_VACATED"));
}

TEST_CASE("phraseology_hints: uncontrolled IDLE on CTAF -> self announce",
          "[phraseology_hints]") {
  LoadGuard g;
  auto q = make_query(ATCState::IDLE, FlightPhase::PARKED, /*is_towered=*/false,
                      FrequencyType::CTAF);
  auto hints = phraseology_hints::lookup(q);
  REQUIRE(contains(hints, "SELF_ANNOUNCE"));
  REQUIRE_FALSE(contains(hints, "INITIAL_CALL_GROUND"));
}

TEST_CASE("phraseology_hints: EN_ROUTE without relevant freq is empty",
          "[phraseology_hints]") {
  LoadGuard g;
  auto q = make_query(ATCState::EN_ROUTE, FlightPhase::CRUISE,
                      /*is_towered=*/true, FrequencyType::UNKNOWN);
  auto hints = phraseology_hints::lookup(q);
  REQUIRE(hints.empty());
}

TEST_CASE("phraseology_hints: lookup before init returns empty",
          "[phraseology_hints]") {
  // Explicitly stop in case a previous test left rules loaded.
  phraseology_hints::stop();
  auto q = make_query(ATCState::IDLE, FlightPhase::PARKED, /*is_towered=*/true,
                      FrequencyType::GROUND);
  REQUIRE(phraseology_hints::lookup(q).empty());
}
