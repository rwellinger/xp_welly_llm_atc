# Phase 1 — Status

> Branch: `feature/traffic_injection`
> Spec: `.claude/tasks/traffic-phase-1-foundation.md`
> Last commit: `63857c9 traffic-phase-1-foundation`
> Build: `make all` clean (0 lint warnings on traffic TUs).

## Done — all 9 tasks landed in `63857c9`

- [x] Task 1 — `src/data/traffic_context.{hpp,cpp}` — struct, enums,
  `trim_callsign`, `set_for_test`/`current` snapshot accessor.
- [x] Task 2 — `src/data/traffic_geometry.{hpp,cpp}` — pure haversine
  `bearing_deg`, `distance_nm`, `clock_position` (in `(0, 12]`).
- [x] Task 3 — `src/data/traffic_context_runtime.cpp` (plugin-only).
  Caches dataRef handles in `init()` for
  `sim/cockpit2/tcas/targets/position/{x,y,z}`, `modeS_id`, `flight_id`,
  `V_msc`, `vertical_speed`, `psi`, `wake/wake_cat` (graceful skip if
  null). `update()` reads all arrays once, skips slot 0, drops sentinel
  `>9999999.0` and `modeS_id==0`, converts via `XPLMLocalToWorld`,
  filters >40 NM, sorts by distance. AGL via
  `xplane_context::airport_elevation_ft(nearest_airport_id)`.
- [x] Task 4 — `airport_elevation_ft` + `airport_elevation_known` cached
  in `build_towered_cache()` from apt.dat code-1 token #1 (was already
  the chosen design — standalone getter, not on `NearbyAirport`). CLI
  stub at `tools/atc_repl/xplane_context_stub.cpp` returns `0.0f /
  false`.
- [x] Task 5 — `settings::debug_traffic()` + setter, default `false` in
  `default_config()`, `"debug_traffic": false` in `data/settings.json`.
- [x] Task 6 — Traffic ImGui tab in `src/ui/atc_ui.cpp:1556`
  (`draw_traffic_tab`), gated on `settings::debug_traffic()`. Columns:
  Callsign, Bearing, Clock, Dist (NM), Alt diff (ft), GS (kts), Phase.
  Top 10 by distance (already sorted at the runtime layer).
- [x] Task 7 — `tools/atc_repl/main.cpp` `--traffic-fixture <file>`.
  Loads via `tools/atc_repl/traffic_fixture.{hpp,cpp}` (shared with
  Catch2 tests), calls `traffic_context::set_for_test(...)`, prints a
  deterministic dump. Fixture: `tests/fixtures/traffic_lszh_basic.json`
  (5 raw targets around LSZH, FAR123 at ~50 NM exercises the cutoff).
- [x] Task 8 — Catch2:
  - `tests/test_traffic_geometry.cpp` — LSZH→LSGG distance + bearing,
    full clock-position table (incl. wraparound + 12 o'clock band edges),
    range invariants.
  - `tests/test_traffic_context.cpp` — `trim_callsign` (NUL term, lead/
    trail whitespace, full 8-byte non-terminated, empty), fixture loader
    enforces 40-NM cutoff, sort order, derived-field correctness for
    DLH123, `set_for_test` round-trip.
- [x] Task 9 — CMake wired:
  - Engine OBJECT lib gets `traffic_geometry.cpp` + `traffic_context.cpp`
    (SDK-free).
  - Plugin module gets `traffic_context_runtime.cpp`.
  - `tools/atc_repl/` gets `traffic_fixture.cpp` + extends
    `xplane_context_stub.cpp` with the elevation accessors.
  - `tests/CMakeLists.txt` adds both new test sources + the fixture
    loader; defines `XP_WELLYS_ATC_TEST_FIXTURES_DIR`.
  - `src/main.cpp:156,219` calls `traffic_context::init()/stop()`.
  - `src/main.cpp:96` throttles `traffic_context::update()` with
    `% 30` (≈ 2 Hz at 60 FPS).

## Verified

- `make build` — plugin + atc_repl + tests build, no warnings on the
  three traffic TUs.
- `make test` — Catch2 unit tests + 37 scenario tests all pass.
- `./build/atc_repl --traffic-fixture tests/fixtures/traffic_lszh_basic.json`
  produces a deterministic 4-target dump (FAR123 filtered as >40 NM,
  remaining targets sorted by distance, derived fields stable).
- `make lint` — 0 clang-tidy warnings touch traffic_*.cpp.

## Still open (acceptance-criteria item left to maintainer)

- [ ] X-Plane smoke test (LSZH + LiveTraffic active): confirm traffic
  appears in the debug tab, bearings/distances respond to user motion,
  clock rotates with yaw, >40 NM is dropped, disabling LiveTraffic
  empties the list within ~1 s. Spec mandates this before the PR is
  marked Definition-of-Done. **Not yet performed** — needs the X-Plane
  process and a traffic-injection plugin running, which the headless
  toolchain cannot stand in for.
- [ ] PR-Reporting block in `traffic-phase-1-foundation.md` (Section
  "PR-Reporting") — fill at PR submit, after the smoke test.

## Notes for the smoke test

- Wake-cat dataRef (`sim/cockpit2/tcas/targets/wake/wake_cat`) — the
  runtime tolerates null. LiveTraffic populates it inconsistently;
  expect Unknown for some/all rows. Phase 5 will decide the
  fallback-to-Medium policy.
- AGL is computed against the *nearest airport's* field elevation, not
  the target's overflown terrain. Good enough for pattern-range traffic;
  expect inflated values for transit traffic 30+ NM out at low cruise.
  Documented limitation, not a bug to chase.
