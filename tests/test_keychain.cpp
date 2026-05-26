/*
 * xp_wellys_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 *
 * Roundtrip test for the macOS Keychain wrapper used to store the
 * OpenAI API key. The test uses a dedicated service name
 * ("com.xp_wellys_atc.test") so it can never clobber a real user key
 * stored under the production service.
 */

#include "persistence/keychain.hpp"

#include <catch2/catch_amalgamated.hpp>

#include <string>

namespace {
const std::string kTestService = "com.xp_wellys_atc.test";
const std::string kTestAccount = "keychain_unit_test";
} // namespace

TEST_CASE("keychain roundtrip with dedicated test service",
          "[keychain][persistence]") {
#if !defined(__APPLE__)
  SKIP("Keychain wrapper is macOS-only");
#else
  // Pre-clean any leftover from a previous run.
  persistence::keychain::remove(kTestService, kTestAccount);
  REQUIRE_FALSE(persistence::keychain::has(kTestService, kTestAccount));

  SECTION("save then load returns the same value") {
    const std::string secret = "sk-test-1234567890ABCDEF";
    REQUIRE(persistence::keychain::save(kTestService, kTestAccount, secret));
    REQUIRE(persistence::keychain::has(kTestService, kTestAccount));
    REQUIRE(persistence::keychain::load(kTestService, kTestAccount) == secret);
  }

  SECTION("save overwrites an existing entry") {
    REQUIRE(persistence::keychain::save(kTestService, kTestAccount, "first"));
    REQUIRE(persistence::keychain::save(kTestService, kTestAccount, "second"));
    REQUIRE(persistence::keychain::load(kTestService, kTestAccount) == "second");
  }

  SECTION("remove makes load return empty") {
    REQUIRE(persistence::keychain::save(kTestService, kTestAccount, "to-delete"));
    REQUIRE(persistence::keychain::remove(kTestService, kTestAccount));
    REQUIRE_FALSE(persistence::keychain::has(kTestService, kTestAccount));
    REQUIRE(persistence::keychain::load(kTestService, kTestAccount).empty());
  }

  SECTION("save rejects empty key") {
    REQUIRE_FALSE(persistence::keychain::save(kTestService, kTestAccount, ""));
  }

  // Final cleanup so nothing lingers in the user's Keychain.
  persistence::keychain::remove(kTestService, kTestAccount);
#endif
}
