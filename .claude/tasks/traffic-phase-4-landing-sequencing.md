# Phase 4 — Landing Sequencing

> Branch `feat/traffic-phase-4-landing-sequencing`. **Depends on Phases 1–3.**
> Self-contained: a fresh session must be able to execute this milestone after Phases 1–3 are merged.

## Context & Goal

Realistic pattern and final sequencing: *"Number 2 to land, follow the Cessna on left base."* Refines the Phase-3 phase classifier with `Pattern` and `Final` detection. Detects runway occupancy and triggers go-around when traffic is on the runway.

## Codebase Pointers (read first)

Read `CLAUDE.md`. Confirm Phases 1–3 are merged.

| Path | What's there |
|---|---|
| `src/data/traffic_phase_classifier.{hpp,cpp}` | (Phase-3) classifier covering `OnGround/Taxi/Takeoff/Landed`. **Refine here**: add `Pattern` and `Final` branches based on alignment with active runway heading + airport pattern direction. |
| `src/core/xplane_context.hpp:32–45` | `RunwayInfo`/`RunwayEnd` (lat/lon/heading per end). `ctx.active_runway` is the user's current active runway string. |
| `src/core/xplane_context.hpp:140` | `find_nearby_airports(...)` — used to map a target's lat/lon → ICAO + runways. |
| `data/regions/eu/airport_vrps.json` | Per-airport pattern direction (`left` / `right`). Loaded by existing module — find it (likely `src/data/airport_vrps.{hpp,cpp}`) and reuse its lookup. |
| `src/atc/traffic_advisor.{hpp,cpp}` | (Phase-2+3) advisor framework. Add sequencing logic. |
| `src/atc/atc_state_machine.cpp` | The `ATCState` enum is still defined here (`APPROACH_CONTACT`, `PATTERN_ENTRY`, `LANDING_CLEARED`, `TOUCH_AND_GO_CLEARED` unchanged) so C++ state comparisons in the trigger keep working. **However**, the actual per-flow process() dispatch has moved into `src/atc/flows/pattern_flow.cpp` and `crosscountry_flow.cpp` — the `compute_landing_sequence` *call site* belongs in `pattern_flow.cpp` (Pattern-side states own landing) with the trigger keyed off `ATCState ∈ {PATTERN_ENTRY, LANDING_CLEARED, TOUCH_AND_GO_CLEARED}`. Approach-contact triggers (`XC/APPROACH_CONTACT`) live in `crosscountry_flow.cpp`. |
| `src/atc/flows/ground_operations.hpp` | Owns `ground_ops::build_vars(msg, ctx)` post-refactor. Extend this to populate `{N}`, `{type}`, `{position}`, `{runway}` from `SequenceResult`. |
| `data/regions/eu/atc_templates.json` | Add new templates under the qualified-name blocks (`Pattern/LANDING_CLEARED` for the sequencing variants, `TRAFFIC_DIALOG` for the go-around advisory). |

## Architecture Constraints

- **SDK-free sequencing**: pure function in `src/atc/traffic_advisor.cpp` (or split into `src/atc/landing_sequence.{hpp,cpp}` if file grows large).
- **Reuse existing airport API** (deviation #1 follow-through): map target lat/lon → ICAO + runway via `find_nearby_airports`.
- **No IFR sequencing** — VFR scope only.
- **No wake-spacing here** — that's Phase 5.
- **Sequencing flows through the main ATC dialog, not the traffic side-channel**: the `number_to_land_follow` line replaces (or augments) the normal `Pattern/LANDING_CLEARED` template response — pilot says "request landing" and ATC answers with the sequence number plus clearance. So `compute_landing_sequence` populates extra `ground_ops::build_vars()` placeholders consumed by the existing pilot-intent template path. **Do NOT route through `traffic_dialog`** — there's no separate ack to wait for; the pilot's normal readback covers it.
- **Go-around is render-only**: when the runway-occupied trigger fires, render the `go_around_traffic_runway` template via `atc_state_machine::render_traffic_advisory` and speak it without changing ATCState or invoking `traffic_dialog::on_advisory_emitted` — it's an urgent controller call, the pilot reacts by flying, not by speaking.
- **Flow-split refactor compatibility**:
  - Template `build_vars` lives in `ground_ops::build_vars` (`src/atc/flows/ground_operations.cpp`), **not** in `atc_state_machine.cpp`. Sequencing placeholders (`{N}`, `{type}`, `{position}`) are populated there.
  - Template state-block names are qualified: use `"Pattern/LANDING_CLEARED"` (not `"LANDING_CLEARED"`) and `"Pattern/PATTERN_ENTRY"` / `"Pattern/TOUCH_AND_GO_CLEARED"` for any new entry or `next_state` field. `TRAFFIC_DIALOG` stays unprefixed.
  - The `compute_landing_sequence` call site belongs in `src/atc/flows/pattern_flow.cpp` (Pattern owns landing). Approach-contact triggers go into `crosscountry_flow.cpp`. Avoid re-introducing landing-specific dispatch into `atc_state_machine.cpp`.

## Pattern / Final Refinement (extend classifier)

For a target near the user's destination airport (within 8 NM, below 3000 ft AGL), classify by alignment with **active runway heading** + `airport_vrps.json` pattern direction.

- `Final`: alignment of target track with active-runway heading is **±10°**, target descending (`vertical_speed_fpm < -200`), within 5 NM of threshold.
- `Pattern`: in airport vicinity (within 5 NM), on a leg compatible with declared pattern direction. Heuristic legs:
  - **Downwind**: target track ≈ active-runway heading **±180°** (opposite direction), abeam runway laterally, distance from threshold 1–3 NM.
  - **Base**: target track ≈ active-runway heading **±90°** (perpendicular), descending, between downwind and final.
  - **Crosswind**: target track ≈ active-runway heading **±90°** but on the upwind side of the threshold.
- Pattern **side** (left/right) determined by relative bearing of target from runway centerline vs. `airport_vrps.json` value.
- These leg sub-classifications are not stored in `TrafficPhase` (which stays at `Pattern`/`Final`); they're computed at sequencing time.

## Destination Airport (deviation #2 — open question)

Spec assumes destination tracking exists. **It doesn't** in `XPlaneContext`. Default approach for this phase:

- If user's ATC state ∈ `{APPROACH_CONTACT, PATTERN_ENTRY, LANDING_CLEARED, TOUCH_AND_GO_CLEARED}`: use `ctx.geometric_nearest_id` (geometric nearest, not the frequency-matched one) when within **15 NM**, else `ctx.nearest_airport_id`.
- Otherwise: skip sequencing (no destination known).
- Document this heuristic in PR description and surface as `Open Question for Maintainer`.

## Sequencing Logic

`compute_landing_sequence(traffic_ctx, user_state, runway_info, threshold_lat, threshold_lon) → SequenceResult`

```
struct SequenceResult {
  int user_position;             // 1 = first to land; 0 = no sequence (no traffic ahead)
  std::optional<TrafficTarget> follow_target;  // the aircraft directly ahead, if any
  bool runway_occupied;          // true if any target is on the runway centerline within 1500 m of threshold
  std::optional<TrafficTarget> occupant;       // the occupier, for messaging
};
```

Algorithm:
1. Build ordered list of all targets in `Final` for the user's active runway, sorted by distance-to-threshold ascending.
2. Insert user into the list by user's distance-to-threshold.
3. `user_position` = user's index + 1.
4. `follow_target` = the target directly ahead of user (index `user_position - 2` in zero-indexed list), or `nullopt` if user is first.
5. Runway occupancy: any target with `phase ∈ {OnGround, Takeoff, Landed}` whose position projects onto the active-runway centerline within 1500 m of threshold (uses pure-geometry helper to project lat/lon onto runway line segment).

## Phraseology (EU)

- *"[Callsign], number [N], follow the [type] on [position], cleared to land runway [rwy]."*
  - `[position]` = "left base" / "right base" / "downwind" / "final" — derived from leg classification of `follow_target`.
- *"[Callsign], continue approach, traffic on the runway."*
- *"[Callsign], go around, traffic on the runway, climb runway heading 3000 feet."*

## Acceptance Criteria

- [ ] `traffic_phase_classifier` extended with `Pattern` / `Final` branches (refines Phase-3 heuristic).
- [ ] Catch2 tests for the refinement: known target geometries → expected phase.
- [ ] `compute_landing_sequence(...)` pure function in `src/atc/traffic_advisor.cpp` (or new `src/atc/landing_sequence.{hpp,cpp}`).
- [ ] Runway-occupancy check helper in `src/data/traffic_geometry.{hpp,cpp}`: `is_on_runway_centerline(target_lat, target_lon, threshold_lat, threshold_lon, runway_heading_deg, length_m, max_lateral_m=30)`.
- [ ] Go-around trigger: when user is `LANDING_CLEARED` AND `runway_occupied = true` AND user within 1 NM of threshold → render `go_around_traffic_runway` via `atc_state_machine::render_traffic_advisory` and speak it (no ATCState change, no traffic_dialog hook).
- [ ] EU templates added to `data/regions/eu/atc_templates.json`:
  - `number_to_land_follow`: `"{callsign}, number {N}, follow the {type} on {position}, cleared to land runway {runway}."` — under the existing **`Pattern/LANDING_CLEARED`** state block (replaces the plain "cleared to land" response when sequencing applies). `next_state` qualified, e.g. `"Pattern/LANDING_CLEARED"`.
  - `continue_approach_traffic_runway`: `"{callsign}, continue approach, traffic on the runway."` — under **`Pattern/LANDING_CLEARED`** state block.
  - `go_around_traffic_runway`: `"{callsign}, go around, traffic on the runway, climb runway heading 3000 feet."` — under the `TRAFFIC_DIALOG` block (unprefixed; rendered without state transition).
- [ ] `ground_ops::build_vars()` (in `src/atc/flows/ground_operations.cpp`) populates `{N}`, `{type}`, `{position}` (leg name) from `SequenceResult`.
- [ ] Destination-airport heuristic documented in code comments + PR description.
- [ ] Reuse `airport_vrps.json` pattern-direction lookup (do not hardcode left/right).
- [ ] Catch2 tests in `tests/landing_sequence_test.cpp`:
  - 3 aircraft in pattern + user on final → user_position calculated correctly.
  - 1 target on runway, user on short final → go-around triggers.
  - User first (no traffic ahead) → no sequencing message (advisor returns `nullopt`).
  - Pattern-side classification (left vs. right) per `airport_vrps.json`.
- [ ] `tests/fixtures/traffic_pattern_lszh_3in_1on.json` — multi-aircraft fixture.
- [ ] `docs/traffic-smoke-test.md` updated with Phase-4 scenarios.

## Files to Create / Modify

**Create**:
- `src/atc/landing_sequence.hpp` (optional split from `traffic_advisor`)
- `src/atc/landing_sequence.cpp` (optional)
- `tests/landing_sequence_test.cpp`
- `tests/fixtures/traffic_pattern_lszh_3in_1on.json`

**Modify**:
- `src/data/traffic_phase_classifier.hpp/cpp` — `Pattern`/`Final` branches.
- `src/data/traffic_geometry.hpp/cpp` — `is_on_runway_centerline()`.
- `src/atc/traffic_advisor.hpp/cpp` — invoke `compute_landing_sequence()` at appropriate user states.
- `src/atc/flows/pattern_flow.cpp` — sequencing call site (gated on Pattern-side states); replaces the spec's older "lives in atc_state_machine.cpp" wording.
- `src/atc/flows/crosscountry_flow.cpp` — approach-contact-side trigger (when `XC/APPROACH_CONTACT`).
- `src/atc/flows/ground_operations.cpp` — `ground_ops::build_vars()` extensions (`{N}`, `{type}`, `{position}`). `atc_state_machine::build_vars` no longer exists.
- `data/regions/eu/atc_templates.json` — three new templates under the qualified blocks (`Pattern/LANDING_CLEARED`, `TRAFFIC_DIALOG`).
- `tests/traffic_phase_classifier_test.cpp` — refinement coverage.
- `tests/CMakeLists.txt` — add new test source.
- `docs/traffic-smoke-test.md`.

## Test Plan

**Unit (Catch2)**:
```bash
make test
```
Expected: `landing_sequence_test.cpp` covers position calculation, runway occupancy, go-around trigger, edge case (user first).

**Headless (atc_repl)**:
```bash
./build/atc_repl run tests/fixtures/traffic_pattern_lszh_3in_1on.json
```
Expected: scripted scenario where user enters pattern → "number 4, follow ..." → on short final → "go around, traffic on the runway".

**X-Plane Smoke**:
1. LSZH approach with LiveTraffic showing 2–3 aircraft in pattern.
2. Tune Tower, request landing.
3. Hear `"number N, follow the X on Y"`.
4. If a target lands and clears slowly while user is on short final → hear go-around.
5. Verify pattern-side ("left base" vs. "right base") matches LSZH's `airport_vrps.json` (left).

## Out of Scope

- IFR sequencing.
- Wake-turbulence spacing (Phase 5).
- Multi-runway parallel ops.

## Definition of Done

- `make all` clean.
- Sequencing fixture deterministic.
- X-Plane smoke: realistic sequence call with multi-target traffic.
- Engine OBJECT lib still SDK-free.

## Open Questions for Maintainer

- **Destination-airport tracking** (deviation #2): proposed heuristic is `geometric_nearest_id` when in approach/landing states + within 15 NM. Is a more authoritative source (flight plan, FMS, X-Plane FMS dataRef) available? If yes, would prefer to use it.
- **Pattern-leg classification thresholds**: ±10° for Final alignment, ±180°/±90° for downwind/base — boundaries are heuristic. Real-flight observation may suggest tightening.
- **Go-around 1 NM trigger**: aggressive go-around may surprise users. Should it be advisory ("traffic on the runway, go-around your discretion") rather than command? Default: command, per spec phraseology.

## PR-Reporting

1. **What works** —
2. **What was deferred** —
3. **What surprised you** —
4. **Smoke-test results** —
5. **Open questions for maintainer** —

---

Commit prefix: `[phase-4] <imperative summary>`.
