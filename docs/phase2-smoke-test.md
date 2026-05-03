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
