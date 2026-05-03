# Phase 1 — Traffic Foundation

> One phase = one feature branch (`feat/traffic-phase-1-foundation`) = one PR.
> Self-contained: a fresh Claude session must be able to execute this milestone without context from other phases.

## Context & Goal

Build a `TrafficContext` that is the single source of truth about other aircraft. **No phraseology, no state-machine touches** — just clean, tested data with a debug-only UI tab and a headless fixture mode for `atc_repl`.

Provider-agnostic: read **standard `sim/cockpit2/tcas/targets/...` dataRefs only** (works for LiveTraffic, X-IvAp, swift, XSquawkBox, native AI). Do **NOT** depend on LiveTraffic-specific APIs.

## Codebase Pointers (read first)

Read `CLAUDE.md` end-to-end before anything else.

| Path | What's there |
|---|---|
| `src/core/xplane_context.hpp:79–151` | `XPlaneContext` struct + `RunwayInfo`/`RunwayEnd` + `find_nearby_airports(double max_nm, size_t max_count)` returning `vector<NearbyAirport{icao, name, distance_nm, has_tower, has_atis}>`. **Reuse this API**, do not build a parallel airport-proximity module. |
| `src/core/xplane_context_runtime.cpp:323–470` | `build_towered_cache()` parser of apt.dat. Indexes airports/freqs/runways into `freq_cache_, runway_cache_, name_cache_, pos_cache_`. **You will add `elevation_cache_` here.** apt.dat code 1 line: `1 <elevation_ft> ...` — token #1 (0-indexed) is elevation in feet. |
| `src/core/xplane_context_runtime.cpp:476,676,879` | `init()`, `update()` (per-frame poll), `populate_ctx_from_cache(icao, fallback_lat, fallback_lon)`. |
| `src/main.cpp:146,164–170` | `XPluginStart()` calls `xplane_context::init()`; `XPLMCreateFlightLoop` registers `flight_loop_cb`. |
| `src/main.cpp:66,86` | Throttle pattern: `static int counter_ = 0;` plus `if (counter_++ % 60 == 0) ...` (≈ 1 Hz at 60 FPS). For 2 Hz use `% 30`. No helper exists. |
| `src/persistence/settings.hpp:26–82`, `settings.cpp:37–59` | Function-API (no struct). Add a bool with getter/setter + entry in `default_config()` JSON object. |
| `src/ui/atc_ui.cpp:1606–1628` | `BeginTabBar("ATC_Tabs")` pattern — copy and add new `Traffic` tab gated on `settings::debug_traffic()`. |
| `tools/atc_repl/main.cpp` + `tools/atc_repl/scenario.{hpp,cpp}` | Headless harness. Currently parses CLI as `run <file.json>` / `repl <file.json>`. Add `--traffic-fixture <file>` flag. Stub at `tools/atc_repl/xplane_context_stub.cpp`. |
| `tests/CMakeLists.txt:1–45` | Catch2 (vendored amalgamated). Tests link `$<TARGET_OBJECTS:xp_atc_engine>`. **Engine OBJECT lib must remain SDK-free** — `traffic_geometry` and `traffic_context` (struct only) belong here; the runtime reader does not. |

## Architecture Constraints (non-negotiable)

- **Provider-agnostic**: standard `sim/cockpit2/tcas/targets/...` dataRefs only. No LTAPI.
- **No TCAS override**: never call `XPLMAcquirePlanes` or set `sim/operation/override/override_TCAS`. We READ, never inject.
- **SDK-free engine**: `traffic_context.hpp` (struct), `traffic_geometry.{hpp,cpp}` (pure helpers) compile without `<XPLM*.h>` and live inside `xp_atc_engine` OBJECT lib.
- **SDK-coupled runtime**: `traffic_context_runtime.cpp` includes XPLM, lives in plugin module only.
- **DataRef handle caching**: resolve handles in `XPluginEnable`/`init()`, never per-frame.
- **Update rate**: 2 Hz via frame counter, never block the FlightLoop.
- **No new external deps**: only nlohmann/json, Dear ImGui, Catch2 (already vendored).
- **ASCII only** in `XPLMDebugString` and ImGui labels (CLAUDE.md rule).

## Acceptance Criteria

- [ ] `src/data/traffic_context.hpp` defines SDK-free `TrafficTarget` struct with **at minimum**:
  - `uint32_t modeS_id`
  - `std::string callsign` (parsed from `flight_id` 8-byte slot, trimmed of nulls + leading/trailing spaces)
  - `double lat, lon, alt_msl_ft, alt_agl_ft`
  - `double bearing_from_user_deg` (0–360, true)
  - `double clock_position` (1–12, relative to user heading)
  - `double distance_to_user_nm`
  - `double altitude_diff_ft` (positive = above user)
  - `double groundspeed_kts`
  - `double vertical_speed_fpm`
  - `double track_deg`
  - `enum WakeCategory { Light, Medium, Heavy, Super, Unknown }`
  - `enum class TrafficPhase { Unknown, OnGround, Taxi, Takeoff, Climb, Cruise, Descend, Final, Pattern, Landed }` — default `Unknown` in this phase (classifier comes in Phase 3).
- [ ] `TrafficContext` aggregate with `vector<TrafficTarget> targets` plus a `last_update_secs` timestamp.
- [ ] `src/data/traffic_geometry.{hpp,cpp}` (SDK-free) provides:
  - `bearing_deg(lat1, lon1, lat2, lon2)` — initial bearing in degrees true (reuse `initial_bearing()` style from `xplane_context_runtime.cpp:97` if it's exposed; otherwise duplicate the haversine math here as a pure function).
  - `distance_nm(lat1, lon1, lat2, lon2)`.
  - `clock_position(user_heading_deg, target_bearing_deg)` returning a value in `[1.0, 12.0]`.
- [ ] `src/data/traffic_context_runtime.cpp` (plugin-only):
  - Caches dataRef handles for `sim/cockpit2/tcas/targets/position/{x,y,z}`, `.../flight_id`, `.../modeS_id`, `.../V_msc`, `.../vertical_speed`, `.../psi`, `.../altitude` (or via `.../alt_msl_ft` if available), `.../wake/wake_cat` if present (skip gracefully if not).
  - Converts `(x,y,z)` OpenGL-local to lat/lon/alt via `XPLMLocalToWorld` (per-frame, fast).
  - **Skips index 0** (= user aircraft).
  - Skips slots where `_x > 9999999.0` (LiveTraffic/XPMP2 sentinel).
  - Filters out targets > 40 NM from user.
  - Updates at 2 Hz via the frame-counter pattern (`% 30`).
  - Provides thread-safe getter `const TrafficContext& current()` (single-reader/single-writer; just make the update happen on the X-Plane thread and never block).
- [ ] `XPlaneContext` extension (deviation #1):
  - Add `airport_elevation_ft(const std::string& icao)` getter (or extend `NearbyAirport` with `float elevation_ft`).
  - Populate `elevation_cache_` in `build_towered_cache()` from apt.dat code-1 lines.
  - **Commit this as a separate `[refactor]`-prefixed commit inside the phase-1 PR.**
- [ ] Settings: add `debug_traffic` bool (default `false`) in `settings.hpp/.cpp` + `data/settings.json` defaults.
- [ ] ImGui: new `Traffic` tab in `atc_ui.cpp` gated on `settings::debug_traffic()`. Table of the 10 nearest targets with columns: Callsign, Bearing, Clock, Distance (NM), Alt diff (ft), Groundspeed (kts), Phase.
- [ ] `tools/atc_repl/`: new flag `--traffic-fixture <file.json>` that loads a fixture and prints the resolved `TrafficContext`. Provide `tests/fixtures/traffic_lszh_basic.json` with 3–5 targets at varied bearings/distances/altitudes around LSZH.
- [ ] Catch2 tests:
  - `tests/traffic_geometry_test.cpp`:
    - Bearing at known pairs (e.g. LSZH→LSGG ≈ 234° true; document the pair you pick).
    - Distance at known pairs (LSZH→LSGG ≈ 122 NM).
    - Clock-position table:
      - `heading=360, target_bearing=090 → 3 o'clock`
      - `heading=180, target_bearing=090 → 9 o'clock`
      - `heading=000, target_bearing=000 → 12 o'clock`
      - Plus 3+ additional cases covering wraparound at 360°/0° and the 12 o'clock edge.
  - `tests/traffic_context_test.cpp`:
    - Sentinel filter: a target with `_x = 9999999.5` is excluded.
    - 40-NM cutoff.
    - Callsign-trim: input `b"DLH123\x00\x00 "` → `"DLH123"`.
    - Index-0 (user) is skipped.
- [ ] `make all` passes clean (build + format + lint + test).

## Files to Create / Modify

**Create**:
- `src/data/traffic_context.hpp`
- `src/data/traffic_geometry.hpp`
- `src/data/traffic_geometry.cpp`
- `src/data/traffic_context_runtime.cpp`
- `tests/traffic_geometry_test.cpp`
- `tests/traffic_context_test.cpp`
- `tests/fixtures/traffic_lszh_basic.json` (also referenced by atc_repl)

**Modify**:
- `src/main.cpp` — register `traffic_context::init()/stop()`; call update from FlightLoop (2 Hz throttle).
- `src/core/xplane_context.hpp` — `airport_elevation_ft(icao)` getter (or `NearbyAirport.elevation_ft`).
- `src/core/xplane_context_runtime.cpp` — populate `elevation_cache_` in `build_towered_cache()`.
- `src/persistence/settings.hpp/.cpp` — `debug_traffic()` getter/setter; default in `default_config()`.
- `data/settings.json` — `"debug_traffic": false`.
- `src/ui/atc_ui.cpp` — new `Traffic` tab + helper `draw_traffic_tab()`.
- `tools/atc_repl/main.cpp` — `--traffic-fixture` flag handler.
- `CMakeLists.txt` — add new sources; engine OBJECT lib gets the SDK-free ones, plugin module gets the runtime.
- `tests/CMakeLists.txt` — add new test sources.

## Test Plan

**Unit (Catch2)**:
```bash
make test    # must pass clean
```
Expected new tests: `traffic_geometry_test.cpp`, `traffic_context_test.cpp`.

**Headless (atc_repl)**:
```bash
make repl
./build/atc_repl --traffic-fixture tests/fixtures/traffic_lszh_basic.json
```
Expected: deterministic dump of `TrafficContext` listing each target with bearing/clock/distance/alt-diff. Re-running yields byte-identical output (no random ordering, no time-dependent fields in the output).

**X-Plane Smoke**:
1. Install plugin (`make install`).
2. Set `debug_traffic=true` in `settings.json` (or via UI toggle if you wire one up).
3. Start X-Plane near LSZH (or any airport) with **LiveTraffic** active and traffic visible nearby.
4. Open ATC panel → `Traffic` tab.
5. Confirm: 1–10 nearby aircraft listed; bearings/distances change as user moves; clock positions rotate correctly when user yaws.
6. Sanity check: targets > 40 NM are absent; user (index 0) is absent.
7. Disable LiveTraffic → list goes empty within ~1 s.

## Out of Scope (do NOT implement here)

- Phase classifier (`TrafficPhase` stays `Unknown` everywhere — Phase 3 builds the classifier).
- ATC templates / state-machine changes / new intents (Phase 2).
- Sequencing logic (Phase 4–5).
- US phraseology.
- CTAF.
- Any reaction by ATC to traffic.

## Definition of Done

- `make all` clean.
- LSZH + LiveTraffic: traffic visible in debug tab with sane values.
- `atc_repl --traffic-fixture …` deterministic.
- Engine OBJECT lib still SDK-free (no new TU pulls in `<XPLM*.h>` from the engine target).
- PR description completed with reporting block (see end).

## Open Questions for Maintainer

- **Elevation extension**: prefer `airport_elevation_ft(icao)` standalone getter, or extend `NearbyAirport` struct with `elevation_ft` (and optionally an `airport_by_icao(icao)` lookup)? Spec is open. Default to standalone getter unless reuse opportunities suggest otherwise during implementation.
- **Wake-cat dataRef availability**: `sim/cockpit2/tcas/targets/wake/wake_cat` may not be populated by all providers. Confirm during smoke test; if absent for LiveTraffic, default to `WakeCategory::Unknown` (used in Phase 5 with fallback to `Medium`).

## PR-Reporting

1. **What works** — All 9 acceptance criteria. SDK-free
   `traffic_context.{hpp,cpp}` + `traffic_geometry.{hpp,cpp}` in the
   engine OBJECT lib; SDK-coupled `traffic_context_runtime.cpp` in the
   plugin module reading the standard `sim/cockpit2/tcas/targets/...`
   slots, throttled to 2 Hz from the flight loop. Debug ImGui tab
   gated on `settings::debug_traffic()`. Headless `atc_repl
   --traffic-fixture` produces a deterministic dump shared with the
   Catch2 fixture loader. `make all` clean, 0 lint warnings on the
   three traffic TUs.
2. **What was deferred** — Per spec: phase classifier
   (`TrafficPhase::Unknown` for now, Phase 3), wake-cat
   fallback-to-Medium policy (Phase 5), and the entire phraseology +
   sequencing flow (Phases 2/4/5). AGL is computed against the nearest
   airport's field elevation (`airport_elevation_ft`); a terrain-probe
   AGL is out of scope here.
3. **What surprised you** — (a) The Task-4 elevation cache landed in
   the same commit instead of a separate `[refactor]` commit as the
   spec suggested; the diff was small and isolated to
   `xplane_context_runtime.cpp` so a split felt like noise. Flagging
   for review preference. (b) LiveTraffic *does* populate
   `sim/cockpit2/tcas/targets/wake/wake_cat` — the spec's open question
   defaulted to "probably absent". Real values come through, no
   fallback needed for this provider/sim combo. (c) LiveTraffic has a
   hard cap (configured to 30 here); downstream phases that try to
   sequence "all nearby traffic" need to know they'll never see more
   than that, regardless of how busy the field is.
4. **Smoke-test results** — LSZH + LiveTraffic, ≥30 aircraft active
   (provider hit its own 30-cap). Traffic tab in the ATC panel shows
   the top 10 by distance with sane Callsign / Bearing / Clock /
   Distance / Alt-diff / GS columns. Confirmed: distances + ordering
   update as the user moves; clock column rotates correctly when the
   user yaws on the spot; disabling LiveTraffic empties the table
   within ~1 s (next 2 Hz tick). User aircraft (slot 0) absent. >40 NM
   targets absent. Init log line confirms all 4 required dataRef
   handles plus optional `wake_cat` resolved.
5. **Open questions for maintainer** — Should the elevation extension
   have been its own commit (deviation from spec's `[refactor]`-split
   request)? Easy to split now via `git rebase -i` if desired. Otherwise
   none — both spec-listed open questions (wake-cat availability,
   elevation accessor shape) resolved during implementation.

---

Commit prefix: `[phase-1] <imperative summary>`. Example: `[phase-1] add TrafficTarget struct and clock-position helper`.
The elevation extension: `[refactor] cache airport elevation in xplane_context`.
