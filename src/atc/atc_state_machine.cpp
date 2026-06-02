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
#include "atc/flows/state_storage.hpp"
#include "atc/traffic_dialog.hpp"
#include "core/logging.hpp"
#include "data/traffic_geometry.hpp"
#include "persistence/settings.hpp"

#include <deque>
#include <unordered_map>

namespace atc_state_machine {

// ── State storage (file-local statics) ──────────────────────────────

static ATCState state_ = ATCState::IDLE;
static bool readback_pending_ = false;
static std::string assigned_runway_; // locked once ATC assigns a runway
static internal::DepartureType departure_type_ =
    internal::DepartureType::PATTERN;
// The "last airport observed" tracker that drives the airport-change
// reset moved to crosscountry_flow.cpp in step 4 — it is XC-specific
// (only consulted while EN_ROUTE) and lives there under the
// Schicht-1 ownership rule.

// Bounded chronological log of state transitions. Front = oldest,
// back = most recent past state. Filled by transition_to(); read by
// downstream consumers (LM-classify prompt, hint filter, intent rules)
// to disambiguate situations where the current state alone is
// insufficient (e.g. post-landing IDLE is not the same as cold-start
// IDLE). Cap chosen for typical inbound sequence
// EN_ROUTE -> PATTERN_ENTRY -> LANDING_CLEARED -> IDLE -> GROUND_CONTACT
// -> TAXI_CLEARED -> IDLE plus a couple of disregards/auto-corrections.
constexpr size_t kHistoryCap = 8;
static std::deque<StateHistoryEntry> history_;

// Last timestamp seen by a public mutating entry point (process,
// disregard, check_auto_correction, check_airport_change). Internal
// helpers calling transition_to() inside one of those flows pick this
// up so every History entry has a sensible timestamp without having
// to thread the value through every helper signature.
static double last_now_secs_ = 0.0;

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
    return departure_type_ == internal::DepartureType::CROSS_COUNTRY
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
  };
  auto it = kMap.find(name);
  return it != kMap.end() ? it->second : ATCState::IDLE;
}

// ── internal:: bridge implementation (private API for ground_ops) ───

namespace internal {

const char *departure_type_name(DepartureType t) {
  return t == DepartureType::CROSS_COUNTRY ? "CROSS_COUNTRY" : "PATTERN";
}

ATCState get_state_ref() { return state_; }
bool readback_pending() { return readback_pending_; }
const std::string &assigned_runway_ref() { return assigned_runway_; }
DepartureType departure_type() { return departure_type_; }

// Single source of truth for state mutations. Pushes the previous
// state into history_ (capped at kHistoryCap, oldest dropped first),
// switches state_ to next, and emits a uniform log line. No-op when
// next == state_ (avoids polluting history with self-transitions).
void transition_to(ATCState next, const char *reason) {
  if (next == state_)
    return;
  history_.push_back(
      StateHistoryEntry{state_, reason ? reason : "", last_now_secs_});
  while (history_.size() > kHistoryCap)
    history_.pop_front();
  logging::info("ATC state: %s -> %s (%s)", state_name(state_),
                state_name(next), reason ? reason : "");
  state_ = next;
}

void set_readback_pending(bool v) { readback_pending_ = v; }
void set_assigned_runway(const std::string &rwy) { assigned_runway_ = rwy; }
void clear_assigned_runway() { assigned_runway_.clear(); }
void set_departure_type(DepartureType t) { departure_type_ = t; }
void set_last_now_secs(double t) { last_now_secs_ = t; }
double last_now_secs() { return last_now_secs_; }

} // namespace internal

// ── Lifecycle ───────────────────────────────────────────────────────

void init() {
  state_ = ATCState::IDLE;
  readback_pending_ = false;
  assigned_runway_.clear();
  departure_type_ = internal::DepartureType::PATTERN;
  history_.clear();
  last_now_secs_ = 0.0;

  // Honor the user's "where am I starting" setting. The default
  // engines_running keeps the IDLE start above; ready_for_takeoff
  // jumps straight to TOWER_CONTACT so a pilot spawning on the
  // runway sees READY_FOR_DEPARTURE hints immediately.
  const std::string mode = settings::start_mode();
  if (mode == "ready_for_takeoff") {
    state_ = ATCState::TOWER_CONTACT;
    logging::info(
        "Initial state: TOWER_CONTACT (start_mode=ready_for_takeoff)");
  }
}

void stop() {
  state_ = ATCState::IDLE;
  readback_pending_ = false;
  assigned_runway_.clear();
  departure_type_ = internal::DepartureType::PATTERN;
  history_.clear();
  last_now_secs_ = 0.0;
}

void reset() {
  state_ = ATCState::IDLE;
  readback_pending_ = false;
  assigned_runway_.clear();
  departure_type_ = internal::DepartureType::PATTERN;
  crosscountry_flow::reset();
  history_.clear();
  last_now_secs_ = 0.0;
  logging::info("ATC state machine reset to IDLE");
}

// ── Disregard ───────────────────────────────────────────────────────

// "Disregard" radius: airborne pilots within this distance from their
// nearest airport are treated as still in the pattern flow; outside,
// they land in EN_ROUTE so an INITIAL_CALL_INBOUND can re-establish.
constexpr double kDisregardPatternRadiusNm = 5.0;

void disregard(const xplane_context::XPlaneContext &ctx,
               flight_phase::FlightPhase phase, double now_secs) {
  last_now_secs_ = now_secs;
  // Always clear the side-channel — a Disregard mid-advisory should
  // also drop the pending traffic ack.
  traffic_dialog::reset();
  readback_pending_ = false;

  if (!flight_phase::is_airborne(phase)) {
    internal::transition_to(ATCState::IDLE, "disregard_on_ground");
    assigned_runway_.clear();
    departure_type_ = internal::DepartureType::PATTERN;
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

ATCState get_state() { return state_; }

bool is_readback_pending() { return readback_pending_; }

void set_state(ATCState state) {
  internal::transition_to(state, "external_set_state");
  if (state == ATCState::IDLE && !assigned_runway_.empty()) {
    logging::info("Runway lock released");
    assigned_runway_.clear();
  }
}

const std::string &assigned_runway() { return assigned_runway_; }

std::string effective_runway(const xplane_context::XPlaneContext &ctx) {
  return assigned_runway_.empty() ? ctx.active_runway : assigned_runway_;
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
  if (msg.intent == intent_parser::PilotIntent::READBACK)
    readback_pending_ = false;
  else if (resp.requires_readback)
    readback_pending_ = true;

  // Leaving the controller's frequency or resetting drops stale readback
  // context. Without this, "frequency change good day" after an unread-back
  // takeoff clearance keeps readback_pending armed, and the UI hint pipeline
  // silences every other hint at the next airport.
  if (resp.next_state == ATCState::EN_ROUTE ||
      resp.next_state == ATCState::IDLE)
    readback_pending_ = false;

  // Lock runway on first clearance that references a runway. Same fallback
  // chain as ground_ops::get_runway() (msg → assigned → active → "28"),
  // inlined here to avoid exposing get_runway() outside ground_operations.
  if (assigned_runway_.empty() && resp.next_state != ATCState::IDLE) {
    std::string rwy = !msg.runway.empty()          ? msg.runway
                      : !ctx.active_runway.empty() ? ctx.active_runway
                                                   : std::string{"28"};
    if (!rwy.empty()) {
      assigned_runway_ = rwy;
      logging::info("Runway locked: %s", rwy.c_str());
    }
  }

  // Apply state transition if we have a response.
  if (!resp.text.empty()) {
    ATCState prev_state = state_;
    std::string reason = "process:";
    reason += intent_parser::intent_template_key(msg.intent);
    internal::transition_to(resp.next_state, reason.c_str());

    // Track departure intent: PATTERN (default) vs CROSS_COUNTRY.
    if (resp.next_state == ATCState::DEPARTURE_CLEARED &&
        prev_state != ATCState::DEPARTURE_CLEARED) {
      departure_type_ =
          (msg.intent == intent_parser::PilotIntent::READY_FOR_DEPARTURE_VFR)
              ? internal::DepartureType::CROSS_COUNTRY
              : internal::DepartureType::PATTERN;
      logging::info("Departure type: %s",
                    internal::departure_type_name(departure_type_));
    } else if (prev_state == ATCState::DEPARTURE_CLEARED &&
               resp.next_state != ATCState::DEPARTURE_CLEARED) {
      departure_type_ = internal::DepartureType::PATTERN;
    }
  }

  // Release runway lock when session ends.
  if (resp.next_state == ATCState::IDLE && !assigned_runway_.empty()) {
    logging::info("Runway lock released");
    assigned_runway_.clear();
  }

  // Tower-only airport: skip ground→tower handoff (no freq change needed).
  // Data-driven via flight_rules.tower_only_auto_advance.
  if (ctx.tower_only) {
    std::string current = state_name(state_);
    std::string next = flight_phase::get_tower_only_auto_advance(current);
    if (!next.empty()) {
      ATCState target = state_from_name(next);
      internal::transition_to(target, "tower_only_auto_advance");
      resp.next_state = target;
    }
  }
}

// ── Main pipeline ───────────────────────────────────────────────────

ATCResponse process(const intent_parser::PilotMessage &msg,
                    const xplane_context::XPlaneContext &ctx, double now_secs) {
  last_now_secs_ = now_secs;
  ATCResponse resp;

  // Airport-change reset is also done per-frame in check_airport_change();
  // this in-process call is a safety net.
  check_airport_change(ctx, now_secs);

  if (ground_ops::handle_negative_correction(msg, ctx, resp))
    return resp;

  ground_ops::apply_state_reverts(msg);

  if (ground_ops::handle_unicom_flow(msg, ctx, resp))
    return resp;

  if (ground_ops::handle_frequency_hint(msg, ctx, resp))
    return resp;

  ground_ops::apply_state_frequency_validity(ctx);
  ground_ops::apply_frequency_auto_corrections(ctx);

  if (ground_ops::handle_idle_redirects(msg, ctx, resp))
    return resp;

  if (ground_ops::check_phase_precondition(msg, ctx, resp))
    return resp;

  if (ground_ops::check_freq_precondition(msg, ctx, resp))
    return resp;

  // Template-based response lookup.
  auto vars = ground_ops::build_vars(msg, ctx);
  std::string intent_key = intent_parser::intent_template_key(msg.intent);
  std::string state_str = state_name(state_);

  auto tmpl = atc_templates::lookup(true, state_str, intent_key);
  resp.text = atc_templates::fill(tmpl.response_template, vars);
  resp.next_state = state_from_name(tmpl.next_state);
  resp.requires_readback = tmpl.requires_readback;

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

// ── Auto-correction state ───────────────────────────────────────────

static std::string active_correction_key_;
static float correction_timer_ = 0.0f;

void check_auto_correction(flight_phase::FlightPhase phase, float dt,
                           double now_secs) {
  last_now_secs_ = now_secs;
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
      if (key != active_correction_key_) {
        active_correction_key_ = key;
        correction_timer_ = 0.0f;
      }
      correction_timer_ += dt;

      if (correction_timer_ >=
          ac.delay_sec * settings::auto_correction_factor()) {
        ATCState new_state = state_from_name(ac.next_state);
        logging::info("Auto-correction: phase=%s, condition=%s, after %.1fs",
                      flight_phase::phase_name(phase), cond_name.c_str(),
                      correction_timer_);
        std::string reason = "auto_correction:";
        reason += cond_name;
        internal::transition_to(new_state, reason.c_str());
        readback_pending_ = false;
        if (new_state == ATCState::IDLE && !assigned_runway_.empty()) {
          logging::info("Runway lock released (auto-correction)");
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

// ── State history accessors ─────────────────────────────────────────

ATCState previous_state() {
  if (history_.empty())
    return ATCState::IDLE;
  return history_.back().state;
}

bool was_recently_in(ATCState s, double max_age_secs, double now_secs) {
  if (state_ == s)
    return true;
  for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
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
  if (is_landing(state_))
    return true;
  // Walk newest-to-oldest. A DEPARTURE_CLEARED *after* the most
  // recent landing means the pilot is on a new flight (mid-taxi to
  // takeoff or already cleared) — drop out of the post-landing
  // window. Other transitional states (GROUND_CONTACT, TAXI_CLEARED,
  // TOWER_CONTACT) can legitimately appear during taxi-in and don't
  // disqualify.
  for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
    if (it->state == ATCState::DEPARTURE_CLEARED)
      return false;
    if (is_landing(it->state))
      return true;
  }
  return false;
}

const std::deque<StateHistoryEntry> &get_history() { return history_; }

std::string history_csv() {
  std::string out;
  for (const auto &e : history_) {
    if (!out.empty())
      out += ',';
    out += state_name(e.state);
  }
  if (!out.empty())
    out += ',';
  out += state_name(state_);
  return out;
}

// ── Traffic-advisory rendering ──────────────────────────────────────

std::string
render_traffic_advisory(const std::map<std::string, std::string> &advisory_vars,
                        const xplane_context::XPlaneContext &ctx) {
  // Build base vars (callsign, airport, etc.) and merge in the
  // advisor-supplied advisory placeholders. ATCState is intentionally
  // not touched here — the traffic dialog runs parallel to the main
  // flow (see traffic_dialog.{hpp,cpp}).
  intent_parser::PilotMessage synthetic_msg;
  auto vars = ground_ops::build_vars(synthetic_msg, ctx);
  for (const auto &[k, v] : advisory_vars)
    vars[k] = v;

  auto tmpl = atc_templates::lookup(true, "TRAFFIC_DIALOG", "traffic_advisory");
  return atc_templates::fill(tmpl.response_template, vars);
}

} // namespace atc_state_machine
