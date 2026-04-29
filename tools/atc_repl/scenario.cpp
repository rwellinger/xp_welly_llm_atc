/*
 * xp_wellys_atc - headless CLI
 *
 * Scenario loader + runner implementation. JSON parsing via
 * nlohmann::json. Engine calls run synchronously because the CLI
 * disables GPT fallback.
 */

#include "scenario.hpp"

#include "atc_state_machine.hpp"
#include "atc_templates.hpp"
#include "engine/engine.hpp"
#include "flight_phase.hpp"
#include "intent_parser.hpp"
#include "persistence/settings.hpp"
#include "xplane_context.hpp"

#include <json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

namespace xplane_context {
extern XPlaneContext g_cli_ctx;
}

namespace scenario {

using json = nlohmann::json;
using FT = xplane_context::FrequencyType;

// ── Helpers ────────────────────────────────────────────────────────

static std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

static FT freq_type_from_string(const std::string &s) {
  static const std::unordered_map<std::string, FT> kMap{
      {"UNKNOWN", FT::UNKNOWN},   {"DELIVERY", FT::DELIVERY},
      {"GROUND", FT::GROUND},     {"TOWER", FT::TOWER},
      {"APPROACH", FT::APPROACH}, {"UNICOM", FT::UNICOM},
      {"CTAF", FT::CTAF},         {"ATIS", FT::ATIS},
  };
  std::string upper = s;
  std::transform(upper.begin(), upper.end(), upper.begin(),
                 [](unsigned char c) { return std::toupper(c); });
  auto it = kMap.find(upper);
  if (it == kMap.end()) {
    throw std::runtime_error("unknown freq_type: " + s);
  }
  return it->second;
}

// Mid-scenario field override. Mirrors the REPL `set` command but only
// touches context fields (region/callsign are scenario-level, not per-step).
// Throws std::runtime_error on unknown field or bad value.
static void apply_field(xplane_context::XPlaneContext &ctx,
                        const std::string &field, const std::string &value) {
  auto parse_bool = [](const std::string &s) {
    std::string lo = s;
    std::transform(lo.begin(), lo.end(), lo.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (lo == "true" || lo == "yes" || lo == "1" || lo == "on")
      return true;
    if (lo == "false" || lo == "no" || lo == "0" || lo == "off")
      return false;
    throw std::runtime_error("expected bool for " + std::string(s));
  };
  if (field == "airport")
    ctx.nearest_airport_id = value;
  else if (field == "airport_name")
    ctx.nearest_airport_name = value;
  else if (field == "towered")
    ctx.is_towered_airport = parse_bool(value);
  else if (field == "tower_only")
    ctx.tower_only = parse_bool(value);
  else if (field == "on_ground")
    ctx.on_ground = parse_bool(value);
  else if (field == "com") {
    ctx.com1_freq_mhz = std::stof(value);
    ctx.active_com = 1;
    // If the scenario provided an apt.dat-style freqs list, derive the
    // frequency type via the real lookup() — this is what exercises the
    // priority-based resolution (TOWER > UNICOM etc.) under test.
    if (!ctx.airport_freqs.all.empty())
      ctx.frequency_type = ctx.airport_freqs.lookup(ctx.com1_freq_mhz);
  } else if (field == "freq_type")
    ctx.frequency_type = freq_type_from_string(value);
  else if (field == "runway")
    ctx.active_runway = value;
  else if (field == "altitude_ft")
    ctx.altitude_ft_msl = std::stof(value);
  else if (field == "agl_ft")
    ctx.height_agl_ft = std::stof(value);
  else if (field == "groundspeed_kt")
    ctx.groundspeed_kts = std::stof(value);
  else if (field == "heading")
    ctx.heading_true = std::stof(value);
  else
    throw std::runtime_error("unknown field: " + field);
}

static std::string basename_no_ext(const std::string &path) {
  auto slash = path.find_last_of("/\\");
  std::string base =
      (slash == std::string::npos) ? path : path.substr(slash + 1);
  auto dot = base.find_last_of('.');
  return (dot == std::string::npos) ? base : base.substr(0, dot);
}

// ── Loader ─────────────────────────────────────────────────────────

Scenario load(const std::string &path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("cannot open file: " + path);
  }
  json j;
  try {
    in >> j;
  } catch (const std::exception &e) {
    throw std::runtime_error("JSON parse error in " + path + ": " + e.what());
  }

  Scenario scn;
  scn.name = j.value("name", basename_no_ext(path));
  {
    std::string r = j.value("region", std::string{"EU"});
    std::string up;
    for (char c : r)
      up += (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
    if (up != "EU" && up != "US") {
      throw std::runtime_error("region must be \"EU\" or \"US\": " + r);
    }
    scn.region = up;
  }

  auto &ctx = scn.ctx;
  if (j.contains("context")) {
    const auto &c = j["context"];
    ctx.nearest_airport_id = c.value("airport", std::string{});
    ctx.nearest_airport_name = c.value("airport_name", std::string{});
    ctx.is_towered_airport = c.value("towered", true);
    ctx.tower_only = c.value("tower_only", false);
    ctx.on_ground = c.value("on_ground", true);
    ctx.com1_freq_mhz = c.value("com", 121.800f);
    ctx.active_com = 1;
    // Optional "freqs": [{"mhz": 126.300, "type": "TOWER"}, ...] — simulates
    // the apt.dat frequency list for the current airport so scenarios can
    // exercise AirportFrequencies::lookup() with conflicting entries
    // (e.g. part-time towers where TOWER + UNICOM share one frequency).
    if (c.contains("freqs")) {
      if (!c["freqs"].is_array())
        throw std::runtime_error("context.freqs must be array in " + path);
      for (const auto &f : c["freqs"]) {
        if (!f.is_object() || !f.contains("mhz") || !f.contains("type"))
          throw std::runtime_error("context.freqs[] needs {mhz, type} in " +
                                   path);
        xplane_context::AirportFrequency af;
        af.freq_khz =
            static_cast<uint32_t>(std::round(f["mhz"].get<float>() * 1000.0f));
        af.type = freq_type_from_string(f["type"].get<std::string>());
        ctx.airport_freqs.all.push_back(af);
      }
    }
    if (c.contains("freq_type")) {
      ctx.frequency_type =
          freq_type_from_string(c["freq_type"].get<std::string>());
    } else if (!ctx.airport_freqs.all.empty()) {
      // Derive via real lookup() when freqs is present but freq_type isn't.
      ctx.frequency_type = ctx.airport_freqs.lookup(ctx.com1_freq_mhz);
    } else {
      ctx.frequency_type = FT::GROUND;
    }
    ctx.active_runway = c.value("runway", std::string{"28"});
    scn.pilot_callsign =
        c.value("callsign", std::string{"November One Two Three Alpha Bravo"});
    ctx.altitude_ft_msl = c.value("altitude_ft", 1400.0f);
    ctx.heading_true = c.value("heading", 0.0f);
    ctx.groundspeed_kts = c.value("groundspeed_kt", 0.0f);
    ctx.height_agl_ft = c.value("agl_ft", 0.0f);
  } else {
    ctx.is_towered_airport = true;
    ctx.on_ground = true;
    ctx.com1_freq_mhz = 121.800f;
    ctx.active_com = 1;
    ctx.frequency_type = FT::GROUND;
    ctx.active_runway = "28";
    scn.pilot_callsign = "November One Two Three Alpha Bravo";
    ctx.altitude_ft_msl = 1400.0f;
  }
  // Always required for engine to find templates and bypass power check
  ctx.com_radio_powered = true;
  ctx.avionics_on = true;
  if (ctx.aircraft_icao.empty())
    ctx.aircraft_icao = "C172";

  if (!j.contains("say") || !j["say"].is_array()) {
    throw std::runtime_error("missing or invalid 'say' array in " + path);
  }
  for (const auto &step : j["say"]) {
    Step s;
    if (step.is_string()) {
      s.text = step.get<std::string>();
    } else if (step.is_object()) {
      if (step.contains("text")) {
        if (!step["text"].is_string())
          throw std::runtime_error("step.text must be string in " + path);
        s.text = step["text"].get<std::string>();
      }
      if (step.contains("expect") && step["expect"].is_string()) {
        s.expect = step["expect"].get<std::string>();
      }
      if (step.contains("expect_state") && step["expect_state"].is_string()) {
        s.expect_state = step["expect_state"].get<std::string>();
      }
      if (step.contains("expect_not") && step["expect_not"].is_string()) {
        s.expect_not = step["expect_not"].get<std::string>();
      }
      if (step.contains("wait_sec") && step["wait_sec"].is_number()) {
        int secs = step["wait_sec"].get<int>();
        if (secs <= 0)
          throw std::runtime_error("step.wait_sec must be > 0 in " + path);
        s.wait_sec = secs;
      }
      if (step.contains("quality") && step["quality"].is_number()) {
        s.quality = step["quality"].get<float>();
      }
      if (step.contains("note") && step["note"].is_string()) {
        s.note = step["note"].get<std::string>();
      }
      if (step.contains("set")) {
        if (!step["set"].is_object())
          throw std::runtime_error("step.set must be object in " + path);
        for (auto it = step["set"].begin(); it != step["set"].end(); ++it) {
          std::string val;
          if (it.value().is_string())
            val = it.value().get<std::string>();
          else if (it.value().is_boolean())
            val = it.value().get<bool>() ? "true" : "false";
          else if (it.value().is_number())
            val = it.value().dump();
          else
            throw std::runtime_error(
                "step.set values must be string/bool/number in " + path);
          s.set_fields.emplace_back(it.key(), std::move(val));
        }
      }
      if (s.text.empty() && s.set_fields.empty() && !s.wait_sec.has_value())
        throw std::runtime_error(
            "step object requires 'text', 'set', or 'wait_sec' in " + path);
      // Trivial-pass guard: expect_not alone could match vacuously on an
      // empty or silent response. Require at least one positive anchor.
      if (s.expect_not.has_value() && !s.expect.has_value() &&
          !s.expect_state.has_value())
        throw std::runtime_error("step.expect_not requires 'expect' or "
                                 "'expect_state' on the same step in " +
                                 path);
    } else {
      throw std::runtime_error("step must be string or object in " + path);
    }
    scn.steps.push_back(std::move(s));
  }
  return scn;
}

// ── Runner ─────────────────────────────────────────────────────────

RunResult run(const Scenario &scn) {
  // Apply scenario-level region override *before* loading CLI context.
  // reload() re-reads data/regions/<region>/{atc_templates,flight_rules}.json.
  const std::string region = scn.region.empty() ? "EU" : scn.region;
  settings::set_flow_region(region);
  atc_templates::reload();
  flight_phase::reload();

  // Scenario callsign drives intent_parser::matches_configured_callsign —
  // must be set before the first process_transcript or the parser rejects
  // the extracted callsign.
  if (!scn.pilot_callsign.empty())
    settings::set_pilot_callsign_raw(scn.pilot_callsign);

  // Prime CLI-global context and reset all engine state that persists
  // across scenarios.
  xplane_context::g_cli_ctx = scn.ctx;
  auto &ctx = xplane_context::g_cli_ctx;

  atc_state_machine::reset();
  engine::reset();

  // Drive flight_phase past its ground-hysteresis window so REQUEST_TAXI
  // and friends don't get rejected as "engines off / parked" on step 1.
  // 30 × 1-second ticks matches the M2 interactive fallback.
  for (int i = 0; i < 30; ++i)
    flight_phase::update(ctx, 1.0f);

  int mismatches = 0;
  int assertions = 0;
  int idx = 0;
  for (const auto &step : scn.steps) {
    ++idx;

    if (!step.set_fields.empty()) {
      for (const auto &[k, v] : step.set_fields) {
        try {
          apply_field(ctx, k, v);
          std::printf("SET   : %s = %s\n", k.c_str(), v.c_str());
        } catch (const std::exception &e) {
          std::fprintf(stderr, "step %d set error: %s\n", idx, e.what());
          ++mismatches;
        }
      }
      for (int i = 0; i < 30; ++i)
        flight_phase::update(ctx, 1.0f);
    }

    if (step.note.has_value())
      std::printf("NOTE  : %s\n", step.note->c_str());

    // wait_sec: advance flight_phase AND drive the state machine's
    // auto-correction timer. This is the only place the runner ticks
    // check_auto_correction — priming loops above stay flight_phase-only
    // so existing happy-flow scenarios cannot silently break.
    if (step.wait_sec.has_value()) {
      const int secs = *step.wait_sec;
      const std::string before_phase =
          flight_phase::phase_name(flight_phase::get());
      const std::string before_state =
          atc_state_machine::state_name(atc_state_machine::get_state());
      for (int i = 0; i < secs; ++i) {
        flight_phase::update(ctx, 1.0f);
        atc_state_machine::check_auto_correction(flight_phase::get(), 1.0f);
      }
      const std::string after_phase =
          flight_phase::phase_name(flight_phase::get());
      const std::string after_state =
          atc_state_machine::state_name(atc_state_machine::get_state());
      std::printf("WAIT  : %d sec (phase=%s", secs, after_phase.c_str());
      if (before_phase != after_phase)
        std::printf(" [%s->%s]", before_phase.c_str(), after_phase.c_str());
      std::printf(", state=%s", after_state.c_str());
      if (before_state != after_state)
        std::printf(" [%s->%s]", before_state.c_str(), after_state.c_str());
      std::printf(")\n");
    }

    if (step.text.empty()) {
      if (step.expect_state.has_value()) {
        ++assertions;
        const std::string actual =
            atc_state_machine::state_name(atc_state_machine::get_state());
        if (actual == *step.expect_state) {
          std::printf("STATE : ok (\"%s\")\n", actual.c_str());
        } else {
          std::printf(
              "STATE : MISMATCH (step %d: expected \"%s\", got \"%s\")\n", idx,
              step.expect_state->c_str(), actual.c_str());
          ++mismatches;
        }
      }
      std::printf("\n");
      continue;
    }

    std::printf("PILOT : %s\n", step.text.c_str());

    engine::Input in{
        /*transcript=*/step.text,
        /*quality=*/step.quality.value_or(1.0f),
        /*ctx=*/&ctx,
        /*pilot_callsign=*/scn.pilot_callsign,
        /*gpt_fallback_enabled=*/false,
    };

    std::string response;
    intent_parser::PilotIntent intent = intent_parser::PilotIntent::UNKNOWN;
    float confidence = 0.0f;
    engine::process_transcript(
        std::move(in), [&response, &intent, &confidence](engine::Output out) {
          response = out.response_text;
          intent = out.parsed.intent;
          confidence = out.parsed.confidence;
        });

    std::printf("INTENT: %s (confidence=%.2f)\n",
                intent_parser::intent_name(intent), confidence);
    std::printf("ATC   : %s\n",
                response.empty() ? "(silent)" : response.c_str());

    if (step.expect.has_value()) {
      ++assertions;
      const std::string needle = to_lower(*step.expect);
      const std::string hay = to_lower(response);
      bool ok = needle.empty() || hay.find(needle) != std::string::npos;
      if (ok) {
        std::printf("EXPECT: ok (\"%s\")\n", step.expect->c_str());
      } else {
        std::printf("EXPECT: MISMATCH (step %d: looking for \"%s\")\n", idx,
                    step.expect->c_str());
        ++mismatches;
      }
    }
    if (step.expect_not.has_value()) {
      ++assertions;
      const std::string needle = to_lower(*step.expect_not);
      const std::string hay = to_lower(response);
      bool ok = needle.empty() || hay.find(needle) == std::string::npos;
      if (ok) {
        std::printf("NOT   : ok (\"%s\" absent)\n", step.expect_not->c_str());
      } else {
        std::printf("NOT   : MISMATCH (step %d: \"%s\" must not appear)\n", idx,
                    step.expect_not->c_str());
        ++mismatches;
      }
    }
    if (step.expect_state.has_value()) {
      ++assertions;
      const std::string actual =
          atc_state_machine::state_name(atc_state_machine::get_state());
      if (actual == *step.expect_state) {
        std::printf("STATE : ok (\"%s\")\n", actual.c_str());
      } else {
        std::printf("STATE : MISMATCH (step %d: expected \"%s\", got \"%s\")\n",
                    idx, step.expect_state->c_str(), actual.c_str());
        ++mismatches;
      }
    }
    std::printf("\n");
  }

  return RunResult{static_cast<int>(scn.steps.size()), assertions, mismatches};
}

} // namespace scenario
