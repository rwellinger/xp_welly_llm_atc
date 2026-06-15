/*
 * Unit tests for cifp_reader — SID selection, initial altitude, last-fix lookup.
 * Uses a minimal CIFP fixture at tests/fixtures/cifp/LFLP.dat containing:
 *   BULO2A (last fix: BULOS), ODIK2A (last fix: ODIKI),
 *   LTP2A  (last fix: LTPNO), ROMA2A (last fix: ROMAM)
 *   All on RW22. Initial altitude 6500 ft (CF record at seq 010).
 */

#include "data/cifp_reader.hpp"

#include <catch2/catch_amalgamated.hpp>
#include <string>

// Fixture directory injected by CMake.
#ifndef XP_WELLYS_ATC_TEST_FIXTURES_DIR
#define XP_WELLYS_ATC_TEST_FIXTURES_DIR "tests/fixtures"
#endif

static const std::string kCifpDir =
    std::string(XP_WELLYS_ATC_TEST_FIXTURES_DIR) + "/cifp";

// Clear the reader cache between test cases so each test is independent.
static void reset() { cifp_reader::clear_cache(); }

// ── initial_altitude ──────────────────────────────────────────────────

TEST_CASE("cifp: initial_altitude returns 6500 ft for LFLP RW22", "[cifp][lflp]") {
  reset();
  auto alt = cifp_reader::initial_altitude(kCifpDir, "LFLP", "22");
  REQUIRE(alt.feet == 6500);
  REQUIRE(!alt.is_fl);
}

TEST_CASE("cifp: initial_altitude returns cached result on second call", "[cifp][lflp]") {
  reset();
  auto a1 = cifp_reader::initial_altitude(kCifpDir, "LFLP", "22");
  auto a2 = cifp_reader::initial_altitude(kCifpDir, "LFLP", "22");
  REQUIRE(a1.feet == a2.feet);
}

TEST_CASE("cifp: initial_altitude returns 0 for unknown airport", "[cifp]") {
  reset();
  auto alt = cifp_reader::initial_altitude(kCifpDir, "ZZZZ", "22");
  REQUIRE(alt.feet == 0);
}

TEST_CASE("cifp: initial_altitude returns 0 for empty inputs", "[cifp]") {
  reset();
  REQUIRE(cifp_reader::initial_altitude("", "LFLP", "22").feet == 0);
  REQUIRE(cifp_reader::initial_altitude(kCifpDir, "", "22").feet == 0);
  REQUIRE(cifp_reader::initial_altitude(kCifpDir, "LFLP", "").feet == 0);
}

// ── sid_name_for_runway (alphabetically first) ────────────────────────

TEST_CASE("cifp: sid_name_for_runway returns alphabetically first SID", "[cifp][lflp]") {
  reset();
  // BULO2A < LTP2A < ODIK2A < ROMA2A
  std::string sid = cifp_reader::sid_name_for_runway(kCifpDir, "LFLP", "22");
  REQUIRE(sid == "BULO2A");
}

TEST_CASE("cifp: sid_name_for_runway returns empty for unknown runway", "[cifp]") {
  reset();
  std::string sid = cifp_reader::sid_name_for_runway(kCifpDir, "LFLP", "04");
  REQUIRE(sid.empty());
}

// ── sid_name_for_last_fix ─────────────────────────────────────────────

TEST_CASE("cifp: sid_name_for_last_fix finds ODIK2A when last fix is ODIKI", "[cifp][lflp]") {
  reset();
  std::string sid =
      cifp_reader::sid_name_for_last_fix(kCifpDir, "LFLP", "22", "ODIKI");
  REQUIRE(sid == "ODIK2A");
}

TEST_CASE("cifp: sid_name_for_last_fix finds LTP2A when last fix is LTPNO", "[cifp][lflp]") {
  reset();
  std::string sid =
      cifp_reader::sid_name_for_last_fix(kCifpDir, "LFLP", "22", "LTPNO");
  REQUIRE(sid == "LTP2A");
}

TEST_CASE("cifp: sid_name_for_last_fix finds ROMA2A when last fix is ROMAM", "[cifp][lflp]") {
  reset();
  std::string sid =
      cifp_reader::sid_name_for_last_fix(kCifpDir, "LFLP", "22", "ROMAM");
  REQUIRE(sid == "ROMA2A");
}

TEST_CASE("cifp: sid_name_for_last_fix returns empty for unknown last fix", "[cifp][lflp]") {
  reset();
  std::string sid =
      cifp_reader::sid_name_for_last_fix(kCifpDir, "LFLP", "22", "AMBET");
  REQUIRE(sid.empty());
}

TEST_CASE("cifp: sid_name_for_last_fix returns empty for unknown airport", "[cifp]") {
  reset();
  std::string sid =
      cifp_reader::sid_name_for_last_fix(kCifpDir, "ZZZZ", "22", "ODIKI");
  REQUIRE(sid.empty());
}

TEST_CASE("cifp: sid_name_for_last_fix returns empty for empty inputs", "[cifp]") {
  reset();
  REQUIRE(cifp_reader::sid_name_for_last_fix("", "LFLP", "22", "ODIKI").empty());
  REQUIRE(cifp_reader::sid_name_for_last_fix(kCifpDir, "", "22", "ODIKI").empty());
  // Note: an empty active_runway is NOT an empty input -- it triggers an
  // any-runway search (see "empty runway searches all runways" below).
  REQUIRE(cifp_reader::sid_name_for_last_fix(kCifpDir, "LFLP", "22", "").empty());
}

TEST_CASE("cifp: empty runway searches all runways", "[cifp]") {
  reset();
  // active_runway="" means "search every runway at the airport" (used by the
  // SimBrief navlog path before the active runway is known). ODIKI is the last
  // fix of SID ODIK2A, so it must still resolve.
  REQUIRE(cifp_reader::sid_name_for_last_fix(kCifpDir, "LFLP", "", "ODIKI") ==
          "ODIK2A");
}

// ── is_sid_valid_for_runway ───────────────────────────────────────────

TEST_CASE("cifp: is_sid_valid_for_runway accepts known SID", "[cifp][lflp]") {
  reset();
  REQUIRE(cifp_reader::is_sid_valid_for_runway(kCifpDir, "LFLP", "ODIK2A", "22"));
  REQUIRE(cifp_reader::is_sid_valid_for_runway(kCifpDir, "LFLP", "LTP2A",  "22"));
}

TEST_CASE("cifp: is_sid_valid_for_runway rejects SID not in CIFP", "[cifp][lflp]") {
  reset();
  REQUIRE(!cifp_reader::is_sid_valid_for_runway(kCifpDir, "LFLP", "AMBET2A", "22"));
}

TEST_CASE("cifp: is_sid_valid_for_runway returns false for unknown airport", "[cifp]") {
  reset();
  REQUIRE(!cifp_reader::is_sid_valid_for_runway(kCifpDir, "ZZZZ", "ODIK2A", "22"));
}
