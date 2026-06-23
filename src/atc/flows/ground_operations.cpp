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

#include "atc/flows/ground_operations.hpp"

#include "atc/atc_templates.hpp"
#include "atc/atis_generator.hpp"
#include "atc/engine.hpp"
#include "atc/flight_phase.hpp"
#include "atc/flows/state_storage.hpp"
#include "core/logging.hpp"
#include "data/airport_vrps.hpp"
#include "data/cifp_reader.hpp"
#include "persistence/settings.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace ground_ops {

using atc_state_machine::ATCState;
using atc_state_machine::state_from_name;
using atc_state_machine::state_name;
namespace internal = atc_state_machine::internal;

// Set when ATC has issued a squawk check and is awaiting correct transponder
// settings. Cleared when the transponder is verified correct.
static bool s_squawk_check_pending_ = false;

// ── Helpers (formerly file-local statics in atc_state_machine.cpp) ──

static std::string abbreviate_callsign(const std::string &cs) {
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

  // ICAO Doc 4444 §5.2: abbreviated callsign = first character + last two
  // characters of the registration. In phonetic form: first word + last 2 words.
  // e.g. "November One One One Romeo Charlie" → "November Romeo Charlie"
  return words.front() + " " +
         words[words.size() - 2] + " " +
         words[words.size() - 1];
}

static std::string get_callsign(const PilotMessage &msg) {
  // Session-locked callsign wins over per-utterance parsing once the
  // dialog has left IDLE — keeps the tower addressing the pilot by
  // the established name even when a later transmission is garbled
  // and the parser pulls a fragment like "Delta" out of the noise.
  const std::string &session_cs = internal::session_callsign_ref();
  std::string cs;
  if (!session_cs.empty())
    cs = session_cs;
  else if (!msg.callsign.empty())
    cs = msg.callsign;
  else
    cs = settings::pilot_callsign();
  if (internal::get_state_ref() != ATCState::IDLE)
    cs = abbreviate_callsign(cs);
  return cs;
}

static std::string get_runway(const PilotMessage &msg,
                              const XPlaneContext &ctx) {
  if (!msg.runway.empty())
    return msg.runway;
  const std::string &assigned = internal::assigned_runway_ref();
  if (!assigned.empty())
    return assigned;
  if (!ctx.active_runway.empty())
    return ctx.active_runway;
  return "28";
}

static std::string format_qnh(int hpa) { return std::to_string(hpa); }

static std::string format_altimeter(float inhg) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%.2f", inhg);
  return buf;
}

static std::string format_wind(float dir, float spd) {
  if (spd < 3.0f)
    return "calm";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%03.0f degrees %02.0f knots", dir, spd);
  return buf;
}

static std::string airport_name(const XPlaneContext &ctx) {
  std::string name = !ctx.nearest_airport_name.empty()
                         ? ctx.nearest_airport_name
                         : (!ctx.nearest_airport_id.empty()
                                ? ctx.nearest_airport_id
                                : "Airport");
  // Use city name only — strip local suffix ("Annecy Meythet" → "Annecy")
  auto sep = name.find_first_of(" -");
  return (sep != std::string::npos) ? name.substr(0, sep) : name;
}

static std::string extract_position(const PilotMessage &msg,
                                    const XPlaneContext &ctx) {
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

// ── SID name TTS formatter ───────────────────────────────────────────

// Converts a SID designator to a speakable form for TTS.
// "ROMA2A" → "ROMA 2 Alpha", "LTP2A" → "LTP 2 Alpha"
// Pattern: last char = letter, second-to-last = digit.
static std::string format_sid_for_tts(const std::string &sid) {
  if (sid.size() < 3)
    return sid;
  char last  = sid.back();
  char prev  = sid[sid.size() - 2];
  if (!std::isalpha(static_cast<unsigned char>(last)) ||
      !std::isdigit(static_cast<unsigned char>(prev)))
    return sid;
  static const char *kNato[] = {
      "Alpha","Bravo","Charlie","Delta","Echo","Foxtrot","Golf","Hotel",
      "India","Juliet","Kilo","Lima","Mike","November","Oscar","Papa",
      "Quebec","Romeo","Sierra","Tango","Uniform","Victor","Whiskey",
      "X-ray","Yankee","Zulu"};
  char letter = static_cast<char>(std::toupper(static_cast<unsigned char>(last)));
  std::string nato = (letter >= 'A' && letter <= 'Z')
                         ? kNato[letter - 'A'] : std::string(1, letter);
  return sid.substr(0, sid.size() - 2) + " " + prev + " " + nato;
}

// ── Squawk generation ────────────────────────────────────────────────

// Generate a random 4-digit octal squawk within the IFR range, avoiding
// reserved codes (7700 emergency, 7600 comm failure, 7500 hijack,
// 7000 VFR conspicuity Europe, 2000 IFR conspicuity).
static std::string generate_squawk() {
  const auto &d = flight_phase::get_ifr_defaults();
  int lo = d.squawk_range_min;
  int hi = d.squawk_range_max;
  static const int kReserved[] = {7700, 7600, 7500, 7000, 2000, 0};

  for (int attempts = 0; attempts < 50; ++attempts) {
    // Each digit 0-7 independently (octal).
    int sq = ((std::rand() & 7) * 1000) + ((std::rand() & 7) * 100) +
             ((std::rand() & 7) * 10) + (std::rand() & 7);
    if (sq < lo || sq > hi)
      continue;
    bool reserved = false;
    for (int i = 0; kReserved[i] != 0; ++i)
      if (sq == kReserved[i]) { reserved = true; break; }
    if (reserved)
      continue;
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%04d", sq);
    return buf;
  }
  return "2341"; // deterministic fallback
}

// ── IFR state redirect ───────────────────────────────────────────────

std::string effective_state_for_template(ATCState state,
                                         const PilotMessage &msg,
                                         const XPlaneContext &ctx) {
  using PI = intent_parser::PilotIntent;
  const bool has_ifr_squawk = !internal::ifr_squawk_ref().empty();

  // IDLE or GROUND_CONTACT + REQUEST_TAXI + IFR plan filed + no clearance yet
  // → redirect so Ground tells the pilot to request IFR clearance first.
  if (!has_ifr_squawk && !ctx.ifr_destination.empty() &&
      (state == ATCState::IDLE || state == ATCState::GROUND_CONTACT) &&
      msg.intent == PI::REQUEST_TAXI) {
    return "IFR/GROUND_NO_CLEARANCE";
  }

  // IFR_APPROACH_CONTACT + INITIAL_CALL_APPROACH but pilot NOT on Approach freq
  // → pilot is reading back Centre's "contact Approach on X.XXX" while still on
  //    Centre frequency, not actually checking in with Approach. Treat as silent
  //    readback so the state stays in IFR_APPROACH_CONTACT until the pilot
  //    switches frequency and calls again.
  if (state == ATCState::IFR_APPROACH_CONTACT &&
      msg.intent == PI::INITIAL_CALL_APPROACH &&
      ctx.frequency_type != xplane_context::FrequencyType::APPROACH) {
    return "IFR/APPROACH_CONTACT_READBACK";
  }

  // IFR_APPROACH_TOWER + any intent while still on Approach freq
  // → pilot is reading back Approach's "contact Tower, report established" before
  //    switching. Treat as silent readback until they call again on Tower.
  if (state == ATCState::IFR_APPROACH_TOWER &&
      ctx.frequency_type == xplane_context::FrequencyType::APPROACH) {
    return "IFR/APPROACH_TOWER_READBACK";
  }

  // TOWER_CONTACT + departure/holding intent + IFR squawk → IFR takeoff flow.
  if (state == ATCState::TOWER_CONTACT && has_ifr_squawk &&
      (msg.intent == PI::READY_FOR_DEPARTURE ||
       msg.intent == PI::READY_FOR_DEPARTURE_VFR ||
       msg.intent == PI::REPORT_HOLDING_SHORT)) {
    return "IFR/TOWER_CONTACT";
  }

  // IFR_LINE_UP_AND_WAIT + ready → virtual state for takeoff clearance.
  if (state == ATCState::IFR_LINE_UP_AND_WAIT &&
      (msg.intent == PI::READY_FOR_DEPARTURE ||
       msg.intent == PI::READY_FOR_DEPARTURE_VFR)) {
    return "IFR/LINE_UP_AND_WAIT_READY";
  }

  return atc_state_machine::state_name(state);
}

// ── build_vars: template variable map ───────────────────────────────

std::map<std::string, std::string> build_vars(const PilotMessage &msg,
                                              const XPlaneContext &ctx) {
  static const char *letter_names[] = {
      "Alpha",  "Bravo",   "Charlie", "Delta",  "Echo",   "Foxtrot", "Golf",
      "Hotel",  "India",   "Juliet",  "Kilo",   "Lima",   "Mike",    "November",
      "Oscar",  "Papa",    "Quebec",  "Romeo",  "Sierra", "Tango",   "Uniform",
      "Victor", "Whiskey", "X-ray",   "Yankee", "Zulu"};
  char letter = atis_generator::current_letter();
  std::string atis_letter_name =
      (letter >= 'A' && letter <= 'Z') ? letter_names[letter - 'A'] : "";
  std::string atis_tail =
      atis_letter_name.empty() ? "" : ", information " + atis_letter_name + " current";

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

  std::string position_remark;
  if (!msg.has_position &&
      (msg.intent == intent_parser::PilotIntent::REQUEST_TAXI ||
       msg.intent == intent_parser::PilotIntent::INITIAL_CALL_GROUND)) {
    position_remark = "say position. ";
  }

  std::string taxi_controller = ctx.tower_only ? "Tower" : "Ground";

  std::string tower_handoff_phrase;
  if (ctx.frequency_type != FT::TOWER && tower_freq >= 100.0f) {
    tower_handoff_phrase = ", contact Tower on " + format_freq(tower_freq);
  }

  // Aircraft type for the VFR initial-call hint, rendered as an optional
  // ", DV20" fragment: empty when X-Plane's acf_ICAO DataRef hasn't been
  // populated yet (cold-start race, payware liveries with empty ICAO),
  // a leading comma + space when set so the template stays readable.
  std::string aircraft_type_phrase;
  if (!ctx.aircraft_icao.empty())
    aircraft_type_phrase = ", " + ctx.aircraft_icao;

  return {
      {"current_altitude", [&]() -> std::string {
        // Always feet — this variable is used at initial Approach/Departure
        // contact which occurs below the transition altitude.
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d feet",
                      static_cast<int>(std::round(ctx.altitude_ft_msl)));
        return buf;
      }()},
      {"callsign", get_callsign(msg)},
      {"airport", airport_name(ctx)},
      {"runway", get_runway(msg, ctx)},
      {"wind", format_wind(ctx.wind_direction_deg, ctx.wind_speed_kt)},
      {"qnh", format_qnh(ctx.qnh_hpa)},
      {"altimeter", format_altimeter(ctx.qnh_inhg)},
      {"atis_letter", atis_letter_name},
      {"atis_tail", atis_tail},
      {"frequency", format_freq(ground_freq)},
      {"tower_frequency", format_freq(tower_freq)},
      {"ground_frequency", format_freq(ground_freq)},
      {"taxi_controller", taxi_controller},
      {"aircraft_type", ctx.aircraft_icao},
      {"aircraft_type_phrase", aircraft_type_phrase},
      {"position", extract_position(msg, ctx)},
      {"pattern_direction",
       [&]() {
         std::string dir = airport_vrps::get_pattern_direction(
             ctx.nearest_airport_id, ctx.active_runway);
         return dir.empty() ? settings::pattern_direction() : dir;
       }()},
      {"entry_vrp", msg.vrp_name},
      {"position_remark", position_remark},
      {"tower_handoff_phrase", tower_handoff_phrase},
      // Traffic-advisory placeholders. Empty for normal pilot-driven
      // intents — populated by render_traffic_advisory() and
      // traffic_dialog before template fill(). {side} is Phase-3's
      // ground-conflict left/right token.
      {"clock", ""},
      {"distance", ""},
      {"direction", ""},
      {"altitude_info", ""},
      {"type", ""},
      {"side", ""},
      // Phase-4 landing-sequence placeholders. Default to empty —
      // pattern_flow::apply_landing_sequence() overwrites them when
      // sequencing actually applies (user_position >= 2 or runway
      // occupied). {seq_position} carries the leg label of the
      // aircraft directly ahead of the user ("left base" / "right
      // downwind" / ...). Namespaced to "seq_" to avoid collision
      // with {position} which carries the user-side pattern leg.
      {"seq_number", ""},
      {"seq_type", ""},
      {"seq_position", ""},
      // ── IFR clearance variables ──────────────────────────────────
      // {squawk}: always fresh on REQUEST_IFR_CLEARANCE (new clearance = new
      // code); subsequent build_vars() calls return the stored value unchanged.
      {"squawk", [&]() -> std::string {
        if (msg.intent == intent_parser::PilotIntent::REQUEST_IFR_CLEARANCE) {
          std::string sq = generate_squawk();
          internal::set_ifr_squawk(sq);
          return sq;
        }
        return internal::ifr_squawk_ref();
      }()},
      // {squawk_check}: active squawk reminder for taxi clearance.
      // Expands to ", verify squawk XXXX mode Charlie" only when the
      // transponder is not already set to the assigned code in ALT/Mode C.
      // Empty when already correct — pilot needs no reminder.
      {"squawk_check", [&]() -> std::string {
        const std::string &sq = internal::ifr_squawk_ref();
        if (sq.empty())
          return std::string{};
        bool code_ok = (std::to_string(ctx.transponder_code) == sq);
        bool mode_ok = (ctx.transponder_mode >= 2); // 2=ALT (Mode C)
        if (code_ok && mode_ok)
          return std::string{};
        return ", verify squawk " + sq + " mode Charlie";
      }()},
      // {ifr_destination}: destination ICAO from X-Plane flight plan DataRef,
      // or "your destination" if no plan is filed.
      {"ifr_destination", ctx.ifr_destination.empty()
                               ? std::string("your destination")
                               : ctx.ifr_destination},
      // {ifr_sid_phrase}: SID name from CIFP (ATC-assigned for the active runway),
      // falling back to the SimBrief filed SID (display only), then "SID".
      // CIFP is authoritative — the SID is assigned by ATC, not the pilot's plan.
      {"ifr_sid_phrase", format_sid_for_tts(
                             !ctx.ifr_cifp_sid.empty() ? ctx.ifr_cifp_sid
                             : (!ctx.ifr_sid.empty()   ? ctx.ifr_sid
                                                       : std::string("SID")))},
      // {ifr_sid_last_fix}: last waypoint on the ATC-assigned SID (from CIFP).
      // Used for ATC-initiated "direct {ifr_sid_last_fix}" shortcut messages.
      {"ifr_sid_last_fix", ctx.ifr_sid_last_fix},
      // {ifr_initial_altitude}: clearance altitude from CIFP SID constraint,
      // falling back to apt.dat 1302 transition_alt then ifr_defaults.
      // {ifr_initial_altitude}: clearance altitude from CIFP SID constraint,
      // falling back to apt.dat 1302 transition_alt then ifr_defaults.
      // No airport-specific override here — constraints like FL090 (LFLP
      // Geneva TMA) are applied at radar contact via {ifr_departure_climb}.
      {"ifr_initial_altitude", [&]() -> std::string {
        auto cifp = cifp_reader::initial_altitude(
            ctx.cifp_dir, ctx.nearest_airport_id, ctx.active_runway);
        if (cifp.feet > 0) {
          if (cifp.is_fl) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "FL%d", cifp.feet / 100);
            return buf;
          }
          char buf[32];
          std::snprintf(buf, sizeof(buf), "%d feet", cifp.feet);
          return buf;
        }
        if (ctx.transition_alt_ft > 0) {
          char buf[32];
          std::snprintf(buf, sizeof(buf), "%d feet", ctx.transition_alt_ft);
          return buf;
        }
        int alt = flight_phase::get_ifr_defaults().initial_altitude_ft;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d feet", alt);
        return buf;
      }()},
      // {ifr_departure_climb}: climb clearance at first radar contact.
      // LFLP: RW04 + LSE/LTP/ROMAM (westbound via SOCOF) → FL090 (Geneva TMA cap);
      //        all other LFLP departures → FL110.
      // Default: midpoint between initial SID alt and SID binding minimum, rounded to 10 FL.
      {"ifr_departure_climb", [&]() -> std::string {
        if (ctx.nearest_airport_id == "LFLP") {
          if (ctx.active_runway == "04") {
            const std::string &fix = ctx.ifr_sid_last_fix.empty()
                                         ? ctx.ifr_fpl_first_fix
                                         : ctx.ifr_sid_last_fix;
            static const char *kWestFixes[] = {"LSE", "LTP", "ROMAM", nullptr};
            for (int i = 0; kWestFixes[i]; ++i)
              if (fix == kWestFixes[i]) return "FL090";
          }
          return "FL110"; // LFLP default — all other runways/SIDs
        }
        auto cifp = cifp_reader::initial_altitude(
            ctx.cifp_dir, ctx.nearest_airport_id, ctx.active_runway);
        int init_ft = cifp.feet > 0 ? cifp.feet
                      : (ctx.transition_alt_ft > 0 ? ctx.transition_alt_ft
                         : flight_phase::get_ifr_defaults().initial_altitude_ft);
        int sid_min = ctx.ifr_sid_min_alt_ft > 0
                          ? ctx.ifr_sid_min_alt_ft
                          : init_ft + 6000;
        int mid_ft = (init_ft + sid_min) / 2;
        int fl = ((mid_ft + 999) / 1000) * 10;
        char buf[16];
        std::snprintf(buf, sizeof(buf), "FL%d", fl);
        return buf;
      }()},
      // {ifr_departure_constraint}: optional departure constraint phrase inserted
      // after {ifr_initial_altitude} in the clearance template.
      // LFLP RW04 westbound (LSE/LTP/ROMAM): require SOCOF at or above 4000 feet.
      {"ifr_departure_constraint", [&]() -> std::string {
        if (ctx.nearest_airport_id == "LFLP" && ctx.active_runway == "04") {
          const std::string &fix = ctx.ifr_sid_last_fix.empty()
                                       ? ctx.ifr_fpl_first_fix
                                       : ctx.ifr_sid_last_fix;
          static const char *kWestFixes[] = {"LSE", "LTP", "ROMAM", nullptr};
          for (int i = 0; kWestFixes[i]; ++i)
            if (fix == kWestFixes[i])
              return std::string(", cross SOCOF at or above 4000 feet");
        }
        return std::string();
      }()},
      // {departure_controller}: uses the apt.dat facility name when present
      // (e.g. "CHAMBERY APP" → "Chambery Approach"), falls back to airport name.
      {"departure_controller", [&]() -> std::string {
        // Known type-suffix abbreviations to strip from the raw facility name.
        static const char *kSuffixes[] = {" APP", " DEP", " CTR", " GND",
                                          " TWR", " DLV", " DEL", " FSS", nullptr};
        auto location_from_raw = [&](const std::string &raw) -> std::string {
          std::string loc = raw;
          for (int i = 0; kSuffixes[i]; ++i) {
            std::string suf(kSuffixes[i]);
            if (loc.size() >= suf.size() &&
                loc.compare(loc.size() - suf.size(), suf.size(), suf) == 0) {
              loc = loc.substr(0, loc.size() - suf.size());
              break;
            }
          }
          if (loc.empty()) return loc;
          // Title-case: "CHAMBERY" → "Chambery"
          bool cap = true;
          for (char &c : loc) {
            if (c == ' ') { cap = true; }
            else if (cap) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); cap = false; }
            else           { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
          }
          return loc;
        };
        const std::string fallback = ctx.nearest_airport_name.empty()
                                         ? ctx.nearest_airport_id
                                         : ctx.nearest_airport_name;
        if (ctx.airport_freqs.has(FT::DEPARTURE)) {
          std::string loc = location_from_raw(ctx.airport_freqs.first_name(FT::DEPARTURE));
          return (loc.empty() ? fallback : loc) + " Departure";
        }
        if (ctx.airport_freqs.has(FT::APPROACH)) {
          std::string loc = location_from_raw(ctx.airport_freqs.first_name(FT::APPROACH));
          return (loc.empty() ? fallback : loc) + " Approach";
        }
        return fallback + " Departure";
      }()},
      // {departure_frequency}: MHz of the Departure or Approach controller.
      {"departure_frequency", [&]() -> std::string {
        float f = ctx.airport_freqs.first_mhz(FT::DEPARTURE);
        if (f < 100.0f) f = ctx.airport_freqs.first_mhz(FT::APPROACH);
        if (f < 100.0f) return "";
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.3f", f);
        return buf;
      }()},
      // {clearance_controller}: "Delivery" when a dedicated DELIVERY frequency
      // exists at the airport, "Tower" otherwise. Used in clearance templates
      // so LFLP-style airports (no Delivery) say "Annecy Tower" not "Annecy Delivery".
      {"clearance_controller", ctx.airport_freqs.has(FT::DELIVERY)
                                   ? std::string("Delivery")
                                   : ctx.airport_freqs.has(FT::GROUND)
                                       ? std::string("Ground")
                                       : std::string("Tower")},
      // {ifr_ground_handoff}: ", contact Ground on X.XXX when ready"
      // appended to the startup-approved readback. Empty when the pilot is
      // already on Ground or Delivery (clearance was issued on Ground directly).
      {"ifr_ground_handoff", [&]() -> std::string {
        if (ctx.tower_only) return ", report when ready to taxi";
        // Already on Ground or Delivery — no freq handoff needed, but still
        // tell the pilot to report when ready to taxi.
        if (ctx.frequency_type == FT::GROUND ||
            ctx.frequency_type == FT::DELIVERY)
          return ", report when ready to taxi";
        float gf = ctx.airport_freqs.first_mhz(FT::GROUND);
        if (gf < 100.0f) return ", report when ready to taxi";
        char buf[64];
        std::snprintf(buf, sizeof(buf), ", contact Ground on %.3f when ready", gf);
        return buf;
      }()},
      // {ifr_departure_contact}: post-departure frequency instruction for the
      // takeoff clearance. Expands to ", passing Xft, contact Approach on Y.YYY"
      // when ctr_departure_contact_alt_ft > 0 and an Approach/Departure frequency
      // exists; empty otherwise (plain "cleared for takeoff" with no follow-on).
      {"ifr_departure_contact", [&]() -> std::string {
        int alt = flight_phase::get_ifr_defaults().ctr_departure_contact_alt_ft;
        if (alt <= 0) return "";
        float freq = ctx.airport_freqs.first_mhz(FT::DEPARTURE);
        if (freq < 100.0f) freq = ctx.airport_freqs.first_mhz(FT::APPROACH);
        if (freq < 100.0f) return "";
        // Re-use departure_controller name computed above — look it up from vars map.
        // Build it inline to avoid ordering dependency in the initializer list.
        const auto &loc_raw = [&]() -> std::string {
          if (ctx.airport_freqs.has(FT::DEPARTURE))
            return ctx.airport_freqs.first_name(FT::DEPARTURE);
          return ctx.airport_freqs.first_name(FT::APPROACH);
        }();
        const std::string facility_name = [&]() -> std::string {
          static const char *kSuf[] = {" APP", " DEP", " CTR", " GND", " TWR", nullptr};
          std::string loc = loc_raw;
          for (int i = 0; kSuf[i]; ++i) {
            std::string s(kSuf[i]);
            if (loc.size() >= s.size() &&
                loc.compare(loc.size() - s.size(), s.size(), s) == 0) {
              loc = loc.substr(0, loc.size() - s.size());
              break;
            }
          }
          if (loc.empty())
            return ctx.airport_freqs.has(FT::DEPARTURE) ? "Departure" : "Approach";
          bool cap = true;
          for (char &c : loc) {
            if (c == ' ') { cap = true; }
            else if (cap) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); cap = false; }
            else           { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
          }
          return loc + (ctx.airport_freqs.has(FT::DEPARTURE) ? " Departure" : " Approach");
        }();
        // Cache the departure label so poll_departure_handoff() can activate
        // it later — even if the nearest airport changes en-route and loses
        // its apt.dat frequency entry by then.
        engine::set_pending_departure_label(facility_name);
        engine::set_pending_handoff_freq(freq);
        char buf[128];
        std::snprintf(buf, sizeof(buf), ", passing %dft QNH %d, contact %s on %.3f",
                      alt, ctx.qnh_hpa, facility_name.c_str(), freq);
        return buf;
      }()},
      // {holding_point}: "holding point Alpha, runway 28" when apt.dat taxiway data
      // is available; "holding point runway 28" as fallback.
      // Uses the assigned/active runway (from clearance) to look up the correct
      // holding point rather than active_runway_holding_point (which tracks the
      // position-based runway, not the cleared runway).
      {"holding_point", [&]() -> std::string {
        static const char *kPhonetic[] = {
            "Alpha",   "Bravo",   "Charlie", "Delta",  "Echo",    "Foxtrot",
            "Golf",    "Hotel",   "India",   "Juliet", "Kilo",    "Lima",
            "Mike",    "November","Oscar",   "Papa",   "Quebec",  "Romeo",
            "Sierra",  "Tango",   "Uniform", "Victor", "Whiskey", "X-ray",
            "Yankee",  "Zulu"};
        const std::string& rwy = get_runway(msg, ctx);
        // Look up holding point for the *assigned* runway, not the position-derived one.
        std::string hp;
        auto it = ctx.runway_holding_points.find(rwy);
        if (it != ctx.runway_holding_points.end())
          hp = it->second;
        if (hp.empty())
          return "holding point runway " + rwy;
        // Convert single A-Z to phonetic; leave multi-char names as-is.
        std::string name;
        if (hp.size() == 1 && hp[0] >= 'A' && hp[0] <= 'Z') {
          name = kPhonetic[hp[0] - 'A'];
        } else if (hp.size() == 1 && hp[0] >= 'a' && hp[0] <= 'z') {
          name = kPhonetic[hp[0] - 'a'];
        } else {
          name = hp;
        }
        return "holding point " + name + ", runway " + rwy;
      }()},
  };
}

// ── Pipeline guards ─────────────────────────────────────────────────

// Maps a cleared/active state to the state where the pilot can
// re-issue the corresponding request. Used by NEGATIVE_CORRECTION.
static ATCState revert_target(ATCState s) {
  switch (s) {
  case ATCState::DEPARTURE_CLEARED:
    return ATCState::TOWER_CONTACT;
  case ATCState::TAXI_CLEARED:
    return ATCState::GROUND_CONTACT;
  case ATCState::LANDING_CLEARED:
  case ATCState::TOUCH_AND_GO_CLEARED:
    return ATCState::PATTERN_ENTRY;
  case ATCState::PATTERN_ENTRY:
    return ATCState::TOWER_CONTACT;
  case ATCState::APPROACH_CONTACT:
    return ATCState::EN_ROUTE;
  case ATCState::GROUND_CONTACT:
  case ATCState::TOWER_CONTACT:
    return ATCState::IDLE;
  default:
    return s;
  }
}

bool handle_negative_correction(const PilotMessage &msg,
                                const XPlaneContext &ctx, ATCResponse &resp) {
  if (msg.intent != intent_parser::PilotIntent::NEGATIVE_CORRECTION)
    return false;
  auto vars = build_vars(msg, ctx);
  ATCState prev = internal::get_state_ref();
  ATCState target = revert_target(prev);
  if (target != prev) {
    internal::transition_to(target, "negative_correction");
    if (prev == ATCState::DEPARTURE_CLEARED)
      internal::set_departure_type(internal::DepartureType::PATTERN);
    resp.text = atc_templates::fill(
        "{callsign}, roger, correction noted, say intentions.", vars);
  } else {
    resp.text = atc_templates::fill("{callsign}, roger, say intentions.", vars);
    logging::info("Correction in neutral state, ack only");
  }
  resp.next_state = internal::get_state_ref();
  return true;
}

void apply_state_reverts(const PilotMessage &msg) {
  std::string intent_key = intent_parser::intent_template_key(msg.intent);
  std::string current_state = state_name(internal::get_state_ref());
  for (const auto &r : flight_phase::get_state_reverts()) {
    if (r.in_state != current_state)
      continue;
    bool intent_matches = false;
    for (const auto &k : r.on_intent_in)
      if (k == intent_key) {
        intent_matches = true;
        break;
      }
    if (!intent_matches)
      continue;
    if (!r.log.empty())
      logging::info("%s", r.log.c_str());
    internal::transition_to(state_from_name(r.revert_to), "state_revert");
    if (r.reset_departure_type)
      internal::set_departure_type(internal::DepartureType::PATTERN);
    return;
  }
}

bool handle_unicom_flow(const PilotMessage &msg, const XPlaneContext &ctx,
                        ATCResponse &resp) {
  using FT = xplane_context::FrequencyType;
  // IFR states are always under radar/centre control — never reset via
  // unicom flow even when the nearest airport happens to be uncontrolled
  // (normal en-route over rural France).
  const std::string state_str =
      atc_state_machine::state_name(internal::get_state_ref());
  if (state_str.rfind("IFR/", 0) == 0)
    return false;

  bool unicom_flow = !ctx.is_towered_airport ||
                     ctx.frequency_type == FT::UNICOM ||
                     ctx.frequency_type == FT::CTAF;
  if (!unicom_flow)
    return false;
  auto vars = build_vars(msg, ctx);
  std::string intent_key = intent_parser::intent_template_key(msg.intent);
  auto tmpl = atc_templates::lookup(false, "IDLE", intent_key);
  resp.text = atc_templates::fill(tmpl.response_template, vars);
  resp.next_state = ATCState::IDLE;
  internal::transition_to(ATCState::IDLE, "unicom_flow_idle");
  return true;
}

bool handle_frequency_hint(const PilotMessage &msg, const XPlaneContext &ctx,
                           ATCResponse &resp) {
  using FT = xplane_context::FrequencyType;
  if (ctx.frequency_type != FT::UNKNOWN || !ctx.is_towered_airport)
    return false;
  if (msg.intent == intent_parser::PilotIntent::READBACK)
    return false;
  // IFR clearance requests on a wrong/unknown frequency are handled by
  // check_freq_precondition with the proper Ground/Delivery redirect message.
  if (msg.intent == intent_parser::PilotIntent::REQUEST_IFR_CLEARANCE)
    return false;
  // Never suggest a local VFR tower to an airborne pilot: the unknown
  // frequency is almost certainly a Centre/FIR freq not in the airport DB.
  if (!ctx.on_ground)
    return false;
  const flight_phase::FrequencyHint *fh = flight_phase::get_frequency_hint();
  if (!fh)
    return false;
  std::string intent_key = intent_parser::intent_template_key(msg.intent);
  bool needs_ground = false;
  for (const auto &k : fh->ground_intents)
    if (k == intent_key) {
      needs_ground = true;
      break;
    }
  const std::string &tmpl = (needs_ground && !ctx.tower_only)
                                ? fh->ground_response
                                : fh->tower_response;
  if (tmpl.empty())
    return false;
  auto vars = build_vars(msg, ctx);
  resp.text = atc_templates::fill(tmpl, vars);
  resp.next_state = internal::get_state_ref();
  logging::info("ATC: wrong frequency, hint given");
  return true;
}

void apply_state_frequency_validity(const XPlaneContext &ctx) {
  using FT = xplane_context::FrequencyType;
  ATCState cur = internal::get_state_ref();
  bool needs_freq_validation =
      ctx.frequency_type != FT::UNKNOWN &&
      !(ctx.tower_only && ctx.frequency_type == FT::TOWER) &&
      cur != ATCState::EN_ROUTE;
  if (!needs_freq_validation)
    return;
  const auto *allowed =
      flight_phase::get_state_frequency_validity(ctx.frequency_type);
  if (!allowed)
    return;
  std::string current = state_name(cur);
  for (const auto &s : *allowed)
    if (s == current)
      return;
  internal::transition_to(ATCState::IDLE, "freq_validity_reset");
}

void apply_frequency_auto_corrections(const XPlaneContext &ctx) {
  ATCState cur = internal::get_state_ref();
  std::string current_state = state_name(cur);
  auto *fc = flight_phase::get_frequency_auto_corrections(current_state);
  if (!fc)
    return;
  for (const auto &[cond_name, rule] : *fc) {
    bool match = false;
    for (auto ft : rule.frequencies)
      if (ctx.frequency_type == ft) {
        match = true;
        break;
      }
    if (!match)
      continue;
    ATCState target = state_from_name(rule.next_state);
    if (target != cur) {
      std::string reason = "freq_auto_correction:";
      reason += rule.log.empty() ? cond_name : rule.log;
      internal::transition_to(target, reason.c_str());
    }
    return;
  }
}

bool handle_idle_redirects(const PilotMessage &msg, const XPlaneContext &ctx,
                           ATCResponse &resp) {
  if (internal::get_state_ref() != ATCState::IDLE)
    return false;
  std::string intent_key = intent_parser::intent_template_key(msg.intent);
  for (const auto &r : flight_phase::get_idle_redirects()) {
    if (r.freq_type != ctx.frequency_type)
      continue;
    if (r.unless_flag == "tower_only" && ctx.tower_only)
      continue;
    bool intent_matches = false;
    for (const auto &k : r.intent_in)
      if (k == intent_key) {
        intent_matches = true;
        break;
      }
    if (!intent_matches)
      continue;
    auto vars = build_vars(msg, ctx);
    resp.text = atc_templates::fill(r.response, vars);
    ATCState dest = r.next_state.empty()
                        ? ATCState::IDLE
                        : atc_state_machine::state_from_name(r.next_state);
    resp.next_state = dest;
    internal::transition_to(dest, "idle_redirect");
    if (!r.log.empty())
      logging::info("%s", r.log.c_str());
    return true;
  }
  return false;
}

bool check_phase_precondition(const PilotMessage &msg, const XPlaneContext &ctx,
                              ATCResponse &resp) {
  std::string intent_key = intent_parser::intent_template_key(msg.intent);
  auto phase = flight_phase::get();
  std::string rejection = flight_phase::check_precondition(intent_key, phase);
  if (rejection.empty())
    return false;
  auto vars = build_vars(msg, ctx);
  resp.text = atc_templates::fill(rejection, vars);
  resp.next_state = internal::get_state_ref();
  logging::info("Phase guard: %s blocked in phase %s", intent_key.c_str(),
                flight_phase::phase_name(phase));
  return true;
}

bool check_handoff_reissue(const PilotMessage &msg, const XPlaneContext &ctx,
                           ATCResponse &resp) {
  using FT = xplane_context::FrequencyType;
  using AS = atc_state_machine::ATCState;
  auto cur = internal::get_state_ref();

  // TOWER_CONTACT + Ground: pilot called back on Ground instead of switching to Tower.
  if (cur == AS::TOWER_CONTACT && ctx.frequency_type == FT::GROUND) {
    // Pilot is reading back the handoff before switching — let it pass silently.
    using PI = intent_parser::PilotIntent;
    if (msg.intent == PI::READBACK || msg.intent == PI::LEAVING_FREQUENCY)
      return false;
    float tower_freq = ctx.airport_freqs.first_mhz(FT::TOWER);
    if (tower_freq >= 100.0f) {
      auto vars = build_vars(msg, ctx);
      char tmpl[128];
      std::snprintf(tmpl, sizeof(tmpl),
                    "{callsign}, I say again, contact Tower on %.3f.", tower_freq);
      resp.text = atc_templates::fill(tmpl, vars);
      resp.next_state = AS::TOWER_CONTACT;
      logging::info("Handoff re-issue (TOWER_CONTACT): pilot on Ground -> Tower %.3f",
                    tower_freq);
      return true;
    }
  }

  // IFR handoff-pending states: pilot called back on old (non-Centre) frequency.
  const std::string state_str = atc_state_machine::state_name(cur);
  bool ifr_handoff = (state_str == "IFR/FREQ_HANDOFF" ||
                      state_str == "IFR/ENROUTE_CRUISE");
  if (ifr_handoff &&
      ctx.frequency_type != FT::UNKNOWN &&
      ctx.frequency_type != FT::CTAF   &&
      ctx.frequency_type != FT::UNICOM) {
    using PI = intent_parser::PilotIntent;
    // Pilot is reading back the handoff instruction before switching — let it
    // pass silently. "I say again" only fires for non-readback transmissions
    // (check-ins, requests) that clearly indicate the pilot is still on old freq
    // without intending to switch.
    if (msg.intent == PI::READBACK || msg.intent == PI::LEAVING_FREQUENCY)
      return false;

    float pending_freq = engine::pending_handoff_freq();
    if (pending_freq >= 100.0f) {
      // Pilot already tuned to the handoff frequency — let the check-in through.
      float pilot_freq = (ctx.active_com == 1) ? ctx.com1_freq_mhz
                                               : ctx.com2_freq_mhz;
      if (std::fabs(pilot_freq - pending_freq) < 0.005f)
        return false;
    }
    const std::string &ctrl = engine::current_controller_label().empty()
                                  ? engine::pending_departure_label()
                                  : engine::current_controller_label();
    if (pending_freq >= 100.0f && !ctrl.empty()) {
      auto vars = build_vars(msg, ctx);
      char tmpl[256];
      std::snprintf(tmpl, sizeof(tmpl),
                    "{callsign}, I say again, contact %s on %.3f.",
                    ctrl.c_str(), pending_freq);
      resp.text = atc_templates::fill(tmpl, vars);
      resp.next_state = cur;
      logging::info("Handoff re-issue (%s): pilot on old freq -> %s %.3f",
                    state_str.c_str(), ctrl.c_str(), pending_freq);
      return true;
    }
  }

  return false;
}

bool check_freq_precondition(const PilotMessage &msg, const XPlaneContext &ctx,
                             ATCResponse &resp) {
  using FT  = xplane_context::FrequencyType;
  using PI  = intent_parser::PilotIntent;
  // IFR clearance requested on the wrong frequency (Tower, UNKNOWN, ATIS, …)
  // when Delivery or Ground is available: redirect with controller name + freq.
  // Covers Tower (common mistake) and any unrecognised freq (e.g. airport DB
  // mismatch). Tower-only airports are exempt — Tower IS the clearance freq.
  if (msg.intent == PI::REQUEST_IFR_CLEARANCE &&
      ctx.frequency_type != FT::DELIVERY &&
      ctx.frequency_type != FT::GROUND   &&
      !ctx.tower_only &&
      (ctx.airport_freqs.has(FT::DELIVERY) || ctx.airport_freqs.has(FT::GROUND))) {
    auto vars = build_vars(msg, ctx);
    bool has_del = ctx.airport_freqs.has(FT::DELIVERY);
    const char *ctrl_name = has_del ? "Delivery" : "Ground";
    float ctrl_freq = ctx.airport_freqs.first_mhz(has_del ? FT::DELIVERY : FT::GROUND);
    char tmpl[160];
    std::snprintf(tmpl, sizeof(tmpl),
                  "{callsign}, for IFR clearance contact %s on %.3f.",
                  ctrl_name, ctrl_freq);
    resp.text = atc_templates::fill(tmpl, vars);
    resp.next_state = internal::get_state_ref();
    logging::info("IFR clearance on wrong freq (%d) redirected to %s %.3f",
                  static_cast<int>(ctx.frequency_type), ctrl_name, ctrl_freq);
    return true;
  }
  if (ctx.tower_only && ctx.frequency_type == FT::TOWER)
    return false;
  // IFR airborne states: pilot is on a Centre/en-route frequency that will
  // never appear in any local airport DB (always UNKNOWN). Skip the guard.
  {
    const std::string s = atc_state_machine::state_name(internal::get_state_ref());
    if (s == "IFR/RADAR_CONTACT"    || s == "IFR/ENROUTE_CRUISE" ||
        s == "IFR/FREQ_HANDOFF"     || s == "IFR/APPROACH_CONTACT" ||
        s == "IFR/APPROACH_DESCENT")
      return false;
  }
  std::string intent_key = intent_parser::intent_template_key(msg.intent);
  std::string rejection = flight_phase::check_frequency_precondition(
      intent_key, ctx.frequency_type);
  if (rejection.empty())
    return false;
  auto vars = build_vars(msg, ctx);
  resp.text = atc_templates::fill(rejection, vars);
  resp.next_state = internal::get_state_ref();
  // "Ready for departure" on Ground means the pilot wants to go: the answer is
  // "contact Tower", and the next logical state is TOWER_CONTACT regardless of
  // whether they were still TAXI_CLEARED or already TOWER_CONTACT. The guard
  // itself does not transition (a block normally keeps the state), so commit the
  // advance explicitly -- otherwise the TAXI_CLEARED case stays put forever.
  if ((msg.intent == PI::READY_FOR_DEPARTURE ||
       msg.intent == PI::READY_FOR_DEPARTURE_VFR) &&
      ctx.frequency_type == FT::GROUND) {
    resp.next_state = atc_state_machine::ATCState::TOWER_CONTACT;
    if (internal::get_state_ref() != atc_state_machine::ATCState::TOWER_CONTACT)
      internal::transition_to(atc_state_machine::ATCState::TOWER_CONTACT,
                              "ready_for_departure_on_ground");
  }
  logging::info("Frequency guard: %s blocked on freq_type %d",
                intent_key.c_str(), static_cast<int>(ctx.frequency_type));
  return true;
}

bool check_no_flight_plan(const PilotMessage &msg, const XPlaneContext &ctx,
                          ATCResponse &resp) {
  if (msg.intent != intent_parser::PilotIntent::REQUEST_IFR_CLEARANCE)
    return false;
  if (internal::get_state_ref() != ATCState::IDLE)
    return false;
  if (!ctx.ifr_destination.empty())
    return false;

  auto vars = build_vars(msg, ctx);
  resp.text = atc_templates::fill(
      "{callsign}, unable IFR clearance, no flight plan on file."
      " Load your SimBrief OFP in the IFR tab first.",
      vars);
  resp.next_state = ATCState::IDLE;
  logging::info("IFR clearance blocked: no flight plan (ifr_destination empty)");
  return true;
}

bool check_atis_confirmation(const PilotMessage &msg, const XPlaneContext &ctx,
                             ATCResponse &resp) {
  if (msg.intent != intent_parser::PilotIntent::REQUEST_IFR_CLEARANCE)
    return false;
  if (internal::get_state_ref() != ATCState::IDLE)
    return false;
  char atis_letter = atis_generator::current_letter();
  if (atis_letter == '\0')
    return false;

  std::string lower = msg.raw_transcript;
  for (auto &c : lower)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (lower.find("information") != std::string::npos)
    return false;

  auto vars = build_vars(msg, ctx);
  resp.text = atc_templates::fill(
      "{callsign}, advise information {atis_letter} received, then re-state IFR request.", vars);
  resp.next_state = ATCState::IDLE;
  logging::info("IFR clearance blocked: ATIS not acknowledged (letter %c)",
                atis_letter);
  return true;
}

bool check_runway_at_holding_point(const PilotMessage &msg,
                                    const XPlaneContext &ctx,
                                    ATCResponse &resp) {
  if (msg.intent != intent_parser::PilotIntent::REPORT_HOLDING_SHORT)
    return false;
  if (internal::ifr_squawk_ref().empty())
    return false; // VFR flight — no assigned runway to compare
  const std::string &assigned = internal::assigned_runway_ref();
  if (assigned.empty() || ctx.active_runway.empty())
    return false;
  if (assigned == ctx.active_runway)
    return false;

  auto vars = build_vars(msg, ctx);
  char buf[128];
  std::snprintf(buf, sizeof(buf),
                "{callsign}, confirm runway %s, you appear to be at holding "
                "point runway %s.",
                assigned.c_str(), ctx.active_runway.c_str());
  resp.text = atc_templates::fill(buf, vars);
  resp.next_state = internal::get_state_ref();
  logging::info("IFR runway mismatch at holding point: assigned=%s position=%s",
                assigned.c_str(), ctx.active_runway.c_str());
  return true;
}

bool check_squawk_at_holding_point(const PilotMessage &msg,
                                    const XPlaneContext &ctx,
                                    ATCResponse &resp) {
  using PI = intent_parser::PilotIntent;
  // Trigger on initial REPORT_HOLDING_SHORT, or on any subsequent message
  // while a squawk check is still pending (transponder not yet correct).
  const bool initial = (msg.intent == PI::REPORT_HOLDING_SHORT);
  if (!initial && !s_squawk_check_pending_)
    return false;

  const std::string &assigned = internal::ifr_squawk_ref();
  if (assigned.empty()) {
    s_squawk_check_pending_ = false;
    return false; // no IFR squawk assigned — VFR flight, nothing to check
  }

  char actual_buf[8];
  std::snprintf(actual_buf, sizeof(actual_buf), "%04d", ctx.transponder_code);
  const bool code_ok = (std::string(actual_buf) == assigned);
  // X-Plane transponder_mode: 0=OFF, 1=STBY, 2=ON(Mode A), 3=ALT(Mode C).
  const bool mode_ok = (ctx.transponder_mode >= 3);

  if (code_ok && mode_ok) {
    s_squawk_check_pending_ = false;
    return false; // transponder correct — let normal flow proceed
  }

  s_squawk_check_pending_ = true;
  auto vars = build_vars(msg, ctx);
  resp.text = atc_templates::fill(
      "{callsign}, squawk {squawk} mode Charlie, confirm.", vars);
  resp.next_state = internal::get_state_ref();
  logging::info("IFR squawk check at holding point: assigned=%s actual=%s mode=%d",
                assigned.c_str(), actual_buf, ctx.transponder_mode);
  return true;
}

bool check_sid_visibility(const PilotMessage &msg, const XPlaneContext &ctx,
                          ATCResponse &resp) {
  using PI = intent_parser::PilotIntent;
  if (msg.intent != PI::REQUEST_IFR_CLEARANCE)
    return false;
  if (internal::get_state_ref() != ATCState::IDLE)
    return false;
  if (ctx.nearest_airport_id != "LFLP" || ctx.active_runway != "04")
    return false;
  const std::string &fix = ctx.ifr_sid_last_fix.empty()
                               ? ctx.ifr_fpl_first_fix
                               : ctx.ifr_sid_last_fix;
  static const char *kWestFixes[] = {"LSE", "LTP", "ROMAM", nullptr};
  bool is_west = false;
  for (int i = 0; kWestFixes[i]; ++i)
    if (fix == kWestFixes[i]) { is_west = true; break; }
  if (!is_west)
    return false;
  if (ctx.visibility_m >= 5000.0f)
    return false;
  auto vars = build_vars(msg, ctx);
  resp.text = atc_templates::fill(
      "{callsign}, unable, minimum visibility 5 kilometres required for {ifr_sid_phrase}.", vars);
  resp.next_state = ATCState::IDLE;
  logging::info("LFLP RW04 westbound clearance blocked: visibility %.0f m (< 5000 m)",
                ctx.visibility_m);
  return true;
}

} // namespace ground_ops
