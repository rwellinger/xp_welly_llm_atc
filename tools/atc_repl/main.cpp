/*
 * xp_wellys_atc — headless CLI
 *
 * Three modes:
 *   atc_repl run <file.json> [<file.json> ...]   scenario batch (M3)
 *   atc_repl repl <file.json>                    REPL seeded from scenario
 *   atc_repl                                     REPL with LSZH default
 */

#include "atc_state_machine.hpp"
#include "atc_templates.hpp"
#include "flight_phase.hpp"
#include "repl.hpp"
#include "scenario.hpp"
#include "core/xplane_context.hpp"

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <utility>

static int run_batch(int argc, char **argv) {
  struct FileResult {
    std::string path;
    std::string name;
    scenario::RunResult rr;
    bool load_error = false;
    std::string err;
  };

  std::vector<FileResult> results;
  results.reserve(static_cast<size_t>(argc));

  for (int i = 0; i < argc; ++i) {
    FileResult fr;
    fr.path = argv[i];
    try {
      auto scn = scenario::load(argv[i]);
      fr.name = scn.name;
      std::printf("=== %s (%s) ===\n", scn.name.c_str(), argv[i]);
      fr.rr = scenario::run(scn);
    } catch (const std::exception &e) {
      fr.load_error = true;
      fr.err = e.what();
      std::fprintf(stderr, "ERROR loading %s: %s\n", argv[i], e.what());
    }
    results.push_back(std::move(fr));
  }

  // ── JUnit-style summary ────────────────────────────────────────────
  int total_steps = 0, total_assertions = 0, total_mismatches = 0;
  int files_passed = 0, files_failed = 0, files_errored = 0;

  std::printf(
      "\n================================================================\n");
  std::printf("Scenario results\n");
  std::printf(
      "----------------------------------------------------------------\n");
  for (const auto &fr : results) {
    if (fr.load_error) {
      std::printf("  [ERROR] %s — %s\n", fr.path.c_str(), fr.err.c_str());
      ++files_errored;
      continue;
    }
    total_steps += fr.rr.steps;
    total_assertions += fr.rr.assertions;
    total_mismatches += fr.rr.mismatches;
    const char *tag = fr.rr.mismatches == 0 ? "[PASS]" : "[FAIL]";
    if (fr.rr.mismatches == 0)
      ++files_passed;
    else
      ++files_failed;
    std::printf("  %s %-60s %2d steps, %2d/%d asserts\n", tag, fr.name.c_str(),
                fr.rr.steps, fr.rr.assertions - fr.rr.mismatches,
                fr.rr.assertions);
  }
  std::printf(
      "================================================================\n");
  std::printf("Scenarios:   %d total   %d passed   %d failed   %d errors\n",
              static_cast<int>(results.size()), files_passed, files_failed,
              files_errored);
  std::printf("Steps:       %d total\n", total_steps);
  std::printf("Assertions:  %d total   %d passed   %d failed\n",
              total_assertions, total_assertions - total_mismatches,
              total_mismatches);
  std::printf("Status:      %s\n",
              (files_failed == 0 && files_errored == 0) ? "PASS" : "FAIL");
  std::printf(
      "================================================================\n");

  return (files_failed == 0 && files_errored == 0) ? 0 : 1;
}

static xplane_context::XPlaneContext default_ctx() {
  xplane_context::XPlaneContext ctx;
  ctx.nearest_airport_id = "LSZH";
  ctx.nearest_airport_name = "Zurich";
  ctx.is_towered_airport = true;
  ctx.on_ground = true;
  ctx.com1_freq_mhz = 121.800f;
  ctx.active_com = 1;
  ctx.frequency_type = xplane_context::FrequencyType::GROUND;
  ctx.active_runway = "28";
  return ctx;
}

int main(int argc, char **argv) {
  atc_templates::init();
  flight_phase::init();
  atc_state_machine::init();

  if (argc >= 3 && std::strcmp(argv[1], "run") == 0) {
    return run_batch(argc - 2, argv + 2);
  }

  xplane_context::XPlaneContext ctx;
  std::string callsign = "November One Two Three Alpha Bravo";

  if (argc >= 3 && std::strcmp(argv[1], "repl") == 0) {
    try {
      auto scn = scenario::load(argv[2]);
      ctx = std::move(scn.ctx);
      callsign = std::move(scn.pilot_callsign);
      std::fprintf(stderr, "Loaded: %s\n", scn.name.c_str());
    } catch (const std::exception &e) {
      std::fprintf(stderr, "ERROR loading %s: %s\n", argv[2], e.what());
      return 1;
    }
  } else {
    ctx = default_ctx();
  }

  return repl::run(std::move(ctx), std::move(callsign));
}
