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

#include "atc/atc_state_machine.hpp"
#include "atc/atc_templates.hpp"
#include "atc/flight_phase.hpp"
#include "atc/flows/crosscountry_flow.hpp"
#include "atc/flows/ground_operations.hpp"
#include "atc/flows/pattern_flow.hpp"
#include "atc/flows/state_storage.hpp"
#include "atc/traffic_dialog.hpp"
#include "core/logging.hpp"
#include "data/traffic_context.hpp"
#include "data/traffic_geometry.hpp"
#include "atc/readback_verifier.hpp"
#include "persistence/settings.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <deque>
#include <set>
#include <thread>
#include <unordered_map>

namespace atc_state_machine {

// ──────────────────────────────────────────────────────────────────────
//   ATC MACHINE STATE — SINGLE OWNERSHIP, GEN-COUNTER DISCIPLINE
// ──────────────────────────────────────────────────────────────────────
//
// All semantic state lives in `g_state` below. This struct is
// file-local (anonymous namespace, no header export) so external TUs
// must go through the atc_state_machine::internal::* bridge — direct
// field writes from outside this TU are compile-errors by design.
//
// Snapshot/Restore semantics (consumed by atc_session's TTS revert
// guard): a snapshot copies the WHOLE struct; restore overwrites the
// whole struct AND bumps `gen` once. This keeps `gen` strictly
// monotonic forever, so any in-flight callback holding an old
// `expected_gen` is invalidated the instant a restore happens.
//
// gen-bump rule:
//   * EVERY function that mutates a SEMANTIC field MUST call
//     bump_gen() on entry (or at the end if it may early-return without
//     mutating). Semantic fields:
//       state_, history_, readback_pending_, assigned_runway_,
//       session_callsign_, departure_type_, last_clearance_text_,
//       last_tower_response_text_, was_airborne_
//   * Heartbeat-only fields DO NOT bump gen — they tick every frame
//     and would render the counter useless for the revert guard:
//       last_now_secs_, active_correction_key_, correction_timer_
//   * Lifecycle resets (init/stop/reset) bump gen exactly once even
//     though they clear many fields at once.
//
// Adding a new field? Decide: semantic or heartbeat. If semantic, every
// new mutator-call-site MUST bump_gen(). Add a Catch2 monotonicity test
// in tests/test_state_revert_guard.cpp to catch silent omissions.
//
// Threading: every mutator must run on the X-Plane flight-loop thread.
// `g_flight_loop_thread_id` is captured by init() and verified via
// `assert_flight_loop_thread()` in the snapshot/restore API. Worker
// threads (curl, llama, whisper) marshal their results through
// backends::manager::enqueue_callback() which drains in the flight
// loop — see manager.cpp::drain_callback_queue() / main.cpp:101.

namespace {

constexpr size_t kHistoryCap = 8;

// Readback-reminder cadence (problem #3). First reminder fires 20 s
// after the readback became due; subsequent ones every 25 s. After
// In real ICAO phraseology the controller gives the clearance and waits —
// they never say "readback please" proactively. The reminder ticks silently
// (for internal timeout tracking); only the final cancel is spoken.
// 3 × 45 s ≈ 2 min 15 s of silence before the clearance is cancelled.
constexpr double kReadbackFirstReminderSec = 45.0;
constexpr double kReadbackRepeatReminderSec = 45.0;
constexpr int kReadbackMaxReminders = 3;

struct AtcMachineState {
  // Monotonic generation counter. Bumped by every semantic mutation
  // (see rule above). Never decreases — restore() bumps it again
  // after copying the snapshot back, so a stale snapshot can't
  // resurrect an old gen value.
  uint64_t gen = 0;

  ATCState state_ = ATCState::IDLE;
  bool readback_pending_ = false;
  // Reminder cadence for forgotten readbacks. Set the moment
  // readback_pending_ flips from false → true, cleared together with
  // it. consume_readback_reminder() reads + mutates these to schedule
  // "Tower, say again your readback" calls when the pilot goes
  // silent. Counter caps reminders at kReadbackMaxReminders; after
  // that the clearance is cancelled by reverting to IDLE.
  double readback_pending_since_secs_ = 0.0;
  double readback_last_reminder_secs_ = 0.0;
  int readback_reminder_count_ = 0;

  // Session-lifecycle flag: true once the aircraft has been airborne at
  // any point since the last init/stop/reset or the last new-departure-
  // cycle reset. Survives Touch-and-Go (no reset on landing) and is
  // explicitly cleared when a new departure cycle starts on the ground
  // (REQUEST_TAXI / INITIAL_CALL_GROUND/TOWER / generic INITIAL_CALL
  // processed while ctx.on_ground=true). INITIAL_CALL_INBOUND does NOT
  // reset — an inbound call is the opposite of a departure cycle.
  //
  // Drives the RUNWAY_VACATED-impossibility veto: vacated is physically
  // impossible without a prior airborne phase, so a "runway vacated"
  // heard before the first takeoff is interpreted as a malformed
  // clearance readback (when readback_pending) or routed through the LM
  // repair path (otherwise). See src/atc/atc_state_machine.hpp's
  // session-lifecycle table for the documented set/reset matrix.
  bool was_airborne_ = false;

  std::string assigned_runway_; // locked once ATC assigns a runway

  // Pilot callsign locked at the first non-IDLE transition with a
  // non-empty msg.callsign. Holds for the remainder of the session so
  // a later mid-session utterance with a garbled callsign (STT
  // mishears "HB-DSV" as "Delta") cannot make the tower address the
  // pilot by the fragment. Cleared on every IDLE return alongside
  // assigned_runway_ — same lifecycle, same release points.
  std::string session_callsign_;

  // Most recent tower clearance text that demanded a readback. Set by
  // apply_post_transition_hooks() whenever resp.requires_readback is
  // true, cleared on init/stop/reset/airport-change and whenever the
  // readback expectation resolves. Surfaced in the UI clearance display.
  std::string last_clearance_text_;

  // Fields that have already been correctly read back in a prior turn of the
  // current readback exchange. Prevents the "loop" where the pilot reads back
  // altitude in one turn and squawk in the next, but ATC keeps cycling because
  // each turn's transcript only contains one item. Cleared alongside
  // last_clearance_text_ (i.e. when the readback succeeds or is cancelled).
  std::set<std::string> readback_ok_fields_;

  // Most recent NON-corrective tower utterance — used by
  // REQUEST_REPEAT to replay "the last real clearance". That is what the
  // pilot wants when forgetting the QNH and asking the tower to repeat —
  // they need the original numbers.
  std::string last_tower_response_text_;

  internal::DepartureType departure_type_ = internal::DepartureType::PATTERN;

  // Squawk code assigned during IFR pre-departure clearance.
  // Empty until first IFR clearance is issued this session.
  std::string ifr_squawk_;

  // Bounded chronological log of state transitions. Front = oldest,
  // back = most recent past state. Filled by transition_to(); read
  // by downstream consumers (LM-classify prompt, hint filter, intent
  // rules) to disambiguate situations where the current state alone
  // is insufficient (e.g. post-landing IDLE is not the same as
  // cold-start IDLE). Cap chosen for typical inbound sequence
  // EN_ROUTE -> PATTERN_ENTRY -> LANDING_CLEARED -> IDLE ->
  // GROUND_CONTACT -> TAXI_CLEARED -> IDLE plus a couple of
  // disregards/auto-corrections.
  std::deque<StateHistoryEntry> history_;

  // ── Heartbeat fields (DO NOT bump gen) ────────────────────────────
  // Last timestamp seen by a public mutating entry point (process,
  // disregard, check_auto_correction, check_airport_change). Internal
  // helpers calling transition_to() inside one of those flows pick
  // this up so every History entry has a sensible timestamp without
  // having to thread the value through every helper signature.
  double last_now_secs_ = 0.0;
  std::string active_correction_key_;
  float correction_timer_ = 0.0f;
};

static AtcMachineState g_state;

// Flight-loop thread fingerprint. Captured by init(), checked by
// assert_flight_loop_thread() on snapshot/restore. A null-id (no
// init() called yet — only happens in tests or pre-XPluginStart) is
// accepted to keep unit tests trivial.
static std::atomic<std::thread::id> g_flight_loop_thread_id{};

inline void bump_gen() { ++g_state.gen; }

inline void assert_flight_loop_thread() {
  auto expected = g_flight_loop_thread_id.load();
  if (expected == std::thread::id{})
    return; // not yet captured — pre-init / test context
  assert(std::this_thread::get_id() == expected &&
         "atc_state_machine mutated off the flight-loop thread");
}

} // namespace

// ── State name <-> string ───────────────────────────────────────────
//
// Step 3b of the A1 flow-split refactor qualifies state names with a
// flow prefix:
//   - GroundOps states (IDLE, GROUND_CONTACT, TAXI_CLEARED, TOWER_CONTACT,
//     UNICOM_ACTIVE) keep their raw name.
//   - Pattern states (PATTERN_ENTRY, LANDING_CLEARED, TOUCH_AND_GO_CLEARED)
//     gain the "Pattern/" prefix.
//   - CrossCountry states (EN_ROUTE, APPROACH_CONTACT) gain the "XC/" prefix.
//   - DEPARTURE_CLEARED is the only state that can sit in either flow.
//     Its qualified name is selected from departure_type_ — Pattern when
//     READY_FOR_DEPARTURE was the trigger, CrossCountry when
//     READY_FOR_DEPARTURE_VFR was the trigger.
//
// state_from_name() accepts both qualified ("Pattern/PATTERN_ENTRY") and
// raw ("PATTERN_ENTRY") forms during the migration window so external
// callers (scenario JSONs, log scrapers) don't have to flip in lock-step
// with the C++ change.

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
    // Pattern/XC discrimination via the legacy departure_type_ flag —
    // disappears in step 4 when each flow owns its own state enum.
    return g_state.departure_type_ == internal::DepartureType::CROSS_COUNTRY
               ? "XC/DEPARTURE_CLEARED"
               : "Pattern/DEPARTURE_CLEARED";
  case ATCState::PATTERN_ENTRY:
    return "Pattern/PATTERN_ENTRY";
  case ATCState::LANDING_CLEARED:
    return "Pattern/LANDING_CLEARED";
  case ATCState::TOUCH_AND_GO_CLEARED:
    return "Pattern/TOUCH_AND_GO_CLEARED";
  case ATCState::UNICOM_ACTIVE:
    return "UNICOM_ACTIVE";
  case ATCState::EN_ROUTE:
    return "XC/EN_ROUTE";
  case ATCState::APPROACH_CONTACT:
    return "XC/APPROACH_CONTACT";
  case ATCState::IFR_PREDEP_CLEARANCE:
    return "IFR/PREDEP_CLEARANCE";
  case ATCState::IFR_CLEARED:
    return "IFR/CLEARED";
  case ATCState::IFR_LINE_UP_AND_WAIT:
    return "IFR/LINE_UP_AND_WAIT";
  case ATCState::IFR_DEPARTURE_CLEARED:
    return "IFR/DEPARTURE_CLEARED";
  case ATCState::IFR_FREQ_HANDOFF:
    return "IFR/FREQ_HANDOFF";
  case ATCState::IFR_EN_ROUTE:
    return "IFR/EN_ROUTE";
  case ATCState::IFR_RADAR_CONTACT:
    return "IFR/RADAR_CONTACT";
  case ATCState::IFR_ENROUTE_CRUISE:
    return "IFR/ENROUTE_CRUISE";
  case ATCState::IFR_APPROACH_CONTACT:
    return "IFR/APPROACH_CONTACT";
  case ATCState::IFR_APPROACH_DESCENT:
    return "IFR/APPROACH_DESCENT";
  case ATCState::IFR_APPROACH_TOWER:
    return "IFR/APPROACH_TOWER";
  }
  return "UNKNOWN";
}

ATCState state_from_name(const std::string &name) {
  // Map both qualified ("Pattern/PATTERN_ENTRY") and raw ("PATTERN_ENTRY")
  // forms. The qualified form is the post-step-3b canonical; raw is kept
  // for backwards compatibility with set_state() callers and any log
  // scrapers that pre-date the rename.
  static const std::unordered_map<std::string, ATCState> kMap = {
      {"IDLE", ATCState::IDLE},
      {"GROUND_CONTACT", ATCState::GROUND_CONTACT},
      {"TAXI_CLEARED", ATCState::TAXI_CLEARED},
      {"TOWER_CONTACT", ATCState::TOWER_CONTACT},
      {"UNICOM_ACTIVE", ATCState::UNICOM_ACTIVE},
      {"DEPARTURE_CLEARED", ATCState::DEPARTURE_CLEARED},
      {"Pattern/DEPARTURE_CLEARED", ATCState::DEPARTURE_CLEARED},
      {"XC/DEPARTURE_CLEARED", ATCState::DEPARTURE_CLEARED},
      {"PATTERN_ENTRY", ATCState::PATTERN_ENTRY},
      {"Pattern/PATTERN_ENTRY", ATCState::PATTERN_ENTRY},
      {"LANDING_CLEARED", ATCState::LANDING_CLEARED},
      {"Pattern/LANDING_CLEARED", ATCState::LANDING_CLEARED},
      {"TOUCH_AND_GO_CLEARED", ATCState::TOUCH_AND_GO_CLEARED},
      {"Pattern/TOUCH_AND_GO_CLEARED", ATCState::TOUCH_AND_GO_CLEARED},
      {"EN_ROUTE", ATCState::EN_ROUTE},
      {"XC/EN_ROUTE", ATCState::EN_ROUTE},
      {"APPROACH_CONTACT", ATCState::APPROACH_CONTACT},
      {"XC/APPROACH_CONTACT", ATCState::APPROACH_CONTACT},
      {"IFR/PREDEP_CLEARANCE", ATCState::IFR_PREDEP_CLEARANCE},
      {"IFR_PREDEP_CLEARANCE", ATCState::IFR_PREDEP_CLEARANCE},
      {"IFR/CLEARED", ATCState::IFR_CLEARED},
      {"IFR_CLEARED", ATCState::IFR_CLEARED},
      {"IFR/LINE_UP_AND_WAIT", ATCState::IFR_LINE_UP_AND_WAIT},
      {"IFR_LINE_UP_AND_WAIT", ATCState::IFR_LINE_UP_AND_WAIT},
      {"IFR/DEPARTURE_CLEARED", ATCState::IFR_DEPARTURE_CLEARED},
      {"IFR_DEPARTURE_CLEARED", ATCState::IFR_DEPARTURE_CLEARED},
      {"IFR/FREQ_HANDOFF", ATCState::IFR_FREQ_HANDOFF},
      {"IFR_FREQ_HANDOFF", ATCState::IFR_FREQ_HANDOFF},
      {"IFR/EN_ROUTE", ATCState::IFR_EN_ROUTE},
      {"IFR_EN_ROUTE", ATCState::IFR_EN_ROUTE},
      {"IFR/RADAR_CONTACT", ATCState::IFR_RADAR_CONTACT},
      {"IFR_RADAR_CONTACT", ATCState::IFR_RADAR_CONTACT},
      {"IFR/ENROUTE_CRUISE", ATCState::IFR_ENROUTE_CRUISE},
      {"IFR_ENROUTE_CRUISE", ATCState::IFR_ENROUTE_CRUISE},
      {"IFR/APPROACH_CONTACT", ATCState::IFR_APPROACH_CONTACT},
      {"IFR_APPROACH_CONTACT", ATCState::IFR_APPROACH_CONTACT},
      {"IFR/APPROACH_DESCENT", ATCState::IFR_APPROACH_DESCENT},
      {"IFR_APPROACH_DESCENT", ATCState::IFR_APPROACH_DESCENT},
      {"IFR/APPROACH_TOWER", ATCState::IFR_APPROACH_TOWER},
      {"IFR_APPROACH_TOWER", ATCState::IFR_APPROACH_TOWER},
  };
  auto it = kMap.find(name);
  return it != kMap.end() ? it->second : ATCState::IDLE;
}

// ── internal:: bridge implementation (private API for ground_ops) ───

namespace internal {

const char *departure_type_name(DepartureType t) {
  return t == DepartureType::CROSS_COUNTRY ? "CROSS_COUNTRY" : "PATTERN";
}

ATCState get_state_ref() { return g_state.state_; }
bool readback_pending() { return g_state.readback_pending_; }
const std::string &assigned_runway_ref() { return g_state.assigned_runway_; }
const std::string &session_callsign_ref() { return g_state.session_callsign_; }
DepartureType departure_type() { return g_state.departure_type_; }

// Single source of truth for state mutations. Pushes the previous
// state into history_ (capped at kHistoryCap, oldest dropped first),
// switches state_ to next, and emits a uniform log line. No-op when
// next == state_ (avoids polluting history with self-transitions).
void transition_to(ATCState next, const char *reason) {
  if (next == g_state.state_)
    return;
  assert_flight_loop_thread();
  bump_gen();
  g_state.history_.push_back(StateHistoryEntry{
      g_state.state_, reason ? reason : "", g_state.last_now_secs_});
  while (g_state.history_.size() > kHistoryCap)
    g_state.history_.pop_front();
  logging::info("ATC state: %s -> %s (%s)", state_name(g_state.state_),
                state_name(next), reason ? reason : "");
  g_state.state_ = next;
}

void set_readback_pending(bool v) {
  assert_flight_loop_thread();
  bump_gen();
  g_state.readback_pending_ = v;
}

void set_was_airborne(bool v) {
  // Idempotent — flight_phase::update() calls this every frame the
  // aircraft is airborne, so a no-op write must not bump the
  // revert-guard gen counter (would invalidate every in-flight
  // snapshot).
  if (g_state.was_airborne_ == v)
    return;
  assert_flight_loop_thread();
  bump_gen();
  g_state.was_airborne_ = v;
}
void set_assigned_runway(const std::string &rwy) {
  assert_flight_loop_thread();
  bump_gen();
  g_state.assigned_runway_ = rwy;
}
void clear_assigned_runway() {
  assert_flight_loop_thread();
  bump_gen();
  g_state.assigned_runway_.clear();
}
void set_session_callsign(const std::string &cs) {
  assert_flight_loop_thread();
  bump_gen();
  g_state.session_callsign_ = cs;
}
void clear_session_callsign() {
  assert_flight_loop_thread();
  bump_gen();
  g_state.session_callsign_.clear();
}
void set_departure_type(DepartureType t) {
  assert_flight_loop_thread();
  bump_gen();
  g_state.departure_type_ = t;
}
// Heartbeat — does NOT bump gen (every frame would invalidate the
// revert guard's expected_gen).
void set_last_now_secs(double t) { g_state.last_now_secs_ = t; }
double last_now_secs() { return g_state.last_now_secs_; }

const std::string &ifr_squawk_ref() { return g_state.ifr_squawk_; }
void set_ifr_squawk(const std::string &sq) {
  assert_flight_loop_thread();
  bump_gen();
  g_state.ifr_squawk_ = sq;
}
void clear_ifr_squawk() {
  assert_flight_loop_thread();
  bump_gen();
  g_state.ifr_squawk_.clear();
}

} // namespace internal

void arm_readback(const std::string &clearance_text) {
  assert_flight_loop_thread();
  bump_gen();
  g_state.readback_pending_            = true;
  g_state.last_clearance_text_         = clearance_text;
  g_state.readback_ok_fields_.clear();
  g_state.readback_pending_since_secs_ = g_state.last_now_secs_;
  g_state.readback_last_reminder_secs_ = g_state.last_now_secs_;
  g_state.readback_reminder_count_     = 0;
}

void cancel_readback() {
  assert_flight_loop_thread();
  if (!g_state.readback_pending_)
    return;
  bump_gen();
  g_state.readback_pending_ = false;
  g_state.last_clearance_text_.clear();
  g_state.readback_ok_fields_.clear();
  g_state.readback_pending_since_secs_ = 0.0;
  g_state.readback_last_reminder_secs_ = 0.0;
  g_state.readback_reminder_count_     = 0;
}

// ── Lifecycle ───────────────────────────────────────────────────────

void init() {
  // Capture flight-loop thread fingerprint for assert_flight_loop_thread.
  // Allowed to run multiple times — main.cpp calls init() on XPluginStart
  // and after settings reload.
  g_flight_loop_thread_id.store(std::this_thread::get_id());

  // Full reset is one semantic transition — single bump_gen() covers
  // the whole field sweep below.
  bump_gen();
  g_state.state_ = ATCState::IDLE;
  g_state.readback_pending_ = false;
  g_state.was_airborne_ = false;
  g_state.assigned_runway_.clear();
  g_state.session_callsign_.clear();
  g_state.departure_type_ = internal::DepartureType::PATTERN;
  g_state.history_.clear();
  g_state.last_now_secs_ = 0.0;
  g_state.last_clearance_text_.clear();
    g_state.readback_ok_fields_.clear();
  g_state.readback_pending_since_secs_ = 0.0;
  g_state.readback_last_reminder_secs_ = 0.0;
  g_state.readback_reminder_count_ = 0;

  // Honor the user's "where am I starting" setting. The default
  // engines_running keeps the IDLE start above; ready_for_takeoff
  // jumps straight to TOWER_CONTACT so a pilot spawning on the
  // runway sees READY_FOR_DEPARTURE hints immediately.
  const std::string mode = settings::start_mode();
  if (mode == "ready_for_takeoff") {
    g_state.state_ = ATCState::TOWER_CONTACT;
    logging::info(
        "Initial state: TOWER_CONTACT (start_mode=ready_for_takeoff)");
  }
}

void stop() {
  bump_gen();
  g_state.state_ = ATCState::IDLE;
  g_state.readback_pending_ = false;
  g_state.was_airborne_ = false;
  g_state.assigned_runway_.clear();
  g_state.session_callsign_.clear();
  g_state.departure_type_ = internal::DepartureType::PATTERN;
  g_state.ifr_squawk_.clear();
  g_state.history_.clear();
  g_state.last_now_secs_ = 0.0;
  g_state.last_clearance_text_.clear();
    g_state.readback_ok_fields_.clear();
  g_state.last_tower_response_text_.clear();
  g_state.readback_pending_since_secs_ = 0.0;
  g_state.readback_last_reminder_secs_ = 0.0;
  g_state.readback_reminder_count_ = 0;
}

void reset() {
  assert_flight_loop_thread();
  bump_gen();
  g_state.state_ = ATCState::IDLE;
  g_state.readback_pending_ = false;
  g_state.was_airborne_ = false;
  g_state.assigned_runway_.clear();
  g_state.session_callsign_.clear();
  g_state.departure_type_ = internal::DepartureType::PATTERN;
  g_state.ifr_squawk_.clear();
  crosscountry_flow::reset();
  g_state.history_.clear();
  g_state.last_now_secs_ = 0.0;
  g_state.last_clearance_text_.clear();
    g_state.readback_ok_fields_.clear();
  g_state.last_tower_response_text_.clear();
  g_state.readback_pending_since_secs_ = 0.0;
  g_state.readback_last_reminder_secs_ = 0.0;
  g_state.readback_reminder_count_ = 0;
  logging::info("ATC state machine reset to IDLE");
}

// ── Disregard ───────────────────────────────────────────────────────

// "Disregard" radius: airborne pilots within this distance from their
// nearest airport are treated as still in the pattern flow; outside,
// they land in EN_ROUTE so an INITIAL_CALL_INBOUND can re-establish.
constexpr double kDisregardPatternRadiusNm = 5.0;

void disregard(const xplane_context::XPlaneContext &ctx,
               flight_phase::FlightPhase phase, double now_secs) {
  assert_flight_loop_thread();
  bump_gen();
  g_state.last_now_secs_ = now_secs;
  // Always clear the side-channel — a Disregard mid-advisory should
  // also drop the pending traffic ack.
  traffic_dialog::reset();
  g_state.readback_pending_ = false;
  g_state.last_clearance_text_.clear();
    g_state.readback_ok_fields_.clear();
  g_state.last_tower_response_text_.clear();

  if (!flight_phase::is_airborne(phase)) {
    internal::transition_to(ATCState::IDLE, "disregard_on_ground");
    g_state.assigned_runway_.clear();
    g_state.session_callsign_.clear();
    g_state.departure_type_ = internal::DepartureType::PATTERN;
    return;
  }

  // Airborne. Decide between PATTERN_ENTRY (still over the home
  // pattern) and EN_ROUTE (transit). The user's last airport ICAO is
  // tracked in ctx; if we don't have a position fix yet, fall back to
  // PATTERN_ENTRY since the pilot was just talking to that tower.
  bool near_airport = true;
  if (ctx.airport_lat != 0.0 || ctx.airport_lon != 0.0) {
    double d_nm = traffic_geometry::distance_nm(
        ctx.latitude, ctx.longitude, ctx.airport_lat, ctx.airport_lon);
    near_airport = d_nm <= kDisregardPatternRadiusNm;
  }

  // Keep assigned_runway_ — pilot is still in the same airspace.
  internal::transition_to(near_airport ? ATCState::PATTERN_ENTRY
                                       : ATCState::EN_ROUTE,
                          near_airport ? "disregard_airborne_near_airport"
                                       : "disregard_airborne_transit");
}

// ── Public read accessors ───────────────────────────────────────────

ATCState get_state() { return g_state.state_; }

bool is_readback_pending() { return g_state.readback_pending_; }

const std::string &last_clearance_text() {
  return g_state.last_clearance_text_;
}

std::string consume_readback_reminder(double now_secs) {
  // Gate 1: no pending readback → nothing to remind about.
  if (!g_state.readback_pending_)
    return {};

  const double elapsed_since_last =
      now_secs - g_state.readback_last_reminder_secs_;

  // Gate 2: did enough time pass since the last nudge? First reminder
  // uses a slightly shorter delay (20 s) so the pilot gets a quick
  // "Tower expects readback" cue; repeats space out (25 s each).
  const double required_delay = g_state.readback_reminder_count_ == 0
                                    ? kReadbackFirstReminderSec
                                    : kReadbackRepeatReminderSec;
  if (elapsed_since_last < required_delay)
    return {};

  // Gate 3: budget exhausted → escalate to cancellation. Tower says
  // "clearance cancelled, contact again when ready" and we revert
  // state to IDLE so the pilot can start over. The clear in
  // apply_post_transition_hooks won't fire here (we're outside the
  // process() path), so do it manually.
  //
  // Exception: IFR_ENROUTE_CRUISE — resetting to IDLE for a missed readback on
  // an en-route advisory (sector change, direct-to, altitude step) would wipe all
  // poll_enroute() statics (cleared altitude, descent flags) and permanently block
  // proactive descent clearances for the rest of the flight.  For en-route, just
  // discard the pending readback and let poll_enroute() re-issue if needed.
  if (g_state.readback_reminder_count_ >= kReadbackMaxReminders) {
    const char *cur = state_name(g_state.state_);
    const bool is_enroute_cruise =
        std::strcmp(cur, "IFR/ENROUTE_CRUISE") == 0;
    bump_gen();
    g_state.readback_pending_ = false;
    g_state.last_clearance_text_.clear();
    g_state.readback_ok_fields_.clear();
    g_state.readback_pending_since_secs_ = 0.0;
    g_state.readback_last_reminder_secs_ = 0.0;
    g_state.readback_reminder_count_ = 0;
    if (is_enroute_cruise) {
      // Stay in ENROUTE_CRUISE — the pilot will respond when ready or the
      // proactive clearance will be re-issued.
      logging::info("Readback timeout in ENROUTE_CRUISE — discarded (no state reset)");
      return {};
    }
    // IFR non-cruise states get a neutral "say again". VFR uses "clearance cancelled".
    const bool is_ifr = (cur[0] == 'I' && cur[1] == 'F' && cur[2] == 'R' && cur[3] == '/');
    internal::transition_to(ATCState::IDLE, "readback_timeout_cancel");
    logging::info("Readback timeout — clearance cancelled");
    return is_ifr ? "readback_cancel_ifr" : "readback_cancel";
  }

  // Gate 4: tick the internal counter silently. In real ATC phraseology the
  // controller does not say "readback please" — they simply wait. The counter
  // advancing here drives the cancel timeout (Gate 3 on the next fire).
  bump_gen();
  ++g_state.readback_reminder_count_;
  g_state.readback_last_reminder_secs_ = now_secs;
  logging::info("Readback: no response after %.0f s (tick %d/%d, waiting silently)",
                elapsed_since_last,
                g_state.readback_reminder_count_, kReadbackMaxReminders);
  return {};
}

bool was_airborne() { return g_state.was_airborne_; }

void set_was_airborne(bool v) { internal::set_was_airborne(v); }

void set_state(ATCState state) {
  internal::transition_to(state, "external_set_state");
  if (state == ATCState::IDLE) {
    if (!g_state.assigned_runway_.empty()) {
      logging::info("Runway lock released");
      bump_gen();
      g_state.assigned_runway_.clear();
    }
    if (!g_state.session_callsign_.empty()) {
      bump_gen();
      g_state.session_callsign_.clear();
    }
  }
}

const std::string &assigned_runway() { return g_state.assigned_runway_; }

void set_assigned_runway(const std::string &rwy) {
  internal::set_assigned_runway(rwy);
}

const std::string &session_callsign() { return g_state.session_callsign_; }

std::string effective_runway(const xplane_context::XPlaneContext &ctx) {
  return g_state.assigned_runway_.empty() ? ctx.active_runway
                                          : g_state.assigned_runway_;
}

// ── Post-template hooks ─────────────────────────────────────────────

// Apply the post-template hooks that mutate persistent state — runway lock,
// readback tracking, departure-type, tower-only auto-advance. Step 4 will
// split these between the per-flow modules; for now they live alongside
// process().
static void
apply_post_transition_hooks(const intent_parser::PilotMessage &msg,
                            const xplane_context::XPlaneContext &ctx,
                            ATCResponse &resp) {
  // Track readback state.
  if (msg.intent == intent_parser::PilotIntent::READBACK) {
    // Verify the readback when a clearance is pending.
    if (g_state.readback_pending_ && !g_state.last_clearance_text_.empty()) {
      // Accumulate fields the pilot got right in this turn.
      for (const auto &f : readback_verifier::matched_fields(
               g_state.last_clearance_text_, msg.raw_transcript))
        g_state.readback_ok_fields_.insert(f);

      // Filter mismatches: skip fields already verified in a prior turn.
      // This breaks the cycling loop where the pilot reads back altitude
      // in one turn and squawk in the next but never both in one go.
      auto mismatches = readback_verifier::check(g_state.last_clearance_text_,
                                                 msg.raw_transcript);
      mismatches.erase(
          std::remove_if(mismatches.begin(), mismatches.end(),
                         [](const readback_verifier::Mismatch &m) {
                           return g_state.readback_ok_fields_.count(m.field) > 0;
                         }),
          mismatches.end());

      if (!mismatches.empty()) {
        // First mismatch drives the correction. Prefix with callsign.
        const std::string &cs = g_state.session_callsign_;
        resp.text = (cs.empty() ? "" : cs + ", ") + mismatches[0].correction +
                    ".";
        resp.next_state     = g_state.state_;  // stay in current state
        resp.requires_readback = true;         // re-arm the timer
        logging::info(
            "Readback error: field=%s expected=%s stated=%s (ok_fields=%zu)",
            mismatches[0].field.c_str(), mismatches[0].expected.c_str(),
            mismatches[0].stated.empty() ? "(missing)"
                                         : mismatches[0].stated.c_str(),
            g_state.readback_ok_fields_.size());
        return;  // do NOT clear readback state
      }
    }
    bump_gen();
    g_state.readback_pending_ = false;
    g_state.last_clearance_text_.clear();
    g_state.readback_ok_fields_.clear();
    g_state.readback_pending_since_secs_ = 0.0;
    g_state.readback_last_reminder_secs_ = 0.0;
    g_state.readback_reminder_count_ = 0;
  } else if (resp.requires_readback) {
    bump_gen();
    g_state.readback_pending_ = true;
    // Snapshot the clearance text for the UI clearance display.
    g_state.last_clearance_text_ = resp.text;
    // Start the reminder timer the moment the readback becomes due —
    // last_now_secs_ is the heartbeat written by process() each frame
    // and tracks the same monotonic clock that consume_readback_reminder
    // will be polled against.
    g_state.readback_pending_since_secs_ = g_state.last_now_secs_;
    g_state.readback_last_reminder_secs_ = g_state.last_now_secs_;
    g_state.readback_reminder_count_ = 0;
  } else if (g_state.readback_pending_ && !resp.text.empty()) {
    // Tower spoke while a readback was already pending (LM-_INVALID
    // fallback, or any other non-readback reply during the readback
    // window). The pilot just heard a
    // tower utterance — give them the full reminder delay again
    // before nudging, and reset the count so a multi-attempt back-
    // and-forth doesn't burn through the 3-reminder cancellation
    // budget mid-dialog. Without this the reminder fires 20 s after
    // the tower's correction even though the pilot was actively
    // talking the whole time.
    bump_gen();
    g_state.readback_last_reminder_secs_ = g_state.last_now_secs_;
    g_state.readback_reminder_count_ = 0;
  }

  // Leaving the controller's frequency or resetting drops stale readback
  // context. Without this, "frequency change good day" after an unread-back
  // takeoff clearance keeps readback_pending armed, and the UI hint pipeline
  // silences every other hint at the next airport.
  if (resp.next_state == ATCState::EN_ROUTE ||
      resp.next_state == ATCState::IDLE) {
    bump_gen();
    g_state.readback_pending_ = false;
    g_state.last_clearance_text_.clear();
    g_state.readback_ok_fields_.clear();
    g_state.readback_pending_since_secs_ = 0.0;
    g_state.readback_last_reminder_secs_ = 0.0;
    g_state.readback_reminder_count_ = 0;
  }

  // Lock runway on first clearance that references a runway. Same fallback
  // chain as ground_ops::get_runway() (msg → assigned → active → "28"),
  // inlined here to avoid exposing get_runway() outside ground_operations.
  if (g_state.assigned_runway_.empty() && resp.next_state != ATCState::IDLE) {
    std::string rwy = !msg.runway.empty()          ? msg.runway
                      : !ctx.active_runway.empty() ? ctx.active_runway
                                                   : std::string{"28"};
    if (!rwy.empty()) {
      bump_gen();
      g_state.assigned_runway_ = rwy;
      logging::info("Runway locked: %s", rwy.c_str());
    }
  }

  // Lock pilot callsign on first transition out of IDLE. Captures
  // whatever the parser extracted from the initial-call utterance,
  // when the pilot still speaks deliberately. Held until the dialog
  // returns to IDLE so a mid-session mishear cannot overwrite it.
  if (g_state.session_callsign_.empty() && !msg.callsign.empty() &&
      resp.next_state != ATCState::IDLE) {
    bump_gen();
    g_state.session_callsign_ = msg.callsign;
    logging::info("Session callsign locked: %s", msg.callsign.c_str());
  }

  // Apply state transition if we have a response OR if the template
  // explicitly targets a different state (silent acknowledgment — e.g.
  // IFR/FREQ_HANDOFF READBACK: controller says nothing but the session
  // should advance to the next phase).
  if (!resp.text.empty() || resp.next_state != g_state.state_) {
    ATCState prev_state = g_state.state_;
    std::string reason = "process:";
    reason += intent_parser::intent_template_key(msg.intent);
    internal::transition_to(resp.next_state, reason.c_str());

    // Track departure intent: PATTERN (default) vs CROSS_COUNTRY.
    if (resp.next_state == ATCState::DEPARTURE_CLEARED &&
        prev_state != ATCState::DEPARTURE_CLEARED) {
      bump_gen();
      g_state.departure_type_ =
          (msg.intent == intent_parser::PilotIntent::READY_FOR_DEPARTURE_VFR)
              ? internal::DepartureType::CROSS_COUNTRY
              : internal::DepartureType::PATTERN;
      logging::info("Departure type: %s",
                    internal::departure_type_name(g_state.departure_type_));
    } else if (prev_state == ATCState::DEPARTURE_CLEARED &&
               resp.next_state != ATCState::DEPARTURE_CLEARED) {
      bump_gen();
      g_state.departure_type_ = internal::DepartureType::PATTERN;
    }
  }

  // Release runway lock and session callsign when session ends.
  if (resp.next_state == ATCState::IDLE) {
    if (!g_state.assigned_runway_.empty()) {
      bump_gen();
      logging::info("Runway lock released");
      g_state.assigned_runway_.clear();
    }
    if (!g_state.session_callsign_.empty()) {
      bump_gen();
      g_state.session_callsign_.clear();
    }
  }

  // Tower-only airport: skip ground→tower handoff (no freq change needed).
  // Data-driven via flight_rules.tower_only_auto_advance.
  if (ctx.tower_only) {
    std::string current = state_name(g_state.state_);
    std::string next = flight_phase::get_tower_only_auto_advance(current);
    if (!next.empty()) {
      ATCState target = state_from_name(next);
      internal::transition_to(target, "tower_only_auto_advance");
      resp.next_state = target;
    }
  }

  // Snapshot the (non-corrective) tower response so REQUEST_REPEAT can
  // replay it — last_tower_response_text_ keeps the REAL last clearance,
  // which is what a pilot who forgot the QNH actually wants to hear again.
  if (!resp.text.empty()) {
    bump_gen();
    g_state.last_tower_response_text_ = resp.text;
  }
}

// ── Main pipeline ───────────────────────────────────────────────────

ATCResponse process(const intent_parser::PilotMessage &msg_in,
                    const xplane_context::XPlaneContext &ctx, double now_secs) {
  assert_flight_loop_thread();

  // IFR is an EU-only feature. Strip IFR-only intents in non-EU profiles so
  // the IFR departure flow can never be entered, regardless of what the
  // intent rules or the LM fallback produce. Work on a local copy since the
  // caller's message is const; the rest of this function uses `msg`.
  intent_parser::PilotMessage msg = msg_in;
  if (settings::atc_profile() != "EU" &&
      (msg.intent == intent_parser::PilotIntent::REQUEST_IFR_CLEARANCE ||
       msg.intent == intent_parser::PilotIntent::REPORT_HOLDING_SHORT)) {
    logging::info("[ifr-gate] IFR intent ignored in non-EU profile (%s)",
                  settings::atc_profile().c_str());
    msg.intent = intent_parser::PilotIntent::UNKNOWN;
  }

  g_state.last_now_secs_ = now_secs; // heartbeat — no gen bump
  ATCResponse resp;

  // Airport-change reset is also done per-frame in check_airport_change();
  // this in-process call is a safety net.
  check_airport_change(ctx, now_secs);

  if (ground_ops::handle_negative_correction(msg, ctx, resp))
    return resp;

  // REQUEST_REPEAT — pilot asked the tower to repeat the last real
  // clearance ("SAY AGAIN").
  // Replay last_tower_response_text_ verbatim. State does NOT
  // advance and readback_pending_ is preserved — if the pilot still
  // owed a readback before asking for the repeat, they still do
  // after hearing it again.
  if (msg.intent == intent_parser::PilotIntent::REQUEST_REPEAT) {
    if (!g_state.last_tower_response_text_.empty()) {
      resp.text = g_state.last_tower_response_text_;
    } else {
      std::string cs;
      if (!g_state.session_callsign_.empty())
        cs = g_state.session_callsign_;
      else if (!msg.callsign.empty())
        cs = msg.callsign;
      else
        cs = settings::pilot_callsign();
      resp.text = cs + ", keine vorherige Anweisung zum Wiederholen.";
    }
    resp.next_state = g_state.state_;
    resp.requires_readback = g_state.readback_pending_;
    return resp;
  }

  ground_ops::apply_state_reverts(msg);

  if (ground_ops::handle_unicom_flow(msg, ctx, resp))
    return resp;

  if (ground_ops::handle_frequency_hint(msg, ctx, resp))
    return resp;

  ground_ops::apply_state_frequency_validity(ctx);
  ground_ops::apply_frequency_auto_corrections(ctx);

  if (ground_ops::handle_idle_redirects(msg, ctx, resp))
    return resp;

  if (ground_ops::check_handoff_reissue(msg, ctx, resp))
    return resp;

  if (ground_ops::check_phase_precondition(msg, ctx, resp))
    return resp;

  if (ground_ops::check_freq_precondition(msg, ctx, resp))
    return resp;

  if (ground_ops::check_no_flight_plan(msg, ctx, resp))
    return resp;

  if (ground_ops::check_atis_confirmation(msg, ctx, resp))
    return resp;

  if (ground_ops::check_sid_visibility(msg, ctx, resp))
    return resp;

  if (ground_ops::check_runway_at_holding_point(msg, ctx, resp))
    return resp;

  if (ground_ops::check_squawk_at_holding_point(msg, ctx, resp))
    return resp;

  // Template-based response lookup. IFR flows may redirect the state key
  // (e.g. TOWER_CONTACT + departure intent + IFR squawk → IFR/TOWER_CONTACT).
  auto vars = ground_ops::build_vars(msg, ctx);
  std::string intent_key = intent_parser::intent_template_key(msg.intent);
  std::string state_str =
      ground_ops::effective_state_for_template(g_state.state_, msg, ctx);

  auto tmpl =
      atc_templates::lookup(true, state_str, intent_key, ctx.tower_only);
  resp.text = atc_templates::fill(tmpl.response_template, vars);
  resp.next_state = state_from_name(tmpl.next_state);
  resp.requires_readback = tmpl.requires_readback;

  // Phase-4: per-flow sequencing overlay. The Pattern side owns the
  // "number N to land, follow X on Y" / "continue approach, traffic on
  // the runway" rewrites; the XC side is a no-op placeholder for
  // Phase 5 ("expect number N" prefixes from Approach). Both inspect
  // resp + vars and may rewrite resp.text in place.
  const auto &traffic_now = traffic_context::current();
  if (pattern_flow::is_pattern_state(resp.next_state))
    pattern_flow::apply_landing_sequence(msg, ctx, traffic_now, vars, resp);
  else if (crosscountry_flow::is_xc_state(resp.next_state))
    crosscountry_flow::apply_landing_sequence(msg, ctx, traffic_now, vars,
                                              resp);

  // Reset the session-lifecycle was_airborne flag when a new departure
  // cycle starts on the ground. The intent list documents the intent
  // (departure-anmeldung); the on_ground gate is the physical
  // invariant ("a new departure cycle begins on the ground") and
  // catches parser misclassifications mid-air. INITIAL_CALL_INBOUND is
  // deliberately NOT in the list — inbound is the opposite of a
  // departure cycle.
  //
  // F.2 interaction: REQUEST_TAXI gets remapped to REQUEST_TAXI_PARKING
  // by apply_adjustments() upstream when at_airport_after_landing is
  // true; REQUEST_TAXI_PARKING is not in the reset list, so the flag
  // stays armed during the post-landing taxi-back. The reset only
  // fires when REQUEST_TAXI survives the remap — i.e. when the post-
  // landing window has already closed and the pilot really is starting
  // a new flight.
  using PI = intent_parser::PilotIntent;
  if (ctx.on_ground &&
      (msg.intent == PI::REQUEST_TAXI ||
       msg.intent == PI::INITIAL_CALL_GROUND ||
       msg.intent == PI::INITIAL_CALL_TOWER || msg.intent == PI::INITIAL_CALL))
    internal::set_was_airborne(false);

  apply_post_transition_hooks(msg, ctx, resp);
  return resp;
}

void check_airport_change(const xplane_context::XPlaneContext &ctx,
                          double now_secs) {
  // Owned by crosscountry_flow since step 4 — EN_ROUTE is the only
  // state this reacts to. Facade delegates so external callers
  // (atc_session, process()'s in-process safety net) keep their
  // existing entry point.
  crosscountry_flow::check_airport_change(ctx, now_secs);
}

// ── Auto-correction ─────────────────────────────────────────────────
// State for the per-frame correction timer now lives in
// g_state.active_correction_key_ / g_state.correction_timer_ (heartbeat
// fields — no gen-bump on per-frame ticking).

void check_auto_correction(flight_phase::FlightPhase phase, float dt,
                           double now_secs) {
  assert_flight_loop_thread();
  g_state.last_now_secs_ = now_secs; // heartbeat — no gen bump
  if (g_state.state_ == ATCState::IDLE ||
      g_state.state_ == ATCState::UNICOM_ACTIVE)
    return;

  std::string current_state = state_name(g_state.state_);
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
      // The legacy CROSS_COUNTRY suppression hack (skip Pattern
      // auto-correction when DEPARTURE_CLEARED + departure_type ==
      // CROSS_COUNTRY → PATTERN_ENTRY) lived here pre-step-4. After
      // step 3b the flight_rules.json on_airborne entry only exists
      // under "Pattern/DEPARTURE_CLEARED" — when the pilot is in
      // XC/DEPARTURE_CLEARED, get_auto_corrections returns nullptr
      // and we never reach this branch. The check was redundant; the
      // structural split in the data layer now does the job.
      std::string key;
      key.reserve(current_state.size() + 1 + cond_name.size());
      key += current_state;
      key += ':';
      key += cond_name;
      if (key != g_state.active_correction_key_) {
        g_state.active_correction_key_ = key;
        g_state.correction_timer_ = 0.0f;
      }
      g_state.correction_timer_ += dt;

      if (g_state.correction_timer_ >=
          ac.delay_sec * settings::auto_correction_factor()) {
        ATCState new_state = state_from_name(ac.next_state);
        logging::info("Auto-correction: phase=%s, condition=%s, after %.1fs",
                      flight_phase::phase_name(phase), cond_name.c_str(),
                      g_state.correction_timer_);
        std::string reason = "auto_correction:";
        reason += cond_name;
        internal::transition_to(new_state, reason.c_str());
        bump_gen();
        g_state.readback_pending_ = false;
        if (new_state == ATCState::IDLE) {
          if (!g_state.assigned_runway_.empty()) {
            logging::info("Runway lock released (auto-correction)");
            g_state.assigned_runway_.clear();
          }
          g_state.session_callsign_.clear();
        }
        g_state.active_correction_key_.clear();
        g_state.correction_timer_ = 0.0f;
      }
      return;
    }
  }

  // No matching condition — reset timer (heartbeat, no gen bump)
  g_state.active_correction_key_.clear();
  g_state.correction_timer_ = 0.0f;
}

// ── State history accessors ─────────────────────────────────────────

ATCState previous_state() {
  if (g_state.history_.empty())
    return ATCState::IDLE;
  return g_state.history_.back().state;
}

bool was_recently_in(ATCState s, double max_age_secs, double now_secs) {
  if (g_state.state_ == s)
    return true;
  for (auto it = g_state.history_.rbegin(); it != g_state.history_.rend();
       ++it) {
    if (now_secs - it->timestamp_secs > max_age_secs)
      return false; // entries beyond this are even older
    if (it->state == s)
      return true;
  }
  return false;
}

bool just_landed(double now_secs, double window_secs) {
  return was_recently_in(ATCState::LANDING_CLEARED, window_secs, now_secs) ||
         was_recently_in(ATCState::TOUCH_AND_GO_CLEARED, window_secs, now_secs);
}

bool at_airport_after_landing(const xplane_context::XPlaneContext &ctx) {
  if (!ctx.on_ground)
    return false;
  auto is_landing = [](ATCState s) {
    return s == ATCState::LANDING_CLEARED ||
           s == ATCState::TOUCH_AND_GO_CLEARED;
  };
  if (is_landing(g_state.state_))
    return true;
  // Walk newest-to-oldest. A DEPARTURE_CLEARED *after* the most
  // recent landing means the pilot is on a new flight (mid-taxi to
  // takeoff or already cleared) — drop out of the post-landing
  // window. Other transitional states (GROUND_CONTACT, TAXI_CLEARED,
  // TOWER_CONTACT) can legitimately appear during taxi-in and don't
  // disqualify.
  for (auto it = g_state.history_.rbegin(); it != g_state.history_.rend();
       ++it) {
    if (it->state == ATCState::DEPARTURE_CLEARED)
      return false;
    if (is_landing(it->state))
      return true;
  }
  return false;
}

const std::deque<StateHistoryEntry> &get_history() { return g_state.history_; }

std::string history_csv() {
  std::string out;
  for (const auto &e : g_state.history_) {
    if (!out.empty())
      out += ',';
    out += state_name(e.state);
  }
  if (!out.empty())
    out += ',';
  out += state_name(g_state.state_);
  return out;
}

// ── Snapshot / Restore (TTS revert guard) ───────────────────────────

struct AtcStateSnapshot::Impl {
  AtcMachineState state;
};

AtcStateSnapshot::AtcStateSnapshot() : impl_(std::make_unique<Impl>()) {}
AtcStateSnapshot::AtcStateSnapshot(const AtcStateSnapshot &other)
    : impl_(std::make_unique<Impl>(*other.impl_)) {}
AtcStateSnapshot::AtcStateSnapshot(AtcStateSnapshot &&) noexcept = default;
AtcStateSnapshot &AtcStateSnapshot::operator=(const AtcStateSnapshot &other) {
  if (this != &other)
    impl_ = std::make_unique<Impl>(*other.impl_);
  return *this;
}
AtcStateSnapshot &
AtcStateSnapshot::operator=(AtcStateSnapshot &&) noexcept = default;
AtcStateSnapshot::~AtcStateSnapshot() = default;

AtcStateSnapshot capture_snapshot() {
  assert_flight_loop_thread();
  AtcStateSnapshot snap;
  snap.impl_->state = g_state; // full struct copy, gen included
  return snap;
}

uint64_t current_gen() { return g_state.gen; }

bool restore_snapshot_if_gen(const AtcStateSnapshot &snap,
                             uint64_t expected_gen) {
  assert_flight_loop_thread();
  if (g_state.gen != expected_gen)
    return false; // stale — a later mutation already advanced state
  // Bump the restored gen so it ends up strictly above any value the
  // caller has observed. Any other pending guard with an
  // expected_gen <= current g_state.gen will see a mismatch on its own
  // restore attempt and bail cleanly.
  const uint64_t new_gen = g_state.gen + 1;
  g_state = snap.impl_->state;
  g_state.gen = new_gen;
  logging::info("ATC state restored from snapshot (gen %llu -> %llu)",
                static_cast<unsigned long long>(expected_gen),
                static_cast<unsigned long long>(new_gen));
  return true;
}

// ── Traffic-advisory rendering ──────────────────────────────────────

std::string
render_traffic_advisory(const std::map<std::string, std::string> &advisory_vars,
                        const xplane_context::XPlaneContext &ctx,
                        const std::string &template_key) {
  // Build base vars (callsign, airport, etc.) and merge in the
  // advisor-supplied advisory placeholders. ATCState is intentionally
  // not touched here — the traffic dialog runs parallel to the main
  // flow (see traffic_dialog.{hpp,cpp}).
  intent_parser::PilotMessage synthetic_msg;
  auto vars = ground_ops::build_vars(synthetic_msg, ctx);
  for (const auto &[k, v] : advisory_vars)
    vars[k] = v;

  auto tmpl = atc_templates::lookup(true, "TRAFFIC_DIALOG", template_key);
  return atc_templates::fill(tmpl.response_template, vars);
}

} // namespace atc_state_machine
