/*
 * xp_wellys_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef ENGINE_ENGINE_HPP
#define ENGINE_ENGINE_HPP

#include "atc/intent_parser.hpp"
#include "core/xplane_context.hpp"

#include <functional>
#include <string>

namespace engine {

struct Input {
  std::string transcript;
  float quality = 1.0f; // Whisper quality; 1.0 for text-only tests
  // Non-owning pointer to the current XPlaneContext. The plugin passes
  // &xplane_context::get(); the CLI will pass a scenario-built context.
  // Lifetime must cover the duration of process_transcript + any async
  // callbacks it spawns.
  const xplane_context::XPlaneContext *ctx = nullptr;
  std::string pilot_callsign;
  // Monotonic clock used by traffic_advisor cooldowns when the pilot's
  // utterance is a TRAFFIC_* acknowledgement. Plugin passes
  // XPLMGetElapsedTime; the headless CLI / tests pass a deterministic
  // counter. Defaults to 0 — fine for code paths that never enter the
  // traffic dialog.
  double now_secs = 0.0;
};

struct Output {
  // Empty = silent transition (state changed but no response to speak).
  std::string response_text;
  intent_parser::PilotMessage parsed;
  // True for radio-discipline warnings — caller uses this if it needs to
  // distinguish "ATC clearance" from "ATC correction". State is unchanged
  // when is_warning is true.
  bool is_warning = false;
};

using Done = std::function<void(Output)>;

// Reset internal counters (profanity warnings, LLM call count). Call from
// plugin init / re-enable. Separate from the per-call flow so engine has
// no "stop" phase.
void reset();

// Number of LLM inferences kicked off by the engine since last reset()
// (intent classification, sub-variant disambiguation). Callers that
// maintain an aggregate inference counter (STT + TTS + LM) add this in.
int lm_inferences();

// Count of consecutive unintelligible pilot transmissions since the
// last successful intent. Reset by reset() and by any clear pilot
// reply. Exposed for tests / instrumentation; the engine drives the
// "say again, use standard phraseology" escalation off this internally.
int unclear_streak();

// Process a pilot transcript end-to-end:
//   - quality check (low quality -> say again)
//   - rule-based intent parse
//   - INAPPROPRIATE_LANGUAGE interception (escalating warnings)
//   - departure sub-variant disambiguation via local LLM (if loaded)
//   - state machine invocation with two-stage (direct vs. LLM) routing
//
// `done` is always called exactly once. On the sync path it runs before
// process_transcript returns; on the LLM-async path it runs later on the
// thread that the LLM callback is dispatched on (main thread, via the
// plugin's callback drain).
void process_transcript(Input in, Done done);

// Per-tick traffic-advisory poll. SDK-free: takes the current
// XPlaneContext, reads traffic_context::current() for the live
// snapshot, and runs traffic_advisor::evaluate(). On a positive
// decision, renders the advisory text via
// atc_state_machine::render_traffic_advisory() and notifies
// traffic_dialog so the next pilot transcript is routed there for
// acknowledgement. Returns true iff an advisory was emitted (caller is
// responsible for routing the text to TTS / transcript display).
//
// `now_secs` is the monotonic clock the cooldown logic compares
// against. In the plugin this is XPLMGetElapsedTime; in the headless
// CLI / tests the caller passes a deterministic counter.
bool poll_traffic_advisory(const xplane_context::XPlaneContext &ctx,
                           double now_secs, std::string *out_text);

// Phase-4 unsolicited go-around trigger. Frame-driven, render-only:
//   - user is in Pattern/LANDING_CLEARED
//   - user is within 1 NM of the active-runway threshold
//   - a ground-phase target sits on the active runway centerline
//   - no go-around has been emitted in the last 60 s
//
// On a positive decision, renders the `go_around_traffic_runway`
// template via atc_state_machine::render_traffic_advisory() and
// returns true. Does NOT change ATCState — go-around is a flight
// command, not a dialog turn (no readback, no ack channel).
//
// `now_secs` is the same monotonic clock poll_traffic_advisory uses.
bool poll_go_around(const xplane_context::XPlaneContext &ctx, double now_secs,
                    std::string *out_text);

// Per-tick readback-reminder poll. When a pending readback has gone
// unanswered for the configured cadence (20 s first, 25 s for
// repeats, max 3 nudges before the clearance is cancelled), renders
// the appropriate TRAFFIC_DIALOG entry ("readback_reminder" or, on
// the final timeout, "readback_cancel") and returns true. On a
// cancel, atc_state_machine has already moved state to IDLE and
// cleared readback_pending_ before this returns. Uses the same
// monotonic clock as poll_traffic_advisory / poll_go_around.
bool poll_readback_reminder(const xplane_context::XPlaneContext &ctx,
                            double now_secs, std::string *out_text);

// IFR departure handoff: fires ~10 s into CLIMB after IFR_DEPARTURE_CLEARED.
// Tells the pilot to contact Departure (large airport) or Approach (small).
// Transitions to IFR_EN_ROUTE; returns true when the handoff fired.
// out_text is empty when neither frequency exists (silent transition).
bool poll_departure_handoff(const xplane_context::XPlaneContext &ctx,
                            float dt, std::string *out_text);

// IFR SID climb management: fires ATC-initiated step climbs and an optional
// direct-to shortcut after the pilot checks in with Departure
// (IFR_RADAR_CONTACT state). Three phases:
//   1. ~20-40 s after check-in: "direct {last_fix}, climb FL{step1}" or
//      "climb FL{step1}" if no last fix.
//   2. When aircraft is within 500 ft of step1 alt (or 40 s timeout):
//      "climb FL{cruise}".
//   3. When altitude_ft_msl >= radar_handoff_alt_ft:
//      "contact Area Control, good day" → transitions to IFR_EN_ROUTE.
// Returns true when the message was fired this frame.
bool poll_sid_climb(const xplane_context::XPlaneContext &ctx,
                    float dt, std::string *out_text);

// IFR en-route management: fires while in IFR_ENROUTE_CRUISE (on Centre).
// Three sub-functions:
//   1. ~90-120 s after Centre check-in: "direct {fix}, when able." (navlog shortcut)
//   2. When openair_db detects entry into destination TMA: Centre issues descent
//      clearance + Approach handoff → transitions to IFR_APPROACH_CONTACT.
//      Fired proactively — ATC does NOT wait for the pilot to request descent.
//   3. Cross-track deviation > 5 NM vs navlog: "confirm routing, you appear off track."
//      (3-minute cooldown between warnings)
// Returns true when a message was emitted (caller routes to TTS + transcript).
bool poll_enroute(const xplane_context::XPlaneContext &ctx,
                  float dt, std::string *out_text);

// Label of the last controller the pilot was handed off to (e.g. "Lyon",
// "Marseille"). Empty until the first IFR departure handoff fires.
// Used by atc_ui to show the correct controller name in the transcript.
const std::string &current_controller_label();

// Store a controller label without triggering a full handoff (used by
// ground_operations when the departure contact is embedded in the takeoff
// clearance — so the label is set before poll_departure_handoff() runs).
void set_controller_label(const std::string &label);

// Store the departure controller label when the takeoff clearance is built
// (ground phase). poll_departure_handoff() activates it into
// current_controller_label() so it never appears in ground-phase transcript entries.
void set_pending_departure_label(const std::string &label);
const std::string &pending_departure_label();

// Frequency the pilot was last instructed to switch to (MHz).
// Set whenever a handoff is issued (departure, TMA exit, en-route).
// Used by check_handoff_reissue() to re-state the instruction if the pilot
// calls back on the wrong frequency.
void  set_pending_handoff_freq(float mhz);
float pending_handoff_freq();

} // namespace engine

#endif
