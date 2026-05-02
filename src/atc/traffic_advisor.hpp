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
 *
 * Phase-2 traffic advisor. SDK-free pure-function evaluator that
 * decides whether — and which — traffic target deserves a controller-
 * issued advisory on this tick. engine::poll_traffic_advisory drives
 * the evaluator each frame and renders the text via
 * atc_state_machine::render_traffic_advisory(); traffic_dialog tracks
 * the pilot-acknowledgement side-channel.
 *
 * Trigger logic is fully deterministic:
 *   - ATC contact established (atc_state != IDLE AND user is on a
 *     TOWER/GROUND/APPROACH freq).
 *   - A target T satisfies all of:
 *       2.0 <= distance_nm <= 8.0
 *       |altitude_diff| <= 1500 ft
 *       T in user's forward arc (clock 9-12 or 12-3, i.e. {9,10,11,12,1,2,3})
 *       Closure rate positive OR target track perpendicular to user
 *       T not advised in the last 60 s (per modeS_id)
 *   - Cooldown: at most one global advisory every 20 s.
 *
 * The evaluator is pure: callers own AdvisoryHistory and update it
 * after dispatch.
 */

#ifndef ATC_TRAFFIC_ADVISOR_HPP
#define ATC_TRAFFIC_ADVISOR_HPP

#include "atc/atc_state_machine.hpp"
#include "data/traffic_context.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>

namespace traffic_advisor {

// Snapshot of user state needed for the trigger evaluation. Filled
// at the call site from XPlaneContext + atc_state_machine.
struct UserState {
  atc_state_machine::ATCState atc_state = atc_state_machine::ATCState::IDLE;
  bool on_active_atc_freq = false;

  double lat = 0.0;
  double lon = 0.0;
  double alt_msl_ft = 0.0;
  double heading_deg = 0.0; // true heading
  double track_deg = 0.0;   // ground track
  double groundspeed_kts = 0.0;
  // Has the target Mode-C / pressure altitude (i.e. is target.alt_msl_ft
  // a real reading)? Plumbed through so format_altitude_info() can
  // pick the right phraseology fragment without needing X-Plane DataRefs.
  // Set this from the provider; default false is safe.
  bool target_has_mode_c_default = true;
};

// Per-target last-issued timestamp + the global "last advisory at"
// timestamp. Time unit is seconds (XPLMGetElapsedTime in plugin,
// monotonic test clock in unit tests). Owned by the caller.
//
// `acknowledged_visual_secs` carries a *longer* lockout horizon for
// targets the pilot has acknowledged with positive visual contact
// (TRAFFIC_IN_SIGHT). Without this, a slow GA target lingering in the
// 2-8 NM forward arc gets re-advised every 60 s as long as the pilot
// is still in contact — even though the pilot already knows about it.
struct AdvisoryHistory {
  std::unordered_map<uint32_t, double> last_issued_secs;
  std::unordered_map<uint32_t, double> acknowledged_visual_secs;
  double last_global_emit_secs = -1e9; // far in the past
};

// One advisory ready to render via atc_state_machine::render_traffic_advisory.
// `vars` already contains {clock, distance, direction, altitude_info, type}
// pre-rendered for the active region's phraseology template.
struct TrafficAdvisory {
  uint32_t modeS_id = 0;
  std::map<std::string, std::string> vars;
};

// Pure evaluation. Returns nullopt when no target qualifies.
std::optional<TrafficAdvisory>
evaluate(const traffic_context::TrafficContext &traffic, const UserState &user,
         const AdvisoryHistory &history, double now_secs);

// Mutation helper: stamp an emit into history. Callers run this after
// dispatching an advisory.
void mark_emitted(AdvisoryHistory &history, uint32_t modeS_id, double now_secs);

// Mutation helper: stamp a visual-contact acknowledgement. Suppresses
// re-advising the same target for kVisualAckLockoutSec.
void mark_acknowledged_visual(AdvisoryHistory &history, uint32_t modeS_id,
                              double now_secs);

// Constants exposed for testing / documentation.
constexpr double kMinDistanceNm = 2.0;
constexpr double kMaxDistanceNm = 8.0;
constexpr double kMaxAltDiffFt = 1500.0;
constexpr double kPerTargetCooldownSec = 60.0;
constexpr double kGlobalCooldownSec = 20.0;
constexpr double kVisualAckLockoutSec = 300.0;

} // namespace traffic_advisor

#endif // ATC_TRAFFIC_ADVISOR_HPP
