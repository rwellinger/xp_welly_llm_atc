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
static float       s_departure_handoff_timer  = 0.0f;
static std::string s_current_controller_label;  // last handoff target (for transcript)
constexpr float kDepartureHandoffDelaySec = 10.0f;

// IFR en-route management (IFR_ENROUTE_CRUISE state).
static float  s_enroute_timer            = 0.0f;  // accumulates while in IFR_ENROUTE_CRUISE
static bool   s_enroute_direct_issued    = false;
static float  s_enroute_direct_delay_sec = 0.0f;  // pseudo-random 90-120 s, set on first entry
static bool   s_enroute_descent_issued   = false;
static float  s_enroute_deviation_cooldown_sec = 0.0f; // countdown between deviation warnings
// Last airspace class seen while in IFR_ENROUTE_CRUISE.
// Transitions CTA/FIR/UIR → TMA trigger the approach descent clearance.
static openair_db::AirspaceClass s_enroute_last_ac_class = openair_db::AirspaceClass::OTHER;
static bool   s_enroute_was_in_enroute_airspace = false; // true after first non-TMA position

// IFR SID climb management (IFR_RADAR_CONTACT state).
static bool  s_sid_direct_issued        = false;
static bool  s_sid_step1_issued         = false;
static bool  s_sid_cruise_issued        = false;
static bool  s_sid_radar_handoff_issued = false;
static float s_sid_climb_timer          = 0.0f;
static int   s_sid_step1_alt_ft         = 0;   // computed once on first entry
static float s_sid_direct_delay_sec     = 0.0f; // random 20-40 s, set on first entry

void reset() {
  profanity_warnings_ = 0;
  lm_inferences_ = 0;
  unclear_streak_ = 0;
  advisory_history_ = traffic_advisor::AdvisoryHistory{};
  last_go_around_emit_secs_ = -1e9;
  s_departure_handoff_timer   = 0.0f;
  s_enroute_timer             = 0.0f;
  s_enroute_direct_issued     = false;
  s_enroute_direct_delay_sec  = 0.0f;
  s_enroute_descent_issued    = false;
  s_enroute_deviation_cooldown_sec = 0.0f;
  s_enroute_last_ac_class     = openair_db::AirspaceClass::OTHER;
  s_enroute_was_in_enroute_airspace = false;
  s_sid_direct_issued         = false;
  s_sid_step1_issued          = false;
  s_sid_cruise_issued         = false;
  s_sid_radar_handoff_issued  = false;
  s_sid_climb_timer           = 0.0f;
  s_sid_step1_alt_ft          = 0;
  s_sid_direct_delay_sec      = 0.0f;
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
    const auto state    = atc_state_machine::get_state();
    const auto freq_t   = ctx.frequency_type;
    bool wrong_freq = false;

    // IFR airborne states that require APPROACH or DEPARTURE: pilot has been
    // handed off and must check in on the departure/approach frequency.
    if (state == AS::IFR_EN_ROUTE || state == AS::IFR_RADAR_CONTACT) {
      wrong_freq = (freq_t != FT::APPROACH && freq_t != FT::DEPARTURE);
    }
    // Ground/tower states: APPROACH and DEPARTURE are wrong.
    // Excluded from this guard:
    //   EN_ROUTE / APPROACH_CONTACT — VFR cross-country, unguarded (pilot may be on
    //     Tower or Approach depending on whether flight following has been established).
    //   IFR_DEPARTURE_CLEARED / IFR_FREQ_HANDOFF — IFR post-clearance; pilot may
    //     already have switched to the departure/approach frequency.
    else if (state != AS::UNICOM_ACTIVE && state != AS::IDLE &&
             state != AS::EN_ROUTE && state != AS::APPROACH_CONTACT &&
             state != AS::IFR_DEPARTURE_CLEARED && state != AS::IFR_FREQ_HANDOFF) {
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
  std::string text = atc_state_machine::render_traffic_advisory(
      {}, ctx, template_key);
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
  static const char *kSuffixes[] = {" APP", " DEP", " CTR", " GND",
                                    " TWR", " DLV", " DEL", " FSS", nullptr};
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
  bool cap = true;
  for (char &c : loc) {
    if (c == ' ') { cap = true; }
    else if (cap) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); cap = false; }
    else          { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
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
        ctx.latitude, ctx.longitude,
        static_cast<int>(ctx.altitude_ft_msl));
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
      int   ctr_msl = openair_db::ctr_ceiling_ft(ctx.airport_lat, ctx.airport_lon);
      float threshold_agl = ctr_msl > 0
          ? static_cast<float>(ctr_msl) - airport_elev_ft
          : 2500.0f;
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
    const airspace_db::Controller *tracon =
        airspace_db::find_by_role_near(airspace_db::ControllerRole::TRACON,
                                       ctx.latitude, ctx.longitude,
                                       ctx.altitude_ft_msl);
    if (tracon && !tracon->freqs_khz.empty()) {
      freq            = static_cast<float>(tracon->freqs_khz.front()) / 1000.0f;
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
      controller_label = raw.empty()
          ? (fallback + " Departure")
          : controller_location(raw) + " Departure";
      if (!raw.empty() &&
          (raw.find("RADAR") != std::string::npos ||
           raw.find("CONTROL") != std::string::npos ||
           raw.find("CTL") != std::string::npos))
        controller_label = controller_location(raw);
      freq = dep_freq;
    } else if (app_freq >= 100.0f) {
      std::string raw = ctx.airport_freqs.first_name(FT::APPROACH);
      controller_label = raw.empty()
          ? (fallback + " Approach")
          : controller_location(raw) + " Approach";
      if (!raw.empty() &&
          (raw.find("RADAR") != std::string::npos ||
           raw.find("CONTROL") != std::string::npos ||
           raw.find("CTL") != std::string::npos))
        controller_label = controller_location(raw);
      freq = app_freq;
    }
  }

  // Transition to IFR_FREQ_HANDOFF: pilot must read back the frequency before
  // advancing to IFR_EN_ROUTE. Even with no frequency we advance so the state
  // doesn't get stuck in IFR_DEPARTURE_CLEARED forever.
  s_current_controller_label = controller_label;
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

bool poll_sid_climb(const xplane_context::XPlaneContext &ctx,
                    float dt, std::string *out_text) {
  using AS = atc_state_machine::ATCState;
  using FP = flight_phase::FlightPhase;

  if (atc_state_machine::get_state() != AS::IFR_RADAR_CONTACT) {
    // Reset all flags when not in target state.
    s_sid_direct_issued        = false;
    s_sid_step1_issued         = false;
    s_sid_cruise_issued        = false;
    s_sid_radar_handoff_issued = false;
    s_sid_climb_timer          = 0.0f;
    s_sid_step1_alt_ft         = 0;
    s_sid_direct_delay_sec     = 0.0f;
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

  // One-time initialisation of step1 altitude and random delay on first entry.
  if (s_sid_direct_delay_sec < 1.0f) {
    // Pseudo-random delay 20-40 s derived from callsign hash (deterministic
    // per session so replay is consistent; no std::rand needed).
    unsigned hash = 0;
    for (char c : callsign) hash = hash * 31u + static_cast<unsigned char>(c);
    s_sid_direct_delay_sec = 20.0f + static_cast<float>(hash % 21u); // [20,40]

    // Step1 altitude = midpoint between SID minimum and cruise, rounded to FL.
    int floor_ft  = ctx.ifr_sid_min_alt_ft > 0 ? ctx.ifr_sid_min_alt_ft : 5000;
    int cruise_ft = ctx.ifr_cruise_alt_ft  > 0 ? ctx.ifr_cruise_alt_ft  : floor_ft + 8000;
    int mid_ft    = (floor_ft + cruise_ft) / 2;
    s_sid_step1_alt_ft = round_to_fl(mid_ft) * 100; // store in feet
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
          ctx.latitude, ctx.longitude,
          static_cast<int>(ctx.altitude_ft_msl));
      exited_tma = (enc.ac_class != openair_db::AirspaceClass::CTR &&
                    enc.ac_class != openair_db::AirspaceClass::TMA);
    } else {
      // Fallback: configured altitude or cruise - 2000 ft, min step1 + 1000.
      int handoff_ft = defaults.radar_handoff_alt_ft > 0
          ? defaults.radar_handoff_alt_ft
          : (ctx.ifr_cruise_alt_ft > 2000 ? ctx.ifr_cruise_alt_ft - 2000 : 14000);
      handoff_ft = std::max(handoff_ft, s_sid_step1_alt_ft + 1000);
      exited_tma = static_cast<int>(ctx.altitude_ft_msl) >= handoff_ft;
    }

    if (exited_tma) {
      s_sid_radar_handoff_issued = true;
      atc_state_machine::set_state(AS::IFR_ENROUTE_CRUISE);

      // Look up Centre controller — always, so the label survives whether
      // out_text is null or not (used by atc_ui transcript prefix).
      std::string centre_label;
      float centre_freq = 0.0f;
      const airspace_db::Controller *ctr =
          airspace_db::find_by_role_near(airspace_db::ControllerRole::CTR,
                                         ctx.latitude, ctx.longitude,
                                         ctx.altitude_ft_msl);
      if (ctr && !ctr->freqs_khz.empty()) {
        centre_label = controller_location(ctr->name);
        centre_freq  = static_cast<float>(ctr->freqs_khz.front()) / 1000.0f;
      }
      if (centre_label.empty())
        centre_label = "Area Control";
      s_current_controller_label = centre_label;

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
    int cruise_fl = round_to_fl(ctx.ifr_cruise_alt_ft > 0 ? ctx.ifr_cruise_alt_ft : s_sid_step1_alt_ft + 4000);
    bool near_step1 = std::abs(static_cast<int>(ctx.altitude_ft_msl) - s_sid_step1_alt_ft) < 500;
    bool timeout    = s_sid_climb_timer > (s_sid_direct_delay_sec + 40.0f);
    if (near_step1 || timeout) {
      s_sid_cruise_issued = true;
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
  if (!s_sid_step1_issued && s_sid_climb_timer >= s_sid_direct_delay_sec) {
    s_sid_step1_issued  = true;
    s_sid_direct_issued = true;
    int step1_fl = round_to_fl(s_sid_step1_alt_ft);
    if (out_text) {
      const std::string &last_fix = ctx.ifr_sid_last_fix;
      char buf[128];
      if (!last_fix.empty()) {
        std::snprintf(buf, sizeof(buf), "%s, direct %s, climb flight level %d.",
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
static double cross_track_nm(double lat_p, double lon_p,
                              double lat_a, double lon_a,
                              double lat_b, double lon_b) {
  constexpr double kRnm = 3440.065; // Earth radius in NM
  double d_ap = traffic_geometry::distance_nm(lat_a, lon_a, lat_p, lon_p);
  if (d_ap < 0.001) return 0.0;
  double theta_ab = traffic_geometry::bearing_deg(lat_a, lon_a, lat_b, lon_b);
  double theta_ap = traffic_geometry::bearing_deg(lat_a, lon_a, lat_p, lon_p);
  double ang_diff = (theta_ap - theta_ab) * (3.14159265358979323846 / 180.0);
  double xt = std::asin(std::sin(d_ap / kRnm) * std::sin(ang_diff)) * kRnm;
  return xt;
}

// Find the minimum absolute cross-track error (NM) from the aircraft to any
// navlog leg. Returns a large value when the navlog is empty.
static double min_cross_track_nm(const xplane_context::XPlaneContext &ctx,
                                 const std::vector<simbrief_ofp::NavlogFix> &navlog) {
  if (navlog.size() < 2) return 1e9;
  double min_xt = 1e9;
  for (size_t i = 0; i + 1 < navlog.size(); ++i) {
    double xt = cross_track_nm(ctx.latitude, ctx.longitude,
                               navlog[i].lat, navlog[i].lon,
                               navlog[i + 1].lat, navlog[i + 1].lon);
    if (std::abs(xt) < std::abs(min_xt))
      min_xt = xt;
  }
  return min_xt;
}

// Pick the first non-SID/STAR navlog fix that is ahead of the aircraft
// (distance > 20 NM) for the en-route direct-to shortcut.
static std::string pick_direct_fix(const xplane_context::XPlaneContext &ctx,
                                   const std::vector<simbrief_ofp::NavlogFix> &navlog) {
  for (const auto &fix : navlog) {
    if (fix.is_sid_star) continue;
    if (fix.ident.empty()) continue;
    double dist = traffic_geometry::distance_nm(ctx.latitude, ctx.longitude,
                                                fix.lat, fix.lon);
    if (dist >= 20.0 && dist < 500.0)
      return fix.ident;
  }
  return {};
}

bool poll_enroute(const xplane_context::XPlaneContext &ctx,
                  float dt, std::string *out_text) {
  using AS = atc_state_machine::ATCState;
  using FP = flight_phase::FlightPhase;

  if (atc_state_machine::get_state() != AS::IFR_ENROUTE_CRUISE) {
    // Reset all flags when not in target state.
    s_enroute_timer                  = 0.0f;
    s_enroute_direct_issued          = false;
    s_enroute_direct_delay_sec       = 0.0f;
    s_enroute_descent_issued         = false;
    s_enroute_deviation_cooldown_sec = 0.0f;
    s_enroute_last_ac_class          = openair_db::AirspaceClass::OTHER;
    s_enroute_was_in_enroute_airspace = false;
    return false;
  }

  auto phase = flight_phase::get();
  if (phase == FP::PARKED || phase == FP::TAXI)
    return false; // auto_correction handles ground reset

  s_enroute_timer             += dt;
  s_enroute_deviation_cooldown_sec =
      std::max(0.0f, s_enroute_deviation_cooldown_sec - dt);

  const auto &defaults = flight_phase::get_ifr_defaults();
  const std::string &cs       = atc_state_machine::session_callsign();
  const std::string &callsign = cs.empty() ? settings::pilot_callsign() : cs;

  // One-time initialisation of pseudo-random direct-to delay (90-120 s).
  if (s_enroute_direct_delay_sec < 1.0f) {
    unsigned hash = 0;
    for (char c : callsign) hash = hash * 31u + static_cast<unsigned char>(c);
    s_enroute_direct_delay_sec = 90.0f + static_cast<float>(hash % 31u); // [90, 120]
  }

  // ── Sub-phase 1: en-route direct-to shortcut ─────────────────────────
  // Fires once, ~90-120 s after Centre check-in. Requires navlog with at
  // least one non-SID/STAR fix still ahead.
  if (!s_enroute_direct_issued && s_enroute_timer >= s_enroute_direct_delay_sec) {
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

  // ── Sub-phase 2: TMA entry → descent clearance + Approach handoff ─────
  // ATC proactively issues descent when the aircraft enters the destination TMA
  // (openair_db: CTA/FIR/UIR → TMA transition). Falls back to a configured
  // altitude threshold when airspace.txt is absent.
  if (!s_enroute_descent_issued) {
    bool enter_tma = false;
    if (openair_db::ready()) {
      auto enc = openair_db::find_enclosing(
          ctx.latitude, ctx.longitude,
          static_cast<int>(ctx.altitude_ft_msl));
      // Track when we've been in en-route (CTA/FIR) airspace at least once
      // so we don't immediately trigger on departure TMA exit.
      bool in_enroute_airspace = (enc.ac_class == openair_db::AirspaceClass::CTA ||
                                  enc.ac_class == openair_db::AirspaceClass::FIR ||
                                  enc.ac_class == openair_db::AirspaceClass::UIR);
      if (in_enroute_airspace)
        s_enroute_was_in_enroute_airspace = true;

      // Entering TMA while previously in en-route airspace → destination approach
      if (s_enroute_was_in_enroute_airspace &&
          enc.ac_class == openair_db::AirspaceClass::TMA &&
          s_enroute_last_ac_class != openair_db::AirspaceClass::TMA)
        enter_tma = true;

      s_enroute_last_ac_class = enc.ac_class;
    } else {
      // airspace.txt absent — TMA entry detection requires OpenAir data.
      // No fallback: avoid spurious descent clearances without airspace boundaries.
      if (!s_enroute_was_in_enroute_airspace && s_enroute_timer > 60.0f) {
        logging::info("IFR en-route: no airspace.txt, TMA entry detection disabled.");
        s_enroute_was_in_enroute_airspace = true; // suppress repeated log
      }
    }

    if (enter_tma) {
      s_enroute_descent_issued = true;

      // Look up Approach controller at the destination TMA.
      std::string app_label;
      float app_freq = 0.0f;
      const airspace_db::Controller *tracon =
          airspace_db::find_by_role_near(airspace_db::ControllerRole::TRACON,
                                         ctx.latitude, ctx.longitude,
                                         ctx.altitude_ft_msl);
      if (tracon && !tracon->freqs_khz.empty()) {
        app_freq  = static_cast<float>(tracon->freqs_khz.front()) / 1000.0f;
        app_label = controller_location(tracon->name) + " Approach";
      }
      if (app_label.empty())
        app_label = ctx.ifr_destination.empty() ? "Approach" : ctx.ifr_destination + " Approach";

      s_current_controller_label = app_label;
      atc_state_machine::set_state(AS::IFR_APPROACH_CONTACT);

      int desc_ft = defaults.approach_entry_alt_ft;
      if (out_text) {
        char buf[200];
        if (app_freq >= 100.0f)
          std::snprintf(buf, sizeof(buf),
                        "%s, descend %s, contact %s on %.3f.",
                        callsign.c_str(), format_alt(desc_ft).c_str(),
                        app_label.c_str(), app_freq);
        else
          std::snprintf(buf, sizeof(buf),
                        "%s, descend %s, contact %s.",
                        callsign.c_str(), format_alt(desc_ft).c_str(),
                        app_label.c_str());
        *out_text = buf;
      }
      logging::info("IFR en-route: TMA entry, descend %s, contact %s",
                    format_alt(desc_ft).c_str(), app_label.c_str());
      return true;
    }
  }

  // ── Sub-phase 3: cross-track deviation warning ────────────────────────
  // Fires when the aircraft is more than 5 NM off the filed route.
  // 3-minute cooldown between warnings.
  if (s_enroute_deviation_cooldown_sec <= 0.0f) {
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

} // namespace engine
