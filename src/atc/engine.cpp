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
#include <optional>

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
// Set when ATC issues the pre-TOD "advise when ready to descend" prompt.
static bool s_enroute_descent_prompt_issued = false;
// Set when the Approach frequency handoff ("contact Approach on X.XXX") is issued.
static bool s_enroute_approach_handoff_issued = false;

// IFR SID climb management (IFR_RADAR_CONTACT state).
static bool s_sid_direct_issued = false;
static bool s_sid_step1_issued = false;
static bool s_sid_cruise_issued = false;
static bool s_sid_radar_handoff_issued = false;
static bool s_sid_initialized = false; // guards one-time init block
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
  s_enroute_deviation_cooldown_sec = 0.0f;
  s_enroute_sector_freq_khz = 0;
  s_enroute_sector_check_sec = 0.0f;
  s_enroute_cleared_alt_ft = 0;
  s_enroute_alt_warn_cooldown = 0.0f;
  s_sid_direct_issued = false;
  s_sid_step1_issued = false;
  s_sid_cruise_issued = false;
  s_sid_radar_handoff_issued = false;
  s_sid_climb_timer = 0.0f;
  s_sid_step1_alt_ft = 0;
  s_sid_initialized = false;
  s_sid_deviation_cooldown_sec = 0.0f;
  s_sid_direct_origin_lat = 0.0;
  s_sid_direct_origin_lon = 0.0;
  s_departure_apt_lat = 0.0;
  s_departure_apt_lon = 0.0;
  traffic_dialog::reset();
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
             state != AS::IFR_FREQ_HANDOFF && state != AS::IFR_ENROUTE_CRUISE) {
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

  const std::string prompt_key = (settings::backend_language() == "de")
                                     ? "gpt_classify_prompt_de"
                                     : "gpt_classify_prompt";
  std::string sys_prompt = atc_templates::get_prompt(prompt_key);
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
  static const char *kSuffixes[] = {" APP", " DEP", " CTR", " GND", " TWR",
                                    " DLV", " DEL", " FSS", nullptr};
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

  // Fire when the aircraft has left the CTR. Use find_enclosing() on the
  // aircraft's current 3-D position: while still inside a CTR we hold;
  // once the aircraft is in a TMA / FIR (or uncontrolled) we hand off.
  // Fall back to a 2500 ft AGL threshold when airspace.txt is absent.
  {
    auto enc = openair_db::find_enclosing(
        ctx.latitude, ctx.longitude, static_cast<int>(ctx.altitude_ft_msl));
    if (enc.ac_class == openair_db::AirspaceClass::CTR)
      return false; // still inside CTR
    if (enc.ac_class == openair_db::AirspaceClass::OTHER &&
        openair_db::ready()) {
      // airspace.txt loaded but aircraft outside all indexed zones —
      // treat as CTR-exited and continue.
    }
    if (!openair_db::ready()) {
      // Fallback: 2500 ft AGL covers most European CTRs/ATZs.
      float airport_elev_ft = ctx.altitude_ft_msl - ctx.height_agl_ft;
      int ctr_msl =
          openair_db::ctr_ceiling_ft(ctx.airport_lat, ctx.airport_lon);
      float threshold_agl =
          ctr_msl > 0 ? static_cast<float>(ctr_msl) - airport_elev_ft : 2500.0f;
      if (ctx.height_agl_ft < threshold_agl)
        return false;
    }
  }

  // Primary: atc.dat (airspace_db) identifies the TRACON at the aircraft's
  // current 3-D position — gives the real controller name and frequency
  // (e.g. "Marseille Control" on 120.550) rather than apt.dat airport entries.
  std::string controller_label;
  float freq = 0.0f;
  {
    const airspace_db::Controller *tracon = airspace_db::find_by_role_near(
        airspace_db::ControllerRole::TRACON, ctx.latitude, ctx.longitude,
        ctx.altitude_ft_msl);
    if (tracon && !tracon->freqs_khz.empty()) {
      freq = static_cast<float>(tracon->freqs_khz.front()) / 1000.0f;
      controller_label = controller_location(tracon->name);
    }
  }
  // Fallback: apt.dat airport-centric departure / approach frequency.
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
    } else if (app_freq >= 100.0f) {
      std::string raw = ctx.airport_freqs.first_name(FT::APPROACH);
      controller_label = raw.empty() ? (fallback + " Approach")
                                     : controller_location(raw) + " Approach";
      if (!raw.empty() && (raw.find("RADAR") != std::string::npos ||
                           raw.find("CONTROL") != std::string::npos ||
                           raw.find("CTL") != std::string::npos))
        controller_label = controller_location(raw);
      freq = app_freq;
    }
  }

  // If neither atc.dat nor apt.dat found the departure controller, fall back
  // to the label stored when the takeoff clearance was built (ground phase).
  // This covers the common case where the nearest airport has changed en-route
  // and no longer has an APPROACH frequency in its apt.dat entry.
  if (controller_label.empty() && !s_pending_departure_label.empty())
    controller_label = s_pending_departure_label;

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

    bool exited_tma = false;
    if (openair_db::ready()) {
      auto enc = openair_db::find_enclosing(
          ctx.latitude, ctx.longitude, static_cast<int>(ctx.altitude_ft_msl));
      exited_tma = (enc.ac_class != openair_db::AirspaceClass::CTR &&
                    enc.ac_class != openair_db::AirspaceClass::TMA);
    } else {
      // Fallback: configured altitude or cruise - 2000 ft, min step1 + 1000.
      int handoff_ft =
          defaults.radar_handoff_alt_ft > 0
              ? defaults.radar_handoff_alt_ft
              : (ctx.ifr_cruise_alt_ft > 2000 ? ctx.ifr_cruise_alt_ft - 2000
                                              : 14000);
      handoff_ft = std::max(handoff_ft, s_sid_step1_alt_ft + 1000);
      exited_tma = static_cast<int>(ctx.altitude_ft_msl) >= handoff_ft;
    }

    if (exited_tma) {
      s_sid_radar_handoff_issued = true;
      // Seed en-route altitude monitoring from the last ATC-issued clearance.
      // If cruise clearance (phase 2) already fired, s_enroute_cleared_alt_ft
      // is already correct. Otherwise carry over step1 altitude so the fallback
      // (which would use SimBrief cruise FL, never an actual ATC clearance)
      // doesn't fire with the wrong value.
      if (s_enroute_cleared_alt_ft == 0 && s_sid_step1_alt_ft > 0)
        s_enroute_cleared_alt_ft = s_sid_step1_alt_ft;
      atc_state_machine::set_state(AS::IFR_ENROUTE_CRUISE);

      // Look up Centre controller — always, so the label survives whether
      // out_text is null or not (used by atc_ui transcript prefix).
      std::string centre_label;
      float centre_freq = 0.0f;
      const airspace_db::Controller *ctr = airspace_db::find_by_role_near(
          airspace_db::ControllerRole::CTR, ctx.latitude, ctx.longitude,
          ctx.altitude_ft_msl);
      if (ctr && !ctr->freqs_khz.empty()) {
        centre_label = controller_location(ctr->name);
        centre_freq = static_cast<float>(ctr->freqs_khz.front()) / 1000.0f;
        s_enroute_sector_freq_khz =
            ctr->freqs_khz.front(); // baseline for sector monitoring
      }
      if (centre_label.empty())
        centre_label = "Area Control";
      s_current_controller_label = centre_label;
      s_pending_handoff_freq_mhz = centre_freq;

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
      logging::info(
          "IFR SID climb: radar handoff at %.0f ft MSL (exited TMA/CTR)",
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
  // Use CIFP STAR entry fix altitude when it is an exact or at-or-above
  // constraint (not a ceiling).  At-or-below ceilings (like ABDIL ≤ FL190)
  // are upper bounds, not descent targets — fall back to star_entry_alt_ft.
  int star_alt_ft = defaults.star_entry_alt_ft;
  std::string star_name;
  std::string dest_runway;

  if (!ofp.navlog.empty() && !ctx.cifp_dir.empty() &&
      !ofp.destination_icao.empty()) {
    std::string star_entry_ident;
    for (const auto &fix : ofp.navlog) {
      if (fix.is_sid_star && !fix.ident.empty()) {
        star_entry_ident = fix.ident;
        break;
      }
    }
    if (!star_entry_ident.empty()) {
      star_name = cifp_reader::star_name_for_entry_fix(
          ctx.cifp_dir, ofp.destination_icao, "", star_entry_ident);
      if (!star_name.empty()) {
        auto entry = cifp_reader::star_entry_fix(
            ctx.cifp_dir, ofp.destination_icao, star_name);
        int cruise_ref = s_enroute_cleared_alt_ft > 0 ? s_enroute_cleared_alt_ft
                                                       : ctx.ifr_cruise_alt_ft;
        if (entry.alt.feet > 0 && !entry.is_ceiling && entry.alt.feet < cruise_ref)
          star_alt_ft = entry.alt.feet;
        dest_runway =
            cifp_reader::runway_for_star(ctx.cifp_dir, ofp.destination_icao, star_name);
      }
    }
  }

  // ── 2. Expected approach type ─────────────────────────────────────────
  std::string approach_phrase;
  if (!dest_runway.empty() && !ctx.cifp_dir.empty() &&
      !ofp.destination_icao.empty()) {
    auto appr = cifp_reader::best_approach(ctx.cifp_dir, ofp.destination_icao,
                                           dest_runway, ctx.visibility_m);
    if (!appr.type_str.empty())
      approach_phrase = ", expect " + appr.type_str + " approach runway " + appr.runway;
  }

  // ── 3. STAR phrase ────────────────────────────────────────────────────
  std::string star_phrase;
  if (!star_name.empty())
    star_phrase = ", expect " + star_name + " arrival";

  // ── 4. Commit ─────────────────────────────────────────────────────────
  s_enroute_descent_issued = true;

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
static bool build_approach_handoff(const xplane_context::XPlaneContext &ctx,
                                   const std::string &callsign,
                                   std::string *out_text) {
  using AS = atc_state_machine::ATCState;
  if (s_enroute_approach_handoff_issued)
    return false;

  auto ofp = simbrief_ofp::get();

  double lookup_lat = ctx.latitude;
  double lookup_lon = ctx.longitude;
  if (!ofp.navlog.empty()) {
    lookup_lat = ofp.navlog.back().lat;
    lookup_lon = ofp.navlog.back().lon;
  }

  std::string app_label;
  float app_freq = 0.0f;
  const airspace_db::Controller *tracon = airspace_db::find_by_role_near(
      airspace_db::ControllerRole::TRACON, lookup_lat, lookup_lon, 8000.0f);
  if (tracon && !tracon->freqs_khz.empty()) {
    app_freq  = static_cast<float>(tracon->freqs_khz.front()) / 1000.0f;
    app_label = controller_location(tracon->name) + " Approach";
  }
  if (app_label.empty())
    app_label = ofp.destination_icao.empty() ? "Approach"
                                              : ofp.destination_icao + " Approach";

  s_enroute_approach_handoff_issued = true;
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
    s_enroute_deviation_cooldown_sec = 0.0f;
    s_enroute_sector_freq_khz = 0;
    s_enroute_sector_check_sec = 0.0f;
    s_enroute_cleared_alt_ft = 0;
    s_enroute_alt_warn_cooldown = 0.0f;
    return false;
  }

  auto phase = flight_phase::get();
  if (phase == FP::PARKED || phase == FP::TAXI)
    return false; // auto_correction handles ground reset

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
      s_enroute_sector_check_sec = 30.0f; // re-arm

      // Find all sectors enclosing the aircraft and pick the most specific
      // CTR-role one (highest floor = narrowest sector at this altitude).
      auto enclosing = airspace_db::find_enclosing(ctx.latitude, ctx.longitude,
                                                   ctx.altitude_ft_msl);
      const airspace_db::Controller *best = nullptr;
      for (const auto *c : enclosing) {
        if (c->role != airspace_db::ControllerRole::CTR)
          continue;
        if (c->freqs_khz.empty())
          continue;
        if (!best || c->floor_ft > best->floor_ft)
          best = c;
      }

      if (best) {
        uint32_t new_freq_khz = best->freqs_khz.front();
        // Initialise on first check (s_enroute_sector_freq_khz may be 0 if
        // the TMA-exit lookup missed the sector or returned a different entry).
        if (s_enroute_sector_freq_khz == 0) {
          s_enroute_sector_freq_khz = new_freq_khz;
          s_current_controller_label = controller_location(best->name);
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

    // Compute distance and TOD threshold.
    double dist_nm = 1e9;
    float tod_nm = 30.0f;
    {
      auto ofp = simbrief_ofp::get();
      if (ofp.valid && !ofp.navlog.empty()) {
        const auto &dest_fix = ofp.navlog.back();
        dist_nm = traffic_geometry::distance_nm(
            ctx.latitude, ctx.longitude, dest_fix.lat, dest_fix.lon);
        int cruise_ref = s_enroute_cleared_alt_ft > 0 ? s_enroute_cleared_alt_ft
                                                       : ctx.ifr_cruise_alt_ft;
        tod_nm = static_cast<float>(cruise_ref - defaults.star_entry_alt_ft) / 300.0f + 20.0f;
        if (tod_nm < 30.0f)
          tod_nm = 30.0f;
      }
    }

    // Pre-TOD prompt: 15 NM before computed TOD, issued once.
    if (!s_enroute_descent_prompt_issued &&
        dist_nm <= static_cast<double>(tod_nm + 15.0f)) {
      s_enroute_descent_prompt_issued = true;
      if (out_text) {
        char buf[120];
        std::snprintf(buf, sizeof(buf), "%s, advise when ready to descend.",
                      callsign.c_str());
        *out_text = buf;
      }
      logging::info("IFR en-route: pre-TOD prompt (%.1f NM to dest, tod=%.0f NM)",
                    dist_nm, tod_nm);
      return true;
    }

    // Actual TOD reached without pilot response → issue clearance directly.
    if (s_enroute_descent_prompt_issued && dist_nm <= static_cast<double>(tod_nm)) {
      logging::info("IFR en-route: TOD reached (%.1f NM), pilot did not respond to prompt",
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
  // Fallback: ≤30 NM to destination when openair_db has no TMA coverage.
  if (s_enroute_descent_issued && !s_enroute_approach_handoff_issued) {
    bool fire_handoff = false;
    if (openair_db::ready()) {
      auto enc = openair_db::find_enclosing(
          ctx.latitude, ctx.longitude, static_cast<int>(ctx.altitude_ft_msl));
      if (enc.ac_class == openair_db::AirspaceClass::TMA ||
          enc.ac_class == openair_db::AirspaceClass::CTR) {
        fire_handoff = true;
        logging::info("IFR en-route: TMA/CTR entry -- handoff to Approach");
      }
    }
    if (!fire_handoff) {
      // No openair_db coverage: fall back to distance threshold.
      auto ofp = simbrief_ofp::get();
      if (ofp.valid && !ofp.navlog.empty()) {
        double dist_nm = traffic_geometry::distance_nm(
            ctx.latitude, ctx.longitude,
            ofp.navlog.back().lat, ofp.navlog.back().lon);
        if (dist_nm <= 30.0) {
          fire_handoff = true;
          logging::info("IFR en-route: 30 NM fallback (%.1f NM, no TMA data) -- handoff to Approach",
                        dist_nm);
        }
      }
    }
    if (fire_handoff) {
      if (build_approach_handoff(ctx, callsign, out_text))
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

void set_pending_handoff_freq(float mhz) {
  if (mhz >= 100.0f)
    s_pending_handoff_freq_mhz = mhz;
}

float pending_handoff_freq() { return s_pending_handoff_freq_mhz; }

} // namespace engine
