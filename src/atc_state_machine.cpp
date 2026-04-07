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
#include "settings.hpp"

#include <XPLMUtilities.h>

#include <cmath>
#include <cstdio>
#include <unordered_map>

namespace atc_state_machine {

static ATCState state_ = ATCState::IDLE;

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

// Helper: get runway — pilot speech > wind-determined > fallback
static std::string
get_runway(const intent_parser::PilotMessage &msg,
           const xplane_context::XPlaneContext &ctx) {
  if (!msg.runway.empty())
    return msg.runway;
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

void init() { state_ = ATCState::IDLE; }

void stop() { state_ = ATCState::IDLE; }

void reset() {
  state_ = ATCState::IDLE;
  XPLMDebugString("[xp_wellys_atc] ATC state machine reset to IDLE\n");
}

ATCState get_state() { return state_; }

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

  // Frequency-based state validation at towered airports
  // Skip validation for: unknown freq (pilot between frequencies) and
  // tower-only airports on tower freq (all states valid on single frequency)
  bool needs_freq_validation =
      ctx.frequency_type != FT::UNKNOWN &&
      !(ctx.tower_only && ctx.frequency_type == FT::TOWER);

  if (needs_freq_validation) {
    if (ctx.frequency_type == FT::GROUND) {
      if (state_ != ATCState::IDLE && state_ != ATCState::GROUND_CONTACT &&
          state_ != ATCState::TAXI_CLEARED) {
        state_ = ATCState::IDLE;
      }
    } else if (ctx.frequency_type == FT::TOWER) {
      if (state_ != ATCState::IDLE && state_ != ATCState::TOWER_CONTACT &&
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
    if (msg.intent == PI::READY_FOR_DEPARTURE &&
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

  // Template-based response lookup
  auto vars = build_vars(msg, ctx);
  std::string intent_key = intent_parser::intent_template_key(msg.intent);
  std::string state_str = state_name(state_);

  auto tmpl = atc_templates::lookup(true, state_str, intent_key);
  resp.text = atc_templates::fill(tmpl.response_template, vars);
  resp.next_state = state_from_name(tmpl.next_state);
  resp.requires_readback = tmpl.requires_readback;

  // Apply state transition if we have a response
  if (!resp.text.empty()) {
    char log[256];
    std::snprintf(log, sizeof(log), "[xp_wellys_atc] ATC state: %s -> %s\n",
                  state_name(state_), state_name(resp.next_state));
    XPLMDebugString(log);
    state_ = resp.next_state;
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

} // namespace atc_state_machine
