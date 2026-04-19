# Testing — scenario suite under `testscripts/`

`make test` builds `build/atc_repl` and replays every JSON scenario through the
real engine (intent_parser, atc_state_machine, atc_templates, flight_phase). GPT
fallback is disabled for tests, so all coverage exercises the rule-based path
end-to-end.

## How the test engine works

The headless CLI (`tools/atc_repl/`) is the test harness. It links the same
engine objects the plugin uses (`xp_atc_engine` CMake OBJECT library) but
replaces the X-Plane / Keychain dependencies with thin stubs so it can run on
any machine without X-Plane installed.

### Components

| Path                                         | Role                                                                                  |
|----------------------------------------------|---------------------------------------------------------------------------------------|
| `tools/atc_repl/main.cpp`                    | Entry point. Three modes: batch run, REPL seeded from a scenario, default LSZH REPL.  |
| `tools/atc_repl/scenario.{hpp,cpp}`          | JSON loader and runner. Parses `testscripts/*.json`, drives the engine, asserts.      |
| `tools/atc_repl/repl.{hpp,cpp}`              | Interactive REPL. Reads pilot transcripts on stdin, prints ATC responses on stdout.   |
| `tools/atc_repl/xplane_context_stub.cpp`     | Replaces the plugin's X-Plane DataRef reader with a single mutable `g_cli_ctx`.       |
| `tools/atc_repl/settings_stub.cpp`           | Replaces Keychain / plugin settings with env vars (`XP_ATC_DEBUG`, `XP_ATC_REGION`).  |

The engine itself (`src/engine/`, `src/intent_parser`, `src/atc_state_machine`,
`src/atc_templates`, `src/flight_phase`) is reused unmodified. No test-only
code paths exist in the engine — what the harness exercises is what the plugin
ships.

### What runs per scenario

For each `testscripts/*.json` file, `scenario::run` does the following:

1. **Region select.** Reads `region` from JSON (`EU` or `US`, default `EU`),
   updates `settings::set_flow_region`, then calls `atc_templates::reload()` and
   `flight_phase::reload()` so the next steps see the right
   `data/regions/<region>/{atc_templates,flight_rules}.json`.
2. **Callsign prime.** `settings::set_pilot_callsign_raw` is set to the
   scenario callsign so `intent_parser::matches_configured_callsign` accepts
   the transcript.
3. **Context load.** The JSON `context` block populates `g_cli_ctx`
   (airport, frequency, on-ground state, etc.). Defaults match a parked C172
   at LSZG on Ground 121.800.
4. **Engine reset.** `atc_state_machine::reset()` and `engine::reset()` zero
   out state and the profanity counter so scenarios cannot leak state between
   each other.
5. **Phase prime.** `flight_phase::update` is called 30 times with `dt=1.0s`
   to push past the ground-hysteresis window. Without this, intents like
   `REQUEST_TAXI` get rejected as "engines off / parked" on step 1.
6. **Step loop.** Each step:
   - applies any `set` field changes (with another 30 phase ticks to settle),
   - prints the optional `note`,
   - if `text` is non-empty, builds an `engine::Input` (with `quality` from the
     step or default `1.0f`) and calls `engine::process_transcript` synchronously
     (GPT fallback off → engine returns immediately),
   - asserts `expect` (case-insensitive substring match against the response)
     and `expect_state` (equality against `state_name(get_state())`).
7. **Result.** The `RunResult` (steps, assertions, mismatches) is returned.
   `main.cpp` aggregates across files into the JUnit-style summary.

### Differences from the plugin path

These are the only things the harness does not exercise:

- **GPT classification & fallback.** The CLI hard-codes
  `gpt_fallback_enabled=false`, so low-confidence intents go through the
  state machine's `_INVALID` template fallback. GPT call paths are not tested.
- **Whisper / TTS / audio.** No microphone capture, no MP3 playback. The
  pilot's transcript is taken straight from the JSON `text` field.
- **Real-time timing.** `flight_phase::update` is called with synthetic 1.0s
  ticks. Auto-correction delays (e.g. 5s after airborne) are reached by the
  set-field hysteresis priming, not wall-clock time.
- **X-Plane DataRefs.** No nav-aid lookups, no apt.dat parsing. Airport
  frequencies / runway selection are taken from the JSON `context` directly.

These trade-offs are deliberate: the suite is meant to be a fast, deterministic
regression net for the rule-based engine and the JSON-driven phraseology.
Anything that needs the network (GPT, Whisper, TTS) or hardware (audio, X-Plane)
is integration testing and lives outside `make test`.

## File-name convention

| Prefix       | Purpose                                                              |
|--------------|----------------------------------------------------------------------|
| `flow_NN_*`  | Happy-flow scenarios — pilot does the right thing, ATC responds.     |
| `bad_NN_*`   | Bad-case / error-handling scenarios — pilot makes a mistake.         |
| (legacy)     | A handful of unprefixed scripts predate the convention; kept for now.|

The runner enumerates files alphabetically, so `bad_*` rows appear above
`flow_*` rows in the summary.

## Scenario schema (relevant subset)

Each step in the `say` array supports:

- `text` — pilot transcript. Empty (omit) for set-only steps.
- `expect` — case-insensitive substring assertion against the ATC response.
- `expect_state` — assertion against `atc_state_machine::state_name(get_state())`
  after processing the step. Useful when the bad-case behaviour is "no state
  change" rather than a specific phrase.
- `quality` — float, mapped to `engine::Input.quality`. Defaults to `1.0f`.
  Values below `0.3f` trigger the `"say again"` short-circuit in
  `engine::process_transcript`.
- `set` — object of context fields applied **before** processing `text`. Common
  fields: `com`, `freq_type`, `on_ground`, `agl_ft`, `altitude_ft`,
  `groundspeed_kt`, `airport`, `runway`.
- `note` — printed before the step for log readability.

## Bad-case taxonomy (M5)

Each bad-case scenario is structured in two halves:

1. **Part A** — the pilot makes the mistake. Assertion verifies the controller
   reaction (and that no state corruption occurred via `expect_state`).
2. **Part B** — the pilot recovers (switches frequency, climbs out, retransmits
   clearly, etc.). Assertion verifies the flow continues normally.

| File                                              | Category                       | Engine path under test                                              |
|---------------------------------------------------|--------------------------------|---------------------------------------------------------------------|
| `bad_01_taxi_on_tower_eu.json`                    | Wrong frequency                | IDLE-state freq routing: REQUEST_TAXI on TOWER → "contact ground"   |
| `bad_02_ready_for_departure_on_ground_us.json`    | Wrong frequency (US)           | TAXI_CLEARED + READY_FOR_DEPARTURE on GROUND → "monitor Tower"      |
| `bad_03_no_position_on_initial_call.json`         | Missing position phrase        | `position_remark` variable inserted, taxi still cleared             |
| `bad_04_inbound_while_parked.json`                | Wrong flight phase             | `flight_phase::check_precondition` rejects INITIAL_CALL_INBOUND     |
| `bad_05_runway_vacated_while_airborne.json`       | Wrong flight phase             | precondition rejects RUNWAY_VACATED while airborne, state preserved |
| `bad_06_profanity_escalation.json`                | Radio discipline               | engine `profanity_warnings_` counter, no state mutation             |
| `bad_07_low_quality_transcript.json`              | Low Whisper quality            | `Input.quality < 0.3f` short-circuit to "say again"                 |
| `bad_08_self_correction.json`                     | ICAO "correction" phraseology  | intent_parser drops everything before the last "correction"         |

## Authoring rules

These are non-negotiable — they exist to keep the suite a real safety net:

1. **Never weaken an assertion to make it pass.** If `expect` mismatches the
   actual response, either fix the engine in a separate commit or document the
   gap. See M5 §4 for the original wording.
2. **Never bypass the engine in the harness.** No `reset:true` flags, no magic
   context shortcuts. The runner already calls `engine::reset()` between
   scenarios.
3. **Keep `expect_state` on bad cases that should NOT mutate state.** A passing
   substring match isn't enough proof when the correct behaviour is silence.
4. **Document deferred bad cases.** If an engine gap blocks a scenario, capture
   it here under "Known limitations" rather than deleting the test idea.

## Known limitations (deferred)

- **REQUEST_FLIGHT_FOLLOWING on TOWER**: the `intent_frequency` map in
  `flight_rules.json` lists APPROACH as the only valid frequency, but
  enforcement currently lives in `atc_ui.cpp` only. The state machine routes the
  intent through the IDLE template, which both responds with "Approach,
  squawk 4527" while the pilot is on TOWER and corrupts state to
  `APPROACH_CONTACT`. A bad-case scenario for this was drafted during M5 and
  removed pending engine work to push `intent_frequency` enforcement into
  `atc_state_machine` (M6 candidate).

## Running the suite

```bash
make test                                # full suite
./build/atc_repl run testscripts/bad_*.json   # bad cases only
./build/atc_repl run testscripts/bad_07_low_quality_transcript.json   # one file
./build/atc_repl repl testscripts/flow_01_eu_pattern_grenchen.json    # interactive REPL seeded from a scenario
```
