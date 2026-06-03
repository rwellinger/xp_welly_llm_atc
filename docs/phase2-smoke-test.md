# Phase 2 — VFR En-Route Traffic Advisories: Smoke Test

This is the live in-sim verification step for Phase 2. The unit + scenario tests
cover every deterministic branch (`make test`); this doc covers what `make test`
cannot — the X-Plane ↔ LiveTraffic ↔ plugin loop and the audio path.

Branch under test: `feat/traffic-phase-2-enroute-advisories`.

## Prerequisites

- `make all` clean on the branch (engine OBJECT lib SDK-free, all tests green).
- LiveTraffic installed and active (other providers untested in Phase 2).
- All three local models downloaded (Whisper, Llama, Piper) — check the in-sim
  Models tab. Without TTS the controller can't speak the advisory.
- Audio output routed to your headset (Settings → Sound → ATIS / ATC bus).
- A test aircraft of your choice. Cessna 172 default scenery is fine.

## Scenario 1 — LSZH transit, head-on advisory

Goal: a real LiveTraffic target appears in the forward arc, advisor fires once,
pilot acknowledges, state restores.

1. Set X-Plane location to ~5 NM south of LSZH at 1500 ft MSL, heading 360.
2. Start the engine, taxi/take off is not required — set the aircraft airborne
   in flight or use Custom Scenery `Set Position`.
3. Tune COM1 to **Zurich Tower 118.100**. Confirm the plugin's Frequencies panel
   shows `freq_type = TOWER`. Confirm Status panel ATCState advances past `IDLE`
   on first call (e.g. *"Zurich Tower, Hotel Bravo X-ray Yankee Zulu, transit
   from south."*).
4. Wait for LiveTraffic to inject a target inbound from the north. Watch the
   `xp_wellys_atc` log lines:
   - `traffic_context update: <N> targets`
   - on trigger: `Engine emitted traffic advisory (target_id=...)`
5. Expected speech (TTS): something like *"X-ray Yankee Zulu, traffic, 12
   o'clock, 5 miles, opposite direction, indicating 4500 feet, type unknown."*
6. Reply with **PTT: "Traffic in sight, HB-XYZ"**.
7. Expected speech: *"X-ray Yankee Zulu, roger, maintain visual separation."*
8. Confirm Status panel ATCState restores to whatever it was before the
   advisory (probably `TOWER_CONTACT` or `EN_ROUTE`).

## Scenario 2 — same target, pilot does not see traffic

Repeat Scenario 1 steps 1–5, then:

6. Reply with **PTT: "Negative contact, HB-XYZ"**.
7. Expected speech: *"X-ray Yankee Zulu, roger, traffic now N o'clock, M
   miles."* — clock + distance reflect the *current* geometry of the same
   target, not the geometry at first issue.
8. Confirm ATCState restored.
9. Wait ~30 s. Confirm the advisor does **not** re-fire on the same target —
   per-target cooldown is 60 s.

## Scenario 3 — cooldown sanity

1. Repeat Scenario 1 to fire one advisory.
2. Within 20 s, deliberately position another LiveTraffic target into the
   forward arc (or just wait for one).
3. Confirm the advisor stays silent — global 20 s cooldown is in force.

## What to log if anything misfires

When something looks wrong, capture:

- The plugin log lines around the time of the misfire. The advisor is verbose
  on emit (target_id, geometry vars).
- The TCAS slot dump (atc_repl `--traffic-fixture` round-tripped against a
  saved snapshot is the offline equivalent — log lines `traffic_context init`
  + `update` show what the runtime reader saw).
- The ATCState transitions printed by `atc_state_machine` (every transition
  logs `ATC state: <prev> -> <next>`).

If the advisor does **not** fire when you expect one:
- Confirm `atc_state != IDLE` — the gate refuses to fire while the pilot has
  not made first contact.
- Confirm `frequency_type` is one of `TOWER / GROUND / APPROACH`. UNKNOWN /
  UNICOM / CTAF / ATIS all gate out by design.
- Confirm the target's `clock_position` is in the forward arc (1, 2, 3, 9, 10,
  11, 12). 4–8 are deliberately suppressed.
- Confirm `2.0 ≤ distance_nm ≤ 8.0` and `|altitude_diff_ft| ≤ 1500`.

## Out of scope for this smoke test

- Multiple simultaneous advisories (Phase 2 emits the nearest qualifying
  target; chains are deferred).
- US phraseology (Phase 2 is EU-only).
- CTAF / Unicom advisories (deferred, not in Phase-2 scope).
- Wake-cat phraseology (Phase 5).

## Pass criteria

The phase is signed off when all three scenarios above produce the expected
speech + state transitions on a LiveTraffic-driven flight, and the cooldowns
behave as documented.

---

# Phase 3 — Ground / Taxi Conflict Advisories: Smoke Test

Branch under test: `feat/traffic-phase-3-ground-taxi`.

Phase-3 extends the Phase-2 advisor with a surface-safety side branch. It
fires *only* when the user's flight phase is `TAXI` (see
`src/atc/flight_phase`) and a nearby traffic target is itself taxiing /
taking off / just landed on an intersecting path. Ground-conflict callouts
do **not** require ATC contact (no Tower/Ground frequency needed) and do
**not** expect a voice acknowledgement — the pilot reacts by stopping or
giving way, not by speaking.

## Scenario 1 — taxi conflict, hold position

1. Park at LSZH with engine running. Cold-and-dark is fine after start.
2. Begin taxiing at ~10 kts towards the runway.
3. As another LiveTraffic ground aircraft crosses ahead of you within
   ~550 m and inside ±30° of your nose, expect speech:
   - *"<callsign>, hold position, traffic crossing."*
4. Stop. Wait for the other aircraft to clear. The advisor does NOT
   re-fire on the same target for 30 s.

## Scenario 2 — taxi conflict, side caution

1. Same setup as Scenario 1, but pick a position where another
   aircraft is taxiing abeam your side (e.g. on a parallel taxiway,
   clock 2 or clock 10).
2. Expect:
   - slow target → *"<callsign>, caution, aircraft taxiing on your left."*
     (or *right*)
   - faster target on a converging side path →
     *"<callsign>, give way to the [type] approaching from your right."*

## Scenario 3 — ground-conflict global cooldown

1. Trigger one ground advisory.
2. Within 15 s, deliberately position into another conflict. The advisor
   stays silent — global cooldown is 15 s for ground events (vs 20 s
   for airborne).
3. After 16 s, the next conflict fires.

## Deterministic round-trip via atc_repl

```bash
./build/atc_repl --traffic-fixture tests/fixtures/traffic_taxi_conflict.json
```

Expected: Target A (TAXI1) shown at `clk=12 dist=0.1` with `phase=Taxi`;
Target B (PARK1) shown at `clk=3 dist=0.3` with `phase=OnGround`. The
ground-conflict trigger inside `traffic_advisor::evaluate()` selects
Target A and renders `taxi_hold_position` — covered end-to-end by the
Catch2 test `advisor: taxiing user, crossing target -> hold_position`
inside `tests/test_traffic_advisor.cpp`.

## What to log if anything misfires

- Plugin log lines around the time of the misfire. The advisor logs
  `Engine emitted traffic advisory (target_id=..., template=taxi_*)` on
  every ground fire — confirm the template key matches expectation.
- The per-target `TrafficPhase` field. If a target is `Unknown`, the
  classifier rejected it (alt_agl missing? speed in a no-rule band?).

If the advisor does **not** fire when you expect one:
- Confirm the user's flight phase is `TAXI` — not `PARKED` or
  `TAKEOFF_ROLL`. Phase 3 only triggers in the strict TAXI band.
- Confirm the target's `phase` is `Taxi`, `Takeoff`, or `Landed`. A
  parked `OnGround` target is never a conflict (not moving).
- Confirm the target is within 0.3 NM and inside the ±30° / 200 m cone
  forward of the user's nose.

## Out of scope for this phase

- Specific taxiway names (no taxiway topology — known limitation).
- Runway-incursion detection (Phase 5).
- Refined classifier states `Climb/Cruise/Descend/Final/Pattern` (Phase 4).

## Pass criteria

Three scenarios produce the expected `taxi_*` template responses with no
duplicate fires inside the per-target / global cooldown windows, and the
deterministic `atc_repl` round-trip on `traffic_taxi_conflict.json`
fires exactly once on Target A.
