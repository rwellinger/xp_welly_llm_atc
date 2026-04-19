/*
 * xp_wellys_atc - headless CLI
 *
 * Scenario loader + runner. Parses JSON under testscripts/ into an
 * XPlaneContext + a list of pilot-say steps and drives them through
 * engine::process_transcript. Optional substring assertions are
 * checked case-insensitively against ATC response_text.
 */

#ifndef ATC_REPL_SCENARIO_HPP
#define ATC_REPL_SCENARIO_HPP

#include "xplane_context.hpp"

#include <optional>
#include <string>
#include <vector>

namespace scenario {

struct Step {
  std::string text;                  // empty = set-only step (no transcript)
  std::optional<std::string> expect; // empty = execute only, no assertion
  std::optional<std::string> expect_state; // assert ATCState name post-step
  std::optional<float> quality; // engine::Input.quality (default 1.0f)
  // Context fields to apply BEFORE processing `text`. Ordered so an
  // airport+frequency+freq_type bundle can be changed atomically.
  std::vector<std::pair<std::string, std::string>> set_fields;
  std::optional<std::string> note; // printed to stderr before step
};

struct Scenario {
  std::string name;   // JSON "name" or fallback to filename
  std::string region; // "EU" or "US" — default "EU" if absent
  xplane_context::XPlaneContext ctx;
  std::string pilot_callsign;
  std::vector<Step> steps;
};

// Throws std::runtime_error on parse failure (missing required field,
// wrong type, unknown freq_type string).
Scenario load(const std::string &path);

struct RunResult {
  int steps = 0;      // total steps executed (including set-only)
  int assertions = 0; // steps that had an `expect` clause
  int mismatches = 0; // assertions that failed
};

// Run all steps. Prints on stdout: "PILOT: ...", "ATC: ...",
// "EXPECT: <ok|MISMATCH>". No summary — caller aggregates across files.
RunResult run(const Scenario &scn);

} // namespace scenario
#endif
