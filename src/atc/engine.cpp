/*
 * xp_wellys_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "engine.hpp"

#include "atc/atc_state_machine.hpp"
#include "atc/atc_templates.hpp"
#include "atc/flight_phase.hpp"
#include "atc/landing_sequence.hpp"
#include "atc/traffic_advisor.hpp"
#include "atc/traffic_dialog.hpp"
#include "backends/manager.hpp"
#include "core/logging.hpp"
#include "data/airspace_db.hpp"
#include "data/cifp_reader.hpp"
#include "data/openair_db.hpp"
#include "data/simbrief_ofp.hpp"
#include "data/traffic_context.hpp"
#include "data/traffic_geometry.hpp"
#include "persistence/settings.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace engine {

static int profanity_warnings_ = 0;
static int lm_inferences_ = 0;
static traffic_advisor::AdvisoryHistory advisory_history_;
// Counts back-to-back unintelligible transmissions. Reset whenever the
// pilot lands a valid intent. Drives the escalation from "garbled" to
// "use standard phraseology" so a controller-style nudge follows the
// pilot's repeated unclear calls.
static int unclear_streak_ = 0;

// Phase-4 go-around throttle. Last monotonic clock at which a
// go-around was emitted; -1e9 = never. Keeps the trigger from re-
// firing every frame while the runway stays occupied.
static double last_go_around_emit_secs_ = -1e9;
constexpr double kGoAroundCooldownSec = 60.0;
constexpr double kGoAroundTriggerDistanceNm = 1.0;

// IFR departure handoff timer. Accumulates seconds while in
// IFR_DEPARTURE_CLEARED + CLIMB. Reset whenever the state is not
// IFR_DEPARTURE_CLEARED so a re-entry starts fresh.
static float s_departure_handoff_timer = 0.0f;
static std::string
    s_current_controller_label; // last handoff target (for transcript)
// Departure label stored when the takeoff clearance is built (ground phase).
// Activated into s_current_controller_label by poll_departure_handoff() so
// the label never appears in ground-phase transcript entries.
static std::string s_pending_departure_label;
// Frequency (MHz) the pilot was last asked to switch to. Used by
// check_handoff_reissue() to re-state the instruction when the pilot calls
// back on the old frequency.
static float s_pending_handoff_freq_mhz = 0.0f;

// IFR en-route management (IFR_ENROUTE_CRUISE state).
static float s_enroute_timer = 0.0f; // accumulates while in IFR_ENROUTE_CRUISE
static bool s_enroute_direct_issued = false;
static float s_enroute_direct_delay_sec =
    0.0f; // pseudo-random 90-120 s, set on first entry
static bool s_enroute_descent_issued = false;
static float s_enroute_deviation_cooldown_sec =
    0.0f; // countdown between deviation warnings
// Sector frequency monitoring: detect when the aircraft crosses into a
// different ACC/FIR sector (e.g. Marseille Nord → Marseille Sud).
static uint32_t s_enroute_sector_freq_khz = 0;  // 0 = not yet initialised
static float s_enroute_sector_check_sec = 0.0f; // countdown; fires at 0
// Altitude deviation monitoring during cruise.
// RVSM (>=FL290): threshold 200 ft. Below FL290: 300 ft.
static int s_enroute_cleared_alt_ft =
    0; // ATC-cleared cruise altitude (0 = unknown)
static float s_enroute_alt_warn_cooldown =
    0.0f; // countdown between altitude warnings
// Set to true when the pilot says "request descent" in IFR_ENROUTE_CRUISE.
// Consumed by poll_enroute on the next frame to issue the descent clearance.
static bool s_pilot_requested_descent = false;
// Set when the proactive step-up climb (cleared_alt < cruise_alt) has fired.
static bool s_cruise_stepup_issued = false;
// Set when ATC issues the pre-TOD "advise when ready to descend" prompt.
static bool s_enroute_descent_prompt_issued = false;
// Set when the Approach frequency handoff ("contact Approach on X.XXX") is issued.
static bool s_enroute_approach_handoff_issued = false;
static float s_enroute_app_check_sec = 0.0f; // throttle TMA-entry poll to 1 Hz
static float s_enroute_approach_freq_mhz = 0.0f; // set by build_approach_handoff

static int round_to_fl(int feet); // defined near poll_sid_climb
static void init_route_fixes(const xplane_context::XPlaneContext &ctx); // defined near poll_approach

// IFR approach STAR constraint tracking (IFR_APPROACH_CONTACT / IFR_APPROACH_DESCENT).
static std::string s_assigned_star_name;             // set by build_descent_clearance
static std::string s_assigned_dest_icao;             // set by build_descent_clearance
static std::string s_assigned_approach_designator;   // set by build_descent_clearance
static std::string s_assigned_landing_runway;        // set at APPROACH_CONTACT from CIFP
static std::vector<cifp_reader::StarWaypoint> s_approach_waypoints;
static int   s_approach_waypoint_idx   = 0;   // next constraint to issue
static float s_approach_timer          = 0.0f;
static int   s_approach_initial_fl     = 0;    // FL issued at Approach check-in
static bool              s_approach_final_issued     = false; // final altitude + QNH issued
static bool              s_approach_tower_handed_off = false; // "contact Tower, report established"
static cifp_reader::FafFix s_approach_faf;                   // FAF from CIFP + earth_fix.dat
// Expedite-descent monitor: proactive warning when required VS > current VS * 1.5.
// Fires only AFTER a step-down clearance has been issued (s_expedite_last_cleared_ft > 0).
// Distinct from s_enroute_deviation_cooldown_sec (airway/sector off-track, en-route only).
static float s_expedite_cooldown       = 0.0f;  // counts down; fires when <= 0
static int   s_expedite_last_cleared_ft = 0;    // altitude of last issued step-down
// Lateral-deviation monitor (after FAF, Tower state): cross-track from runway centerline.
static float s_alignment_cooldown = 0.0f;

// Route fix tracker — logging only, no ATC speech.
struct RouteFix { std::string ident; double lat; double lon; };
static std::vector<RouteFix> s_route_fixes;
static int   s_route_fix_idx      = 0;
static float s_route_tracker_tick = 0.0f; // seconds since last distance check
// Pending ATC-direct event from poll_approach — returned by poll_route_tracker
// so atc_session picks it up via the existing System transcript push.
static std::string s_pending_route_direct;
// Step-down trigger: fires when route tracker passes the last-cleared fix index.
// Falls back to 3-min timer when no step-down has been issued yet (idx == -1).
static int  s_last_cleared_route_idx   = -1;
static int  s_faf_route_idx            = -1; // route idx of FAF (Tower handoff trigger)
static int  s_faf_ap_idx               = -1; // FAF index in s_approach_waypoints
static int  s_map_ap_idx               = -1; // MAP index (post-MAP = GO_AROUND territory)
static bool s_approach_has_visual_final = false; // MDA approach: offset final, "runway in sight"

// IFR SID climb management (IFR_RADAR_CONTACT state).
static bool s_sid_direct_issued = false;
static bool s_sid_step1_issued = false;
static bool s_sid_cruise_issued = false;
static bool s_sid_radar_handoff_issued = false;
static bool s_sid_initialized = false; // guards one-time init block
// True once the aircraft has been detected INSIDE a CTR or TMA at least once.
// The TMA-exit handoff is only issued when this transitions from true → false,
// preventing a spurious handoff when the departure altitude is below the TMA
// floor (aircraft was never inside the TMA to begin with).
static bool s_sid_was_in_tma = false;
static float s_sid_tma_check_sec = 0.0f; // throttle openair_db TMA-exit poll to 1 Hz
static float s_sid_climb_timer = 0.0f;
static int s_sid_step1_alt_ft = 0; // computed once on first entry
static float s_sid_deviation_cooldown_sec = 0.0f;
// Aircraft position when ATC issued the direct-to clearance.
// Used to build the direct leg (origin → fix) for post-direct deviation check.
static double s_sid_direct_origin_lat = 0.0;
static double s_sid_direct_origin_lon = 0.0;
// Departure airport position captured at radar-contact entry.
// Kept here so nearest_airport_id cannot drift as the aircraft flies away.
static double s_departure_apt_lat = 0.0;
static double s_departure_apt_lon = 0.0;

// Ground runway-change detection: ATC must announce when active runway changes
// while on the ground.
static std::string s_ground_last_announced_runway; // last runway ATC announced on ground

void reset() {
  profanity_warnings_ = 0;
  lm_inferences_ = 0;
  unclear_streak_ = 0;
  advisory_history_ = traffic_advisor::AdvisoryHistory{};
  last_go_around_emit_secs_ = -1e9;
  s_departure_handoff_timer = 0.0f;
  s_enroute_timer = 0.0f;
  s_enroute_direct_issued = false;
  s_enroute_direct_delay_sec = 0.0f;
  s_enroute_descent_issued = false;
  s_pilot_requested_descent = false;
  s_enroute_descent_prompt_issued = false;
  s_enroute_approach_handoff_issued = false;
  s_enroute_approach_freq_mhz = 0.0f;
  s_enroute_deviation_cooldown_sec = 0.0f;
  s_cruise_stepup_issued = false;
  s_enroute_sector_freq_khz = 0;
  s_enroute_sector_check_sec = 0.0f;
  s_enroute_cleared_alt_ft = 0;
  s_enroute_alt_warn_cooldown = 0.0f;
  s_sid_direct_issued = false;
  s_sid_step1_issued = false;
  s_sid_cruise_issued = false;
  s_sid_radar_handoff_issued = false;
  s_sid_was_in_tma = false;
  s_sid_tma_check_sec = 0.0f;
  s_sid_climb_timer = 0.0f;
  s_sid_step1_alt_ft = 0;
  s_sid_initialized = false;
  s_sid_deviation_cooldown_sec = 0.0f;
  s_sid_direct_origin_lat = 0.0;
  s_sid_direct_origin_lon = 0.0;
  s_departure_apt_lat = 0.0;
  s_departure_apt_lon = 0.0;
  s_ground_last_announced_runway.clear();
  s_assigned_star_name.clear();
  s_assigned_dest_icao.clear();
  s_assigned_approach_designator.clear();
  s_approach_waypoints.clear();
  s_approach_waypoint_idx = 0;
  s_approach_timer = 0.0f;
  s_approach_initial_fl = 0;
  s_approach_final_issued = false;
  s_approach_tower_handed_off = false;
  s_approach_faf = {};
  s_last_cleared_route_idx    = -1;
  s_faf_route_idx             = -1;
  s_faf_ap_idx                = -1;
  s_map_ap_idx                = -1;
  s_approach_has_visual_final = false;
  s_assigned_landing_runway.clear();
  s_route_fixes.clear();
  s_route_fix_idx = 0;
  s_route_tracker_tick = 0.0f;
  s_pending_route_direct.clear();
  traffic_dialog::reset();
}

void training_jump_enroute(int cleared_alt_ft) {
  // Aircraft is already at cruise altitude — skip phases that have already passed.
  s_enroute_direct_issued = true;    // skip "direct X, when able" shortcut
  s_cruise_stepup_issued = true;     // already at cruise, no FL step-up needed
  s_enroute_timer = 0.0f;
  s_enroute_sector_freq_khz = 0;
  s_enroute_sector_check_sec = 120.0f;
  s_enroute_cleared_alt_ft = cleared_alt_ft > 0 ? cleared_alt_ft : 0;
  s_enroute_descent_issued = false;
  s_enroute_descent_prompt_issued = false;
  s_pilot_requested_descent = false;
  s_enroute_approach_handoff_issued = false;
  s_enroute_deviation_cooldown_sec = 0.0f;
  atc_state_machine::set_state(atc_state_machine::ATCState::IFR_ENROUTE_CRUISE);
}

void training_jump_approach() {
  // Skip en-route phase; pilot will call Approach to begin.
  s_enroute_descent_issued = true;
  s_enroute_approach_handoff_issued = true;
  s_enroute_approach_freq_mhz = 0.0f; // unknown at training jump — accept any frequency
  // Populate dest ICAO from OFP so poll_approach can load STAR waypoints once
  // s_assigned_star_name is set via the normal descent-clearance path.
  auto ofp = simbrief_ofp::get();
  s_assigned_dest_icao = ofp.destination_icao;
  s_assigned_star_name.clear();
  s_assigned_approach_designator.clear();
  s_assigned_landing_runway.clear();
  s_approach_waypoints.clear();
  s_approach_waypoint_idx = 0;
  s_approach_timer = 0.0f;
  s_approach_initial_fl = 0;
  s_approach_final_issued = false;
  s_approach_tower_handed_off = false;
  s_approach_faf = {};
  s_last_cleared_route_idx    = -1;
  s_faf_route_idx             = -1;
  s_faf_ap_idx                = -1;
  s_map_ap_idx                = -1;
  s_approach_has_visual_final = false;
  s_route_fixes.clear();
  s_route_fix_idx = 0;
  s_route_tracker_tick = 0.0f;
  s_pending_route_direct.clear();
  atc_state_machine::set_state(atc_state_machine::ATCState::IFR_APPROACH_CONTACT);
  // Set a temporary approach label so the transcript doesn't fall back to
  // the nearest airport name during check-in (training jump skips handoff).
  s_current_controller_label =
      s_assigned_dest_icao.empty() ? "Approach" : (s_assigned_dest_icao + " Approach");
}

void training_jump_predep() {
  atc_state_machine::set_state(atc_state_machine::ATCState::IFR_PREDEP_CLEARANCE);
}

int unclear_streak() { return unclear_streak_; }

int lm_inferences() { return lm_inferences_; }

// Lower-case copy used for keyword scanning. ASCII only — Whisper
// transcripts don't contain anything else.
static std::string to_lower_copy(const std::string &s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

// Extract the multiset of digits-only tokens from a transcript. Used by
// the LM-repair validator below: a 3B model occasionally invents runway
// numbers / frequencies / altitudes that were never in the pilot's input
// (the example pattern in the prompt has been observed leaking into
// inputs that contain no number at all). If the repair carries a
// numeric token that the original lacks, we discard the repair and fall
// back to the raw Whisper transcript. Letters and "9er" → "9" mappings
// are deliberately ignored — only contiguous digit runs are compared.
static std::vector<std::string> extract_digit_tokens(const std::string &s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (std::isdigit(static_cast<unsigned char>(c))) {
      cur += c;
    } else if (!cur.empty()) {
      out.push_back(cur);
      cur.clear();
    }
  }
  if (!cur.empty())
    out.push_back(cur);
  return out;
}

// True when `repaired` contains a digit token that is absent from
// `original`. The check is multiset-based so a repeated runway is fine
// as long as both sides have it. Catches the canonical hallucination:
//   original: "Clear for takeoff Delta Chari Hotel"  (no digits)
//   repaired: "Cleared for takeoff runway 06, ..."   (introduces "06")
static bool repair_invents_digits(const std::string &original,
                                  const std::string &repaired) {
  auto orig_digits = extract_digit_tokens(original);
  auto rep_digits = extract_digit_tokens(repaired);
  for (const auto &d : rep_digits) {
    auto it = std::find(orig_digits.begin(), orig_digits.end(), d);
    if (it == orig_digits.end())
      return true;
    orig_digits.erase(it);
  }
  return false;
}

// Plausibility guard against post-landing repair hallucinations. The
// 3B local LM occasionally rewrites "runway 06 located" (Whisper
// mishearing of "vacated") into "Cleared for takeoff runway 06" when
// it loses track of the just-landed context. Even with the prompt
// updated to forbid that, the model sometimes drifts; this hard check
// is a deterministic safety net.
//
// When `just_landed_flag` is true, any repair containing a tokenised
// takeoff/departure phrase is rejected outright. Caller falls back to
// the raw Whisper transcript.
static bool repair_violates_history(const std::string &repaired,
                                    bool just_landed_flag) {
  if (!just_landed_flag || repaired.empty())
    return false;
  std::string lower = to_lower_copy(repaired);
  static const char *kForbidden[] = {
      "cleared for takeoff", "clear for takeoff", "ready for departure",
      "ready for take",      "line up",
  };
  for (const char *needle : kForbidden) {
    if (lower.find(needle) != std::string::npos)
      return true;
  }
  return false;
}

// True if the transcript carries at least one identifiable ATC element
// (callsign extracted, runway extracted, or any of a handful of
// unambiguous EU/ICAO keywords). Used to distinguish a partially-
// understood transmission ("Tower ... runway 14 ...") from total
// noise. The set is deliberately small: words common across pilot
// requests AND readbacks, picked so a single match means the pilot
// was using radio phraseology even if Whisper killed a key word.
static bool has_recognisable_elements(const intent_parser::PilotMessage &msg) {
  if (!msg.callsign.empty())
    return true;
  if (!msg.runway.empty())
    return true;
  std::string t = to_lower_copy(msg.raw_transcript);
  static const char *kKeywords[] = {
      "tower",    "ground",    "approach",    "runway",  "request", "ready",
      "downwind", "base",      "final",       "holding", "qnh",     "wilco",
      "roger",    "departure", "information", "inbound", "vacated",
  };
  for (const char *kw : kKeywords) {
    if (t.find(kw) != std::string::npos)
      return true;
  }
  return false;
}

// Three-tier "I didn't get that" response. Increments unclear_streak_;
// the caller resets it when a valid intent finally lands. EU/ICAO
// phraseology (Doc 4444 / EU 2020/469):
//   - elements recognised        -> "garbled, say again"
//   - nothing recognised         -> "say again"
//   - 2nd unclear in a row       -> "say again, use standard phraseology"
static std::string
build_unclear_response(const intent_parser::PilotMessage &msg,
                       const std::string &fallback_cs) {
  ++unclear_streak_;
  // Prefer the session-locked callsign so a mistranscribed utterance
  // ("Delta ...") cannot hijack the tower's salutation mid-session.
  const std::string &session_cs = atc_state_machine::session_callsign();
  std::string cs;
  if (!session_cs.empty())
    cs = session_cs;
  else if (!msg.callsign.empty())
    cs = msg.callsign;
  else
    cs = fallback_cs;
  std::string prefix = cs.empty() ? std::string{} : cs + ", ";

  if (unclear_streak_ >= 2)
    return prefix +
           atc_templates::lookup_fallback("say_again_use_standard_phraseology",
                                          "say again, use standard "
                                          "phraseology.");
  if (has_recognisable_elements(msg))
    return prefix + atc_templates::lookup_fallback(
                        "garbled_say_again",
                        "your transmission was garbled, say again.");
  return prefix + atc_templates::lookup_fallback("say_again", "say again.");
}

// Convenience for the quality-rejection path which has no parsed
// PilotMessage yet — only the raw transcript and a probably-empty
// callsign hint from the cockpit settings.
static std::string build_unclear_response_raw(const std::string &transcript,
                                              const std::string &fallback_cs) {
  intent_parser::PilotMessage stub;
  stub.raw_transcript = transcript;
  return build_unclear_response(stub, fallback_cs);
}

// Reset the back-to-back unclear counter. Called whenever a meaningful
// reply (template-rendered, traffic dialog, profanity etc.) is about to
// be returned to the pilot.
static void mark_clear() { unclear_streak_ = 0; }

static std::string build_profanity_response(int warning_number,
                                            const std::string &callsign) {
  if (warning_number == 1) {
    return callsign + ", maintain proper radio discipline. Use standard "
                      "phraseology on this frequency.";
  }
  if (warning_number == 2) {
    return callsign + ", this is your final warning. Continued inappropriate "
                      "language on this frequency will be reported to the "
                      "civil aviation authority. Use standard phraseology.";
  }
  return callsign + ", your conduct has been noted and will be reported to "
                    "the aviation authority. Maintain radio discipline "
                    "immediately.";
}

// Side-channel: when traffic_dialog is awaiting a pilot ack, route the
// transcript there first. Returns true if traffic_dialog handled it
// (the caller should skip the main flow). Updates advisory_history_'s
// visual-ack lockout when the pilot reported visual contact.
static bool try_traffic_dialog(const intent_parser::PilotMessage &msg,
                               const xplane_context::XPlaneContext &ctx,
                               double now_secs, Output &out) {
  if (!traffic_dialog::is_awaiting_ack())
    return false;

  uint32_t target_id = traffic_dialog::pending_target_id();
  auto reply = traffic_dialog::handle_pilot(msg, ctx);
  if (!reply.handled)
    return false;

  if (reply.acknowledged_with_visual)
    traffic_advisor::mark_acknowledged_visual(advisory_history_, target_id,
                                              now_secs);

  if (settings::debug_logging())
    logging::debug("Traffic dialog reply: %s",
                   reply.text.empty() ? "(silent)" : reply.text.c_str());
  out.parsed = msg;
  out.response_text = std::move(reply.text);
  // Pilot landed an intelligible TRAFFIC_* reply — break any in-flight
  // "say again" escalation.
  mark_clear();
  return true;
}

static Output run_state_machine(const intent_parser::PilotMessage &msg,
                                const xplane_context::XPlaneContext &ctx_now,
                                double now_secs) {
  auto atc_resp = atc_state_machine::process(msg, ctx_now, now_secs);
  if (settings::debug_logging())
    logging::debug("ATC response text: %s",
                   atc_resp.text.empty() ? "(silent)" : atc_resp.text.c_str());
  // A landed intent (rule parser or LM both produce non-UNKNOWN) means
  // the pilot was understood — even if the state machine subsequently
  // rejected the request via _INVALID/phase guard. Break the streak so
  // the next garbled call still starts at the friendly "garbled, say
  // again" tier rather than the escalation.
  if (msg.intent != intent_parser::PilotIntent::UNKNOWN)
    mark_clear();
  Output out;
  out.parsed = msg;
  out.response_text = atc_resp.text;
  return out;
}

void process_transcript(Input in, Done done) {
  if (settings::debug_logging())
    logging::debug("Whisper response (quality=%.2f): \"%s\"", in.quality,
                   in.transcript.c_str());

  // Poor transcript quality — likely noise or engine sounds. Even at
  // very low quality the transcript may still contain a recognised
  // ATC keyword, so route via the unclear-response builder instead of
  // the fixed "say again". Never the moment to land a valid intent,
  // so the streak counter advances normally.
  if (in.quality < 0.3f) {
    logging::info("Transcript quality too low, requesting say again");
    Output out;
    out.response_text =
        build_unclear_response_raw(in.transcript, in.pilot_callsign);
    done(std::move(out));
    return;
  }

  const auto &ctx = *in.ctx;

  // Frequency guard: only process pilot transmissions on the correct frequency
  // for the current ATC state. A call on the wrong radio is silently ignored —
  // the pilot must retune and call again.
  {
    using AS = atc_state_machine::ATCState;
    using FT = xplane_context::FrequencyType;
    const auto state = atc_state_machine::get_state();
    const auto freq_t = ctx.frequency_type;
    bool wrong_freq = false;

    // IFR airborne states that require APPROACH or DEPARTURE: pilot has been
    // handed off and must check in on the departure/approach frequency.
    if (state == AS::IFR_EN_ROUTE || state == AS::IFR_RADAR_CONTACT) {
      wrong_freq = (freq_t != FT::APPROACH && freq_t != FT::DEPARTURE);
    }
    // Ground/tower states: APPROACH and DEPARTURE are wrong.
    // Excluded from this guard:
    //   EN_ROUTE / APPROACH_CONTACT — VFR cross-country, unguarded (pilot may
    //   be on
    //     Tower or Approach depending on whether flight following has been
    //     established).
    //   IFR_DEPARTURE_CLEARED / IFR_FREQ_HANDOFF — IFR post-clearance; pilot
    //   may
    //     already have switched to the departure/approach frequency.
    else if (state != AS::UNICOM_ACTIVE && state != AS::IDLE &&
             state != AS::EN_ROUTE && state != AS::APPROACH_CONTACT &&
             state != AS::IFR_DEPARTURE_CLEARED &&
             state != AS::IFR_FREQ_HANDOFF && state != AS::IFR_ENROUTE_CRUISE &&
             state != AS::IFR_APPROACH_CONTACT &&
             state != AS::IFR_APPROACH_DESCENT &&
             state != AS::IFR_APPROACH_TOWER) {
      wrong_freq = (freq_t == FT::APPROACH || freq_t == FT::DEPARTURE ||
                    freq_t == FT::ATIS);
    }

    if (wrong_freq) {
      logging::info("Wrong frequency (%s) for state %s -- ignoring",
                    xplane_context::frequency_type_name(freq_t),
                    atc_state_machine::state_name(state));
      done(Output{});
      return;
    }
  }

  // Parse intent
  auto parsed = intent_parser::parse(in.transcript, ctx);

  if (settings::debug_logging())
    logging::debug("Intent: %s (confidence=%.2f), callsign=%s",
                   intent_parser::intent_name(parsed.intent), parsed.confidence,
                   parsed.callsign.empty() ? "(none)"
                                           : parsed.callsign.c_str());

  // Traffic dialog short-circuit. When the controller is awaiting a
  // pilot acknowledgement of a traffic advisory and the pilot just
  // matched a TRAFFIC_* intent at high confidence, route directly there
  // and skip the main flow + LM disambig.
  if (traffic_dialog::is_awaiting_ack() &&
      (parsed.intent == intent_parser::PilotIntent::TRAFFIC_IN_SIGHT ||
       parsed.intent == intent_parser::PilotIntent::TRAFFIC_NEGATIVE_CONTACT ||
       parsed.intent == intent_parser::PilotIntent::TRAFFIC_LOOKING) &&
      parsed.confidence >= 0.7f) {
    Output out;
    if (try_traffic_dialog(parsed, ctx, in.now_secs, out)) {
      done(std::move(out));
      return;
    }
  }

  // Inappropriate language — intercept before state machine.
  // Does NOT change ATC state, pilot can continue normally after.
  if (parsed.intent == intent_parser::PilotIntent::INAPPROPRIATE_LANGUAGE) {
    ++profanity_warnings_;
    const std::string &session_cs = atc_state_machine::session_callsign();
    std::string cs;
    if (!session_cs.empty())
      cs = session_cs;
    else if (!parsed.callsign.empty())
      cs = parsed.callsign;
    else
      cs = in.pilot_callsign;
    logging::info("Radio discipline warning #%d", profanity_warnings_);
    Output out;
    out.parsed = parsed;
    out.response_text = build_profanity_response(profanity_warnings_, cs);
    out.is_warning = true;
    // Coherent (if rude) utterance — no "say again" loop carries over.
    mark_clear();
    done(std::move(out));
    return;
  }

  using PI = intent_parser::PilotIntent;

  // IFR en-route descent request: set flag so poll_enroute fires the
  // clearance on the next frame. No state-machine response here.
  if (parsed.intent == PI::REQUEST_DESCENT &&
      atc_state_machine::get_state() ==
          atc_state_machine::ATCState::IFR_ENROUTE_CRUISE) {
    s_pilot_requested_descent = true;
    done(Output{});
    return;
  }

  // IFR en-route climb request: pilot asks for a higher FL.
  // Issue cruise FL clearance if aircraft is below cruise altitude;
  // otherwise maintain current level.
  if (parsed.intent == PI::REQUEST_HIGHER &&
      atc_state_machine::get_state() ==
          atc_state_machine::ATCState::IFR_ENROUTE_CRUISE) {
    const std::string &cs_r = atc_state_machine::session_callsign();
    const std::string cs_h = cs_r.empty() ? in.pilot_callsign : cs_r;
    const xplane_context::XPlaneContext &ctx_h = *in.ctx;
    int cruise_fl = 0;
    if (ctx_h.ifr_cruise_alt_ft > 0)
      cruise_fl = round_to_fl(ctx_h.ifr_cruise_alt_ft);
    else if (s_enroute_cleared_alt_ft > 0)
      cruise_fl = round_to_fl(s_enroute_cleared_alt_ft + 2000);
    Output out_h;
    if (cruise_fl > 0 &&
        cruise_fl * 100 >
            static_cast<int>(ctx_h.altitude_ft_msl) + 500) {
      s_enroute_cleared_alt_ft = cruise_fl * 100;
      char buf[128];
      std::snprintf(buf, sizeof(buf), "%s, climb flight level %d.",
                    cs_h.c_str(), cruise_fl);
      out_h.response_text = buf;
      logging::info("IFR en-route: REQUEST_HIGHER -> FL%d", cruise_fl);
    } else {
      // Already at or above cruise altitude
      int fl_now = round_to_fl(static_cast<int>(ctx_h.altitude_ft_msl));
      char buf[128];
      std::snprintf(buf, sizeof(buf), "%s, maintain flight level %d.",
                    cs_h.c_str(), fl_now);
      out_h.response_text = buf;
    }
    done(std::move(out_h));
    return;
  }

  // IFR Approach check-in: intercept INITIAL_CALL_APPROACH in APPROACH_CONTACT
  // to issue "identified, descend FL[initial]" directly (template cannot hold
  // the dynamic FL). Transitions state to IFR_APPROACH_DESCENT.
  // Gate on the pilot being on the approach frequency — prevents a readback
  // on the previous Centre frequency from triggering the check-in.
  const bool on_approach_freq =
      s_enroute_approach_freq_mhz < 100.0f ||               // unknown (training)
      std::fabs((ctx.active_com == 1 ? ctx.com1_freq_mhz : ctx.com2_freq_mhz) -
                s_enroute_approach_freq_mhz) < 0.010f;
  if (parsed.intent == PI::INITIAL_CALL_APPROACH &&
      on_approach_freq &&
      atc_state_machine::get_state() ==
          atc_state_machine::ATCState::IFR_APPROACH_CONTACT) {
    using AS = atc_state_machine::ATCState;
    const std::string &cs_ref = atc_state_machine::session_callsign();
    const std::string cs = cs_ref.empty() ? in.pilot_callsign : cs_ref;

    // Training jump: s_assigned_star_name not set — derive from OFP last fix.
    // The last navlog fix before the destination is the STAR entry point;
    // CIFP maps (entry_fix, dest_runway) -> STAR name.
    if (s_assigned_star_name.empty() && !s_assigned_dest_icao.empty() &&
        !ctx.cifp_dir.empty()) {
      const auto &ofp_tj = simbrief_ofp::get();
      std::string entry_fix;
      for (int i = static_cast<int>(ofp_tj.navlog.size()) - 1; i >= 0; --i) {
        const auto &f = ofp_tj.navlog[i];
        if (!f.ident.empty() && f.ident != s_assigned_dest_icao) {
          entry_fix = f.ident;
          break;
        }
      }
      if (!entry_fix.empty()) {
        const std::string dest_rwy = cifp_reader::best_runway_for_approach(
            ctx.cifp_dir, s_assigned_dest_icao,
            ctx.wind_direction_deg, ctx.visibility_m);
        s_assigned_star_name = cifp_reader::star_name_for_entry_fix(
            ctx.cifp_dir, s_assigned_dest_icao, dest_rwy, entry_fix);
        if (s_assigned_star_name.empty())
          s_assigned_star_name = cifp_reader::star_name_for_entry_fix(
              ctx.cifp_dir, s_assigned_dest_icao, "", entry_fix);
      }
      if (s_assigned_star_name.empty()) {
        const std::string dest_rwy = cifp_reader::best_runway_for_approach(
            ctx.cifp_dir, s_assigned_dest_icao,
            ctx.wind_direction_deg, ctx.visibility_m);
        if (!dest_rwy.empty())
          s_assigned_star_name = cifp_reader::first_star_for_runway(
              ctx.cifp_dir, s_assigned_dest_icao, dest_rwy);
      }
    }

    // Load STAR waypoints now so poll_approach can use them.
    if (!s_assigned_star_name.empty() && !s_assigned_dest_icao.empty() &&
        s_approach_waypoints.empty()) {
      s_approach_waypoints = cifp_reader::star_waypoints(
          ctx.cifp_dir, s_assigned_dest_icao, s_assigned_star_name);
    }

    // Confirm approach type and append IAF-transition waypoints.
    // Must run before altitude selection so all constrained waypoints are loaded.
    std::string approach_confirm;
    if (!s_assigned_star_name.empty() && !s_assigned_dest_icao.empty() &&
        !ctx.cifp_dir.empty()) {
      std::string rwy = cifp_reader::runway_for_star(
          ctx.cifp_dir, s_assigned_dest_icao, s_assigned_star_name);
      // STAR may serve all runways — use wind-favoured runway in that case.
      if (rwy.empty())
        rwy = cifp_reader::best_runway_for_approach(
            ctx.cifp_dir, s_assigned_dest_icao,
            ctx.wind_direction_deg, ctx.visibility_m);
      if (!rwy.empty()) {
        const auto &ofp_ac = simbrief_ofp::get();
        cifp_reader::ApproachInfo appr;
        if (!ofp_ac.preferred_approach_designator.empty())
          appr = cifp_reader::approach_by_designator(
              ctx.cifp_dir, s_assigned_dest_icao,
              ofp_ac.preferred_approach_designator);
        if (appr.type_str.empty())
          appr = cifp_reader::best_approach(
              ctx.cifp_dir, s_assigned_dest_icao, rwy, ctx.visibility_m);
        if (!appr.type_str.empty()) {
          // Persist the CIFP runway so Tower uses the correct landing runway
          // regardless of which airport ctx.active_runway points to.
          s_assigned_landing_runway = appr.runway;
          // Look up FAF position so poll_approach() can trigger Tower handoff.
          if (!appr.designator.empty()) {
            s_approach_faf = cifp_reader::approach_faf(
                ctx.cifp_dir, s_assigned_dest_icao, appr.designator);
            s_assigned_approach_designator = appr.designator;
            // Append IAF-transition waypoints (skip FM vectoring + IF entry).
            const std::string iaf =
                cifp_reader::star_last_fix(ctx.cifp_dir, s_assigned_dest_icao,
                                           s_assigned_star_name);
            if (!iaf.empty()) {
              auto proc = cifp_reader::approach_procedure_waypoints(
                  ctx.cifp_dir, s_assigned_dest_icao, appr.designator, iaf);
              if (!proc.empty()) {
                for (auto &w : proc)
                  s_approach_waypoints.push_back(w);
                s_approach_final_issued = true;
                // Locate FAF and MAP in the waypoint array once, so
                // poll_approach can skip GO_AROUND territory efficiently.
                s_faf_ap_idx = -1;
                s_map_ap_idx = -1;
                for (int i = 0; i < static_cast<int>(s_approach_waypoints.size()); ++i) {
                  const auto &w = s_approach_waypoints[i];
                  if (s_faf_ap_idx < 0 && w.is_approach_proc &&
                      w.ident == s_approach_faf.ident)
                    s_faf_ap_idx = i;
                  if (s_map_ap_idx < 0 && w.is_approach_proc && w.is_map)
                    s_map_ap_idx = i;
                }
                logging::info("[route] FAF ap_idx=%d MAP ap_idx=%d",
                              s_faf_ap_idx, s_map_ap_idx);
              }
            }
          }
          // Extract variant suffix (e.g. "I04LZ" -> "Zulu").
          std::string suffix_word;
          if (!appr.designator.empty()) {
            char last = appr.designator.back();
            if (std::isalpha(static_cast<unsigned char>(last)) &&
                last != 'L' && last != 'R' && last != 'C') {
              static const char *nato[] = {
                "Alpha","Bravo","Charlie","Delta","Echo","Foxtrot","Golf",
                "Hotel","India","Juliet","Kilo","Lima","Mike","November",
                "Oscar","Papa","Quebec","Romeo","Sierra","Tango","Uniform",
                "Victor","Whiskey","X-ray","Yankee","Zulu"};
              int idx = std::toupper(static_cast<unsigned char>(last)) - 'A';
              if (idx >= 0 && idx < 26)
                suffix_word = std::string(" ") + nato[idx];
            }
          }
          approach_confirm = ", " + appr.type_str + suffix_word +
                             " approach runway " + appr.runway;
        }
      }
    }

    // Initial descent altitude: find the STAR/approach waypoint closest to
    // the aircraft. Handles mid-STAR training jumps (pilot may have already
    // passed the STAR entry fix). Falls back to first ceiling below current
    // altitude, then approach_entry_alt_ft.
    const int defaults_ft = flight_phase::get_ifr_defaults().approach_entry_alt_ft;
    int initial_ft = defaults_ft;
    if (!s_approach_waypoints.empty()) {
      // Build ident->position map from OFP navlog for distance lookups.
      const auto &ofp_pos = simbrief_ofp::get();
      std::unordered_map<std::string, std::pair<double, double>> fix_pos;
      for (const auto &nf : ofp_pos.navlog)
        if (!nf.ident.empty())
          fix_pos[nf.ident] = {nf.lat, nf.lon};

      float best_dist = -1.0f;
      int pos_ft = 0;
      for (const auto &wp : s_approach_waypoints) {
        // Skip floor-only (at-or-above) constraints and no-altitude entries.
        if (wp.alt.feet <= 0 || (wp.is_floor && !wp.is_ceiling))
          continue;
        // Only issue a descent, never a climb.
        if (wp.alt.feet >= static_cast<int>(ctx.pressure_alt_ft))
          continue;
        auto it = fix_pos.find(wp.ident);
        if (it == fix_pos.end())
          continue;
        auto d = static_cast<float>(traffic_geometry::distance_nm(
            ctx.latitude, ctx.longitude, it->second.first, it->second.second));
        if (best_dist < 0.0f || d < best_dist) {
          best_dist = d;
          pos_ft = wp.alt.feet;
        }
      }
      if (pos_ft > 0) {
        initial_ft = pos_ft;
      } else {
        // No OFP coordinates for any constrained fix: first ceiling below alt.
        for (const auto &wp : s_approach_waypoints) {
          if (wp.is_ceiling && wp.alt.feet > 0 &&
              wp.alt.feet < static_cast<int>(ctx.pressure_alt_ft)) {
            initial_ft = wp.alt.feet;
            break;
          }
        }
      }
    }
    // Floor-only constraints (e.g. MUS FL080+) must be respected: never issue
    // an initial clearance below the constraint altitude at a floor-only fix
    // that the aircraft has not yet passed.
    for (const auto &wp : s_approach_waypoints) {
      if (wp.is_floor && !wp.is_ceiling && wp.alt.feet > initial_ft)
        initial_ft = wp.alt.feet;
    }
    s_approach_initial_fl = initial_ft;

    // Skip waypoints already covered by the initial descent clearance.
    // Rules:
    //   (a) Ceiling, exact, or block constraints at or above initial_ft
    //       are cleared — pilot descends through them.
    //   (b) Floor-only (at-or-above) constraints the aircraft already
    //       meets are trivially satisfied — no instruction needed.
    // The loop continues past (b) so (a) constraints that follow are
    // also reached and skipped (e.g. MN261 "B" block after MUS floor).
    while (s_approach_waypoint_idx <
           static_cast<int>(s_approach_waypoints.size())) {
      const auto &skip_wp = s_approach_waypoints[s_approach_waypoint_idx];
      // (a) not floor-only AND altitude at or above initial clearance
      if (skip_wp.alt.feet > 0 &&
          !(skip_wp.is_floor && !skip_wp.is_ceiling) &&
          skip_wp.alt.feet >= initial_ft) {
        s_approach_waypoint_idx++;
        continue;
      }
      // (b) floor-only constraint already satisfied by current altitude
      if (skip_wp.is_floor && !skip_wp.is_ceiling &&
          skip_wp.alt.feet > 0 &&
          ctx.pressure_alt_ft >= static_cast<float>(skip_wp.alt.feet)) {
        s_approach_waypoint_idx++;
        continue;
      }
      break;
    }

    // Approach confirm precedes the descent instruction (ICAO order).
    // QNH appended when initial clearance is in feet, not FL.
    const int ta_ic = (ctx.transition_alt_ft > 0) ? ctx.transition_alt_ft : 5000;
    char alt_buf_ic[64];
    if (initial_ft > ta_ic)
      std::snprintf(alt_buf_ic, sizeof(alt_buf_ic), "flight level %d", initial_ft / 100);
    else
      std::snprintf(alt_buf_ic, sizeof(alt_buf_ic), "%d feet, QNH %d",
                    initial_ft, ctx.qnh_hpa);
    char buf[240];
    std::snprintf(buf, sizeof(buf), "%s, radar contact, identified%s, descend %s.",
                  cs.c_str(), approach_confirm.c_str(), alt_buf_ic);

    atc_state_machine::set_state(AS::IFR_APPROACH_DESCENT);

    // Build route fix list now that STAR + approach waypoints are complete.
    init_route_fixes(ctx);
    if (!s_approach_faf.ident.empty()) {
      for (int i = 0; i < static_cast<int>(s_route_fixes.size()); ++i) {
        if (s_route_fixes[i].ident == s_approach_faf.ident) {
          s_faf_route_idx = i;
          logging::info("[route] FAF %s at route idx=%d", s_approach_faf.ident.c_str(), i);
          break;
        }
      }
    }

    Output out;
    out.parsed = parsed;
    out.response_text = buf;
    done(std::move(out));
    return;
  }

  // IFR Tower check-in: pilot contacts Tower after Approach hands off.
  // INITIAL_CALL_TOWER / INITIAL_CALL in IFR_APPROACH_TOWER → landing clearance.
  // Without this handler the VFR phase precondition fires "already airborne".
  // Frequency guard: readbacks on the Approach freq that get mis-classified
  // as INITIAL_CALL must not trigger a premature clearance — only fire when
  // the pilot has switched to the Tower frequency.
  if ((parsed.intent == PI::INITIAL_CALL_TOWER ||
       parsed.intent == PI::INITIAL_CALL) &&
      atc_state_machine::get_state() ==
          atc_state_machine::ATCState::IFR_APPROACH_TOWER) {
    // Frequency guard: readbacks on the Approach freq mis-classified as
    // INITIAL_CALL must not trigger the landing clearance prematurely.
    const float tower_freq_mhz =
        xplane_context::tower_mhz_for(s_assigned_dest_icao);
    const float active_com_mhz =
        (ctx.active_com == 1) ? ctx.com1_freq_mhz : ctx.com2_freq_mhz;
    const bool on_tower_freq =
        tower_freq_mhz < 100.0f || // unknown (no OFP / training) — allow any
        std::fabs(active_com_mhz - tower_freq_mhz) < 0.010f;
    if (on_tower_freq) {
      using ASt = atc_state_machine::ATCState;
      const std::string &cs_at_ref = atc_state_machine::session_callsign();
      const std::string cs_at =
          cs_at_ref.empty() ? in.pilot_callsign : cs_at_ref;
      // Use the CIFP-derived approach runway (set at Approach check-in).
      // ctx.active_runway can belong to a different nearby airport when the
      // aircraft is still on approach, causing "runway JCA"-style mistakes.
      const std::string rwy_at =
          !s_assigned_landing_runway.empty() ? s_assigned_landing_runway :
          !ctx.active_runway.empty()         ? ctx.active_runway :
                                               ctx.nearest_airport_id;
      int wind_dir = static_cast<int>(std::round(ctx.wind_direction_deg));
      int wind_kt  = static_cast<int>(std::round(ctx.wind_speed_kt));
      char buf_at[160];
      std::snprintf(buf_at, sizeof(buf_at),
                    "%s, runway %s, cleared to land, wind %03d degrees %02d knots.",
                    cs_at.c_str(), rwy_at.c_str(), wind_dir, wind_kt);
      s_current_controller_label =
          s_assigned_dest_icao.empty() ? "Tower" : (s_assigned_dest_icao + " Tower");
      atc_state_machine::set_state(ASt::LANDING_CLEARED);
      Output out_at;
      out_at.parsed = parsed;
      out_at.response_text = buf_at;
      done(std::move(out_at));
      return;
    }
    // Pilot still on Approach freq — suppress; they need to switch to Tower.
    // Without this return the VFR machine fires "you are already airborne".
    Output out_ap;
    out_ap.parsed = parsed;
    out_ap.response_text = "";  // silent ack — pilot is reading back the handoff
    done(std::move(out_ap));
    return;
  }

  // ── LM-not-ready fast path ────────────────────────────────────────
  // Headless tools, scenario tests, and the brief window between
  // plugin start and "models verified" all hit this path. The
  // rule-based parser is authoritative here — same behaviour as
  // before always-on classification was introduced.
  if (!backends::lm_ready()) {
    if (parsed.intent == PI::UNKNOWN) {
      Output out;
      out.parsed = parsed;
      out.response_text = build_unclear_response(parsed, in.pilot_callsign);
      logging::info("ATC (LM unavailable, UNKNOWN): %s",
                    out.response_text.c_str());
      done(std::move(out));
      return;
    }
    done(run_state_machine(parsed, ctx, in.now_secs));
    return;
  }

  // ── LM as fallback only ───────────────────────────────────────────
  // The rule-based parser (data-driven matchers in intent_rules.json
  // + state-history-aware adjustments such as just_landed) is
  // authoritative. The local LM only fires when the rule parser is
  // genuinely unsure (UNKNOWN or confidence < 0.7).
  //
  // Field measurement on Apple Silicon: even with Metal flash-
  // attention and QOS_UTILITY workers, every Llama 3.2 3B classify
  // call costs visible FPS in X-Plane. At conf >= 0.7 the rule
  // parser was empirically right in nearly every observed case
  // (see LSZG circuit log 2026-05-04: REQUEST_TAXI / READBACK /
  // RUNWAY_VACATED / REPORT_POSITION_* all classified correctly at
  // 0.90, while the LM frequently disagreed wrongly or returned
  // _INVALID and was overridden by safety nets).
  if (parsed.confidence >= 0.7f && parsed.intent != PI::UNKNOWN) {
    if (settings::debug_logging())
      logging::debug("Rule-based path: %s (conf=%.2f) — skip LM",
                     intent_parser::intent_name(parsed.intent),
                     parsed.confidence);
    done(run_state_machine(parsed, ctx, in.now_secs));
    return;
  }

  // ── Always-on LM classification with constrained JSON output ──────
  // The LM gets the rule-based parser's intent as a low-priority
  // hint, the valid_intents enum for the current state (grammar-
  // enforced — model literally cannot return anything else), and the
  // flight context. It returns {intent, repaired_transcript,
  // whisper_fix}. Whisper-artifact repair is the LM's job; pilot
  // phraseology errors fall through to the state machine which still
  // reacts realistically (frequency guards, phase guards, _INVALID
  // templates).
  using FT = xplane_context::FrequencyType;
  bool is_towered = ctx.is_towered_airport &&
                    ctx.frequency_type != FT::UNICOM &&
                    ctx.frequency_type != FT::CTAF;

  std::string state_str =
      atc_state_machine::state_name(atc_state_machine::get_state());

  // IFR states live exclusively in the "towered" template section —
  // valid_intents must look there even when the nearest airport is uncontrolled
  // (e.g. en-route over rural airspace far from any towered field).
  if (state_str.rfind("IFR/", 0) == 0)
    is_towered = true;

  std::string previous_state_str =
      atc_state_machine::state_name(atc_state_machine::previous_state());
  std::string state_history_csv = atc_state_machine::history_csv();
  bool just_landed_flag = atc_state_machine::just_landed(in.now_secs);
  auto valid = atc_templates::valid_intents(is_towered, state_str);

  // Always include the traffic-acknowledgement intents — they are
  // valid any time the controller has just issued a traffic advisory,
  // regardless of which ATC state we're in.
  for (const char *t :
       {"TRAFFIC_IN_SIGHT", "TRAFFIC_NEGATIVE_CONTACT", "TRAFFIC_LOOKING"}) {
    if (std::find(valid.begin(), valid.end(), t) == valid.end())
      valid.emplace_back(t);
  }

  std::string valid_list;
  for (const auto &v : valid) {
    if (!valid_list.empty())
      valid_list += ", ";
    valid_list += v;
  }

  std::string sys_prompt = atc_templates::get_prompt("gpt_classify_prompt");
  if (sys_prompt.empty()) {
    sys_prompt = "You are an ATC intent classifier. State: {state}. "
                 "Valid intents: {valid_intents}. Hint: {hint_intent}. "
                 "Transcript: \"{transcript}\". Respond with strict JSON "
                 "{\"intent\":\"...\",\"repaired\":\"...\",\"whisper_fix\":"
                 "false}.";
  }
  sys_prompt = atc_templates::fill(
      sys_prompt,
      {{"state", state_str},
       {"previous_state", previous_state_str},
       {"state_history_csv", state_history_csv},
       {"just_landed", just_landed_flag ? "true" : "false"},
       {"valid_intents", valid_list},
       {"transcript", in.transcript},
       {"frequency_type",
        xplane_context::frequency_type_name(ctx.frequency_type)},
       {"on_ground", ctx.on_ground ? "true" : "false"},
       {"altitude_ft", std::to_string(static_cast<int>(ctx.altitude_ft_msl))},
       {"groundspeed_kts",
        std::to_string(static_cast<int>(ctx.groundspeed_kts))},
       {"airport", ctx.nearest_airport_id},
       {"hint_intent", intent_parser::intent_name(parsed.intent)}});

  if (settings::debug_logging())
    logging::debug("Routing to local LM classify_with_repair (rule hint=%s "
                   "conf=%.2f)",
                   intent_parser::intent_name(parsed.intent),
                   parsed.confidence);

  // Snapshot ctx + transcript so the async callback sees the state at
  // the moment the pilot spoke, not whatever ctx contains when the LM
  // responds.
  xplane_context::XPlaneContext ctx_snapshot = ctx;
  double now_secs = in.now_secs;
  std::string fallback_cs = in.pilot_callsign;
  std::string original_transcript = in.transcript;
  // Snapshot the just-landed flag too — the async callback may fire
  // after the state machine has moved on, but the post-landing
  // plausibility decision must reflect the moment the pilot spoke.
  bool just_landed_snapshot = just_landed_flag;
  ++lm_inferences_;
  backends::lm::classify_with_repair_async(
      in.transcript, sys_prompt, valid,
      // NOLINTNEXTLINE(bugprone-exception-escape)
      [parsed, ctx_snapshot, now_secs, fallback_cs, original_transcript,
       just_landed_snapshot, done = std::move(done)](
          const backends::lm::ClassifyResult &result) mutable {
        std::string intent_key =
            result.success ? result.intent_name : std::string("_INVALID");

        if (settings::debug_logging()) {
          logging::debug(
              "LM classified: intent=%s whisper_fix=%d repaired=\"%s\"",
              intent_key.c_str(), result.whisper_fix ? 1 : 0,
              result.repaired_transcript.c_str());
        }

        // Telemetry: log when LM and rule-based parser disagree.
        // Helps decide whether the 3B model is good enough or we need
        // a bigger one.
        auto rule_intent = parsed.intent;
        auto lm_intent = intent_parser::intent_from_key(intent_key);
        if (rule_intent != intent_parser::PilotIntent::UNKNOWN &&
            lm_intent != rule_intent && intent_key != "_INVALID") {
          logging::info("LM/rule disagree: rule=%s (conf=%.2f) llm=%s",
                        intent_parser::intent_name(rule_intent),
                        parsed.confidence, intent_key.c_str());
        }

        // Readback safety net: trust rule=READBACK whenever the rule
        // parser is confident (>=0.90), regardless of whether
        // readback_pending is currently armed. Two cases this catches:
        //   1) Mid-clearance readbacks where readback_pending=true.
        //      LM occasionally hallucinates TRAFFIC_IN_SIGHT or
        //      READY_FOR_DEPARTURE for a taxi readback whose Whisper
        //      transcription was garbled.
        //   2) Closing readbacks AFTER state→IDLE has already cleared
        //      readback_pending (e.g. post-landing "general aviation
        //      parking via Alpha, good day"). Without this widened
        //      check, LM=REQUEST_TAXI wins and triggers a brand-new
        //      departure cycle (TAXI_CLEARED → TOWER_CONTACT auto-
        //      advance), turning the parking-arrival readback into a
        //      bogus takeoff briefing.
        // The rule parser's READBACK matchers are keyword-anchored
        // (wilco/roger/good day/holding point/cleared+takeoff/qnh/
        // hold short/runway-suffix endings), so false positives are
        // rare. Letting the LM override these consistently produces
        // wrong ATC chatter at moments ICAO requires silence.
        if (rule_intent == intent_parser::PilotIntent::READBACK &&
            parsed.confidence >= 0.90f &&
            lm_intent != intent_parser::PilotIntent::READBACK) {
          logging::info("Readback safety net: keeping rule=READBACK over "
                        "LM=%s (rule_conf=%.2f, readback_pending=%s)",
                        intent_key.c_str(), parsed.confidence,
                        atc_state_machine::is_readback_pending() ? "true"
                                                                 : "false");
          intent_key = "READBACK";
          lm_intent = intent_parser::PilotIntent::READBACK;
        }

        // Validate the repair before letting it influence anything
        // downstream. If the LM invented digits that weren't in the
        // original (a runway number, a frequency, an altitude), drop
        // the repair and keep the raw Whisper text. Logged at info so
        // the rejection is visible without debug-mode.
        bool repair_accepted =
            result.whisper_fix && !result.repaired_transcript.empty();
        if (repair_accepted &&
            repair_invents_digits(original_transcript,
                                  result.repaired_transcript)) {
          logging::info("Whisper repair rejected (invented digits): "
                        "\"%s\" -> \"%s\"",
                        original_transcript.c_str(),
                        result.repaired_transcript.c_str());
          repair_accepted = false;
        } else if (repair_accepted &&
                   repair_violates_history(result.repaired_transcript,
                                           just_landed_snapshot)) {
          logging::info("Whisper repair rejected (post-landing context): "
                        "\"%s\" -> \"%s\"",
                        original_transcript.c_str(),
                        result.repaired_transcript.c_str());
          repair_accepted = false;
        } else if (repair_accepted) {
          logging::info("Whisper repair: \"%s\" -> \"%s\"",
                        original_transcript.c_str(),
                        result.repaired_transcript.c_str());
        }

        // _INVALID: controller asks for say-again. Tier picks itself
        // based on whether anything in the transcript was recognisable.
        if (intent_key == "_INVALID") {
          Output out;
          out.parsed = parsed;
          out.response_text = build_unclear_response(parsed, fallback_cs);
          logging::info("ATC (LM _INVALID): %s", out.response_text.c_str());
          done(std::move(out));
          return;
        }

        // Build a PilotMessage with the LM-classified intent. Keep
        // the rule-based callsign / runway / VRP extraction — those
        // are deterministic and don't benefit from LM interpretation.
        auto lm_msg = parsed;
        lm_msg.intent = lm_intent;
        lm_msg.confidence = 0.85f;
        if (repair_accepted) {
          // Replace the raw transcript with the repaired one so the
          // UI history shows what the controller acted on. The
          // confidence stays at 0.85 — repair doesn't make us more
          // certain about intent classification.
          lm_msg.raw_transcript = result.repaired_transcript;
        }

        // Traffic dialog short-circuit. The rule parser frequently
        // misses softer phrasings ("looking", "have the traffic") and
        // only the LM lands them on TRAFFIC_*.
        Output out;
        if (try_traffic_dialog(lm_msg, ctx_snapshot, now_secs, out)) {
          done(std::move(out));
          return;
        }

        auto atc_resp =
            atc_state_machine::process(lm_msg, ctx_snapshot, now_secs);

        if (settings::debug_logging())
          logging::debug("ATC response text: %s", atc_resp.text.empty()
                                                      ? "(silent)"
                                                      : atc_resp.text.c_str());

        // LM produced a concrete intent — pilot was understood, even
        // if the state machine subsequently rejected the request.
        if (lm_msg.intent != intent_parser::PilotIntent::UNKNOWN)
          mark_clear();
        out.parsed = lm_msg;
        out.response_text = atc_resp.text;
        done(std::move(out));
      });
}

namespace {

// Resolve the active landing runway from a XPlaneContext snapshot.
// Mirrors pattern_flow::resolve_active_runway but kept local so the
// frame-driven go-around path does not have to link pattern_flow's
// internal anonymous namespace.
std::optional<landing_sequence::ActiveRunway>
resolve_active_runway_for_go_around(const xplane_context::XPlaneContext &ctx) {
  if (ctx.active_runway.empty() || ctx.runways.empty())
    return std::nullopt;
  for (const auto &rw : ctx.runways) {
    const xplane_context::RunwayEnd *end = nullptr;
    double heading = 0.0;
    if (rw.end1.number == ctx.active_runway) {
      end = &rw.end1;
      heading = static_cast<double>(rw.end1.heading_deg);
    } else if (rw.end2.number == ctx.active_runway) {
      end = &rw.end2;
      heading = static_cast<double>(rw.end2.heading_deg);
    }
    if (!end)
      continue;
    landing_sequence::ActiveRunway out;
    out.threshold_lat = end->lat;
    out.threshold_lon = end->lon;
    out.heading_deg = heading;
    out.length_m = static_cast<double>(rw.length_m);
    if (out.length_m < 500.0)
      out.length_m = 2500.0;
    out.designator = end->number;
    return out;
  }
  return std::nullopt;
}

} // namespace

bool poll_go_around(const xplane_context::XPlaneContext &ctx, double now_secs,
                    std::string *out_text) {
  // Gate 1: user is on a granted landing clearance.
  if (atc_state_machine::get_state() !=
      atc_state_machine::ATCState::LANDING_CLEARED)
    return false;

  // Gate 2: cooldown — never fire two go-arounds inside 60 s.
  if (now_secs - last_go_around_emit_secs_ < kGoAroundCooldownSec)
    return false;

  // Gate 3: active runway must resolve to a concrete threshold.
  auto rwy_opt = resolve_active_runway_for_go_around(ctx);
  if (!rwy_opt.has_value())
    return false;

  // Gate 4: user within 1 NM of the threshold.
  const double user_dist_nm = traffic_geometry::distance_nm(
      rwy_opt->threshold_lat, rwy_opt->threshold_lon, ctx.latitude,
      ctx.longitude);
  if (user_dist_nm > kGoAroundTriggerDistanceNm)
    return false;

  // Gate 5: runway-occupied scan via the same sequencing primitive
  // pattern_flow uses for the "continue approach" overlay. We can't
  // cheap-out to a single-target scan — the occupant may be the
  // second-nearest target rather than the first.
  const auto &traffic = traffic_context::current();
  landing_sequence::UserPosition user{ctx.latitude, ctx.longitude};
  auto seq =
      landing_sequence::compute_landing_sequence(traffic, user, *rwy_opt);
  if (!seq.runway_occupied)
    return false;

  // All gates passed — render the unsolicited go-around call. No state
  // change, no traffic_dialog ack hook: this is a controller flight
  // command, the pilot's reaction is to fly, not to speak.
  std::string text = atc_state_machine::render_traffic_advisory(
      {}, ctx, "go_around_traffic_runway");
  last_go_around_emit_secs_ = now_secs;
  if (out_text)
    *out_text = std::move(text);
  logging::info("Engine emitted go-around (user dist=%.2f NM, occupant id=%u)",
                user_dist_nm,
                seq.occupant.has_value() ? seq.occupant->modeS_id : 0u);
  return true;
}

bool poll_readback_reminder(const xplane_context::XPlaneContext &ctx,
                            double now_secs, std::string *out_text) {
  std::string template_key =
      atc_state_machine::consume_readback_reminder(now_secs);
  if (template_key.empty())
    return false;
  // render_traffic_advisory pulls callsign + airport from the live
  // context — identical pipeline used by traffic advisories, go-around
  // call etc. The {callsign} placeholder is filled with session_callsign
  // when set (so reminders address the same callsign Tower used in the
  // clearance).
  std::string text =
      atc_state_machine::render_traffic_advisory({}, ctx, template_key);
  if (out_text)
    *out_text = std::move(text);
  return true;
}

bool poll_traffic_advisory(const xplane_context::XPlaneContext &ctx,
                           double now_secs, std::string *out_text) {
  using FT = xplane_context::FrequencyType;

  // Don't fire fresh advisories while the previous one hasn't been
  // acknowledged yet — the dialog is the gate, not the main ATCState.
  if (traffic_dialog::is_awaiting_ack())
    return false;

  traffic_advisor::UserState user;
  user.atc_state = atc_state_machine::get_state();
  user.on_active_atc_freq = ctx.frequency_type == FT::TOWER ||
                            ctx.frequency_type == FT::GROUND ||
                            ctx.frequency_type == FT::APPROACH;
  user.lat = ctx.latitude;
  user.lon = ctx.longitude;
  user.alt_msl_ft = static_cast<double>(ctx.altitude_ft_msl);
  user.heading_deg = static_cast<double>(ctx.heading_true);
  // Ground track == heading_true is a small simplification (no wind
  // crab) but matches the precision the advisory geometry needs (clock
  // positions are rounded to the hour).
  user.track_deg = static_cast<double>(ctx.heading_true);
  user.groundspeed_kts = static_cast<double>(ctx.groundspeed_kts);
  user.on_ground = ctx.on_ground;
  user.target_has_mode_c_default = true;
  user.user_taxiing = flight_phase::get() == flight_phase::FlightPhase::TAXI;

  const auto &traffic = traffic_context::current();

  auto adv =
      traffic_advisor::evaluate(traffic, user, advisory_history_, now_secs);
  if (!adv.has_value())
    return false;

  std::string text = atc_state_machine::render_traffic_advisory(
      adv->vars, ctx, adv->template_key);
  traffic_advisor::mark_emitted(advisory_history_, adv->modeS_id, now_secs);
  // Ground-conflict advisories don't expect a voice ack — the pilot
  // reacts by stopping / giving way. Skip the dialog side-channel so
  // the next pilot transcript still flows through the normal ATC
  // pipeline.
  if (adv->requires_ack)
    traffic_dialog::on_advisory_emitted(adv->modeS_id);

  if (out_text)
    *out_text = std::move(text);

  logging::info("Engine emitted traffic advisory (target_id=%u, template=%s)",
                adv->modeS_id, adv->template_key.c_str());
  return true;
}

// Strip known controller-role suffixes from a raw apt.dat name and title-case
// the remainder. "CHAMBERY APP" -> "Chambery", "ZURICH DEP" -> "Zurich".
static std::string controller_location(const std::string &raw) {
  static const char *kSuffixes[] = {
      // Long-form first so they match before their 3-letter abbreviations.
      " APPROACH", " DEPARTURE", " ARRIVAL",  " CONTROL",
      " CENTRE",   " CENTER",    " DIRECTOR",
      // 3-letter ATC.dat abbreviations.
      " APP", " DEP", " CTR", " GND", " TWR", " DLV", " DEL", " FSS",
      nullptr};
  std::string loc = raw;
  for (int i = 0; kSuffixes[i]; ++i) {
    std::string suf(kSuffixes[i]);
    if (loc.size() >= suf.size() &&
        loc.compare(loc.size() - suf.size(), suf.size(), suf) == 0) {
      loc = loc.substr(0, loc.size() - suf.size());
      break;
    }
  }
  if (loc.empty())
    return loc;
  bool cap = true;
  for (char &c : loc) {
    if (c == ' ') {
      cap = true;
    } else if (cap) {
      c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      cap = false;
    } else {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
  }
  return loc;
}

bool poll_departure_handoff(const xplane_context::XPlaneContext &ctx,
                            float /*dt*/, std::string *out_text) {
  using AS = atc_state_machine::ATCState;
  using FP = flight_phase::FlightPhase;
  using FT = xplane_context::FrequencyType;

  if (atc_state_machine::get_state() != AS::IFR_DEPARTURE_CLEARED)
    return false;

  auto phase = flight_phase::get();
  if (phase != FP::CLIMB && phase != FP::CRUISE)
    return false;

  // If the takeoff clearance already embedded the departure instruction via
  // {ifr_departure_contact} (e.g. "passing 3000ft, contact Chambery Approach
  // on 121.205"), the pilot already knows where to go — advance state silently
  // without issuing a duplicate or conflicting "contact X" message.
  if (!s_pending_departure_label.empty()) {
    s_current_controller_label = s_pending_departure_label;
    atc_state_machine::set_state(AS::IFR_FREQ_HANDOFF);
    s_departure_handoff_timer = 0.0f;
    logging::info("IFR departure handoff: silent (frequency already in clearance: %s)",
                  s_pending_departure_label.c_str());
    return false; // no second message — clearance already gave the instruction
  }

  // Takeoff clearance had no departure contact. Fire an explicit "contact X"
  // when the aircraft has left the CTR. Use find_enclosing() for 3-D check;
  // fall back to an AGL threshold when OpenAir data is absent or incomplete.
  openair_db::AirspaceEntry enc;
  {
    enc = openair_db::find_enclosing(
        ctx.latitude, ctx.longitude, static_cast<int>(ctx.altitude_ft_msl));
    logging::info(
        "IFR departure handoff: openair enc='%s' class=%d floor=%dft ceil=%dft"
        " at %.0fft MSL pos=%.4f,%.4f",
        enc.name.c_str(), static_cast<int>(enc.ac_class),
        enc.floor_ft, enc.ceiling_ft,
        ctx.altitude_ft_msl, ctx.latitude, ctx.longitude);
    if (enc.ac_class == openair_db::AirspaceClass::CTR)
      return false; // still inside CTR — wait

    if (enc.ac_class == openair_db::AirspaceClass::OTHER) {
      // OpenAir file absent OR this airport's CTR not in the dataset.
      // Use AGL threshold so the handoff doesn't fire at ground level.
      float airport_elev_ft = ctx.altitude_ft_msl - ctx.height_agl_ft;
      int ctr_msl =
          openair_db::ctr_ceiling_ft(ctx.airport_lat, ctx.airport_lon);
      float threshold_agl =
          ctr_msl > 0 ? static_cast<float>(ctr_msl) - airport_elev_ft : 2500.0f;
      if (ctx.height_agl_ft < threshold_agl)
        return false;
    }
    // TMA / CTA / FIR / UIR → aircraft has left the CTR, proceed.
  }

  std::string controller_label;
  float freq = 0.0f;

  // Primary: use the openair TMA name to identify the correct TRACON in
  // atc.dat by name.  This is geometrically exact: openair has altitude-aware
  // polygons (e.g. CHAMBERY TMA 1000-9500ft vs GENEVA TMA FL095-FL195), so
  // the aircraft can never be handed off to a controller whose airspace starts
  // above its current altitude.
  //
  // "CHAMBERY TMA SECTOR 1" → city fragment "CHAMBERY"
  // → find atc.dat TRACON with NAME containing "CHAMBERY" → Chambery APP.
  if (enc.ac_class == openair_db::AirspaceClass::TMA ||
      enc.ac_class == openair_db::AirspaceClass::CTA) {
    // Extract city: everything before the first "TMA"/"CTA"/"FIR" keyword.
    std::string fragment = enc.name;
    for (const char *kw : {"TMA", "CTA", "FIR", "UIR", " SECTOR", " SEC"}) {
      auto pos = fragment.find(kw);
      if (pos != std::string::npos) {
        fragment = fragment.substr(0, pos);
        break;
      }
    }
    while (!fragment.empty() && fragment.back() == ' ')
      fragment.pop_back();

    if (!fragment.empty()) {
      const airspace_db::Controller *tracon =
          airspace_db::find_by_role_name_contains(
              airspace_db::ControllerRole::TRACON, fragment);
      if (tracon && !tracon->freqs_khz.empty()) {
        freq = static_cast<float>(tracon->freqs_khz.front()) / 1000.0f;
        controller_label = controller_location(tracon->name) + " Approach";
        logging::info(
            "IFR departure handoff: [P1-openair] '%s' -> fragment '%s' "
            "-> TRACON '%s' %.3f",
            enc.name.c_str(), fragment.c_str(), tracon->name.c_str(), freq);
      } else {
        logging::info(
            "IFR departure handoff: [P1-openair] '%s' -> fragment '%s' "
            "-> no TRACON match, falling back",
            enc.name.c_str(), fragment.c_str());
      }
    }
  }

  // Fallback 1: departure airport's own DEPARTURE / APPROACH frequency from
  // apt.dat.  Used when openair has no named TMA (OTHER) or name matching
  // failed.
  if (freq < 100.0f) {
    const std::string fallback = ctx.nearest_airport_name.empty()
                                     ? ctx.nearest_airport_id
                                     : ctx.nearest_airport_name;
    float dep_freq = ctx.airport_freqs.first_mhz(FT::DEPARTURE);
    float app_freq = ctx.airport_freqs.first_mhz(FT::APPROACH);
    if (dep_freq >= 100.0f) {
      std::string raw = ctx.airport_freqs.first_name(FT::DEPARTURE);
      controller_label = raw.empty() ? (fallback + " Departure")
                                     : controller_location(raw) + " Departure";
      if (!raw.empty() && (raw.find("RADAR") != std::string::npos ||
                           raw.find("CONTROL") != std::string::npos ||
                           raw.find("CTL") != std::string::npos))
        controller_label = controller_location(raw);
      freq = dep_freq;
      logging::info("IFR departure handoff: [P2-apt.dat DEP] %s %.3f",
                    controller_label.c_str(), freq);
    } else if (app_freq >= 100.0f) {
      std::string raw = ctx.airport_freqs.first_name(FT::APPROACH);
      controller_label = raw.empty() ? (fallback + " Approach")
                                     : controller_location(raw) + " Approach";
      if (!raw.empty() && (raw.find("RADAR") != std::string::npos ||
                           raw.find("CONTROL") != std::string::npos ||
                           raw.find("CTL") != std::string::npos))
        controller_label = controller_location(raw);
      freq = app_freq;
      logging::info("IFR departure handoff: [P2-apt.dat APP] %s %.3f",
                    controller_label.c_str(), freq);
    } else {
      logging::info(
          "IFR departure handoff: [P2-apt.dat] no DEP/APP freq for %s",
          ctx.nearest_airport_id.c_str());
    }
  }

  // Fallback 2: nearest TRACON in atc.dat, queried at the DEPARTURE AIRPORT
  // position (not the aircraft's current position).  Querying at the airport
  // avoids selecting a controller that is geographically closer to the aircraft
  // but belongs to a different airport's TMA.
  if (freq < 100.0f) {
    const airspace_db::Controller *tracon = airspace_db::find_by_role_near(
        airspace_db::ControllerRole::TRACON,
        ctx.airport_lat, ctx.airport_lon,
        ctx.altitude_ft_msl, /*prefer_largest_area=*/true);
    if (tracon && !tracon->freqs_khz.empty()) {
      freq = static_cast<float>(tracon->freqs_khz.front()) / 1000.0f;
      controller_label = controller_location(tracon->name);
      logging::info(
          "IFR departure handoff: [P3-atc.dat at airport %.4f,%.4f] %s %.3f",
          ctx.airport_lat, ctx.airport_lon,
          controller_label.c_str(), freq);
    } else {
      logging::info(
          "IFR departure handoff: [P3-atc.dat] no TRACON near airport %.4f,%.4f",
          ctx.airport_lat, ctx.airport_lon);
    }
  }

  // Transition to IFR_FREQ_HANDOFF: pilot must read back the frequency before
  // advancing to IFR_EN_ROUTE. Even with no frequency we advance so the state
  // doesn't get stuck in IFR_DEPARTURE_CLEARED forever.
  if (!controller_label.empty())
    s_current_controller_label = controller_label;
  s_pending_handoff_freq_mhz = freq;
  atc_state_machine::set_state(AS::IFR_FREQ_HANDOFF);
  s_departure_handoff_timer = 0.0f;

  if (controller_label.empty())
    return false; // uncontrolled airspace — silent transition, nothing to speak

  const std::string &cs = atc_state_machine::session_callsign();
  const std::string &callsign = cs.empty() ? settings::pilot_callsign() : cs;

  if (out_text) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s, contact %s on %.3f, good day.",
                  callsign.c_str(), controller_label.c_str(), freq);
    *out_text = buf;
  }

  logging::info("IFR departure handoff: contact %s %.3f",
                controller_label.c_str(), freq);
  return true;
}

// Helper: round feet to the nearest FL boundary (500 ft increments) and
// return the FL number as an integer (e.g. 19000 ft → 190).
static int round_to_fl(int feet) {
  // Round up to the nearest 500 ft multiple that gives a whole FL.
  int fl_units = (feet + 499) / 500;
  return fl_units * 5; // FL = units of 100 ft; 500 ft = 5 FL units
}

static double
procedure_deviation_nm(const xplane_context::XPlaneContext &ctx,
                       const std::vector<simbrief_ofp::NavlogFix> &navlog,
                       bool sid_star_only, const std::string &direct_fix,
                       double direct_from_lat, double direct_from_lon);

bool poll_sid_climb(const xplane_context::XPlaneContext &ctx, float dt,
                    std::string *out_text) {
  using AS = atc_state_machine::ATCState;
  using FP = flight_phase::FlightPhase;

  if (atc_state_machine::get_state() != AS::IFR_RADAR_CONTACT) {
    s_sid_direct_issued = false;
    s_sid_step1_issued = false;
    s_sid_cruise_issued = false;
    s_sid_radar_handoff_issued = false;
    s_sid_was_in_tma = false;
    s_sid_tma_check_sec = 0.0f;
    s_sid_initialized = false;
    s_sid_climb_timer = 0.0f;
    s_sid_step1_alt_ft = 0;
    s_sid_deviation_cooldown_sec = 0.0f;
    s_sid_direct_origin_lat = 0.0;
    s_sid_direct_origin_lon = 0.0;
    s_departure_apt_lat = 0.0;
    s_departure_apt_lon = 0.0;
    return false;
  }

  auto phase = flight_phase::get();
  if (phase == FP::PARKED || phase == FP::TAXI) {
    // Aircraft back on ground — auto_correction in flight_rules.json handles
    // the state reset; don't fire climb clearances.
    return false;
  }

  s_sid_climb_timer += dt;

  const auto &defaults = flight_phase::get_ifr_defaults();
  const std::string &cs = atc_state_machine::session_callsign();
  const std::string &callsign = cs.empty() ? settings::pilot_callsign() : cs;

  // One-time initialisation on first entry to IFR_RADAR_CONTACT.
  if (!s_sid_initialized) {
    s_sid_initialized = true;
    s_departure_apt_lat = ctx.airport_lat;
    s_departure_apt_lon = ctx.airport_lon;

    // Step1 altitude: LFLP RW04 westbound → FL110 (clear of Geneva TMA
    // after SOCOF). All other cases: midpoint between SID minimum and
    // cruise, rounded to the nearest FL.
    bool lflp_west = false;
    if (ctx.nearest_airport_id == "LFLP" && ctx.active_runway == "04") {
      const std::string &fix = ctx.ifr_sid_last_fix.empty()
                                   ? ctx.ifr_fpl_first_fix
                                   : ctx.ifr_sid_last_fix;
      static const char *kWestFixes[] = {"LSE", "LTP", "ROMAM", nullptr};
      for (int i = 0; kWestFixes[i]; ++i)
        if (fix == kWestFixes[i]) {
          lflp_west = true;
          break;
        }
    }
    if (lflp_west) {
      s_sid_step1_alt_ft = 11000; // FL110 — clear of Geneva TMA after SOCOF
    } else {
      int floor_ft = ctx.ifr_sid_min_alt_ft > 0 ? ctx.ifr_sid_min_alt_ft : 5000;
      int cruise_ft =
          ctx.ifr_cruise_alt_ft > 0 ? ctx.ifr_cruise_alt_ft : floor_ft + 8000;
      int mid_ft = (floor_ft + cruise_ft) / 2;
      s_sid_step1_alt_ft = round_to_fl(mid_ft) * 100;
    }
  }

  // ── Phase 3: radar handoff — fires when aircraft exits the TMA ───────
  // Requires step1 already issued so we never hand off before the first
  // climb clearance. Use find_enclosing() on the aircraft's 3-D position:
  // while still inside a CTR or TMA we hold; once in CTA/FIR/uncontrolled
  // we hand off to Area Control / Radar.
  // Fall back to a configured or computed altitude when airspace.txt is absent.
  if (!s_sid_radar_handoff_issued && s_sid_step1_issued) {
    // Don't hand off until the aircraft is approaching step1 altitude.
    // This prevents an immediate handoff right after step1 is issued
    // when the aircraft is still far below (e.g. at 6700 ft for FL170 step1).
    if (static_cast<int>(ctx.altitude_ft_msl) < s_sid_step1_alt_ft - 2000)
      goto skip_tma_check;

    // Compute the altitude-based fallback threshold regardless of openair_db.
    // Used when openair_db is absent OR when it is present but never detected
    // the aircraft inside a CTR/TMA (data gap — e.g. Chambery TMA not in atc.dat).
    int handoff_fallback_ft =
        defaults.radar_handoff_alt_ft > 0
            ? defaults.radar_handoff_alt_ft
            : (ctx.ifr_cruise_alt_ft > 2000 ? ctx.ifr_cruise_alt_ft - 2000
                                            : 14000);
    handoff_fallback_ft =
        std::max(handoff_fallback_ft, s_sid_step1_alt_ft + 1000);

    bool exited_tma = false;
    s_sid_tma_check_sec -= dt;
    if (s_sid_tma_check_sec <= 0.0f) {
      s_sid_tma_check_sec = 1.0f; // 1 Hz — TMA boundary at cruise speed ~4 NM/min
      if (openair_db::ready()) {
        auto enc = openair_db::find_enclosing(
            ctx.latitude, ctx.longitude, static_cast<int>(ctx.altitude_ft_msl));
        bool in_tma_now = (enc.ac_class == openair_db::AirspaceClass::CTR ||
                           enc.ac_class == openair_db::AirspaceClass::TMA);
        if (in_tma_now)
          s_sid_was_in_tma = true;
        // Primary: TMA-polygon exit (requires prior entry to prevent false fires
        // when the departure altitude is below the TMA floor).
        if (s_sid_was_in_tma && !in_tma_now) {
          exited_tma = true;
        } else if (!s_sid_was_in_tma) {
          // openair_db present but this airport's CTR/TMA is not in the dataset
          // (data gap). Fall back to altitude threshold so the handoff still fires.
          exited_tma =
              static_cast<int>(ctx.altitude_ft_msl) >= handoff_fallback_ft;
        }
      } else {
        exited_tma =
            static_cast<int>(ctx.altitude_ft_msl) >= handoff_fallback_ft;
      }
    }

    if (exited_tma) {
      // Look up Centre controller first — needed for both the combined
      // message and the handoff-only message.
      std::string centre_label;
      float centre_freq = 0.0f;
      const airspace_db::Controller *ctr = airspace_db::find_by_role_near(
          airspace_db::ControllerRole::CTR, ctx.latitude, ctx.longitude,
          ctx.altitude_ft_msl, /*prefer_largest_area=*/true);
      if (ctr && !ctr->freqs_khz.empty()) {
        centre_label = controller_location(ctr->name);
        centre_freq = static_cast<float>(ctr->freqs_khz.front()) / 1000.0f;
        // Do NOT seed s_enroute_sector_freq_khz here. The TMA-exit proximity
        // lookup (find_by_role_near) and the sector check polygon lookup
        // (find_enclosing) use different methods and may return different
        // controllers at the same position. Leaving it at 0 ensures the first
        // sector check silently records a baseline with no false announcement.
      }
      if (centre_label.empty())
        centre_label = "Area Control";
      s_current_controller_label = centre_label;
      s_pending_handoff_freq_mhz = centre_freq;

      s_sid_radar_handoff_issued = true;
      if (s_enroute_cleared_alt_ft == 0) {
        if (ctx.ifr_cruise_alt_ft > 0)
          s_enroute_cleared_alt_ft = round_to_fl(ctx.ifr_cruise_alt_ft) * 100;
        else if (s_sid_step1_alt_ft > 0)
          s_enroute_cleared_alt_ft = s_sid_step1_alt_ft;
      }
      atc_state_machine::set_state(AS::IFR_ENROUTE_CRUISE);

      if (!s_sid_cruise_issued) {
        // Phase 2 (near step1) hasn't fired yet — combine cruise clearance
        // and radar handoff in a single transmission.  Issuing them as two
        // rapid consecutive messages (previous two-frame split) caused pilots
        // to miss the altitude change while reacting to the freq change.
        s_sid_cruise_issued = true;
        int cruise_fl =
            round_to_fl(ctx.ifr_cruise_alt_ft > 0 ? ctx.ifr_cruise_alt_ft
                                                  : s_sid_step1_alt_ft + 4000);
        s_enroute_cleared_alt_ft = cruise_fl * 100;
        if (out_text) {
          char buf[200];
          if (centre_freq >= 100.0f)
            std::snprintf(buf, sizeof(buf),
                          "%s, climb flight level %d, contact %s on %.3f, "
                          "good day.",
                          callsign.c_str(), cruise_fl, centre_label.c_str(),
                          centre_freq);
          else
            std::snprintf(buf, sizeof(buf),
                          "%s, climb flight level %d, contact %s, good day.",
                          callsign.c_str(), cruise_fl, centre_label.c_str());
          *out_text = buf;
        }
        logging::info(
            "IFR SID climb: FL%d + handoff to %s at %.0f ft MSL (TMA exit, "
            "combined)",
            cruise_fl, centre_label.c_str(), ctx.altitude_ft_msl);
        return true;
      }

      // Phase 2 already issued the cruise clearance — just hand off.
      if (out_text) {
        char buf[160];
        if (centre_freq >= 100.0f)
          std::snprintf(buf, sizeof(buf), "%s, contact %s on %.3f, good day.",
                        callsign.c_str(), centre_label.c_str(), centre_freq);
        else
          std::snprintf(buf, sizeof(buf), "%s, contact %s, good day.",
                        callsign.c_str(), centre_label.c_str());
        *out_text = buf;
      }
      logging::info("IFR SID climb: radar handoff at %.0f ft MSL (exited TMA/CTR)",
                    ctx.altitude_ft_msl);
      return true;
    }
  }
skip_tma_check:;

  // ── Phase 2: climb to cruise FL when near step1 altitude ──────────────
  if (s_sid_step1_issued && !s_sid_cruise_issued) {
    int cruise_fl =
        round_to_fl(ctx.ifr_cruise_alt_ft > 0 ? ctx.ifr_cruise_alt_ft
                                              : s_sid_step1_alt_ft + 4000);
    bool near_step1 = std::abs(static_cast<int>(ctx.altitude_ft_msl) -
                               s_sid_step1_alt_ft) < 500;
    bool timeout = s_sid_climb_timer > 900.0f; // 15-min safety net
    if (near_step1 || timeout) {
      s_sid_cruise_issued = true;
      s_enroute_cleared_alt_ft =
          cruise_fl * 100; // record for en-route altitude monitoring
      if (out_text) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s, climb flight level %d.",
                      callsign.c_str(), cruise_fl);
        *out_text = buf;
      }
      logging::info("IFR SID climb: FL%d (cruise clearance)", cruise_fl);
      return true;
    }
  }

  // ── Phase 1: direct-to shortcut + initial step climb ──────────────────
  // Fire when the aircraft is ≥10 NM from the departure airport.
  // The 600 s fallback catches cases where airport_lat/lon were not captured
  // (e.g. apt.dat parse still in progress when radar contact was established).
  {
    double dist_nm = (s_departure_apt_lat != 0.0 || s_departure_apt_lon != 0.0)
                         ? traffic_geometry::distance_nm(
                               ctx.latitude, ctx.longitude, s_departure_apt_lat,
                               s_departure_apt_lon)
                         : 0.0;
    bool far_enough = dist_nm >= 10.0;
    bool fallback = s_sid_climb_timer > 600.0f;
    if (!s_sid_step1_issued && (far_enough || fallback)) {
      s_sid_step1_issued = true;
      s_sid_direct_issued = true;
      s_sid_direct_origin_lat = ctx.latitude;
      s_sid_direct_origin_lon = ctx.longitude;
      int step1_fl = round_to_fl(s_sid_step1_alt_ft);
      if (out_text) {
        const std::string &last_fix = ctx.ifr_sid_last_fix;
        char buf[128];
        if (!last_fix.empty()) {
          std::snprintf(buf, sizeof(buf),
                        "%s, direct %s, climb flight level %d.",
                        callsign.c_str(), last_fix.c_str(), step1_fl);
        } else {
          std::snprintf(buf, sizeof(buf), "%s, climb flight level %d.",
                        callsign.c_str(), step1_fl);
        }
        *out_text = buf;
      }
      logging::info("IFR SID climb: FL%d%s (step1)", step1_fl,
                    ctx.ifr_sid_last_fix.empty() ? "" : " direct");
      return true;
    }
  }

  // ── SID cross-track deviation warning ─────────────────────────────────
  // Tolerance 2 NM (tighter than en-route 5 NM — SIDs follow specific
  // terrain/obstacle clearance tracks). 2-minute cooldown.
  // Before direct: check vs SID legs in the navlog (is_sid_star == true).
  // After direct:  check vs the direct leg (stored origin → direct fix).
  s_sid_deviation_cooldown_sec =
      std::max(0.0f, s_sid_deviation_cooldown_sec - dt);
  if (s_sid_deviation_cooldown_sec <= 0.0f) {
    auto ofp = simbrief_ofp::get();
    if (ofp.valid && !ofp.navlog.empty()) {
      const std::string &direct_fix =
          s_sid_direct_issued ? ctx.ifr_sid_last_fix : std::string{};
      double xt_nm = procedure_deviation_nm(ctx, ofp.navlog,
                                            /*sid_star_only=*/true, direct_fix,
                                            s_sid_direct_origin_lat,
                                            s_sid_direct_origin_lon);
      if (xt_nm > 2.0 && xt_nm < 1e8) {
        s_sid_deviation_cooldown_sec = 120.0f;
        if (out_text) {
          char buf[160];
          std::snprintf(
              buf, sizeof(buf),
              "%s, confirm SID routing, you appear %.0f NM off track.",
              callsign.c_str(), xt_nm);
          *out_text = buf;
        }
        logging::info("IFR SID: cross-track deviation %.1f NM (direct=%s)",
                      xt_nm, s_sid_direct_issued ? "yes" : "no");
        return true;
      }
    }
  }

  return false;
}

// ── Helpers for poll_enroute ──────────────────────────────────────────────

// Format an altitude as ATC phraseology: FL when >= 5000 ft, else "N feet".
static std::string format_alt(int alt_ft) {
  char buf[32];
  if (alt_ft >= 5000) {
    std::snprintf(buf, sizeof(buf), "flight level %d", alt_ft / 100);
  } else {
    std::snprintf(buf, sizeof(buf), "%d feet", alt_ft);
  }
  return buf;
}

// Cross-track distance (NM) from point P to the great-circle leg A→B.
// Positive = right of track, negative = left. Returns large value when
// the leg has zero length.
static double cross_track_nm(double lat_p, double lon_p, double lat_a,
                             double lon_a, double lat_b, double lon_b) {
  constexpr double kRnm = 3440.065; // Earth radius in NM
  double d_ap = traffic_geometry::distance_nm(lat_a, lon_a, lat_p, lon_p);
  if (d_ap < 0.001)
    return 0.0;
  double theta_ab = traffic_geometry::bearing_deg(lat_a, lon_a, lat_b, lon_b);
  double theta_ap = traffic_geometry::bearing_deg(lat_a, lon_a, lat_p, lon_p);
  double ang_diff = (theta_ap - theta_ab) * (3.14159265358979323846 / 180.0);
  double xt = std::asin(std::sin(d_ap / kRnm) * std::sin(ang_diff)) * kRnm;
  return xt;
}

// Find the minimum absolute cross-track error (NM) from the aircraft to any
// navlog leg. Returns a large value when the navlog is empty.
static double
min_cross_track_nm(const xplane_context::XPlaneContext &ctx,
                   const std::vector<simbrief_ofp::NavlogFix> &navlog) {
  if (navlog.size() < 2)
    return 1e9;
  double min_xt = 1e9;
  for (size_t i = 0; i + 1 < navlog.size(); ++i) {
    double xt =
        cross_track_nm(ctx.latitude, ctx.longitude, navlog[i].lat,
                       navlog[i].lon, navlog[i + 1].lat, navlog[i + 1].lon);
    if (std::abs(xt) < std::abs(min_xt))
      min_xt = xt;
  }
  return min_xt;
}

// Cross-track deviation for a procedure (SID or STAR).
// Two modes:
//   direct_fix empty  → check vs navlog legs where is_sid_star matches
//   sid_star_only. direct_fix set    → check vs a single leg (direct_from →
//   direct_fix position in navlog).
// Returns absolute deviation in NM, or 1e9 when no usable data.
static double
procedure_deviation_nm(const xplane_context::XPlaneContext &ctx,
                       const std::vector<simbrief_ofp::NavlogFix> &navlog,
                       bool sid_star_only, const std::string &direct_fix,
                       double direct_from_lat, double direct_from_lon) {

  if (direct_fix.empty()) {
    // Normal procedure legs: filter by SID/STAR flag.
    std::vector<simbrief_ofp::NavlogFix> legs;
    legs.reserve(navlog.size());
    for (const auto &f : navlog)
      if (f.is_sid_star == sid_star_only)
        legs.push_back(f);
    return std::abs(min_cross_track_nm(ctx, legs));
  }

  // Direct-to mode: find the target fix in the navlog.
  for (const auto &f : navlog) {
    if (f.ident != direct_fix)
      continue;
    double xt = cross_track_nm(ctx.latitude, ctx.longitude, direct_from_lat,
                               direct_from_lon, f.lat, f.lon);
    return std::abs(xt);
  }
  return 1e9; // fix not found
}

struct StarEntryResult {
  std::string ident;
  std::string star_name;  // empty if CIFP has no matching STAR
  double lat = 0.0;
  double lon = 0.0;
  int entry_alt_ft = 0;  // 0 = use defaults; set only for non-ceiling CIFP constraint
};

// Finds the STAR entry fix for the OFP destination.
// Per FPL convention: the last navlog fix before the destination ICAO is always
// the STAR entry (FPL filed as ROMAM...ABDIL...LFMN without explicit SID/STAR).
// Primary path uses is_sid_star flags (explicit STAR in SimBrief OFP).
// star_name may be empty when CIFP has no matching STAR (lat/lon still usable).
static bool find_star_entry(const std::string &cifp_dir,
                            const simbrief_ofp::OfpData &ofp,
                            StarEntryResult &out) {
  if (ofp.destination_icao.empty() || ofp.navlog.empty())
    return false;

  // Primary: track first fix of each is_sid_star group; last group = STAR.
  std::string star_entry_ident;
  bool in_group = false;
  for (const auto &fix : ofp.navlog) {
    if (fix.is_sid_star && !in_group &&
        !fix.ident.empty() && fix.ident != ofp.destination_icao)
      star_entry_ident = fix.ident;
    in_group = fix.is_sid_star;
  }

  // Fallback: last navlog fix before destination (standard FPL convention).
  if (star_entry_ident.empty()) {
    for (int i = (int)ofp.navlog.size() - 1; i >= 0; --i) {
      const auto &fix = ofp.navlog[i];
      if (!fix.ident.empty() && fix.ident != ofp.destination_icao) {
        star_entry_ident = fix.ident;
        break;
      }
    }
  }
  if (star_entry_ident.empty())
    return false;

  logging::info("IFR STAR entry: dest=%s entry_fix=%s cifp=%s",
                ofp.destination_icao.c_str(), star_entry_ident.c_str(),
                cifp_dir.empty() ? "(empty)" : "ok");

  // Locate lat/lon from navlog.
  for (const auto &fix : ofp.navlog) {
    if (fix.ident != star_entry_ident)
      continue;
    out.ident = star_entry_ident;
    out.lat   = fix.lat;
    out.lon   = fix.lon;
    // Optional CIFP STAR name + altitude constraint.
    if (!cifp_dir.empty()) {
      out.star_name = cifp_reader::star_name_for_entry_fix(
          cifp_dir, ofp.destination_icao, "", star_entry_ident);
      if (!out.star_name.empty()) {
        auto entry = cifp_reader::star_entry_fix(
            cifp_dir, ofp.destination_icao, out.star_name);
        if (entry.alt.feet > 0 && !entry.is_ceiling)
          out.entry_alt_ft = entry.alt.feet;
      }
    }
    logging::info("IFR STAR entry: star_name=%s entry_alt_ft=%d",
                  out.star_name.empty() ? "(none)" : out.star_name.c_str(),
                  out.entry_alt_ft);
    return true;
  }
  return false;
}

// Build Phase 3 descent clearance: STAR entry altitude + STAR name + expected
// approach type.  Does NOT issue the Approach frequency handoff — that comes
// later via build_approach_handoff() when the aircraft reaches the CTA boundary.
// Sets s_enroute_descent_issued; state remains IFR_ENROUTE_CRUISE.
// Returns false when already issued.
static bool build_descent_clearance(const xplane_context::XPlaneContext &ctx,
                                    const std::string &callsign,
                                    const flight_phase::IfrDefaults &defaults,
                                    std::string *out_text) {
  if (s_enroute_descent_issued)
    return false;

  auto ofp = simbrief_ofp::get();

  // ── 1. Descent altitude ───────────────────────────────────────────────
  // Baseline: the higher of the profile default (FL110) and cruise-5000
  // (so an FL195 turboprop gets FL140, not FL110 — a more realistic first step).
  // At-or-below ceilings (e.g. ABDIL <= FL190) are upper bounds, not targets.
  // Exact / at-or-above CIFP constraints override when they are below cruise.
  int cruise_ref_dc = s_enroute_cleared_alt_ft > 0 ? s_enroute_cleared_alt_ft
                                                    : ctx.ifr_cruise_alt_ft;
  int star_alt_ft = std::max(defaults.star_entry_alt_ft,
                             (cruise_ref_dc - 5000) / 1000 * 1000);
  std::string star_name;
  std::string dest_runway;

  {
    StarEntryResult se;
    if (find_star_entry(ctx.cifp_dir, ofp, se)) {
      star_name = se.star_name;
      if (se.entry_alt_ft > 0 && se.entry_alt_ft < cruise_ref_dc)
        star_alt_ft = se.entry_alt_ft;
      if (!star_name.empty())
        dest_runway = cifp_reader::runway_for_star(
            ctx.cifp_dir, ofp.destination_icao, star_name);
    }
  }

  // ── 2. Expected approach type ─────────────────────────────────────────
  // When STAR serves ALL runways, dest_runway is empty — pick the best runway
  // using wind alignment and L-over-R preference.
  if (dest_runway.empty() && !ctx.cifp_dir.empty() && !ofp.destination_icao.empty())
    dest_runway = cifp_reader::best_runway_for_approach(
        ctx.cifp_dir, ofp.destination_icao, ctx.wind_direction_deg, ctx.visibility_m);

  std::string approach_phrase;
  if (!dest_runway.empty() && !ctx.cifp_dir.empty() &&
      !ofp.destination_icao.empty()) {
    cifp_reader::ApproachInfo appr;
    if (!ofp.preferred_approach_designator.empty())
      appr = cifp_reader::approach_by_designator(ctx.cifp_dir, ofp.destination_icao,
                                                 ofp.preferred_approach_designator);
    if (appr.type_str.empty())
      appr = cifp_reader::best_approach(ctx.cifp_dir, ofp.destination_icao,
                                        dest_runway, ctx.visibility_m);
    if (!appr.type_str.empty()) {
      // Variant letter (Y, Z, A…) follows the type+runway portion of the designator.
      // "R04LZ" → variant "Z"; "I04L" → no variant.
      std::string variant;
      if (appr.designator.size() > 1 + appr.runway.size())
        variant = appr.designator.substr(1 + appr.runway.size());
      if (variant.empty())
        approach_phrase = ", expect " + appr.type_str + " approach runway " + appr.runway;
      else
        approach_phrase = ", expect " + appr.type_str + " " + variant +
                          " approach runway " + appr.runway;
      s_assigned_approach_designator = appr.designator;
    }
  }

  // ── 2b. CIFP fallback: no STAR from navlog → use first STAR for runway ──
  // When the pilot's FPL doesn't include a STAR entry fix (e.g. filed without
  // ABDIL), navlog lookup returns no star_name.  Fall back to the alphabetically
  // first STAR that serves the active destination runway from CIFP data alone.
  if (star_name.empty() && !dest_runway.empty() && !ctx.cifp_dir.empty() &&
      !ofp.destination_icao.empty()) {
    star_name = cifp_reader::first_star_for_runway(ctx.cifp_dir, ofp.destination_icao,
                                                   dest_runway);
    if (!star_name.empty()) {
      auto entry = cifp_reader::star_entry_fix(ctx.cifp_dir, ofp.destination_icao, star_name);
      if (entry.alt.feet > 0 && !entry.is_ceiling && entry.alt.feet < cruise_ref_dc)
        star_alt_ft = entry.alt.feet;
    }
  }

  // ── 3. STAR phrase ────────────────────────────────────────────────────
  std::string star_phrase;
  if (!star_name.empty())
    star_phrase = ", cleared via " + star_name + " arrival";

  // ── 4. Commit ─────────────────────────────────────────────────────────
  s_enroute_descent_issued = true;
  // Store for poll_approach() to load STAR waypoints at Approach check-in.
  s_assigned_star_name = star_name;
  s_assigned_dest_icao = ofp.destination_icao;

  if (out_text) {
    char buf[240];
    std::snprintf(buf, sizeof(buf), "%s, descend %s%s%s.",
                  callsign.c_str(), format_alt(star_alt_ft).c_str(),
                  star_phrase.c_str(), approach_phrase.c_str());
    *out_text = buf;
  }
  logging::info("IFR en-route: descent -> %s, STAR=%s, rwy=%s",
                format_alt(star_alt_ft).c_str(),
                star_name.empty() ? "(none)" : star_name.c_str(),
                dest_runway.empty() ? "(none)" : dest_runway.c_str());
  return true;
}

// Issue the Approach frequency handoff ("contact Nice Approach on X.XXX").
// Called when the aircraft crosses the CTA/TMA boundary during descent.
// Sets s_enroute_approach_handoff_issued and transitions to IFR_APPROACH_CONTACT.
// Returns false when already issued.
// enc: the openair airspace the aircraft just entered (TMA/CTR), or an OTHER
// entry when the 30 NM distance fallback fired (no openair TMA data).
static bool build_approach_handoff(const xplane_context::XPlaneContext &ctx,
                                   const std::string &callsign,
                                   std::string *out_text,
                                   const openair_db::AirspaceEntry &enc) {
  using AS = atc_state_machine::ATCState;
  using FT = xplane_context::FrequencyType;
  if (s_enroute_approach_handoff_issued)
    return false;

  auto ofp = simbrief_ofp::get();

  std::string app_label;
  float app_freq = 0.0f;

  logging::info(
      "IFR arrival handoff: openair enc='%s' class=%d floor=%dft ceil=%dft"
      " at %.0fft MSL pos=%.4f,%.4f",
      enc.name.c_str(), static_cast<int>(enc.ac_class),
      enc.floor_ft, enc.ceiling_ft,
      ctx.altitude_ft_msl, ctx.latitude, ctx.longitude);

  // Primary: use the openair TMA name to find the correct TRACON by name
  // (same logic as departure handoff — altitude-correct, not centroid-distance).
  // "CHAMBERY TMA SECTOR 1" → "CHAMBERY" → Chambery TRACON.
  if (enc.ac_class == openair_db::AirspaceClass::TMA ||
      enc.ac_class == openair_db::AirspaceClass::CTA) {
    std::string fragment = enc.name;
    for (const char *kw : {"TMA", "CTA", "FIR", "UIR", " SECTOR", " SEC"}) {
      auto pos = fragment.find(kw);
      if (pos != std::string::npos) {
        fragment = fragment.substr(0, pos);
        break;
      }
    }
    while (!fragment.empty() && fragment.back() == ' ')
      fragment.pop_back();
    if (!fragment.empty()) {
      const airspace_db::Controller *tracon =
          airspace_db::find_by_role_name_contains(
              airspace_db::ControllerRole::TRACON, fragment);
      if (tracon && !tracon->freqs_khz.empty()) {
        app_freq  = static_cast<float>(tracon->freqs_khz.front()) / 1000.0f;
        app_label = controller_location(tracon->name) + " Approach";
        logging::info(
            "IFR arrival handoff: [P1-openair] '%s' -> fragment '%s' "
            "-> TRACON '%s' %.3f",
            enc.name.c_str(), fragment.c_str(), tracon->name.c_str(), app_freq);
      } else {
        logging::info(
            "IFR arrival handoff: [P1-openair] '%s' -> fragment '%s' "
            "-> no TRACON match, falling back",
            enc.name.c_str(), fragment.c_str());
      }
    }
  }

  // Fallback 1: destination airport's own APPROACH frequency from apt.dat.
  // Available when ctx.nearest_airport_id has switched to the destination
  // (typical within ~50 NM).
  if (app_label.empty()) {
    float arr_app_freq = ctx.airport_freqs.first_mhz(FT::APPROACH);
    float arr_dep_freq = ctx.airport_freqs.first_mhz(FT::DEPARTURE);
    float arr_freq = arr_app_freq >= 100.0f ? arr_app_freq : arr_dep_freq;
    if (arr_freq >= 100.0f) {
      FT ft = arr_app_freq >= 100.0f ? FT::APPROACH : FT::DEPARTURE;
      std::string raw = ctx.airport_freqs.first_name(ft);
      app_label = raw.empty()
                      ? (ctx.nearest_airport_id + " Approach")
                      : controller_location(raw) + " Approach";
      app_freq = arr_freq;
      logging::info("IFR arrival handoff: [P2-apt.dat] %s %.3f (nearest=%s)",
                    app_label.c_str(), app_freq,
                    ctx.nearest_airport_id.c_str());
    } else {
      logging::info(
          "IFR arrival handoff: [P2-apt.dat] no APP/DEP freq for nearest=%s",
          ctx.nearest_airport_id.c_str());
    }
  }

  // Fallback 2: nearest TRACON to the last navlog fix (i.e. near the
  // destination airport, not the current aircraft position).
  if (app_label.empty()) {
    double lookup_lat = ctx.latitude;
    double lookup_lon = ctx.longitude;
    if (!ofp.navlog.empty()) {
      lookup_lat = ofp.navlog.back().lat;
      lookup_lon = ofp.navlog.back().lon;
    }
    const airspace_db::Controller *tracon = airspace_db::find_by_role_near(
        airspace_db::ControllerRole::TRACON, lookup_lat, lookup_lon, 8000.0f,
        /*prefer_largest_area=*/true);
    if (tracon && !tracon->freqs_khz.empty()) {
      app_freq  = static_cast<float>(tracon->freqs_khz.front()) / 1000.0f;
      app_label = controller_location(tracon->name) + " Approach";
      logging::info(
          "IFR arrival handoff: [P3-atc.dat navlog fix %.4f,%.4f] %s %.3f",
          lookup_lat, lookup_lon, app_label.c_str(), app_freq);
    } else {
      logging::info(
          "IFR arrival handoff: [P3-atc.dat] no TRACON near navlog fix %.4f,%.4f",
          lookup_lat, lookup_lon);
    }
  }

  if (app_label.empty())
    app_label = ofp.destination_icao.empty() ? "Approach"
                                              : ofp.destination_icao + " Approach";

  s_enroute_approach_handoff_issued = true;
  s_enroute_approach_freq_mhz = app_freq;  // gate check-in on correct frequency
  s_current_controller_label = app_label;
  atc_state_machine::set_state(AS::IFR_APPROACH_CONTACT);

  if (out_text) {
    char buf[160];
    if (app_freq >= 100.0f)
      std::snprintf(buf, sizeof(buf), "%s, contact %s on %.3f.",
                    callsign.c_str(), app_label.c_str(), app_freq);
    else
      std::snprintf(buf, sizeof(buf), "%s, contact %s.",
                    callsign.c_str(), app_label.c_str());
    *out_text = buf;
  }
  logging::info("IFR en-route: approach handoff -> %s %.3f MHz",
                app_label.c_str(), app_freq);
  return true;
}

// Pick the first non-SID/STAR navlog fix that is ahead of the aircraft
// (distance > 20 NM) for the en-route direct-to shortcut.
// SimBrief pseudo-fix identifiers that are not real nav fixes.
static bool is_pseudo_fix(const std::string &ident) {
  static const char *kPseudo[] = {"TOC", "TOD", "BOC", "BOD", "SOSTA", nullptr};
  for (int i = 0; kPseudo[i]; ++i)
    if (ident == kPseudo[i])
      return true;
  return false;
}

static std::string
pick_direct_fix(const xplane_context::XPlaneContext &ctx,
                const std::vector<simbrief_ofp::NavlogFix> &navlog) {
  for (const auto &fix : navlog) {
    if (fix.is_sid_star)
      continue;
    if (fix.ident.empty() || is_pseudo_fix(fix.ident))
      continue;
    double dist = traffic_geometry::distance_nm(ctx.latitude, ctx.longitude,
                                                fix.lat, fix.lon);
    if (dist >= 20.0 && dist < 500.0)
      return fix.ident;
  }
  return {};
}

bool poll_enroute(const xplane_context::XPlaneContext &ctx, float dt,
                  std::string *out_text) {
  using AS = atc_state_machine::ATCState;
  using FP = flight_phase::FlightPhase;

  if (atc_state_machine::get_state() != AS::IFR_ENROUTE_CRUISE) {
    // Reset all flags when not in target state.
    s_enroute_timer = 0.0f;
    s_enroute_direct_issued = false;
    s_enroute_direct_delay_sec = 0.0f;
    s_enroute_descent_issued = false;
    s_pilot_requested_descent = false;
    s_enroute_descent_prompt_issued = false;
    s_enroute_approach_handoff_issued = false;
    s_enroute_app_check_sec = 0.0f;
    s_enroute_deviation_cooldown_sec = 0.0f;
    s_enroute_sector_freq_khz = 0;
    s_enroute_sector_check_sec = 120.0f;
    s_enroute_cleared_alt_ft = 0;
    s_enroute_alt_warn_cooldown = 0.0f;
    s_cruise_stepup_issued = false;
    return false;
  }

  auto phase = flight_phase::get();
  if (phase == FP::PARKED || phase == FP::TAXI)
    return false; // auto_correction handles ground reset

  // Training jump: s_current_controller_label not set. Find the CTR sector
  // that geometrically encloses the aircraft at its current FL.
  if (s_current_controller_label.empty()) {
    const auto sectors = airspace_db::find_enclosing(
        ctx.latitude, ctx.longitude, ctx.altitude_ft_msl);
    for (const auto *s : sectors) {
      if (s && s->role == airspace_db::ControllerRole::CTR &&
          !s->freqs_khz.empty()) {
        s_current_controller_label = controller_location(s->name);
        break;
      }
    }
    if (s_current_controller_label.empty())
      s_current_controller_label = "Control";
  }

  // Don't issue proactive messages while the pilot is still on the departure/
  // approach frequency — they haven't checked in on Centre yet.
  // Timer only counts while on Centre so the 90-120 s delays are relative to
  // the actual check-in, not to when the handoff was issued.
  using FT = xplane_context::FrequencyType;
  if (ctx.frequency_type != FT::UNKNOWN)
    return false;

  s_enroute_timer += dt;
  s_enroute_deviation_cooldown_sec =
      std::max(0.0f, s_enroute_deviation_cooldown_sec - dt);

  // Fallback: only used when the aircraft entered IFR_ENROUTE_CRUISE without
  // going through the normal SID-climb sequence (e.g. loaded mid-flight,
  // resumed session, or skipped departure phase). The step1 seed above covers
  // the normal case, so this only fires in the edge-case where neither
  // step1 nor cruise clearance was recorded — and even then the deviation
  // warning is suppressed until after the 60-second grace period.
  if (s_enroute_cleared_alt_ft == 0 && ctx.ifr_cruise_alt_ft > 0) {
    s_enroute_cleared_alt_ft = round_to_fl(ctx.ifr_cruise_alt_ft) * 100;
    logging::info("IFR en-route: cleared alt seeded from OFP cruise (%d ft)",
                  s_enroute_cleared_alt_ft);
  }

  // Step-up: proactively issue cruise FL climb if ATC handed off to Centre
  // while the aircraft is still below cruise altitude (e.g. TMA exit fired the
  // handoff before cruise clearance reached the pilot). Fires once, ≥30 s
  // after Centre check-in, only when cleared_alt < cruise_alt.
  if (!s_cruise_stepup_issued && s_enroute_timer >= 30.0f &&
      s_enroute_cleared_alt_ft > 0 && ctx.ifr_cruise_alt_ft > 0 &&
      ctx.ifr_cruise_alt_ft > s_enroute_cleared_alt_ft + 1000) {
    s_cruise_stepup_issued = true;
    int fl = round_to_fl(ctx.ifr_cruise_alt_ft);
    s_enroute_cleared_alt_ft = fl * 100;
    if (out_text) {
      const std::string &cs2 = atc_state_machine::session_callsign();
      const std::string &callsign2 =
          cs2.empty() ? settings::pilot_callsign() : cs2;
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%s, climb flight level %d.",
                    callsign2.c_str(), fl);
      *out_text = buf;
    }
    logging::info("IFR en-route: step-up FL%d (cleared %d ft < cruise %d ft)",
                  fl, s_enroute_cleared_alt_ft / 100,
                  ctx.ifr_cruise_alt_ft / 100);
    return true;
  }

  const auto &defaults = flight_phase::get_ifr_defaults();
  const std::string &cs = atc_state_machine::session_callsign();
  const std::string &callsign = cs.empty() ? settings::pilot_callsign() : cs;

  // One-time initialisation of pseudo-random direct-to delay (90-120 s).
  if (s_enroute_direct_delay_sec < 1.0f) {
    unsigned hash = 0;
    for (char c : callsign)
      hash = hash * 31u + static_cast<unsigned char>(c);
    s_enroute_direct_delay_sec =
        90.0f + static_cast<float>(hash % 31u); // [90, 120]
  }

  // ── Sub-phase 1: en-route direct-to shortcut ─────────────────────────
  // Fires once, ~90-120 s after Centre check-in. Requires navlog with at
  // least one non-SID/STAR fix still ahead.
  if (!s_enroute_direct_issued &&
      s_enroute_timer >= s_enroute_direct_delay_sec) {
    s_enroute_direct_issued = true;
    auto ofp = simbrief_ofp::get();
    if (ofp.valid && !ofp.navlog.empty()) {
      std::string fix = pick_direct_fix(ctx, ofp.navlog);
      if (!fix.empty()) {
        if (out_text) {
          char buf[128];
          std::snprintf(buf, sizeof(buf), "%s, direct %s, when able.",
                        callsign.c_str(), fix.c_str());
          *out_text = buf;
        }
        logging::info("IFR en-route: direct %s shortcut", fix.c_str());
        return true;
      }
    }
    // No navlog or no fix — don't speak, but mark issued so we don't retry.
  }

  // ── Sub-phase 1.5: en-route sector / FIR frequency change ────────────
  // Every 30 s, re-query which ACC/FIR/UIR sector the aircraft is in.
  // When the sector changes (e.g. Marseille Nord → Bordeaux, or crossing a
  // FIR boundary), issue "contact [centre] on [freq]" and update the label.
  // Suppressed once the descent clearance has been issued (Approach takes
  // over).
  if (!s_enroute_descent_issued && airspace_db::enabled()) {
    s_enroute_sector_check_sec -= dt;
    if (s_enroute_sector_check_sec <= 0.0f) {
      s_enroute_sector_check_sec = 120.0f; // 2-minute interval — airspace.txt is altitude-aware

      // Primary: openair_db (airspace.txt) altitude-aware polygon lookup.
      // Extract first word of the sector name as keyword, then map to the
      // atc.dat CTR controller that has that name fragment (which carries
      // the frequency atc.dat provides).
      const airspace_db::Controller *best = nullptr;
      if (openair_db::ready()) {
        auto enc = openair_db::find_enclosing(ctx.latitude, ctx.longitude,
                                             static_cast<int>(ctx.altitude_ft_msl));
        if (enc.ac_class != openair_db::AirspaceClass::OTHER && !enc.name.empty()) {
          std::string keyword = enc.name;
          auto sp = keyword.find(' ');
          if (sp != std::string::npos)
            keyword = keyword.substr(0, sp);
          best = airspace_db::find_by_role_name_contains(
              airspace_db::ControllerRole::CTR, keyword);
        }
      }
      // Fallback: atc.dat highest-floor heuristic (used when airspace.txt absent).
      if (!best) {
        auto enclosing = airspace_db::find_enclosing(ctx.latitude, ctx.longitude,
                                                     ctx.altitude_ft_msl);
        for (const auto *c : enclosing) {
          if (c->role != airspace_db::ControllerRole::CTR)
            continue;
          if (c->freqs_khz.empty())
            continue;
          if (!best || c->floor_ft > best->floor_ft)
            best = c;
        }
      }

      if (best) {
        uint32_t new_freq_khz = best->freqs_khz.front();
        // Initialise on first check (s_enroute_sector_freq_khz may be 0 if
        // the TMA-exit lookup missed the sector or returned a different entry).
        if (s_enroute_sector_freq_khz == 0) {
          // Silent baseline: record the current sector for change-detection.
          // Do NOT update s_current_controller_label — the pilot is on the
          // TMA-exit frequency (e.g. Marseille) and the UI must reflect that.
          s_enroute_sector_freq_khz = new_freq_khz;
        } else if (new_freq_khz != s_enroute_sector_freq_khz) {
          // Sector changed — issue handoff.
          s_enroute_sector_freq_khz = new_freq_khz;
          std::string new_label = controller_location(best->name);
          s_current_controller_label = new_label;
          float new_freq_mhz = static_cast<float>(new_freq_khz) / 1000.0f;
          s_pending_handoff_freq_mhz = new_freq_mhz;
          if (out_text) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%s, contact %s on %.3f.",
                          callsign.c_str(), new_label.c_str(), new_freq_mhz);
            *out_text = buf;
          }
          logging::info("IFR en-route: sector change -> %s %.3f MHz",
                        new_label.c_str(), new_freq_mhz);
          return true;
        }
      }
    }
  }

  // ── Sub-phase 2: pre-TOD prompt → pilot confirms → descent clearance ────
  //
  // Normal flow:
  //   a) At tod_nm + 15 NM: ATC prompts "advise when ready to descend."
  //   b) Pilot replies REQUEST_DESCENT → ATC issues descent + STAR + approach.
  //   c) If pilot requests descent BEFORE the prompt: skip prompt, issue directly.
  //
  // Fallbacks (no OFP or pilot never responds):
  //   - Actual TOD reached (dist <= tod_nm) without pilot response → issue directly.
  //   - 25 min safety net → issue directly (no OFP / unexpected corner cases).
  if (!s_enroute_descent_issued) {
    // Pilot requested (responding to prompt, or proactively before prompt).
    if (s_pilot_requested_descent) {
      s_pilot_requested_descent = false;
      if (build_descent_clearance(ctx, callsign, defaults, out_text))
        return true;
    }

    // Compute distance to STAR entry fix and alert threshold.
    //
    // Alert distance = altitude component + speed component:
    //   altitude:  cruise_alt_ft / 1200  (FL350 → 29 NM, FL195 → 16 NM)
    //   speed:     groundspeed_kts / 30   (480 kts → 16 NM, 180 kts → 6 NM)
    //   result:    clamped [15, 80] NM
    // Examples: FL350 at 300 kts → 39 NM, FL195 at 180 kts → 22 NM.
    //
    // Reference fix: only use a CIFP-confirmed STAR entry (star_name non-empty).
    // If no CIFP match (e.g., last FPL fix is not a LFMN STAR entry), fall back
    // to destination fix so the prompt does not fire prematurely.
    double dist_nm = 1e9;
    float alert_nm = 25.0f; // fallback when groundspeed unavailable
    {
      auto ofp = simbrief_ofp::get();
      if (ofp.valid && !ofp.navlog.empty()) {
        int cruise_ref = s_enroute_cleared_alt_ft > 0 ? s_enroute_cleared_alt_ft
                                                       : ctx.ifr_cruise_alt_ft;
        float gs = ctx.groundspeed_kts > 80.0f ? ctx.groundspeed_kts : 250.0f;
        // Descent target = highest of (cruise-5000 first step, defaults floor).
        // This mirrors build_descent_clearance so the alert fires at exactly the
        // distance needed for a comfortable 3-degree descent to the clearance altitude.
        int descent_target =
            std::max(defaults.star_entry_alt_ft,
                     (cruise_ref - 5000) / 1000 * 1000);
        // Override with a CIFP exact or at-or-above constraint when available.
        StarEntryResult se_alt;
        if (find_star_entry(ctx.cifp_dir, ofp, se_alt) &&
            se_alt.entry_alt_ft > 0 && se_alt.entry_alt_ft < cruise_ref)
          descent_target = se_alt.entry_alt_ft;
        // alert = NM needed for 3-deg descent  +  clearance exchange buffer (gs/20)
        float alt_component =
            static_cast<float>(std::max(0, cruise_ref - descent_target)) / 300.0f;
        float spd_component = gs / 20.0f;
        alert_nm = std::max(15.0f, std::min(80.0f, alt_component + spd_component));

        StarEntryResult se;
        if (find_star_entry(ctx.cifp_dir, ofp, se) && !se.star_name.empty()) {
          // CIFP-confirmed STAR entry: measure to that fix.
          dist_nm = traffic_geometry::distance_nm(
              ctx.latitude, ctx.longitude, se.lat, se.lon);
        } else {
          // No CIFP match — measure to destination so prompt fires at correct time.
          const auto &dest_fix = ofp.navlog.back();
          dist_nm = traffic_geometry::distance_nm(
              ctx.latitude, ctx.longitude, dest_fix.lat, dest_fix.lon);
        }
      }
    }

    // Pre-TOD prompt: fires once, ~5 min before STAR entry fix.
    if (!s_enroute_descent_prompt_issued &&
        dist_nm <= static_cast<double>(alert_nm)) {
      s_enroute_descent_prompt_issued = true;
      if (out_text) {
        char buf[120];
        std::snprintf(buf, sizeof(buf), "%s, advise when ready to descend.",
                      callsign.c_str());
        *out_text = buf;
      }
      logging::info("IFR en-route: pre-TOD prompt (%.1f NM to STAR entry, alert=%.0f NM)",
                    dist_nm, alert_nm);
      return true;
    }

    // Forced clearance when aircraft is very close to STAR entry and pilot has
    // not responded to the prompt (10 NM hard cutoff).
    if (s_enroute_descent_prompt_issued &&
        dist_nm <= static_cast<double>(alert_nm * 0.4f)) {
      logging::info("IFR en-route: forced descent (%.1f NM, pilot did not respond)",
                    dist_nm);
      if (build_descent_clearance(ctx, callsign, defaults, out_text))
        return true;
    }

    // Safety net: 25 min elapsed with no OFP or no TOD ever computed.
    if (s_enroute_timer > 25.0f * 60.0f) {
      logging::info("IFR en-route: 25 min safety net -- issuing descent");
      if (build_descent_clearance(ctx, callsign, defaults, out_text))
        return true;
    }
  }

  // ── Sub-phase 2.5: CTA boundary → Approach frequency handoff ─────────
  // Primary: openair_db TMA/CTR boundary crossing (exact airspace geometry).
  // Fallback: ≤50 NM to destination when openair_db has no TMA coverage.
  // Polled at 1 Hz — no need to run the point-in-polygon every frame.
  if (s_enroute_descent_issued && !s_enroute_approach_handoff_issued) {
    openair_db::AirspaceEntry enc_arrival;
    bool fire_handoff = false;
    s_enroute_app_check_sec -= dt;
    if (s_enroute_app_check_sec <= 0.0f) {
      s_enroute_app_check_sec = 1.0f;
      if (openair_db::ready()) {
        enc_arrival = openair_db::find_enclosing(
            ctx.latitude, ctx.longitude, static_cast<int>(ctx.altitude_ft_msl));
        if (enc_arrival.ac_class == openair_db::AirspaceClass::TMA ||
            enc_arrival.ac_class == openair_db::AirspaceClass::CTR) {
          fire_handoff = true;
          logging::info(
              "IFR en-route: TMA/CTR entry '%s' floor=%dft ceil=%dft"
              " at %.0fft MSL -- handoff to Approach",
              enc_arrival.name.c_str(),
              enc_arrival.floor_ft, enc_arrival.ceiling_ft,
              ctx.altitude_ft_msl);
        }
      }
      if (!fire_handoff) {
        // No openair_db coverage: fall back to distance threshold.
        auto ofp = simbrief_ofp::get();
        if (ofp.valid && !ofp.navlog.empty()) {
          double dist_nm = traffic_geometry::distance_nm(
              ctx.latitude, ctx.longitude,
              ofp.navlog.back().lat, ofp.navlog.back().lon);
          if (dist_nm <= 50.0) {
            fire_handoff = true;
            logging::info("IFR en-route: 50 NM fallback (%.1f NM, no TMA data) -- handoff to Approach",
                          dist_nm);
          }
        }
      }
    }
    if (fire_handoff) {
      if (build_approach_handoff(ctx, callsign, out_text, enc_arrival))
        return true;
    }
  }

  // ── Sub-phase 2.5: cruise altitude deviation warning ──────────────────
  // RVSM (FL290+): threshold 200 ft. Below FL290: 300 ft (ICAO standard).
  // 2-minute cooldown between warnings. Grace period of 60 s after check-in
  // so the aircraft has time to level off before monitoring begins.
  // Suppressed once descent clearance has been issued.
  s_enroute_alt_warn_cooldown =
      std::max(0.0f, s_enroute_alt_warn_cooldown - dt);
  if (!s_enroute_descent_issued && s_enroute_cleared_alt_ft > 0 &&
      s_enroute_timer >= 60.0f && s_enroute_alt_warn_cooldown <= 0.0f) {
    // FL clearances use pressure altitude (1013.25 hPa reference), not GPS/MSL.
    int actual_ft = static_cast<int>(ctx.pressure_alt_ft);
    int deviation_ft = actual_ft - s_enroute_cleared_alt_ft;
    int threshold_ft = (s_enroute_cleared_alt_ft >= 29000) ? 200 : 300;
    if (std::abs(deviation_ft) >= threshold_ft) {
      s_enroute_alt_warn_cooldown = 120.0f;
      if (out_text) {
        int fl = s_enroute_cleared_alt_ft / 100;
        char buf[160];
        std::snprintf(
            buf, sizeof(buf),
            "%s, check altitude, you are %d feet %s assigned flight level %d.",
            callsign.c_str(), std::abs(deviation_ft),
            deviation_ft > 0 ? "above" : "below", fl);
        *out_text = buf;
      }
      logging::info("IFR en-route: altitude deviation %+d ft from FL%d",
                    deviation_ft, s_enroute_cleared_alt_ft / 100);
      return true;
    }
  }

  // ── Sub-phase 3: cross-track deviation warning ────────────────────────
  // Fires when the aircraft is more than 5 NM off the filed route.
  // 3-minute cooldown between warnings.
  // Suppressed after a direct-to has been issued: the original navlog legs are
  // superseded by the direct routing ATC just cleared the aircraft on.
  // Only warn after the direct-to window has opened. Sub-phase 1 runs first
  // in this function so direct-to fires (and sets s_enroute_direct_issued) at
  // the same threshold — this guard therefore only triggers when the direct-to
  // itself was suppressed (e.g. no OFP fix ahead), preventing early false
  // positives while still on the SID.
  if (!s_enroute_direct_issued && s_enroute_deviation_cooldown_sec <= 0.0f &&
      s_enroute_timer >= s_enroute_direct_delay_sec) {
    auto ofp = simbrief_ofp::get();
    if (ofp.valid && ofp.navlog.size() >= 2) {
      double xt_nm = std::abs(min_cross_track_nm(ctx, ofp.navlog));
      if (xt_nm > 5.0) {
        s_enroute_deviation_cooldown_sec = 180.0f;
        if (out_text) {
          char buf[160];
          std::snprintf(buf, sizeof(buf),
                        "%s, confirm routing, you appear off track.",
                        callsign.c_str());
          *out_text = buf;
        }
        logging::info("IFR en-route: cross-track deviation %.1f NM", xt_nm);
        return true;
      }
    }
  }

  return false;
}

const std::string &current_controller_label() {
  return s_current_controller_label;
}

void set_controller_label(const std::string &label) {
  if (!label.empty())
    s_current_controller_label = label;
}

void set_pending_departure_label(const std::string &label) {
  if (!label.empty())
    s_pending_departure_label = label;
}

const std::string &pending_departure_label() {
  return s_pending_departure_label;
}

// ── Helpers for poll_approach ─────────────────────────────────────────────

// Build a STAR constraint clearance string for one waypoint.
// Format: "[cs], direct [fix], descend [alt]."
// QNH is appended when alt is in feet (below transition altitude).
static std::string build_star_constraint(
    const std::string &cs,
    const cifp_reader::StarWaypoint &wp,
    int cleared_ft,
    int qnh_hpa,
    int ta_ft
) {
  const int ta = (ta_ft > 0) ? ta_ft : 5000;
  std::string msg = cs;
  char alt_buf[64];
  if (cleared_ft > ta)
    std::snprintf(alt_buf, sizeof(alt_buf), ", descend flight level %d", cleared_ft / 100);
  else
    std::snprintf(alt_buf, sizeof(alt_buf), ", descend %d feet, QNH %d", cleared_ft, qnh_hpa);
  msg += alt_buf;
  if (wp.speed_kt > 0) {
    char spd[40];
    std::snprintf(spd, sizeof(spd), ", speed %d knots or less", wp.speed_kt);
    msg += spd;
  }
  msg += ".";
  return msg;
}

// Build an approach step-down clearance.
// Format: "[cs], direct [fix], descend [alt]."  (fix_ident may be empty)
// QNH appended when alt is below transition altitude.
static std::string build_approach_final_alt(const std::string &cs,
                                             const std::string &fix_ident,
                                             int alt_ft,
                                             int qnh_hpa,
                                             int ta_ft) {
  const int ta = (ta_ft > 0) ? ta_ft : 5000;
  std::string msg = cs;
  if (!fix_ident.empty())
    msg += ", direct " + fix_ident;
  char alt_buf[80];
  if (alt_ft > ta)
    std::snprintf(alt_buf, sizeof(alt_buf), ", descend flight level %d.", alt_ft / 100);
  else
    std::snprintf(alt_buf, sizeof(alt_buf), ", descend %d feet, QNH %d.", alt_ft, qnh_hpa);
  msg += alt_buf;
  return msg;
}

// ── Route fix tracker ────────────────────────────────────────────────────
// Logging only — no ATC speech, no state change.

// Build the ordered route fix list from OFP navlog + STAR/APP waypoints.
// Looks up lat/lon for STAR/APP idents from earth_fix.dat.
// Called once when STAR/approach waypoints are loaded.
static void init_route_fixes(const xplane_context::XPlaneContext &ctx) {
  s_route_fixes.clear();
  s_route_fix_idx = 0;

  // 1. OFP navlog fixes (already have lat/lon).
  // TOC and TOD are SimBrief pseudo-fixes with no airspace significance.
  const auto &ofp = simbrief_ofp::get();
  std::unordered_set<std::string> seen;
  for (const auto &nf : ofp.navlog) {
    if (nf.ident.empty() || nf.ident == s_assigned_dest_icao) continue;
    if (nf.ident == "TOC" || nf.ident == "TOD") continue;
    if (seen.count(nf.ident)) continue;
    seen.insert(nf.ident);
    s_route_fixes.push_back({nf.ident, nf.lat, nf.lon});
  }

  // 2. STAR/APP waypoints — resolve positions from earth_fix.dat.
  if (!s_approach_waypoints.empty() && !ctx.cifp_dir.empty()) {
    std::vector<std::string> idents;
    for (const auto &wp : s_approach_waypoints)
      if (!wp.ident.empty() && !seen.count(wp.ident))
        idents.push_back(wp.ident);

    const auto pos_map = cifp_reader::lookup_fix_positions(
        ctx.cifp_dir, idents, s_assigned_dest_icao);

    for (const auto &wp : s_approach_waypoints) {
      if (wp.ident.empty() || seen.count(wp.ident)) continue;
      seen.insert(wp.ident);
      double lat = 0.0, lon = 0.0;
      auto it = pos_map.find(wp.ident);
      if (it != pos_map.end()) { lat = it->second.first; lon = it->second.second; }
      s_route_fixes.push_back({wp.ident, lat, lon});
    }
  }

  // 3. Advance past fixes that are already behind the aircraft.
  // Two conditions to skip: (a) within 3 NM — too close, already past;
  // (b) behind the aircraft — bearing to fix differs from heading by > 90°.
  // Condition (b) is necessary when calling in mid-STAR: the navlog starts
  // at the origin airport (far behind), so without heading-based skipping
  // s_route_fix_idx would remain at 0 forever.
  while (s_route_fix_idx < static_cast<int>(s_route_fixes.size())) {
    const auto &rf = s_route_fixes[s_route_fix_idx];
    if (rf.lat == 0.0 && rf.lon == 0.0) { s_route_fix_idx++; continue; }
    float d = static_cast<float>(traffic_geometry::distance_nm(
        ctx.latitude, ctx.longitude, rf.lat, rf.lon));
    if (d < 1.5f) { s_route_fix_idx++; continue; }  // already very close
    double brg = traffic_geometry::bearing_deg(
        ctx.latitude, ctx.longitude, rf.lat, rf.lon);
    double diff = std::abs(brg - static_cast<double>(ctx.heading_true));
    if (diff > 180.0) diff = 360.0 - diff;
    if (diff > 90.0) { s_route_fix_idx++; continue; }  // fix is behind heading
    break;
  }

  logging::info("[route] tracker init: %d fixes, start idx=%d",
                static_cast<int>(s_route_fixes.size()), s_route_fix_idx);
  for (int i = s_route_fix_idx;
       i < static_cast<int>(s_route_fixes.size()) && i < s_route_fix_idx + 10;
       ++i) {
    const auto &rf = s_route_fixes[i];
    logging::info("[route]   [%d] %s (%.4f,%.4f)", i,
                  rf.ident.c_str(), rf.lat, rf.lon);
  }
}

std::string poll_route_tracker(const xplane_context::XPlaneContext &ctx) {
  // Priority: return any pending ATC-direct event before the rate-limited
  // proximity check so atc_session sees it on the very next frame.
  if (!s_pending_route_direct.empty()) {
    std::string ev = s_pending_route_direct;
    s_pending_route_direct.clear();
    return ev;
  }

  if (s_route_fixes.empty()) return {};
  if (s_route_fix_idx >= static_cast<int>(s_route_fixes.size())) return {};

  // Rate-limit to 1 Hz — distance check is not time-critical.
  // atc_session::update() is called at ~60 FPS; we accumulate real dt.
  // Use a simple flight-loop frame counter approximation via a static.
  s_route_tracker_tick += 1.0f / 60.0f; // approximate 60 FPS
  if (s_route_tracker_tick < 1.0f) return {};
  s_route_tracker_tick = 0.0f;

  const auto &fix = s_route_fixes[s_route_fix_idx];

  // Skip fixes whose position is unknown.
  if (fix.lat == 0.0 && fix.lon == 0.0) {
    s_route_fix_idx++;
    return {};
  }

  const float dist = static_cast<float>(traffic_geometry::distance_nm(
      ctx.latitude, ctx.longitude, fix.lat, fix.lon));

  if (dist > 1.5f) return {};

  // Entered 1.5 NM zone around this fix — log and advance.
  const int next_idx = s_route_fix_idx + 1;
  std::string next_ident = "end of route";
  if (next_idx < static_cast<int>(s_route_fixes.size()))
    next_ident = s_route_fixes[next_idx].ident;

  char buf[160];
  std::snprintf(buf, sizeof(buf), "Track: near %s (%.1f NM), next: %s",
                fix.ident.c_str(), dist, next_ident.c_str());

  logging::info("[route] %s", buf);
  s_route_fix_idx++;
  return buf;
}

// ── poll_approach ─────────────────────────────────────────────────────────

bool poll_approach(const xplane_context::XPlaneContext &ctx, float dt,
                   std::string *out_text) {
  using AS = atc_state_machine::ATCState;

  const AS state = atc_state_machine::get_state();
  if (state != AS::IFR_APPROACH_CONTACT && state != AS::IFR_APPROACH_DESCENT) {
    s_approach_waypoints.clear();
    s_approach_waypoint_idx = 0;
    s_approach_timer = 0.0f;
    s_approach_initial_fl = 0;
    s_approach_final_issued = false;
    s_approach_tower_handed_off = false;
    s_approach_faf = {};
    s_last_cleared_route_idx    = -1;
    s_faf_route_idx             = -1;
    s_faf_ap_idx                = -1;
    s_map_ap_idx                = -1;
    s_approach_has_visual_final = false;
    s_expedite_cooldown        = 0.0f;
    s_expedite_last_cleared_ft = 0;
    s_pending_route_direct.clear();
    return false;
  }

  s_approach_timer += dt;

  // Load STAR + approach procedure waypoints on first entry (fallback if not
  // loaded at APPROACH_CONTACT, e.g. training_jump_approach).
  if (s_approach_waypoints.empty() && s_approach_waypoint_idx == 0 &&
      !s_assigned_star_name.empty() && !s_assigned_dest_icao.empty()) {
    s_approach_waypoints = cifp_reader::star_waypoints(
        ctx.cifp_dir, s_assigned_dest_icao, s_assigned_star_name);
    logging::info("IFR approach: loaded %d constrained STAR waypoints for %s",
                  static_cast<int>(s_approach_waypoints.size()),
                  s_assigned_star_name.c_str());
    if (!s_assigned_approach_designator.empty()) {
      const std::string iaf = cifp_reader::star_last_fix(
          ctx.cifp_dir, s_assigned_dest_icao, s_assigned_star_name);
      if (!iaf.empty()) {
        auto proc = cifp_reader::approach_procedure_waypoints(
            ctx.cifp_dir, s_assigned_dest_icao,
            s_assigned_approach_designator, iaf);
        if (!proc.empty()) {
          for (auto &w : proc)
            s_approach_waypoints.push_back(w);
          s_approach_final_issued = true;
          logging::info("IFR approach: appended %d IAF-transition waypoints (%s)",
                        static_cast<int>(proc.size()),
                        s_assigned_approach_designator.c_str());
          s_faf_ap_idx = -1;
          s_map_ap_idx = -1;
          for (int i = 0; i < static_cast<int>(s_approach_waypoints.size()); ++i) {
            const auto &w = s_approach_waypoints[i];
            if (s_faf_ap_idx < 0 && w.is_approach_proc &&
                w.ident == s_approach_faf.ident)
              s_faf_ap_idx = i;
            if (s_map_ap_idx < 0 && w.is_approach_proc && w.is_map)
              s_map_ap_idx = i;
          }
          logging::info("[route] FAF ap_idx=%d MAP ap_idx=%d (lazy)",
                        s_faf_ap_idx, s_map_ap_idx);
        }
      }
    }
    // Route tracker init (lazy path: training jump, waypoints loaded here).
    if (s_route_fixes.empty())
      init_route_fixes(ctx);
    if (s_faf_route_idx < 0 && !s_approach_faf.ident.empty()) {
      for (int i = 0; i < static_cast<int>(s_route_fixes.size()); ++i) {
        if (s_route_fixes[i].ident == s_approach_faf.ident) {
          s_faf_route_idx = i;
          break;
        }
      }
    }
  }

  // Only proactively issue once pilot is on Approach frequency.
  // In APPROACH_CONTACT, wait for pilot INITIAL_CALL_APPROACH which is
  // handled in process_transcript. poll_approach only drives DESCENT steps.
  if (state == AS::IFR_APPROACH_CONTACT)
    return false;

  // IFR_APPROACH_DESCENT: step through constrained waypoints.
  // Trigger: aircraft has descended within 10% above the constraint altitude
  // (or 3-minute fallback after previous clearance).
  const std::string &cs_ref2 = atc_state_machine::session_callsign();
  const std::string cs =
      cs_ref2.empty() ? settings::pilot_callsign() : cs_ref2;

  // Skip waypoints the aircraft is already in compliance with — no instruction
  // needed for a constraint the aircraft already meets.
  // Also skip unconstrained STAR routing waypoints (no altitude, no speed) —
  // these are plain route fixes with no ATC action; skip to the next constrained
  // fix so the clearance names the real target (e.g. "direct BISBO" not MN141).
  while (s_approach_waypoint_idx < static_cast<int>(s_approach_waypoints.size())) {
    const auto &wp = s_approach_waypoints[s_approach_waypoint_idx];
    // Don't silently skip MAP or post-MAP via already_compliant —
    // the step-down block handles them explicitly.
    if (wp.is_approach_proc &&
        (wp.is_map || (s_map_ap_idx >= 0 && s_approach_waypoint_idx > s_map_ap_idx)))
      break;
    // Unconstrained routing fix (no altitude, no speed) — skip silently,
    // keep route tracker in sync. Applies to both STAR and approach-proc
    // fixes (e.g. MAP/NERAS which has no altitude constraint but blocks
    // the Tower handoff if left in the queue).
    if (wp.alt.feet == 0 && wp.speed_kt == 0) {
      if (!wp.ident.empty()) {
        for (int ri = s_route_fix_idx;
             ri < static_cast<int>(s_route_fixes.size()); ++ri) {
          if (s_route_fixes[ri].ident == wp.ident) {
            s_route_fix_idx = ri;
            break;
          }
        }
      }
      s_approach_waypoint_idx++;
      continue;
    }
    if (wp.alt.feet > 0) {
      const float wp_ft = static_cast<float>(wp.alt.feet);
      // 200 ft tolerance for ceiling constraints: pressure altimeter error
      // and residual QNH offsets mean PA can be slightly above the cleared FL.
      bool already_compliant =
          (wp.is_ceiling  && ctx.pressure_alt_ft <= wp_ft + 200.0f) ||
          (wp.is_floor    && ctx.pressure_alt_ft >= wp_ft) ||
          (!wp.is_ceiling && !wp.is_floor && ctx.pressure_alt_ft <= wp_ft + 200.0f);
      if (already_compliant) {
        s_approach_waypoint_idx++;
        s_approach_timer = 0.0f;
        continue;
      }
    }
    break;
  }

  // ── Approach → Tower handoff: "contact Tower when established" ──────────
  // Checked BEFORE the waypoint loop so it fires regardless of any remaining
  // queued waypoints (e.g. MN04A constraint not yet cleared).
  // APP hands off to Tower when the aircraft is established on final (at FAF).
  if (s_approach_final_issued && !s_approach_tower_handed_off) {
    bool at_faf = false;
    if (s_faf_route_idx >= 0) {
      // Primary: route tracker has passed the FAF fix (aircraft within 1.5 NM).
      at_faf = (s_route_fix_idx > s_faf_route_idx);
    } else if (s_approach_faf.lat != 0.0 || s_approach_faf.lon != 0.0) {
      double dist_nm = traffic_geometry::distance_nm(
          ctx.latitude, ctx.longitude, s_approach_faf.lat, s_approach_faf.lon);
      at_faf = (dist_nm < 2.0);
    } else if (s_approach_faf.alt_ft > 0) {
      at_faf = (ctx.pressure_alt_ft <=
                static_cast<float>(s_approach_faf.alt_ft) * 1.1f);
    } else {
      double dist_to_apt = traffic_geometry::distance_nm(
          ctx.latitude, ctx.longitude, ctx.airport_lat, ctx.airport_lon);
      at_faf = (ctx.height_agl_ft < 3500.0f && dist_to_apt < 12.0);
    }

    if (at_faf) {
      s_approach_tower_handed_off = true;
      // MDA detection: compute at handoff time so runway is confirmed and any
      // late runway change (wind shift) is reflected. Offset/circling approach
      // (track vs runway heading > 30 deg) -> pilot must acquire visually.
      logging::info("[approach] Tower: faf_track=%d rwy=%s faf_ap=%d map_ap=%d",
                    s_approach_faf.final_track_deg,
                    s_assigned_landing_runway.c_str(),
                    s_faf_ap_idx, s_map_ap_idx);
      s_approach_has_visual_final = false;
      if (s_approach_faf.final_track_deg > 0 && !s_assigned_landing_runway.empty()) {
        const int rwy_num = std::atoi(s_assigned_landing_runway.c_str());
        if (rwy_num >= 1 && rwy_num <= 36) {
          const float rwy_hdg = static_cast<float>(rwy_num * 10);
          float diff = std::abs(
              static_cast<float>(s_approach_faf.final_track_deg) - rwy_hdg);
          if (diff > 180.0f) diff = 360.0f - diff;
          s_approach_has_visual_final = (diff > 30.0f);
          logging::info("[approach] track=%d rwy=%s hdg=%.0f diff=%.0f visual=%d",
                        s_approach_faf.final_track_deg,
                        s_assigned_landing_runway.c_str(),
                        rwy_hdg, diff,
                        s_approach_has_visual_final ? 1 : 0);
        }
      }
      float tower_mhz = 0.0f;
      if (!s_assigned_dest_icao.empty())
        tower_mhz = xplane_context::tower_mhz_for(s_assigned_dest_icao);
      if (tower_mhz <= 100.0f)
        tower_mhz = ctx.airport_freqs.first_mhz(
            xplane_context::FrequencyType::TOWER);
      if (out_text) {
        char buf[128];
        const char *final_call = s_approach_has_visual_final
                                     ? "runway in sight"
                                     : "report established";
        if (tower_mhz > 100.0f) {
          int khz = static_cast<int>(std::round(tower_mhz * 1000.0f));
          std::snprintf(buf, sizeof(buf),
                        "%s, contact Tower on %d.%03d, %s.",
                        cs.c_str(), khz / 1000, khz % 1000, final_call);
        } else {
          std::snprintf(buf, sizeof(buf),
                        "%s, contact Tower, %s.", cs.c_str(), final_call);
        }
        *out_text = buf;
        atc_state_machine::set_state(AS::IFR_APPROACH_TOWER);
        return true;
      }
    }
  }

  if (s_approach_waypoint_idx < static_cast<int>(s_approach_waypoints.size())) {
    const auto &wp = s_approach_waypoints[s_approach_waypoint_idx];

    // Route-tracker fix trigger: fires when the aircraft passes the last-cleared
    // fix (route tracker advances past s_last_cleared_route_idx). This ensures
    // the next step-down fires as the aircraft reaches the previous cleared fix,
    // not when it happens to descend through an altitude band prematurely.
    // Falls back to 3-minute timer when no step-down has been issued yet.
    bool fix_trigger  = (s_last_cleared_route_idx >= 0 &&
                         s_route_fix_idx > s_last_cleared_route_idx);
    bool time_trigger = (s_approach_timer > 180.0f);

    if (!fix_trigger && !time_trigger)
      return false;

    // Cleared FL: for ceiling constraints, clear to that FL.
    // For floor constraints (at-or-above), descend to the floor value
    // so the crew meets the constraint.
    int cleared_ft = wp.alt.feet;
    if (cleared_ft == 0 && wp.speed_kt > 0)
      cleared_ft = static_cast<int>(ctx.pressure_alt_ft / 100) * 100; // maintain current

    if (cleared_ft > 0 && out_text) {
      if (wp.is_approach_proc) {
        // MAP: no ATC clearance — crew follows the chart from here.
        // Post-MAP: GO_AROUND territory — skip unless a GO_AROUND was fired.
        if (wp.is_map || (s_map_ap_idx >= 0 && s_approach_waypoint_idx > s_map_ap_idx)) {
          s_approach_waypoint_idx++;
          s_approach_timer = 0.0f;
          return false;
        }
        // Type 1 (80%): "direct [current fix], descend [alt]" — normal next-fix routing.
        // Type 2 (20%): shortcut to a fix up to 3 steps ahead, but never to or past
        // the FAF. The FAF is always issued explicitly; waypoints after it (MAP/MDA)
        // are flown by the crew from the chart, not cleared by ATC.
        // The unconditional ++ at the end of this block advances to target_idx+1.
        int target_idx = s_approach_waypoint_idx;
        const int n_app = static_cast<int>(s_approach_waypoints.size());
        // Only shortcut when strictly before the FAF: once at FAF, the next
        // waypoint is the MAP — no "direct MAP" clearance is ever appropriate.
        if (std::rand() % 5 == 0 && !s_approach_faf.ident.empty() &&
            (s_faf_ap_idx < 0 || s_approach_waypoint_idx < s_faf_ap_idx)) {
          const int max_skip = std::min(s_approach_waypoint_idx + 3, n_app - 1);
          std::vector<int> cands;
          for (int k = s_approach_waypoint_idx + 1; k <= max_skip; ++k) {
            if (!s_approach_waypoints[k].is_approach_proc ||
                s_approach_waypoints[k].ident.empty())
              continue;
            if (s_approach_waypoints[k].ident == s_approach_faf.ident)
              break; // FAF is never a shortcut target — always issued explicitly
            cands.push_back(k);
          }
          if (!cands.empty())
            target_idx = cands[std::rand() % static_cast<int>(cands.size())];
        }
        const auto &twp = s_approach_waypoints[target_idx];
        const int tft   = (target_idx != s_approach_waypoint_idx && twp.alt.feet > 0)
                              ? twp.alt.feet : cleared_ft;
        *out_text = build_approach_final_alt(cs, twp.ident, tft, ctx.qnh_hpa,
                                             ctx.transition_alt_ft);
        if (!twp.ident.empty()) {
          for (int ri = s_route_fix_idx;
               ri < static_cast<int>(s_route_fixes.size()); ++ri) {
            if (s_route_fixes[ri].ident == twp.ident) {
              s_route_fix_idx = ri;
              logging::info("[route] ATC direct: %s (idx=%d)", twp.ident.c_str(), ri);
              s_pending_route_direct = "ATC direct: " + twp.ident;
              break;
            }
          }
        }
        s_expedite_last_cleared_ft = tft;
        s_expedite_cooldown        = 60.0f;
        s_approach_waypoint_idx = target_idx; // ++ below lands on target+1
      } else {
        // 20% probability: issue an explicit "direct X, descend Y" STAR clearance.
        // 80% of the time, silently advance past this fix — aircraft follows the
        // STAR naturally without an ATC call (vectoring will be added later).
        if (std::rand() % 5 != 0) {
          s_approach_waypoint_idx++;
          s_approach_timer = 0.0f;
          return false;
        }
        *out_text = build_star_constraint(cs, wp, cleared_ft,
                                          ctx.qnh_hpa, ctx.transition_alt_ft);
        // Advance route tracker to this STAR fix.
        if (!wp.ident.empty()) {
          for (int ri = s_route_fix_idx;
               ri < static_cast<int>(s_route_fixes.size()); ++ri) {
            if (s_route_fixes[ri].ident == wp.ident) {
              s_route_fix_idx = ri;
              logging::info("[route] ATC direct: %s (idx=%d)", wp.ident.c_str(), ri);
              s_pending_route_direct = "ATC direct: " + wp.ident;
              break;
            }
          }
        }
      }
      s_expedite_last_cleared_ft = cleared_ft;
      s_expedite_cooldown        = 60.0f;
      s_last_cleared_route_idx   = s_route_fix_idx; // arm fix_trigger for next step-down
      s_approach_waypoint_idx++;
      s_approach_timer = 0.0f;
      return true;
    }
    s_approach_waypoint_idx++;
    return false;
  }

  // ── Expedite-descent monitor ─────────────────────────────────────────────
  // Fires AFTER a step-down clearance has been issued (s_expedite_last_cleared_ft > 0)
  // when the aircraft is clearly not descending fast enough to meet it.
  // Uses the last-cleared altitude so it always references an altitude the pilot
  // was actually given, never a future waypoint constraint.
  if (s_expedite_last_cleared_ft > 0 &&
      ctx.pressure_alt_ft > static_cast<float>(s_expedite_last_cleared_ft) + 300.0f) {
    s_expedite_cooldown -= dt;
    if (s_expedite_cooldown <= 0.0f) {
      const double dist_apt = traffic_geometry::distance_nm(
          ctx.latitude, ctx.longitude, ctx.airport_lat, ctx.airport_lon);
      if (dist_apt < 60.0) {
        const float gs = ctx.groundspeed_kts > 60.0f ? ctx.groundspeed_kts : 200.0f;
        const float alt_diff =
            ctx.pressure_alt_ft - static_cast<float>(s_expedite_last_cleared_ft);
        // Approximate time to destination airport as proxy for time to cleared fix.
        const float time_min = static_cast<float>(dist_apt) / gs * 60.0f;
        if (time_min > 0.5f) {
          const float required_rate = alt_diff / time_min;
          const float current_rate  = -ctx.vertical_speed_fpm; // + = descending
          const bool nearly_level = current_rate < 300.0f && required_rate > 800.0f;
          const bool too_slow     = required_rate > current_rate * 1.5f &&
                                    required_rate > 800.0f;
          if (nearly_level || too_slow) {
            const int ta = ctx.transition_alt_ft > 0 ? ctx.transition_alt_ft : 5000;
            char buf[160];
            if (s_expedite_last_cleared_ft > ta)
              std::snprintf(buf, sizeof(buf),
                            "%s, expedite descent to flight level %d.",
                            cs.c_str(), s_expedite_last_cleared_ft / 100);
            else
              std::snprintf(buf, sizeof(buf),
                            "%s, expedite descent to %d feet, QNH %d.",
                            cs.c_str(), s_expedite_last_cleared_ft, ctx.qnh_hpa);
            if (out_text) {
              *out_text = buf;
              s_expedite_cooldown = 90.0f;
              return true;
            }
          }
        }
      }
    }
  }

  // All STAR constraints issued — issue final altitude below transition altitude.
  // final_alt_ft comes from ifr_defaults.approach_entry_alt_ft (flight_rules.json).
  const auto &ifrdef = flight_phase::get_ifr_defaults();
  // Fire final altitude when aircraft is below 1.5× the approach entry altitude.
  if (!s_approach_final_issued && s_approach_timer > 60.0f &&
      ctx.pressure_alt_ft < static_cast<float>(ifrdef.approach_entry_alt_ft) * 1.5f) {
    s_approach_final_issued = true;
    const int final_alt_ft = ifrdef.approach_entry_alt_ft;
    if (out_text) {
      *out_text = build_approach_final_alt(cs, "", final_alt_ft, ctx.qnh_hpa,
                                           ctx.transition_alt_ft);
      return true;
    }
  }

  return false;
}

// ── poll_approach_alignment ───────────────────────────────────────────────
// Fires after the FAF (IFR_APPROACH_TOWER state) when the aircraft is more
// than 0.5 NM off the extended runway centerline.
// Distinct from s_enroute_deviation_cooldown_sec (airway off-track, en-route).

bool poll_approach_alignment(const xplane_context::XPlaneContext &ctx, float dt,
                             std::string *out_text) {
  using AS = atc_state_machine::ATCState;
  if (atc_state_machine::get_state() != AS::IFR_APPROACH_TOWER)
    return false;

  s_alignment_cooldown -= dt;
  if (s_alignment_cooldown > 0.0f)
    return false;

  if (s_assigned_landing_runway.empty())
    return false;

  // Only check when within 8 NM of airport and below 3000 ft AGL.
  const double dist_apt = traffic_geometry::distance_nm(
      ctx.latitude, ctx.longitude, ctx.airport_lat, ctx.airport_lon);
  if (dist_apt > 8.0 || ctx.height_agl_ft > 3000.0f)
    return false;

  // Find landing-runway threshold (matching s_assigned_landing_runway).
  double rwy_lat = 0.0, rwy_lon = 0.0;
  float  rwy_hdg = -1.0f;
  for (const auto &rwy : ctx.runways) {
    if (rwy.end1.number == s_assigned_landing_runway) {
      rwy_lat = rwy.end1.lat; rwy_lon = rwy.end1.lon;
      rwy_hdg = rwy.end1.heading_deg;
      break;
    }
    if (rwy.end2.number == s_assigned_landing_runway) {
      rwy_lat = rwy.end2.lat; rwy_lon = rwy.end2.lon;
      rwy_hdg = rwy.end2.heading_deg;
      break;
    }
  }
  if (rwy_hdg < 0.0f)
    return false;

  // Cross-track error from extended centerline.
  // Approach course from threshold (outbound = rwy_hdg + 180°).
  const double approach_course = std::fmod(static_cast<double>(rwy_hdg) + 180.0, 360.0);
  const double bearing_to_acft = traffic_geometry::bearing_deg(
      rwy_lat, rwy_lon, ctx.latitude, ctx.longitude);
  double bearing_diff = bearing_to_acft - approach_course;
  if (bearing_diff >  180.0) bearing_diff -= 360.0;
  if (bearing_diff < -180.0) bearing_diff += 360.0;
  const double dist_nm = traffic_geometry::distance_nm(
      ctx.latitude, ctx.longitude, rwy_lat, rwy_lon);
  const double cross_track_nm =
      std::sin(bearing_diff * M_PI / 180.0) * dist_nm;

  if (std::fabs(cross_track_nm) < 0.5)
    return false;

  const std::string &cs_ref = atc_state_machine::session_callsign();
  const std::string cs = cs_ref.empty() ? settings::pilot_callsign() : cs_ref;

  char buf[160];
  std::snprintf(buf, sizeof(buf),
                "%s, confirm established on the approach, runway %s.",
                cs.c_str(), s_assigned_landing_runway.c_str());
  if (out_text) {
    *out_text = buf;
    s_alignment_cooldown = 60.0f;
    return true;
  }
  return false;
}

bool poll_ground_runway_change(const xplane_context::XPlaneContext &ctx,
                               std::string *out_text) {
  if (!ctx.on_ground || ctx.active_runway.empty())
    return false;

  using AS = atc_state_machine::ATCState;
  AS state = atc_state_machine::get_state();

  // In IDLE: silently track the runway so we don't announce a change that
  // happened before the pilot was engaged.
  if (state == AS::IDLE) {
    s_ground_last_announced_runway = ctx.active_runway;
    return false;
  }

  bool active_ground_state =
      (state == AS::GROUND_CONTACT || state == AS::TAXI_CLEARED ||
       state == AS::TOWER_CONTACT || state == AS::IFR_PREDEP_CLEARANCE ||
       state == AS::IFR_CLEARED);
  if (!active_ground_state)
    return false;

  // Don't interrupt a pending readback — the pilot is mid-clearance.
  if (atc_state_machine::is_readback_pending())
    return false;

  // Seed on first entry into an active state.
  if (s_ground_last_announced_runway.empty()) {
    s_ground_last_announced_runway = ctx.active_runway;
    return false;
  }

  if (ctx.active_runway == s_ground_last_announced_runway)
    return false;

  // Runway changed — update tracking and sync the assigned runway so
  // get_runway() in build_vars uses the new runway for holding point
  // phrases and lineup instructions.
  s_ground_last_announced_runway = ctx.active_runway;
  atc_state_machine::set_assigned_runway(ctx.active_runway);

  if (!out_text)
    return true;

  static const char *kPhonetic[] = {
      "Alpha",   "Bravo",  "Charlie", "Delta",   "Echo",    "Foxtrot",
      "Golf",    "Hotel",  "India",   "Juliet",  "Kilo",    "Lima",
      "Mike",    "November", "Oscar", "Papa",    "Quebec",  "Romeo",
      "Sierra",  "Tango",  "Uniform", "Victor",  "Whiskey", "X-ray",
      "Yankee",  "Zulu"};

  std::string hp_phrase = "runway " + ctx.active_runway;
  auto hp_it = ctx.runway_holding_points.find(ctx.active_runway);
  if (hp_it != ctx.runway_holding_points.end() && !hp_it->second.empty()) {
    const std::string &hp = hp_it->second;
    std::string name;
    if (hp.size() == 1 && hp[0] >= 'A' && hp[0] <= 'Z')
      name = kPhonetic[hp[0] - 'A'];
    else if (hp.size() == 1 && hp[0] >= 'a' && hp[0] <= 'z')
      name = kPhonetic[hp[0] - 'a'];
    else
      name = hp;
    hp_phrase = "holding point " + name + ", runway " + ctx.active_runway;
  }

  const std::string &cs = atc_state_machine::session_callsign();
  const std::string &callsign = cs.empty() ? settings::pilot_callsign() : cs;
  char buf[192];
  std::snprintf(buf, sizeof(buf),
                "%s, be advised, active runway is now runway %s, taxi to %s.",
                callsign.c_str(), ctx.active_runway.c_str(),
                hp_phrase.c_str());
  *out_text = buf;
  logging::info("Ground: active runway changed to %s", ctx.active_runway.c_str());
  return true;
}

void set_pending_handoff_freq(float mhz) {
  if (mhz >= 100.0f)
    s_pending_handoff_freq_mhz = mhz;
}

float pending_handoff_freq() { return s_pending_handoff_freq_mhz; }

} // namespace engine
