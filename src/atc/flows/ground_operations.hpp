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

#ifndef ATC_FLOWS_GROUND_OPERATIONS_HPP
#define ATC_FLOWS_GROUND_OPERATIONS_HPP

// Shared pre-takeoff pipeline. Step 1 of the A1 flow-split refactor
// extracts these helpers out of atc_state_machine.cpp into their own
// namespace so the future PatternFlow and CrossCountryFlow can drive
// the same ground-side logic without duplication. In step 4 the
// pipeline becomes the canonical entry point for the ground-only
// states (IDLE / GROUND_CONTACT / TAXI_CLEARED / TOWER_CONTACT /
// UNICOM_ACTIVE). The functions still mutate the shared state through
// atc_state_machine::internal — that bridge goes away when each flow
// owns its own state.

#include "atc/atc_state_machine.hpp"
#include "atc/intent_parser.hpp"
#include "core/xplane_context.hpp"

#include <map>
#include <string>

namespace ground_ops {

using atc_state_machine::ATCResponse;
using intent_parser::PilotMessage;
using xplane_context::XPlaneContext;

// Build the template variable map used by every spoken ATC response.
// Pure read of state + ctx; never mutates. Callsign is abbreviated to
// the last three words once the dialog has left IDLE.
std::map<std::string, std::string> build_vars(const PilotMessage &msg,
                                              const XPlaneContext &ctx);

// Returns the template-lookup state key, redirecting IFR-aware overrides.
// When the pilot is in TOWER_CONTACT and sends a departure intent with an
// IFR squawk already assigned, this returns "IFR/TOWER_CONTACT" so the
// template engine picks the IFR-specific clearance instead of the VFR one.
std::string effective_state_for_template(
    atc_state_machine::ATCState state, const PilotMessage &msg);

// ── Pipeline guards (run in process() before the template lookup) ──
// Each returns true when it produced a response and the caller should
// stop processing this turn. Helpers without a response (apply_*)
// return void and only mutate state.

bool handle_negative_correction(const PilotMessage &msg,
                                const XPlaneContext &ctx, ATCResponse &resp);

void apply_state_reverts(const PilotMessage &msg);

bool handle_unicom_flow(const PilotMessage &msg, const XPlaneContext &ctx,
                        ATCResponse &resp);

bool handle_frequency_hint(const PilotMessage &msg, const XPlaneContext &ctx,
                           ATCResponse &resp);

void apply_state_frequency_validity(const XPlaneContext &ctx);

void apply_frequency_auto_corrections(const XPlaneContext &ctx);

bool handle_idle_redirects(const PilotMessage &msg, const XPlaneContext &ctx,
                           ATCResponse &resp);

bool check_phase_precondition(const PilotMessage &msg, const XPlaneContext &ctx,
                              ATCResponse &resp);

bool check_handoff_reissue(const PilotMessage &msg, const XPlaneContext &ctx,
                           ATCResponse &resp);

bool check_freq_precondition(const PilotMessage &msg, const XPlaneContext &ctx,
                             ATCResponse &resp);

// IFR-only: reject REQUEST_IFR_CLEARANCE when ATIS is active but the pilot
// did not say "information [letter]". Fires only in IDLE state (clearance
// delivery window). No-op when ATIS is not broadcasting.
bool check_atis_confirmation(const PilotMessage &msg, const XPlaneContext &ctx,
                             ATCResponse &resp);

// IFR-only: verify transponder code and Mode Charlie at the holding point.
// Fires on REPORT_HOLDING_SHORT when an IFR squawk is assigned. If the
// Detects an IFR runway mismatch at the holding point: fires when
// internal::assigned_runway_ref() != ctx.active_runway (position-based).
// Issues a correction and keeps state unchanged.
bool check_runway_at_holding_point(const PilotMessage &msg,
                                    const XPlaneContext &ctx,
                                    ATCResponse &resp);

// transponder code does not match or mode is not ALT/Mode C (>= 3), returns a
// "{callsign}, squawk {squawk} mode Charlie, confirm." response and keeps
// the state unchanged so the pilot must report holding again after fixing it.
bool check_squawk_at_holding_point(const PilotMessage &msg,
                                    const XPlaneContext &ctx,
                                    ATCResponse &resp);

// IFR-only: reject REQUEST_IFR_CLEARANCE when a visibility-constrained SID
// is assigned but reported visibility is below the minimum. Currently covers
// LFLP RW04 westbound (LSE/LTP/ROMAM) which require >= 5 km visibility due
// to the SOCOF terrain routing. Fires only in IDLE state.
bool check_sid_visibility(const PilotMessage &msg, const XPlaneContext &ctx,
                          ATCResponse &resp);

} // namespace ground_ops

#endif // ATC_FLOWS_GROUND_OPERATIONS_HPP
