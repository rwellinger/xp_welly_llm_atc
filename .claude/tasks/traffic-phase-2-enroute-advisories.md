# Phase 2 — VFR En-Route Traffic Advisories

> Branch `feat/traffic-phase-2-enroute-advisories`. **Depends on Phase 1.**
> Self-contained: a fresh session must be able to execute this milestone after Phase 1 has been merged.

> **Implementation note (post-merge):** the shipped design diverges from this spec on the state-handling shape. After a real-flight test surfaced that `TRAFFIC_ADVISORY_PENDING` as a peer ATCState corrupts the main VFR flow (storms of repeat advisories due to a TTS-pending race; flow lost after Disregard; landing unrecoverable), we **decoupled traffic into a parallel side-channel** instead:
> - `TRAFFIC_ADVISORY_PENDING`, `previous_state_`, the `__PREVIOUS__` sentinel, and `emit_traffic_advisory` were **removed**.
> - New `src/atc/traffic_dialog.{hpp,cpp}` module owns dialog state (`IDLE`/`AWAITING_ACK`) parallel to `ATCState`.
> - New `atc_state_machine::render_traffic_advisory(vars, ctx)` renders the controller text **without** changing `ATCState`.
> - `engine::poll_traffic_advisory` calls `render_traffic_advisory` + `traffic_dialog::on_advisory_emitted`; pilot transcripts route through `try_traffic_dialog` first when `is_awaiting_ack()`.
> - JSON template block renamed from `TRAFFIC_ADVISORY_PENDING` to `TRAFFIC_DIALOG`; `next_state` values are unused (kept for schema compat).
> - Cooldown extended: per-target 60 s issued lockout + **5-min `kVisualAckLockoutSec`** after `TRAFFIC_IN_SIGHT` (`AdvisoryHistory::acknowledged_visual_secs`).
> - `Disregard` button now flow-aware (`atc_state_machine::disregard(ctx, phase)`): airborne near home airport → `PATTERN_ENTRY`, airborne in transit → `EN_ROUTE`, on ground → `IDLE`.
> - `tts_pending_` flag in `atc_session` closes the race that allowed the advisor to fire while async TTS was still synthesising.
> - `TrafficTarget::icao_type` populated from `sim/cockpit2/tcas/targets/icao_type` so the `{type}` placeholder reads "C172" / "PA28" instead of always "type unknown".
>
> Use `traffic_dialog` + `render_traffic_advisory` as the established pattern for any future advisor that **expects a voice ack with no flow consequence** (Phase 5c climbout crossing). Render-only (no dialog hook) for advisories that expect pilot **action** rather than voice (Phase 3 ground conflicts, Phase 4 go-around).

## Context & Goal

Controller issues realistic EU-phraseology traffic advisories during VFR cruise/transit when the user is in established ATC contact. Pilot acknowledges via voice (`"Traffic in sight"`, `"Negative contact"`, `"Looking"`); controller follows up appropriately. Trigger logic is deterministic (no LLM). Pilot intent parsing uses the existing rule-based parser with LLM fallback for low confidence.

## Codebase Pointers (read first)

Read `CLAUDE.md` end-to-end. Confirm Phase 1 is merged (`src/data/traffic_context.hpp` etc. exist).

| Path | What's there |
|---|---|
| `src/data/traffic_context.hpp` | (Phase-1 output) `TrafficTarget`, `TrafficContext`. |
| `src/data/traffic_geometry.{hpp,cpp}` | (Phase-1 output) bearing/distance/clock helpers. **Extend with direction-of-movement classifier here.** |
| `src/atc/intent_parser.hpp:28–57` | `PilotIntent` enum. Append three new values at the end (before final brace). |
| `src/atc/intent_parser.cpp` | Rule-based keyword/pattern matcher. Returns `PilotMessage{intent, confidence, callsign, runway, raw_transcript}`. Keywords for each intent live alongside existing matchers — extend that pattern. |
| `src/atc/atc_state_machine.hpp:31–43` | `ATCState` enum. **No sub-state mechanism exists** (deviation #3). Add `TRAFFIC_ADVISORY_PENDING` as a peer state. Add a `previous_state_` private member to `atc_state_machine.cpp` so we can restore on acknowledge. |
| `src/atc/atc_state_machine.hpp:66–68` | `build_vars(msg, ctx)` — extend to populate the new traffic placeholders (`{clock}`, `{distance}`, `{direction}`, `{altitude_info}`, `{type}`). |
| `src/atc/atc_templates.hpp:39–48` | `lookup(is_towered, state, intent_key)`, `valid_intents(is_towered, state)`, `fill(tmpl, vars)`, `get_prompt(key)`. |
| `data/regions/eu/atc_templates.json` | Top-level `towered`/`uncontrolled` → STATE → INTENT → `{response, next_state, requires_readback}`. Add `TRAFFIC_ADVISORY_PENDING` block + `_INVALID` fallback. |
| `data/atc_prompt_templates.json` | `gpt_classify_prompt` with `{valid_intents}` placeholder — `valid_intents` is computed at runtime, but you should add example phrasings for the three new intents to the prompt body. |
| `src/atc/engine.cpp` | (SDK-free orchestrator) — confidence threshold `0.7` lives here; LLM fallback path. Hook the advisor's emit-decision into the per-tick orchestrator output. |
| `src/atc/atc_session.cpp` | (plugin-only) PTT state machine. Coordinates pipeline. Advisor output should reach this layer for TTS playback. |

## Architecture Constraints

- **No LLM for trigger logic** — trigger decision is a deterministic pure function of `(TrafficContext, UserState, AdvisoryHistory)`.
- **SDK-free advisor**: `src/atc/traffic_advisor.{hpp,cpp}` lives in `xp_atc_engine` OBJECT lib. No `<XPLM*.h>`.
- **Provider-agnostic**: do not assume specific traffic provider behavior.
- **EU phraseology authoritative**: this phase is EU-only. US deferred unless trivial.
- **Towered-airport scope**: trigger context is towered airports + tuned to active ATC freq. CTAF deferred.

## EU Regulatory Gating (implement this)

Emit a traffic advisory only when **all** of:

- `current_atc_state != IDLE` AND user is on the active ATC frequency for that service.
- This is a deliberate simplification of EU regulation:
  - Class C/D (Tower/Approach contact) → automatic advisories.
  - Class E/G with FIS contact ("Information") → advisories on the same logic.
  - No contact → no advisories.

## Phraseology (EU, ICAO Annex 10 / EU 2020/469)

Standard form:
> *"[Callsign], traffic, [clock] o'clock, [distance] miles, [direction of movement], [altitude info], [type if known]."*

Examples:
- *"Hotel Bravo X-ray Yankee Zulu, traffic, 2 o'clock, 3 miles, opposite direction, indicating 4500 feet, type unknown."*
- *"HB-XYZ, traffic, 11 o'clock, 5 miles, crossing left to right, 1000 feet below, Cessna."*

Pilot responses (new intents):
- `TRAFFIC_IN_SIGHT` — *"Traffic in sight, HB-XYZ"*
- `TRAFFIC_NEGATIVE_CONTACT` — *"Negative contact, HB-XYZ"*
- `TRAFFIC_LOOKING` — *"Looking"* (treated like `NEGATIVE_CONTACT` for state, allows different follow-up text)

Controller follow-ups:
- After `TRAFFIC_IN_SIGHT`: *"[Callsign], roger, maintain visual separation."* → restore `previous_state_`.
- After `NEGATIVE_CONTACT`/`LOOKING`: *"[Callsign], roger, traffic now [updated clock], [updated distance]."* → re-issue **once** with updated data, then drop and restore `previous_state_`.

## Trigger Logic (deterministic)

Emit a traffic advisory when **all** of:
- ATC contact established (gating above).
- A target T satisfies:
  - `2.0 <= distance_to_user_nm <= 8.0`
  - `|altitude_diff_ft| <= 1500`
  - T in user's forward hemisphere (`clock_position` between 9 and 12, OR between 12 and 3 — i.e. clockwise-forward arc).
  - **Closure rate is positive** (compute from user + target velocity vectors) **OR** target is converging laterally.
  - T not already advised in last **60 seconds** (per `modeS_id`).
- Cooldown: max **one advisory every 20 seconds** globally.

Multi-target: nearest qualifying wins; deferred multi-advisory chains.

## Direction Classifier (extend `traffic_geometry`)

Pure function `classify_relative_track(user_track_deg, target_track_deg)` returning one of:
- `opposite direction` — angular diff 150°–210°
- `same direction` — angular diff 0°–30° or 330°–360°
- `crossing left to right` — target crossing user's path right-of-current → left-of-future ... actually: target moving from user's left side to right side; precise definition: bearing-from-user is decreasing angularly while distance is also affected by lateral velocity. **Simplification**: angular diff 60°–120° AND target bearing is on user's left (`clock_position 9–11.99`) → `crossing left to right`.
- `crossing right to left` — angular diff 240°–300° AND target bearing on user's right (`clock_position 0.01–3`) → `crossing right to left`. (Or symmetric to above.)
- `converging` — fallback when none of the above match cleanly but closure rate is positive.

**Document the angular ranges in code comments** (single line each) and unit-test the boundaries.

## Altitude-info Resolution

`altitude_info` placeholder rules:
- Mode-C present (target has valid `alt_msl_ft`): `"indicating {alt} feet"` (rounded to nearest 100).
- Otherwise, if relative altitude is more useful (target close in altitude, < 2000 ft diff): `"{n} feet above"` / `"{n} feet below"`.
- Otherwise (no usable altitude data): `"altitude unknown"`.

Pure function `format_altitude_info(target_alt_msl_ft, user_alt_msl_ft, has_mode_c)` in `traffic_geometry`.

## Acceptance Criteria

- [ ] New peer state `TRAFFIC_ADVISORY_PENDING` in `ATCState` enum (deviation #3).
- [ ] `previous_state_` private member added to `atc_state_machine.cpp`. Save before transition, restore on acknowledgment.
- [ ] `src/atc/traffic_advisor.{hpp,cpp}` (SDK-free) implements:
  - `struct AdvisoryHistory { map<uint32_t, double> last_issued_secs; double last_global_emit_secs; }`
  - `struct TrafficAdvisory { uint32_t modeS_id; std::map<std::string,std::string> vars; }`
  - `std::optional<TrafficAdvisory> evaluate(const TrafficContext&, const UserState&, const AdvisoryHistory&, double now_secs)` — pure function applying all triggers above.
  - `UserState` is a small struct local to advisor (atc_state, on_active_atc_freq, lat, lon, heading, alt, track, groundspeed); fill from `XPlaneContext` + state machine at the call site.
- [ ] `src/atc/intent_parser.cpp` extended with rule-based keyword matching for three new intents:
  - `TRAFFIC_IN_SIGHT`: keywords `traffic in sight`, `in sight`.
  - `TRAFFIC_NEGATIVE_CONTACT`: keywords `negative contact`, `no contact`.
  - `TRAFFIC_LOOKING`: keyword `looking` (when no other intent matches at higher confidence).
- [ ] `data/atc_prompt_templates.json` — `gpt_classify_prompt` body extended with example phrasings for the three new intents (the placeholder `{valid_intents}` is already runtime-filled).
- [ ] `data/regions/eu/atc_templates.json`:
  - For each state where advisories are valid (e.g. `EN_ROUTE`, `TOWER_CONTACT`, `APPROACH_CONTACT`, `PATTERN_ENTRY`): add a transition into `TRAFFIC_ADVISORY_PENDING` keyed under a synthetic intent like `_TRAFFIC_TRIGGER` (or however the advisor surfaces it — coordinate with `engine.cpp` so the synthetic intent is dispatched without going through the normal pilot-intent path).
  - New `TRAFFIC_ADVISORY_PENDING` state block:
    - `traffic_advisory` entry: `"{callsign}, traffic, {clock} o'clock, {distance} miles, {direction}, {altitude_info}, {type}."`
    - `TRAFFIC_IN_SIGHT` entry: `"{callsign}, roger, maintain visual separation."`, `next_state = <previous_state>` (handled by code, not template) → use sentinel like `__PREVIOUS__` and resolve in state machine.
    - `TRAFFIC_NEGATIVE_CONTACT` / `TRAFFIC_LOOKING` entry: `"{callsign}, roger, traffic now {clock} o'clock, {distance} miles."` (re-issue once)
    - `_INVALID` fallback.
- [ ] `build_vars()` extended to populate `{clock}, {distance}, {direction}, {altitude_info}, {type}` from a `TrafficAdvisory` payload. Type defaults to `"type unknown"` if no ICAO aircraft type known.
- [ ] Catch2 tests in `tests/traffic_advisor_test.cpp`:
  - One converging target → advisory triggers.
  - 60s per-target cooldown prevents duplicate.
  - 20s global cooldown.
  - User not in contact → no advisory.
  - Target behind user (clock 4–8) → no advisory.
  - Multi-target: nearest qualifying wins.
- [ ] Catch2 tests in `tests/traffic_geometry_test.cpp` (extend) — direction classifier boundary cases (149°, 150°, 210°, 211° etc.) + `format_altitude_info()` cases.
- [ ] `atc_repl` integration test: feed a fixture with one converging target + simulated established-contact state, verify advisory text matches expectation. Add to `make test` (not just `make repl`).
- [ ] `docs/phase2-smoke-test.md`: real-flight smoke-test plan — LSZH transit with LiveTraffic, expected behavior, what to log.

## Files to Create / Modify

**Create**:
- `src/atc/traffic_advisor.hpp`
- `src/atc/traffic_advisor.cpp`
- `tests/traffic_advisor_test.cpp`
- `docs/phase2-smoke-test.md`
- `tests/fixtures/traffic_advisory_converging.json` (one converging target near established-contact user)

**Modify**:
- `src/atc/intent_parser.hpp` — three new enum values.
- `src/atc/intent_parser.cpp` — keyword matching for the three new intents.
- `src/atc/atc_state_machine.hpp` — add `TRAFFIC_ADVISORY_PENDING` to `ATCState`.
- `src/atc/atc_state_machine.cpp` — `previous_state_` field, save/restore on enter/exit of `TRAFFIC_ADVISORY_PENDING`. Extend `build_vars()`.
- `src/atc/engine.cpp` — wire the advisor: per-tick check after every state-machine update; on advisory, dispatch a synthetic event into the state machine to enter `TRAFFIC_ADVISORY_PENDING` and emit the template-rendered text via the same response path as a pilot-driven response.
- `src/atc/atc_templates.cpp` — `__PREVIOUS__` sentinel resolution in `lookup()` (or in the state-machine call site).
- `src/data/traffic_geometry.hpp/cpp` — direction classifier + altitude-info formatter.
- `data/regions/eu/atc_templates.json` — new state block, new intent entries.
- `data/atc_prompt_templates.json` — example phrasings for new intents.
- `tests/CMakeLists.txt` — add `traffic_advisor_test.cpp`.

## Test Plan

**Unit (Catch2)**:
```bash
make test
```
Expected new tests: `traffic_advisor_test.cpp` (six trigger-logic cases) + extended `traffic_geometry_test.cpp` (direction + altitude classifier).

**Headless (atc_repl)**:
Add a scenario test that:
1. Loads `tests/fixtures/traffic_advisory_converging.json`.
2. Sets state to e.g. `EN_ROUTE` with active ATC freq tuned.
3. Ticks the engine once with simulated `now_secs`.
4. Expects ATC text matching pattern `^.*, traffic, \d+ o'clock, \d+ miles, .*$`.
5. Sends pilot transcript `"Traffic in sight"`.
6. Expects `"roger, maintain visual separation"` and state restored to `EN_ROUTE`.

**X-Plane Smoke** (`docs/phase2-smoke-test.md`):
1. Tune Zurich Information.
2. Establish contact.
3. Inject LiveTraffic target on converging path.
4. Hear advisory.
5. Respond `"Traffic in sight"`. Hear acknowledgment.
6. Repeat with `"Negative contact"` — hear re-issue with updated position, then silence/drop.

## Out of Scope

- Pattern/Ground/Final phase classification (Phase 3).
- US phraseology.
- CTAF.
- Multi-target sequential advisories (only nearest qualifying for now).
- Wake-cat anything (Phase 5).

## Definition of Done

- `make all` clean.
- atc_repl scenario produces expected advisory + acknowledgment text.
- `docs/phase2-smoke-test.md` exists and is reproducible.
- X-Plane smoke test pass: real LiveTraffic target near LSZH while on Zurich Information triggers advisory with correct geometry.
- Engine OBJECT lib still SDK-free.

## Open Questions for Maintainer

- **`__PREVIOUS__` sentinel**: is template-side or state-machine-side resolution preferred? Default to state-machine-side (resolve before template lookup) — simpler.
- **Synthetic intent dispatch**: how should the advisor inject its emit decision? Default proposal: a new helper method on `atc_state_machine` like `emit_traffic_advisory(advisory_vars)` that bypasses `intent_parser` and directly performs `lookup("TRAFFIC_ADVISORY_PENDING_ENTRY", "traffic_advisory") + fill(...)`. Document the chosen path in PR description.
- **Aircraft type source**: is `sim/cockpit2/tcas/targets/icao_type` available? If not (per provider), fall back to `"type unknown"`.

## PR-Reporting

1. **What works** —
2. **What was deferred** —
3. **What surprised you** —
4. **Smoke-test results** —
5. **Open questions for maintainer** —

---

Commit prefix: `[phase-2] <imperative summary>`.
