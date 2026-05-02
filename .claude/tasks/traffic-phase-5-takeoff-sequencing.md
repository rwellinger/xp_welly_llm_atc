# Phase 5 — Takeoff Sequencing

> Branch `feat/traffic-phase-5-takeoff-sequencing`. **Depends on Phases 1–4.**
> Self-contained: a fresh session must be able to execute this milestone after Phases 1–4 are merged.

## Context & Goal

Realistic pre-takeoff and initial-climb traffic awareness, in three sub-features:

- **5a Lineup Check**: before issuing takeoff clearance, check runway occupancy + final-approach traffic.
- **5b Takeoff Clearance with Spacing**: wake-turbulence advice based on traffic ahead.
- **5c Initial Climb Crossing Traffic**: advisory once after takeoff if a target crosses the departure path.

## Codebase Pointers (read first)

Read `CLAUDE.md`. Confirm Phases 1–4 merged.

| Path | What's there |
|---|---|
| `src/data/traffic_phase_classifier.{hpp,cpp}` | (Phases 3+4) `OnGround/Taxi/Takeoff/Landed/Pattern/Final` classification. Used here to detect runway occupancy. |
| `src/data/traffic_geometry.hpp/cpp` | `is_on_runway_centerline(...)` from Phase 4 — reuse for runway-clear check. |
| `src/data/traffic_context.hpp` | `TrafficTarget.wake_cat` (`enum WakeCategory`). |
| `src/data/traffic_context_runtime.cpp` | DataRef read of `sim/cockpit2/tcas/targets/wake/wake_cat` (Phase-1 wired this if available). Falls back to `WakeCategory::Unknown`. |
| `src/atc/atc_state_machine.hpp:31–43` | `ATCState`. Phase 5a needs a new state `LINEUP_AND_WAIT` (not present). Add it as a regular flow state — the pilot transitions through it normally (taxi-cleared → line-up → cleared-takeoff), so it belongs in the main enum. **Not** a side-channel: traffic_dialog stays reserved for advisories that expect a voice ack with no flow consequences. |
| `src/atc/atc_session.cpp` | (plugin-only) Triggers takeoff clearance flow. Hook lineup-check before clearance is emitted. |
| `src/atc/traffic_dialog.{hpp,cpp}` | (Phase-2) parallel side-channel. **5c (climbout crossing) reuses this pattern**: `engine::poll_traffic_advisory` calls `render_traffic_advisory` + `traffic_dialog::on_advisory_emitted` so the pilot's voice reply (TRAFFIC_IN_SIGHT etc.) is captured the same way Phase 2 does it. 5a / 5b stay in the main ATCState flow. |
| `data/regions/eu/atc_templates.json` | `DEPARTURE_CLEARED` / `TAXI_CLEARED` / Tower contact templates. Add new traffic-aware variants. |

## Architecture Constraints

- **SDK-free**: all new logic in `src/atc/traffic_advisor.cpp` or split helper file. No `<XPLM*.h>`.
- **Reuse Phase-4 geometry**: `is_on_runway_centerline()` is the basis for runway-clear checks.
- **Wake-cat fallback**: when `WakeCategory::Unknown`, treat as `Medium`. Document this in README limitations.
- **EU phraseology** authoritative.

## 5a — Pre-Takeoff Lineup Check

Before emitting takeoff clearance:

- **Runway clear**: no target with `phase ∈ {OnGround, Takeoff, Landed}` on active-runway centerline (uses Phase-4 helper).
- **Final-approach traffic**:
  - Target in `Final` for this runway within **3 NM** → emit `lineup_and_wait_traffic` instead of cleared-takeoff.
  - Target in `Final` within **1.5 NM** → emit `hold_short_traffic` (do not clear, do not line up).
  - Otherwise → normal clearance flow.

State handling:
- Add `LINEUP_AND_WAIT` as a regular flow state in `ATCState` (the Phase-2 "deviation #3 peer state" pattern was retired when traffic was decoupled into `traffic_dialog`; line-up-and-wait is a real flow state, not a side-channel).
- Transition: `TAXI_CLEARED → LINEUP_AND_WAIT` (when lineup-and-wait emitted) → `DEPARTURE_CLEARED` (when traffic clears).
- Transition: `TAXI_CLEARED → TAXI_CLEARED` (no progress) when `hold_short_traffic` is emitted.

## 5b — Takeoff Clearance with Spacing

When clearing for takeoff with traffic ahead (in `Takeoff` phase, last to depart):
- **Same wake category, ≥ 2 NM behind**: clear normally (no caution).
- **Heavier wake category ahead** (`prev_wake > current_wake` per ICAO ordering Light < Medium < Heavy < Super): append `"caution wake turbulence"` to clearance.
- Wake-cat from `sim/cockpit2/tcas/targets/wake/wake_cat`. Default to `Medium` when `Unknown`.

Pure helper in `traffic_advisor.cpp`: `wake_caution_required(prev_wake, our_wake) → bool`.

## 5c — Initial Climb Crossing Traffic

After takeoff (user phase = `Takeoff` or initial `Climb`, AGL < 2000 ft):
- Look for target crossing the departure path within **2 NM**, **±1000 ft** altitude.
- "Departure path" = a 2 NM corridor extending from runway end along runway heading, ±0.25 NM lateral.
- Issue advisory **once** (per modeS_id, no cooldown — but do not re-issue same target):
  *"[Callsign], traffic 12 o'clock, 2 miles, crossing left to right, 500 feet above."*
- Reuse Phase-2 direction classifier and altitude-info formatter.
- Dispatch via the established Phase-2 pipeline: `engine::poll_traffic_advisory` calls `atc_state_machine::render_traffic_advisory(vars, ctx)` for the text, then `traffic_dialog::on_advisory_emitted(modeS_id)` so a pilot reply (TRAFFIC_IN_SIGHT / NEGATIVE_CONTACT / LOOKING) is handled the same way it is for en-route advisories. The 5-min `kVisualAckLockoutSec` lockout guarantees the same target can't re-fire post-ack. Flag `kInitialClimbCrossing` style (single-shot, no cooldown re-issue) is implemented by setting the per-modeS `acknowledged_visual_secs` entry up-front when the advisory is emitted, so even an unacknowledged target doesn't re-fire from this 5c-specific code path.

## Acceptance Criteria

- [ ] Three new functions/blocks in `traffic_advisor.cpp` (or split into `src/atc/takeoff_sequence.{hpp,cpp}`):
  - `evaluate_lineup(traffic_ctx, runway_info, threshold_lat, threshold_lon) → LineupResult`
  - `wake_caution_required(prev_wake, our_wake) → bool`
  - `evaluate_climbout_crossing(traffic_ctx, user_state) → optional<ClimbCrossing>`
- [ ] Each function unit-tested.
- [ ] `LINEUP_AND_WAIT` added to `ATCState` enum + `state_name`/`state_from_name` (regular flow state). Templates for that state added. State-machine handles enter/exit transitions.
- [ ] Wake-turbulence categorization helper with `Unknown → Medium` fallback. Documented in README.
- [ ] EU templates added to `data/regions/eu/atc_templates.json`:
  - `lineup_and_wait_traffic`: `"{callsign}, line up runway {runway} and wait, traffic on {distance}-mile final."` — under `TOWER_CONTACT` block (entry into `LINEUP_AND_WAIT`).
  - `hold_short_traffic`: `"{callsign}, hold short of runway {runway}, traffic on {distance}-mile final."` — under `TOWER_CONTACT` block (state stays).
  - `cleared_takeoff_caution_wake`: `"{callsign}, runway {runway}, cleared for takeoff, caution wake turbulence."` — under `TOWER_CONTACT` and `LINEUP_AND_WAIT` blocks (entry into `DEPARTURE_CLEARED`).
  - `climbout_traffic_advisory`: `"{callsign}, traffic {clock} o'clock, {distance} miles, {direction}, {altitude_info}."` — under the `TRAFFIC_DIALOG` block (rendered via `render_traffic_advisory` + `traffic_dialog::on_advisory_emitted`).
- [ ] Catch2 tests in `tests/takeoff_sequence_test.cpp`:
  - 5a: target on final at 4 NM → normal clearance; at 2.5 NM → lineup-and-wait; at 1.0 NM → hold-short; runway occupied → hold-short.
  - 5b: heavier wake ahead → caution wake; same/lighter → no caution; wake `Unknown` → treat as Medium and apply rule.
  - 5c: crossing target in 2-NM corridor at 500 ft above → advisory; outside corridor → no advisory; same target re-evaluated → no duplicate.
- [ ] `tests/fixtures/traffic_takeoff_lineup_blocked.json` — fixture with target on final 2 NM out.
- [ ] `tests/fixtures/traffic_takeoff_wake_heavy_ahead.json`.
- [ ] `tests/fixtures/traffic_climbout_crossing.json`.
- [ ] `docs/traffic-smoke-test.md` updated with Phase-5 scenarios.
- [ ] `README.md` updated with **honest limitations**:
  - "Wake category falls back to Medium when not provided by traffic source."
  - "No taxiway-name awareness."
  - "No IFR sequencing."
  - "No LAHSO or intersecting-runway departure logic."

## Files to Create / Modify

**Create**:
- `src/atc/takeoff_sequence.hpp` (or extend `traffic_advisor.{hpp,cpp}`)
- `src/atc/takeoff_sequence.cpp` (optional)
- `tests/takeoff_sequence_test.cpp`
- `tests/fixtures/traffic_takeoff_lineup_blocked.json`
- `tests/fixtures/traffic_takeoff_wake_heavy_ahead.json`
- `tests/fixtures/traffic_climbout_crossing.json`

**Modify**:
- `src/atc/traffic_advisor.hpp/cpp` (if not split).
- `src/atc/atc_state_machine.hpp/cpp` — `LINEUP_AND_WAIT` state if needed; build_vars extensions.
- `src/atc/atc_session.cpp` — hook lineup-check before takeoff-clearance emit.
- `data/regions/eu/atc_templates.json` — four new templates + state block for `LINEUP_AND_WAIT` if added.
- `tests/CMakeLists.txt`.
- `docs/traffic-smoke-test.md`.
- `README.md` — limitations section.

## Test Plan

**Unit (Catch2)**:
```bash
make test
```
Expected: `takeoff_sequence_test.cpp` covers all three sub-features.

**Headless (atc_repl)**:
```bash
./build/atc_repl run tests/fixtures/traffic_takeoff_lineup_blocked.json
./build/atc_repl run tests/fixtures/traffic_takeoff_wake_heavy_ahead.json
./build/atc_repl run tests/fixtures/traffic_climbout_crossing.json
```
Each produces deterministic, expected output.

**X-Plane Smoke**:
1. Holding short LSZH 28, request takeoff.
2. With LiveTraffic Heavy on 2 NM final → hear `"line up runway 28 and wait, traffic on 2-mile final"`.
3. Wait until traffic lands → hear `"cleared for takeoff"` (or normal clearance).
4. Heavy departed just before us → hear `"caution wake turbulence"` appended.
5. After rotation, with crossing traffic at 1500 ft AGL ahead → hear single climbout advisory.
6. Verify wake-cat fallback: provider with no wake-cat data still produces clean (no caution) clearance unless logic explicitly defaults heavier — check Medium fallback doesn't spuriously trigger caution.

## Out of Scope

- LAHSO operations.
- Multiple departures from intersecting runways (rare in EU GA).
- IFR-style spacing minima.

## Definition of Done

- `make all` clean.
- All three sub-features unit-tested + fixture-tested.
- X-Plane smoke pass.
- README limitations honestly documented.
- Engine OBJECT lib still SDK-free.

## After Phase 5

**Pause for review.** Document final known limitations in `README.md`. Do not paper over gaps with hardcoded fakes.

## Open Questions for Maintainer

- **`LINEUP_AND_WAIT` state**: spec says add as a regular flow state. Confirm placement between `TAXI_CLEARED` and `DEPARTURE_CLEARED` is the right transition shape (vs. e.g. `TOWER_CONTACT → LINEUP_AND_WAIT → DEPARTURE_CLEARED` for direct-from-tower clearances).
- **Wake-cat ordering for fallback**: when both ahead and self are `Unknown`, both default to `Medium` → no caution. Is that the right behavior, or should `Unknown` behind a known-Heavy still trigger caution?
- **README limitations location**: dedicated `Limitations` section, or scattered inline? Default to dedicated section.

## PR-Reporting

1. **What works** —
2. **What was deferred** —
3. **What surprised you** —
4. **Smoke-test results** —
5. **Open questions for maintainer** —

---

Commit prefix: `[phase-5] <imperative summary>`.
