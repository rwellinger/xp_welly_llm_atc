/*
 * xp_wellys_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "atc_state_machine.hpp"
#include "atis_generator.hpp"
#include "atc_templates.hpp"
#include "flight_phase.hpp"
#include "settings.hpp"

#include <XPLMUtilities.h>

#include <cmath>
#include <cstdio>
#include <unordered_map>

namespace atc_state_machine {

enum class DepartureType { PATTERN, CROSS_COUNTRY };

static ATCState state_ = ATCState::IDLE;
static bool readback_pending_ = false;
static std::string assigned_runway_; // locked once ATC assigns a runway
static DepartureType departure_type_ = DepartureType::PATTERN;

static const char *departure_type_name(DepartureType t) {
  return t == DepartureType::CROSS_COUNTRY ? "CROSS_COUNTRY" : "PATTERN";
}

// Helper: abbreviate callsign to last 3 words (standard ATC practice)
static std::string abbreviate_callsign(const std::string &cs) {
  // Split into words from the end
  std::vector<std::string> words;
  std::string word;
  for (char c : cs) {
    if (c == ' ') {
      if (!word.empty())
        words.push_back(word);
      word.clear();
    } else {
      word += c;
    }
  }
  if (!word.empty())
    words.push_back(word);

  if (words.size() <= 3)
    return cs;

  // Take last 3 words
  std::string result;
  for (size_t i = words.size() - 3; i < words.size(); ++i) {
    if (!result.empty())
      result += " ";
    result += words[i];
  }
  return result;
}

// Helper: get callsign from message or settings fallback.
// After initial contact (state != IDLE), uses abbreviated form.
static std::string get_callsign(const intent_parser::PilotMessage &msg) {
  std::string cs =
      !msg.callsign.empty() ? msg.callsign : settings::pilot_callsign();
  if (state_ != ATCState::IDLE)
    cs = abbreviate_callsign(cs);
  return cs;
}

// Helper: get runway — pilot speech > assigned > wind-determined > fallback
static std::string
get_runway(const intent_parser::PilotMessage &msg,
           const xplane_context::XPlaneContext &ctx) {
  if (!msg.runway.empty())
    return msg.runway;
  if (!assigned_runway_.empty())
    return assigned_runway_;
  if (!ctx.active_runway.empty())
    return ctx.active_runway;
  return "28"; // last resort fallback
}

// Helper: format QNH from inHg
static std::string format_qnh(float inhg) {
  int hpa = static_cast<int>(std::round(inhg * 33.8639f));
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%d", hpa);
  return buf;
}

// Helper: format wind — "calm" below 3 kt, otherwise "XXX degrees XX knots"
static std::string format_wind(float dir, float spd) {
  if (spd < 3.0f)
    return "calm";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%03.0f degrees %02.0f knots", dir, spd);
  return buf;
}

// Helper: airport name — use apt.dat name (e.g. "Grenchen"), fallback to ICAO
static std::string airport_name(const xplane_context::XPlaneContext &ctx) {
  if (!ctx.nearest_airport_name.empty())
    return ctx.nearest_airport_name;
  if (!ctx.nearest_airport_id.empty())
    return ctx.nearest_airport_id;
  return "Airport";
}

void init() {
  state_ = ATCState::IDLE;
  readback_pending_ = false;
  assigned_runway_.clear();
  departure_type_ = DepartureType::PATTERN;
}

void stop() {
  state_ = ATCState::IDLE;
  readback_pending_ = false;
  assigned_runway_.clear();
  departure_type_ = DepartureType::PATTERN;
}

void reset() {
  state_ = ATCState::IDLE;
  readback_pending_ = false;
  assigned_runway_.clear();
  departure_type_ = DepartureType::PATTERN;
  XPLMDebugString("[xp_wellys_atc] ATC state machine reset to IDLE\n");
}

ATCState get_state() { return state_; }

bool is_readback_pending() { return readback_pending_; }

const char *get_departure_type_name() {
  return departure_type_name(departure_type_);
}

const char *state_name(ATCState state) {
  switch (state) {
  case ATCState::IDLE:
    return "IDLE";
  case ATCState::GROUND_CONTACT:
    return "GROUND_CONTACT";
  case ATCState::TAXI_CLEARED:
    return "TAXI_CLEARED";
  case ATCState::TOWER_CONTACT:
    return "TOWER_CONTACT";
  case ATCState::DEPARTURE_CLEARED:
    return "DEPARTURE_CLEARED";
  case ATCState::PATTERN_ENTRY:
    return "PATTERN_ENTRY";
  case ATCState::LANDING_CLEARED:
    return "LANDING_CLEARED";
  case ATCState::TOUCH_AND_GO_CLEARED:
    return "TOUCH_AND_GO_CLEARED";
  case ATCState::UNICOM_ACTIVE:
    return "UNICOM_ACTIVE";
  case ATCState::EN_ROUTE:
    return "EN_ROUTE";
  }
  return "UNKNOWN";
}

ATCState state_from_name(const std::string &name) {
  static const std::unordered_map<std::string, ATCState> kMap = {
      {"IDLE", ATCState::IDLE},
      {"GROUND_CONTACT", ATCState::GROUND_CONTACT},
      {"TAXI_CLEARED", ATCState::TAXI_CLEARED},
      {"TOWER_CONTACT", ATCState::TOWER_CONTACT},
      {"DEPARTURE_CLEARED", ATCState::DEPARTURE_CLEARED},
      {"PATTERN_ENTRY", ATCState::PATTERN_ENTRY},
      {"LANDING_CLEARED", ATCState::LANDING_CLEARED},
      {"TOUCH_AND_GO_CLEARED", ATCState::TOUCH_AND_GO_CLEARED},
      {"UNICOM_ACTIVE", ATCState::UNICOM_ACTIVE},
      {"EN_ROUTE", ATCState::EN_ROUTE},
  };
  auto it = kMap.find(name);
  return it != kMap.end() ? it->second : ATCState::IDLE;
}

void set_state(ATCState state) {
  char log[256];
  std::snprintf(log, sizeof(log),
                "[xp_wellys_atc] ATC state (external): %s -> %s\n",
                state_name(state_), state_name(state));
  XPLMDebugString(log);
  state_ = state;
}

static std::string extract_position(const intent_parser::PilotMessage &msg,
                                    const xplane_context::XPlaneContext &ctx) {
  std::string rwy = get_runway(msg, ctx);
  std::string apt = airport_name(ctx);

  if (ctx.on_ground)
    return "on the ground at " + apt;

  std::string lower = msg.raw_transcript;
  for (auto &c : lower)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  if (lower.find("downwind") != std::string::npos)
    return "downwind runway " + rwy;
  if (lower.find("base") != std::string::npos)
    return "base runway " + rwy;
  if (lower.find("final") != std::string::npos)
    return "final runway " + rwy;
  if (lower.find("crosswind") != std::string::npos)
    return "crosswind runway " + rwy;
  if (lower.find("upwind") != std::string::npos)
    return "upwind runway " + rwy;
  return "in the pattern at " + apt;
}

std::map<std::string, std::string>
build_vars(const intent_parser::PilotMessage &msg,
           const xplane_context::XPlaneContext &ctx) {
  // ATIS letter name (e.g. "Alpha", "Bravo", ...)
  static const char *letter_names[] = {
      "Alpha",   "Bravo",   "Charlie", "Delta",   "Echo",    "Foxtrot",
      "Golf",    "Hotel",   "India",   "Juliet",  "Kilo",    "Lima",
      "Mike",    "November","Oscar",   "Papa",    "Quebec",  "Romeo",
      "Sierra",  "Tango",   "Uniform", "Victor",  "Whiskey", "X-ray",
      "Yankee",  "Zulu"};
  char letter = atis_generator::current_letter();
  std::string atis_letter_name = letter_names[letter - 'A'];

  // Real frequencies from airport database
  using FT = xplane_context::FrequencyType;
  float ground_freq = ctx.airport_freqs.first_mhz(FT::GROUND);
  if (ground_freq < 1.0f && ctx.tower_only)
    ground_freq = ctx.airport_freqs.first_mhz(FT::TOWER);
  float tower_freq = ctx.airport_freqs.first_mhz(FT::TOWER);

  auto format_freq = [](float mhz) -> std::string {
    if (mhz < 100.0f)
      return "";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.3f", mhz);
    return buf;
  };

  return {
      {"callsign", get_callsign(msg)},
      {"airport", airport_name(ctx)},
      {"runway", get_runway(msg, ctx)},
      {"wind", format_wind(ctx.wind_direction_deg, ctx.wind_speed_kt)},
      {"qnh", format_qnh(ctx.qnh_inhg)},
      {"atis_letter", atis_letter_name},
      {"frequency", format_freq(ground_freq)},
      {"tower_frequency", format_freq(tower_freq)},
      {"ground_frequency", format_freq(ground_freq)},
      {"position", extract_position(msg, ctx)},
      {"pattern_direction", settings::pattern_direction()},
      {"entry_vrp", msg.vrp_name},
  };
}

ATCResponse process(const intent_parser::PilotMessage &msg,
                    const xplane_context::XPlaneContext &ctx) {
  ATCResponse resp;

  using FT = xplane_context::FrequencyType;

  // Non-towered airport OR Unicom/CTAF frequency: force UNICOM flow
  bool unicom_flow = !ctx.is_towered_airport ||
                     ctx.frequency_type == FT::UNICOM ||
                     ctx.frequency_type == FT::CTAF;

  if (unicom_flow) {
    auto vars = build_vars(msg, ctx);
    std::string intent_key = intent_parser::intent_template_key(msg.intent);
    auto tmpl = atc_templates::lookup(false, "IDLE", intent_key);

    resp.text = atc_templates::fill(tmpl.response_template, vars);
    resp.next_state = ATCState::IDLE;
    state_ = ATCState::IDLE;

    XPLMDebugString("[xp_wellys_atc] ATC state: UNICOM_ACTIVE -> IDLE "
                    "(non-towered/CTAF)\n");
    return resp;
  }

  // Unknown frequency at towered airport → hint correct frequency
  if (ctx.frequency_type == FT::UNKNOWN && ctx.is_towered_airport) {
    using PI = intent_parser::PilotIntent;
    if (msg.intent != PI::READBACK) {
      auto vars = build_vars(msg, ctx);
      bool needs_ground = (msg.intent == PI::INITIAL_CALL_GROUND ||
                           msg.intent == PI::REQUEST_TAXI ||
                           msg.intent == PI::REQUEST_TAXI_PARKING);
      std::string freq_hint;
      if (needs_ground && !ctx.tower_only) {
        freq_hint = "{callsign}, you are not on the correct frequency. "
                    "Contact {airport} Ground on {ground_frequency}.";
      } else {
        freq_hint = "{callsign}, you are not on the correct frequency. "
                    "Contact {airport} Tower on {tower_frequency}.";
      }
      resp.text = atc_templates::fill(freq_hint, vars);
      resp.next_state = state_;
      XPLMDebugString(
          "[xp_wellys_atc] ATC: wrong frequency, hint given\n");
      return resp;
    }
  }

  // EN_ROUTE: pilot is intentionally off any tower frequency.
  // Do not validate or reset — return silent _INVALID via template.
  // Frequency-based state validation at towered airports
  // Skip validation for: unknown freq (pilot between frequencies),
  // tower-only airports on tower freq, and EN_ROUTE (off-frequency by design)
  bool needs_freq_validation =
      ctx.frequency_type != FT::UNKNOWN &&
      !(ctx.tower_only && ctx.frequency_type == FT::TOWER) &&
      state_ != ATCState::EN_ROUTE;

  if (needs_freq_validation) {
    if (ctx.frequency_type == FT::GROUND) {
      if (state_ != ATCState::IDLE && state_ != ATCState::GROUND_CONTACT &&
          state_ != ATCState::TAXI_CLEARED) {
        state_ = ATCState::IDLE;
      }
    } else if (ctx.frequency_type == FT::TOWER) {
      if (state_ != ATCState::IDLE && state_ != ATCState::TAXI_CLEARED &&
          state_ != ATCState::TOWER_CONTACT &&
          state_ != ATCState::DEPARTURE_CLEARED &&
          state_ != ATCState::PATTERN_ENTRY &&
          state_ != ATCState::LANDING_CLEARED &&
          state_ != ATCState::TOUCH_AND_GO_CLEARED) {
        state_ = ATCState::IDLE;
      }
    }
  }

  // Frequency-aware intent routing in IDLE state
  if (state_ == ATCState::IDLE) {
    using PI = intent_parser::PilotIntent;
    auto vars = build_vars(msg, ctx);

    // REQUEST_TAXI on Tower freq at airport with Ground → redirect to Ground
    if ((msg.intent == PI::REQUEST_TAXI ||
         msg.intent == PI::REQUEST_TAXI_PARKING) &&
        ctx.frequency_type == FT::TOWER && !ctx.tower_only) {
      resp.text = atc_templates::fill("{callsign}, contact ground for taxi.",
                                      vars);
      resp.next_state = ATCState::IDLE;
      state_ = ATCState::IDLE;
      XPLMDebugString("[xp_wellys_atc] ATC: REQUEST_TAXI on Tower freq -> "
                      "redirect to ground\n");
      return resp;
    }

    // READY_FOR_DEPARTURE on Ground freq → redirect to Tower
    if ((msg.intent == PI::READY_FOR_DEPARTURE ||
         msg.intent == PI::READY_FOR_DEPARTURE_VFR) &&
        ctx.frequency_type == FT::GROUND) {
      resp.text = atc_templates::fill(
          "{callsign}, contact tower when ready for departure.", vars);
      resp.next_state = ATCState::IDLE;
      state_ = ATCState::IDLE;
      XPLMDebugString("[xp_wellys_atc] ATC: READY_FOR_DEPARTURE on Ground "
                      "freq -> redirect to tower\n");
      return resp;
    }
  }

  // Flight-phase precondition check
  {
    std::string intent_key = intent_parser::intent_template_key(msg.intent);
    auto phase = flight_phase::get();
    std::string rejection = flight_phase::check_precondition(intent_key, phase);
    if (!rejection.empty()) {
      auto vars = build_vars(msg, ctx);
      resp.text = atc_templates::fill(rejection, vars);
      resp.next_state = state_;
      char log[256];
      std::snprintf(log, sizeof(log),
                    "[xp_wellys_atc] Phase guard: %s blocked in phase %s\n",
                    intent_key.c_str(), flight_phase::phase_name(phase));
      XPLMDebugString(log);
      return resp;
    }
  }

  // Template-based response lookup
  auto vars = build_vars(msg, ctx);
  std::string intent_key = intent_parser::intent_template_key(msg.intent);
  std::string state_str = state_name(state_);

  auto tmpl = atc_templates::lookup(true, state_str, intent_key);
  resp.text = atc_templates::fill(tmpl.response_template, vars);
  resp.next_state = state_from_name(tmpl.next_state);
  resp.requires_readback = tmpl.requires_readback;

  // Track readback state
  if (msg.intent == intent_parser::PilotIntent::READBACK)
    readback_pending_ = false;
  else if (resp.requires_readback)
    readback_pending_ = true;

  // Lock runway on first clearance that references a runway
  if (assigned_runway_.empty() && resp.next_state != ATCState::IDLE) {
    std::string rwy = get_runway(msg, ctx);
    if (!rwy.empty()) {
      assigned_runway_ = rwy;
      char rlog[128];
      std::snprintf(rlog, sizeof(rlog),
                    "[xp_wellys_atc] Runway locked: %s\n", rwy.c_str());
      XPLMDebugString(rlog);
    }
  }

  // Apply state transition if we have a response
  if (!resp.text.empty()) {
    ATCState prev_state = state_;
    char log[256];
    std::snprintf(log, sizeof(log), "[xp_wellys_atc] ATC state: %s -> %s\n",
                  state_name(prev_state), state_name(resp.next_state));
    XPLMDebugString(log);
    state_ = resp.next_state;

    // Track departure intent: PATTERN (default) vs CROSS_COUNTRY.
    // Set on entry to DEPARTURE_CLEARED, reset on exit.
    if (resp.next_state == ATCState::DEPARTURE_CLEARED &&
        prev_state != ATCState::DEPARTURE_CLEARED) {
      departure_type_ =
          (msg.intent == intent_parser::PilotIntent::READY_FOR_DEPARTURE_VFR)
              ? DepartureType::CROSS_COUNTRY
              : DepartureType::PATTERN;
      char dlog[128];
      std::snprintf(dlog, sizeof(dlog),
                    "[xp_wellys_atc] Departure type: %s\n",
                    departure_type_name(departure_type_));
      XPLMDebugString(dlog);
    } else if (prev_state == ATCState::DEPARTURE_CLEARED &&
               resp.next_state != ATCState::DEPARTURE_CLEARED) {
      departure_type_ = DepartureType::PATTERN;
    }
  }

  // Release runway lock when session ends
  if (resp.next_state == ATCState::IDLE && !assigned_runway_.empty()) {
    XPLMDebugString("[xp_wellys_atc] Runway lock released\n");
    assigned_runway_.clear();
  }

  // Tower-only airport: skip ground→tower handoff (no frequency change needed)
  if (ctx.tower_only && state_ == ATCState::TAXI_CLEARED) {
    XPLMDebugString(
        "[xp_wellys_atc] Tower-only: auto-advancing TAXI_CLEARED -> "
        "TOWER_CONTACT\n");
    state_ = ATCState::TOWER_CONTACT;
    resp.next_state = ATCState::TOWER_CONTACT;
  }

  return resp;
}

// ── Auto-correction state ────────────────────────────────────────

static std::string active_correction_key_;
static float correction_timer_ = 0.0f;

void check_auto_correction(flight_phase::FlightPhase phase, float dt) {
  if (state_ == ATCState::IDLE || state_ == ATCState::UNICOM_ACTIVE)
    return;

  std::string current_state = state_name(state_);
  auto *corrections = flight_phase::get_auto_corrections(current_state);
  if (!corrections)
    return;

  // Find first matching correction condition
  for (const auto &[cond_name, ac] : *corrections) {
    bool matches = false;
    for (auto p : ac.phases) {
      if (phase == p) {
        matches = true;
        break;
      }
    }

    if (matches) {
      // Suppress pattern auto-correction for cross-country departures.
      // The pilot will explicitly leave the frequency.
      if (state_ == ATCState::DEPARTURE_CLEARED &&
          departure_type_ == DepartureType::CROSS_COUNTRY &&
          state_from_name(ac.next_state) == ATCState::PATTERN_ENTRY) {
        if (active_correction_key_ != "skipped:cross_country") {
          XPLMDebugString(
              "[xp_wellys_atc] Skipping pattern auto-correction: "
              "cross-country departure\n");
          active_correction_key_ = "skipped:cross_country";
        }
        return;
      }

      std::string key = current_state + ":" + cond_name;
      if (key != active_correction_key_) {
        active_correction_key_ = key;
        correction_timer_ = 0.0f;
      }
      correction_timer_ += dt;

      if (correction_timer_ >= ac.delay_sec) {
        ATCState new_state = state_from_name(ac.next_state);
        char log[256];
        std::snprintf(
            log, sizeof(log),
            "[xp_wellys_atc] Auto-correction: %s -> %s (phase=%s, "
            "condition=%s, after %.1fs)\n",
            state_name(state_), state_name(new_state),
            flight_phase::phase_name(phase), cond_name.c_str(),
            correction_timer_);
        XPLMDebugString(log);
        state_ = new_state;
        readback_pending_ = false;
        if (new_state == ATCState::IDLE && !assigned_runway_.empty()) {
          XPLMDebugString("[xp_wellys_atc] Runway lock released (auto-correction)\n");
          assigned_runway_.clear();
        }
        active_correction_key_.clear();
        correction_timer_ = 0.0f;
      }
      return;
    }
  }

  // No matching condition — reset timer
  active_correction_key_.clear();
  correction_timer_ = 0.0f;
}

} // namespace atc_state_machine
