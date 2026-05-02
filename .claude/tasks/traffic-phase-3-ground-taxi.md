# Phase 3 — Ground / Taxi Traffic

> Branch `feat/traffic-phase-3-ground-taxi`. **Depends on Phase 1 + 2.**
> Self-contained: a fresh session must be able to execute this milestone after Phases 1 + 2 are merged.

## Context & Goal

Warn the user about other aircraft on the airport surface. Introduces the **Traffic Phase Classifier** (used in this and later phases). Trigger ground-conflict advisories when user is taxiing and another target's projected path intersects the user's heading cone.

## Codebase Pointers (read first)

Read `CLAUDE.md`. Confirm Phases 1 + 2 are merged.

| Path | What's there |
|---|---|
| `src/data/traffic_context.hpp` | (Phase-1) `TrafficTarget.alt_agl_ft`, `groundspeed_kts`, `vertical_speed_fpm`, `track_deg`, `enum class TrafficPhase`. |
| `src/data/traffic_context_runtime.cpp` | (Phase-1) populates `alt_msl_ft`. **For `alt_agl_ft` you need elevation lookup via the existing API**: `xplane_context::airport_elevation_ft(icao)` (Phase-1 extension). Map target lat/lon → nearest airport via `find_nearby_airports(...)` to get the icao, then subtract elevation. |
| `src/atc/traffic_advisor.{hpp,cpp}` | (Phase-2) advisor framework. Add ground-conflict logic gated on user phase = `Taxi`. |
| `src/atc/traffic_dialog.{hpp,cpp}` | (Phase-2) parallel side-channel for advisories that **expect a voice ack** (TRAFFIC_IN_SIGHT etc.). Ground-conflict advisories are *not* ack-driven — the pilot reacts by stopping/giving way, not by speaking. **Do NOT route ground conflicts through traffic_dialog**; render-only via `atc_state_machine::render_traffic_advisory`. |
| `src/atc/flight_phase.hpp/.cpp` | Existing **user-aircraft** flight-phase detector (`PARKED, TAXI, ...`). The user's `Taxi` phase comes from here. Do NOT duplicate the logic for traffic; build the new classifier independently. |
| `src/data/traffic_geometry.hpp/.cpp` | (Phase-1+2) extend with heading-cone intersect helper. |
| `data/regions/eu/atc_templates.json` | Add `taxi_*` entries under the `TRAFFIC_DIALOG` block (synthetic, like Phase 2's `traffic_advisory` entry — no ATCState transition). |

## Architecture Constraints

- **SDK-free classifier**: `traffic_phase_classifier.{hpp,cpp}` lives in `xp_atc_engine` OBJECT lib.
- **Provider-agnostic**: don't depend on per-provider phase fields (some providers may set them; we ignore and classify ourselves).
- **No taxiway topology**: known limitation. **Do not fake taxiway names.** Phraseology stays generic ("traffic crossing", "taxiing on your left", "[type] approaching from your right").

## Phase Classifier (introduced here, used in Phases 4–5)

Heuristic, SDK-free, unit-testable. Pure function `classify(target, prev_phase) → TrafficPhase`:

- `OnGround`: `alt_agl_ft < 50` AND `groundspeed_kts < 5`.
- `Taxi`: `alt_agl_ft < 50` AND `5 ≤ groundspeed_kts < 40`.
- `Takeoff`: `alt_agl_ft < 200` AND `groundspeed_kts ≥ 40` AND `vertical_speed_fpm > 200`.
- `Landed`: previously airborne (`prev_phase ∈ {Final, Pattern, Cruise, Descend, Climb}`), now `alt_agl_ft < 50` AND `groundspeed_kts < 80`.
- (`Climb`, `Cruise`, `Descend`, `Final`, `Pattern` — left as `Unknown` here; Phase 4 refines.)

**Critical**: `alt_agl_ft` requires nearest-airport-elevation lookup. Use the existing `xplane_context` API. **Do not duplicate or reinvent airport-lookup logic.** If elevation isn't available for the target's nearest airport (cache miss), set `alt_agl_ft = 0` and `phase = Unknown` for that target.

## Phraseology

- *"[Callsign], hold position, traffic crossing."*
- *"[Callsign], caution, aircraft taxiing on your left."*
- *"[Callsign], give way to the [type] approaching from your right."*

Type defaults to `"aircraft"` if unknown.

## Trigger Logic

Emit a ground-conflict advisory when **all** of:
- User's flight phase = `Taxi` (from `src/atc/flight_phase`).
- A target T satisfies:
  - `T.phase ∈ {Taxi, Takeoff, Landed}`
  - `distance_to_user_nm < 0.3` (≈ 550 m)
  - T's projected path intersects the user's heading cone (±30°) within 200 m.
- Per-target cooldown: **30 s**.
- Global cooldown: **15 s**.

The "projected path intersects" check is a pure-geometry problem — extend `traffic_geometry` with `path_intersects_cone(user_lat, user_lon, user_heading, cone_half_deg, cone_dist_m, target_lat, target_lon, target_track, target_speed_kts, lookahead_secs)` returning `bool`. Lookahead defaults to ~20 s.

## Acceptance Criteria

- [ ] `src/data/traffic_phase_classifier.hpp/cpp` (SDK-free) with full Catch2 coverage of state transitions including the `prev_phase`-dependent `Landed` rule.
- [ ] `traffic_context_runtime.cpp` populates `alt_agl_ft` via the existing airport-elevation API. If elevation lookup misses, set `alt_agl_ft = 0` and `phase = Unknown`.
- [ ] `traffic_context_runtime.cpp` invokes the classifier per target per update tick; previous phase tracked per `modeS_id` in a small map (cleared when target disappears).
- [ ] Ground-conflict trigger added to `src/atc/traffic_advisor.cpp`, gated on user phase = `Taxi`.
- [ ] `src/data/traffic_geometry.{hpp,cpp}` extended with `path_intersects_cone(...)` pure function.
- [ ] EU templates added to `data/regions/eu/atc_templates.json` under the existing `TRAFFIC_DIALOG` block:
  - `taxi_hold_position`: *"{callsign}, hold position, traffic crossing."*
  - `taxi_caution`: *"{callsign}, caution, aircraft taxiing on your {side}."* (`{side}` = `"left"` or `"right"` from clock position)
  - `taxi_give_way`: *"{callsign}, give way to the {type} approaching from your {side}."*
  - Rendered via `atc_state_machine::render_traffic_advisory(vars, ctx)` — **no ATCState transition, no traffic_dialog::on_advisory_emitted** (ground conflicts don't require a voice ack). Ground-conflict path in `traffic_advisor` returns the rendered text directly to `engine::poll_traffic_advisory`'s caller.
- [ ] Catch2 tests:
  - `tests/traffic_phase_classifier_test.cpp` — table of (alt_agl, gs, vs, prev_phase) → expected phase.
  - `tests/traffic_advisor_test.cpp` (extend) — two aircraft taxiing on intersecting paths trigger `taxi_hold_position`. Cooldown prevents duplicates.
  - `tests/traffic_geometry_test.cpp` (extend) — `path_intersects_cone` with known geometry: target moving 90° to user's heading at 100 m on user's left → intersect; same target moving away → no intersect.
- [ ] `tests/fixtures/traffic_taxi_conflict.json` — fixture with user taxiing + 2 ground targets on intersecting paths.
- [ ] `docs/phase2-smoke-test.md` updated with Phase-3 ground scenarios.

## Files to Create / Modify

**Create**:
- `src/data/traffic_phase_classifier.hpp`
- `src/data/traffic_phase_classifier.cpp`
- `tests/traffic_phase_classifier_test.cpp`
- `tests/fixtures/traffic_taxi_conflict.json`

**Modify**:
- `src/data/traffic_context_runtime.cpp` — call classifier; populate `alt_agl_ft` via airport-elevation lookup.
- `src/data/traffic_geometry.hpp/cpp` — `path_intersects_cone`.
- `src/atc/traffic_advisor.hpp/cpp` — ground-conflict trigger logic.
- `src/atc/atc_state_machine.cpp` — `build_vars()` populate `{side}`, `{type}`.
- `data/regions/eu/atc_templates.json` — three new templates.
- `tests/traffic_advisor_test.cpp` — new ground-conflict cases.
- `tests/traffic_geometry_test.cpp` — `path_intersects_cone` cases.
- `tests/CMakeLists.txt` — add `traffic_phase_classifier_test.cpp`.
- `docs/phase2-smoke-test.md` (or rename to `docs/traffic-smoke-test.md` and add Phase-3 section).

## Test Plan

**Unit (Catch2)**:
```bash
make test
```
Expected: `traffic_phase_classifier_test.cpp` covers all rule branches. `traffic_advisor_test.cpp` includes ground-conflict triggers + cooldowns.

**Headless (atc_repl)**:
```bash
./build/atc_repl run tests/fixtures/traffic_taxi_conflict.json
```
Expected: deterministic `taxi_hold_position` or `taxi_caution` text, depending on geometry.

**X-Plane Smoke**:
1. Park at LSZH with engine running.
2. Begin taxi.
3. Inject (or wait for) a LiveTraffic ground target on an intersecting taxiway.
4. Hear `"hold position, traffic crossing"` or `"caution, aircraft taxiing on your left"`.
5. Verify cooldown: same target doesn't re-trigger within 30 s.

## Out of Scope

- Specific taxiway names (no taxiway topology data — known limitation).
- Runway-incursion detection (Phase 5).
- Refined classifier states `Climb/Cruise/Descend/Final/Pattern` (Phase 4).

## Definition of Done

- `make all` clean.
- Classifier unit-tested for all branches.
- atc_repl fixture deterministic.
- X-Plane smoke: ground-conflict advisory fires plausibly.
- Engine OBJECT lib still SDK-free.

## Open Questions for Maintainer

- **Elevation cache miss**: when target is in airspace with no nearby cached airport (e.g. en-route over remote area), the classifier degrades to `phase = Unknown`. Acceptable for Phase 3 since trigger requires user `Taxi` (which implies user at an airport, so target near user has same elevation context). Confirm.
- **`{type}` placeholder source**: resolved in Phase 2 — `TrafficTarget.icao_type` is populated from `sim/cockpit2/tcas/targets/icao_type` when the provider publishes it; falls back to `"aircraft"` here when empty.

## PR-Reporting

1. **What works** —
2. **What was deferred** —
3. **What surprised you** —
4. **Smoke-test results** —
5. **Open questions for maintainer** —

---

Commit prefix: `[phase-3] <imperative summary>`.
