#include "data/traffic_geometry.hpp"

#include <catch2/catch_amalgamated.hpp>

using Catch::Approx;

// LSZH (Zurich) and LSGG (Geneva) reference points. Distance and
// initial bearing are well-known great-circle values; the test allows
// ±1 NM / ±1° tolerance because the published runway lat/lon for each
// airport varies a few hundred metres between sources.
static constexpr double kLszhLat = 47.4583;
static constexpr double kLszhLon = 8.5483;
static constexpr double kLsggLat = 46.2381;
static constexpr double kLsggLon = 6.1090;

TEST_CASE("traffic_geometry::distance_nm: LSZH -> LSGG", "[traffic][geometry]") {
    // Great-circle distance LSZH (47.4583, 8.5483) to LSGG (46.2381, 6.1090)
    // is ~124 NM via haversine. Tolerance is wide enough that small
    // refinements to the reference lat/lon won't churn the test.
    const double d = traffic_geometry::distance_nm(kLszhLat, kLszhLon,
                                                   kLsggLat, kLsggLon);
    REQUIRE(d == Approx(124.0).margin(2.0));
}

TEST_CASE("traffic_geometry::bearing_deg: LSZH -> LSGG", "[traffic][geometry]") {
    const double b = traffic_geometry::bearing_deg(kLszhLat, kLszhLon,
                                                   kLsggLat, kLsggLon);
    REQUIRE(b == Approx(234.0).margin(1.5));
}

TEST_CASE("traffic_geometry::distance_nm: zero distance",
          "[traffic][geometry]") {
    REQUIRE(traffic_geometry::distance_nm(kLszhLat, kLszhLon, kLszhLat,
                                          kLszhLon) == Approx(0.0).margin(1e-6));
}

TEST_CASE("traffic_geometry::bearing_deg: result is in [0, 360)",
          "[traffic][geometry]") {
    // Point due south of LSZH should yield ~180 deg.
    const double b = traffic_geometry::bearing_deg(kLszhLat, kLszhLon,
                                                   kLszhLat - 1.0, kLszhLon);
    REQUIRE(b == Approx(180.0).margin(0.5));
    REQUIRE(b >= 0.0);
    REQUIRE(b < 360.0);
}

// The clock-position helper rounds to the nearest hour and collapses
// 0/12 onto 12 so the result stays in (0, 12]. Each row in the table
// pins down one of the edge cases described in the spec.
struct ClockCase {
    double heading;
    double bearing;
    double expected;
    const char *label;
};

TEST_CASE("traffic_geometry::clock_position table",
          "[traffic][geometry][clock]") {
    const ClockCase cases[] = {
        {360.0, 90.0, 3.0, "head=360 brg=090 -> 3 o'clock"},
        {180.0, 90.0, 9.0, "head=180 brg=090 -> 9 o'clock"},
        {0.0, 0.0, 12.0, "head=000 brg=000 -> 12 o'clock"},
        {350.0, 10.0, 1.0, "head=350 brg=010 -> 1 o'clock (wrap)"},
        {10.0, 350.0, 11.0, "head=010 brg=350 -> 11 o'clock (wrap)"},
        {0.0, 355.0, 12.0, "head=000 brg=355 -> 12 o'clock (just left)"},
        {0.0, 5.0, 12.0, "head=000 brg=005 -> 12 o'clock (just right)"},
        {270.0, 0.0, 3.0, "head=270 brg=000 -> 3 o'clock"},
        {90.0, 270.0, 6.0, "head=090 brg=270 -> 6 o'clock"},
    };
    for (const auto &c : cases) {
        INFO(c.label);
        REQUIRE(traffic_geometry::clock_position(c.heading, c.bearing) ==
                Approx(c.expected));
    }
}

TEST_CASE("traffic_geometry::clock_position: result is always in (0, 12]",
          "[traffic][geometry][clock]") {
    for (int hdg = 0; hdg < 360; hdg += 23) {
        for (int brg = 0; brg < 360; brg += 17) {
            const double c =
                traffic_geometry::clock_position(hdg, brg);
            REQUIRE(c > 0.0);
            REQUIRE(c <= 12.0);
        }
    }
}

// ── classify_relative_track ─────────────────────────────────────────────────

TEST_CASE("classify_relative_track: opposite direction band",
          "[traffic][geometry][classifier]") {
    // user_track = 0; target_track in [150, 210] -> opposite direction.
    // 12 o'clock keeps the result in the "opposite" branch (ahead of user).
    REQUIRE(traffic_geometry::classify_relative_track(0.0, 150.0, 12.0) ==
            "opposite direction");
    REQUIRE(traffic_geometry::classify_relative_track(0.0, 180.0, 12.0) ==
            "opposite direction");
    REQUIRE(traffic_geometry::classify_relative_track(0.0, 210.0, 12.0) ==
            "opposite direction");
    // Just outside the lower bound (149°) drops to "converging".
    REQUIRE(traffic_geometry::classify_relative_track(0.0, 149.0, 12.0) ==
            "converging");
    // Just outside the upper bound (211°) drops to "converging".
    REQUIRE(traffic_geometry::classify_relative_track(0.0, 211.0, 12.0) ==
            "converging");
}

TEST_CASE("classify_relative_track: same direction band",
          "[traffic][geometry][classifier]") {
    REQUIRE(traffic_geometry::classify_relative_track(0.0, 0.0, 12.0) ==
            "same direction");
    REQUIRE(traffic_geometry::classify_relative_track(0.0, 30.0, 12.0) ==
            "same direction");
    REQUIRE(traffic_geometry::classify_relative_track(0.0, 330.0, 12.0) ==
            "same direction");
    // Just outside the upper bound (31°) leaves the same-direction zone.
    REQUIRE(traffic_geometry::classify_relative_track(0.0, 31.0, 12.0) !=
            "same direction");
    // Just outside the lower bound (329°) likewise.
    REQUIRE(traffic_geometry::classify_relative_track(0.0, 329.0, 12.0) !=
            "same direction");
}

TEST_CASE("classify_relative_track: crossing left to right",
          "[traffic][geometry][classifier]") {
    // user_track = 0, target_track = 90 (perpendicular crossing) AND
    // target on the user's left side (clock 9, 10, 11) -> L->R.
    REQUIRE(traffic_geometry::classify_relative_track(0.0, 90.0, 10.0) ==
            "crossing left to right");
    REQUIRE(traffic_geometry::classify_relative_track(0.0, 60.0, 11.0) ==
            "crossing left to right");
    REQUIRE(traffic_geometry::classify_relative_track(0.0, 120.0, 9.5) ==
            "crossing left to right");
    // Same diff but target on the right side -> classifier rejects the
    // L->R label and falls through to "converging".
    REQUIRE(traffic_geometry::classify_relative_track(0.0, 90.0, 3.0) !=
            "crossing left to right");
}

TEST_CASE("classify_relative_track: crossing right to left",
          "[traffic][geometry][classifier]") {
    // user_track = 0, target_track = 270 -> diff = 270 (in [240, 300]) AND
    // target on the user's right side (clock 1, 2, 3) -> R->L.
    REQUIRE(traffic_geometry::classify_relative_track(0.0, 270.0, 2.0) ==
            "crossing right to left");
    REQUIRE(traffic_geometry::classify_relative_track(0.0, 240.0, 3.0) ==
            "crossing right to left");
    REQUIRE(traffic_geometry::classify_relative_track(0.0, 300.0, 1.0) ==
            "crossing right to left");
    // Same diff but target on the left -> falls through.
    REQUIRE(traffic_geometry::classify_relative_track(0.0, 270.0, 9.0) !=
            "crossing right to left");
}

TEST_CASE("classify_relative_track: fallback converging",
          "[traffic][geometry][classifier]") {
    // 45° track diff, target at 12 o'clock — neither perpendicular nor
    // a same/opposite case.
    REQUIRE(traffic_geometry::classify_relative_track(0.0, 45.0, 12.0) ==
            "converging");
}

// ── format_altitude_info ────────────────────────────────────────────────────

TEST_CASE("format_altitude_info: mode-c rounds to nearest 100 ft",
          "[traffic][geometry][altitude]") {
    REQUIRE(traffic_geometry::format_altitude_info(4530.0, 1500.0, true) ==
            "indicating 4500 feet");
    REQUIRE(traffic_geometry::format_altitude_info(4571.0, 1500.0, true) ==
            "indicating 4600 feet");
    REQUIRE(traffic_geometry::format_altitude_info(0.0, 1500.0, true) ==
            "indicating 0 feet");
}

TEST_CASE("format_altitude_info: relative within 2000 ft",
          "[traffic][geometry][altitude]") {
    // Target above by 1000 ft.
    REQUIRE(traffic_geometry::format_altitude_info(2500.0, 1500.0, false) ==
            "1000 feet above");
    // Target below by 1000 ft.
    REQUIRE(traffic_geometry::format_altitude_info(500.0, 1500.0, false) ==
            "1000 feet below");
    // Rounding to nearest 100 ft.
    REQUIRE(traffic_geometry::format_altitude_info(2530.0, 1500.0, false) ==
            "1000 feet above");
}

TEST_CASE("format_altitude_info: unknown when far apart and no mode-c",
          "[traffic][geometry][altitude]") {
    REQUIRE(traffic_geometry::format_altitude_info(5000.0, 1500.0, false) ==
            "altitude unknown");
    REQUIRE(traffic_geometry::format_altitude_info(1500.0, 5000.0, false) ==
            "altitude unknown");
}
