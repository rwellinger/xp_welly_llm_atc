# MILESTONE M3 — Cross-Country Flight (A → B)

Read CLAUDE.md completely before starting.
M2 (Touch and Go) must be complete before this milestone begins.

---

## Goal

At the end of this milestone:
- Complete VFR cross-country flow: departure at airport A → en-route → arrival at airport B
- Frequency change from tower before leaving airspace
- Airport context switches automatically based on nearest airport
- Inbound calls at destination airport work (already implemented via `INITIAL_CALL_INBOUND`)
- Templates extended in `data/atc_templates.json`
- All changes committed to git

---

## Background

M1+M2 cover all operations within a single airport's airspace. M3 extends the flow to cover a flight between two airports:

```
Airport A: Startup → Taxi → Takeoff → Departure Clearance
    → Frequency Change (REQUEST_FREQUENCY or LEAVING_FREQUENCY)
    → EN_ROUTE (no ATC contact)
    → Nearest airport changes → State resets to IDLE
Airport B: INITIAL_CALL_INBOUND → Pattern Entry → Landing → Taxi to Parking
```

Key challenges:
- The pilot changes frequencies mid-flight
- The nearest airport changes mid-flight → state must reset
- ATC identity changes (A Tower → B Tower)
- Auto-correction must not force PATTERN_ENTRY when pilot is departing cross-country

---

## Already Implemented (from M1/M2)

These features already exist and do NOT need to be added:

| Feature | Current Implementation |
|---|---|
| Inbound call intent | `INITIAL_CALL_INBOUND` in `PilotIntent` enum — keywords: "tower" + "inbound"/"landing" |
| Inbound template (towered) | `IDLE → INITIAL_CALL_INBOUND` → "enter {pattern_direction} downwind runway {runway}" → PATTERN_ENTRY |
| Frequency request intent | `REQUEST_FREQUENCY` in `PilotIntent` — keywords: "frequency change", "switching", "with you" |
| State-aware routing | `atc_state_machine::process()` validates frequency type vs state, routes intents correctly |
| Flight-phase preconditions | `INITIAL_CALL_INBOUND` restricted to airborne phases (CLIMB, PATTERN, FINAL_APPROACH, CRUISE) |
| Frequency validation | State machine resets to IDLE on invalid frequency/state combination |

---

## New Intent

Add to `PilotIntent` enum and parser rules:

| Intent | Keywords | Confidence |
|---|---|---|
| `LEAVING_FREQUENCY` | "leaving your frequency", "leaving frequency", "good day" (context: DEPARTURE_CLEARED state) | 0.85 |

Note: `REQUEST_FREQUENCY` already exists and handles "frequency change", "switching". Expand its keywords to also match "request frequency change" and "request to leave frequency".

The `LEAVING_FREQUENCY` intent covers the informal departure: pilot says "good day" or "leaving your frequency" without explicitly requesting permission.

---

## New State

Add to `ATCState` enum:

| State | Description |
|---|---|
| `EN_ROUTE` | Between airports, no active ATC contact |

No `DEPARTURE_FREQUENCY` intermediate state is needed — frequency change transitions directly from `DEPARTURE_CLEARED`.

---

## State Flow

### Departure (cross-country):
```
DEPARTURE_CLEARED → REQUEST_FREQUENCY → EN_ROUTE
DEPARTURE_CLEARED → LEAVING_FREQUENCY → EN_ROUTE
```

### Departure (pattern — existing, unchanged):
```
DEPARTURE_CLEARED → READBACK → PATTERN_ENTRY (silent, existing)
DEPARTURE_CLEARED → REPORT_POSITION_DOWNWIND → PATTERN_ENTRY (existing)
```

### En-route:
```
EN_ROUTE → (nearest airport changes) → IDLE (auto-reset in atc_session::update)
EN_ROUTE → (any PTT) → _INVALID response ("no ATC contact")
```

### Arrival at destination (existing):
```
IDLE → INITIAL_CALL_INBOUND → PATTERN_ENTRY → (M1 landing flow)
```

---

## Template Additions (`data/atc_templates.json`)

### Towered — Add to `DEPARTURE_CLEARED`:

```json
"REQUEST_FREQUENCY": {
    "response": "{callsign}, frequency change approved, good day.",
    "next_state": "EN_ROUTE",
    "requires_readback": false
},
"LEAVING_FREQUENCY": {
    "response": "{callsign}, good day.",
    "next_state": "EN_ROUTE",
    "requires_readback": false
}
```

### Towered — Add new `EN_ROUTE` state:

```json
"EN_ROUTE": {
    "_INVALID": {
        "response": "",
        "next_state": "EN_ROUTE",
        "requires_readback": false
    }
}
```

EN_ROUTE returns empty response (silent) — there is no ATC to talk to en-route in VFR.

### Uncontrolled — Add `INITIAL_CALL_INBOUND` to existing `IDLE`:

```json
"uncontrolled": {
    "IDLE": {
        "INITIAL_CALL_INBOUND": {
            "response": "Traffic, {airport}, {callsign} inbound, will report pattern entry.",
            "next_state": "IDLE",
            "requires_readback": false
        }
    }
}
```

---

## Modifications to Existing Logic

### 1. `intent_parser.hpp` — Add LEAVING_FREQUENCY

```cpp
enum class PilotIntent {
    // ... existing ...
    LEAVING_FREQUENCY,  // NEW
};
```

### 2. `intent_parser.cpp` — Keyword rules

Add `LEAVING_FREQUENCY` match function:
```cpp
static bool match_leaving_frequency(const std::string &t) {
    return contains(t, "leaving") ||
           (contains(t, "good day") && !contains(t, "tower")) ||
           contains(t, "signing off");
}
```

Add to intent rules array (before REQUEST_FREQUENCY, higher priority):
```cpp
{PilotIntent::LEAVING_FREQUENCY, 0.85f, match_leaving_frequency},
```

Expand `match_request_frequency` keywords:
```cpp
static bool match_request_frequency(const std::string &t) {
    return contains(t, "frequency change") || contains(t, "switching") ||
           contains(t, "with you") || contains(t, "request frequency") ||
           contains(t, "leave frequency") || contains(t, "leave the frequency");
}
```

Add to `intent_name()`, `intent_template_key()`, and `intent_from_key()`.

Flight-phase filtering: `LEAVING_FREQUENCY` should be airborne-only (same as `REQUEST_FREQUENCY`).

### 3. `atc_state_machine.hpp` — Add EN_ROUTE

```cpp
enum class ATCState {
    // ... existing ...
    EN_ROUTE,  // NEW — between airports, no ATC contact
};
```

### 4. `atc_state_machine.cpp` — Handle EN_ROUTE

- Add `EN_ROUTE` to `state_name()` and `state_from_name()`
- In `process()`: EN_ROUTE should return silent _INVALID (template handles this)
- In frequency validation (lines 308-328): EN_ROUTE should NOT be reset by frequency checks — it's intentionally off-frequency

### 5. `atc_session.cpp` — Airport change detection

Add to `update()`:

```cpp
static std::string last_airport_id;

const auto& ctx = xplane_context::get();
if (!last_airport_id.empty() &&
    ctx.nearest_airport_id != last_airport_id &&
    atc_state_machine::get_state() == ATCState::EN_ROUTE) {

    char log[256];
    std::snprintf(log, sizeof(log),
        "[xp_wellys_atc] Airport changed: %s -> %s, resetting ATC state\n",
        last_airport_id.c_str(), ctx.nearest_airport_id.c_str());
    XPLMDebugString(log);
    atc_state_machine::reset();
}
last_airport_id = ctx.nearest_airport_id;
```

Note: Only reset when in `EN_ROUTE` state. Other states should not be affected by airport changes (prevents accidental resets during pattern work near airport boundaries).

### 6. `data/flight_rules.json` — Auto-correction adjustment

The current auto-correction `DEPARTURE_CLEARED → on_airborne → PATTERN_ENTRY (5s)` must be preserved for pattern flights. No change needed — the pilot's explicit frequency change request (REQUEST_FREQUENCY or LEAVING_FREQUENCY) transitions to EN_ROUTE before the 5s auto-correction triggers, provided the pilot acts promptly.

However, add EN_ROUTE auto-correction for safety:

```json
"EN_ROUTE": {
    "on_ground": {
        "phases": ["TAXI", "GROUND_READY", "PARKED"],
        "next_state": "IDLE",
        "delay_sec": 3.0
    }
}
```

Add intent preconditions:

```json
"LEAVING_FREQUENCY": {
    "allowed_phases": ["CLIMB", "PATTERN", "CRUISE"],
    "rejection_ground": "{callsign}, unable, you are still on the ground."
}
```

Add intent frequency entry:

```json
"LEAVING_FREQUENCY": ["TOWER"]
```

Add pilot phraseology:

```json
"LEAVING_FREQUENCY": "{callsign}, leaving your frequency, good day"
```

### 7. `CLAUDE.md` — Update state diagram

Update the state machine section to include EN_ROUTE:

```
DEPARTURE_CLEARED → FREQUENCY_CHANGE → EN_ROUTE → (airport change) → IDLE
```

Replace `FREQUENCY_CHANGE → IDLE` with `REQUEST_FREQUENCY/LEAVING_FREQUENCY → EN_ROUTE`.

---

## Files to Modify

| File | Action |
|---|---|
| `data/atc_templates.json` | **MODIFY** — add REQUEST_FREQUENCY + LEAVING_FREQUENCY to DEPARTURE_CLEARED, add EN_ROUTE state, add uncontrolled INITIAL_CALL_INBOUND |
| `src/intent_parser.hpp` | **MODIFY** — add LEAVING_FREQUENCY to enum |
| `src/intent_parser.cpp` | **MODIFY** — add LEAVING_FREQUENCY keywords, expand REQUEST_FREQUENCY keywords |
| `src/atc_state_machine.hpp` | **MODIFY** — add EN_ROUTE to enum |
| `src/atc_state_machine.cpp` | **MODIFY** — handle EN_ROUTE in state_name/state_from_name, frequency validation bypass |
| `src/atc_session.cpp` | **MODIFY** — airport change detection in update() |
| `data/flight_rules.json` | **MODIFY** — add EN_ROUTE auto-correction, LEAVING_FREQUENCY preconditions/frequency/phraseology |
| `CLAUDE.md` | **MODIFY** — update state diagram + intent list |

---

## Verify

### Departure flow (cross-country):

1. Complete M1 flow: ground contact → taxi → tower contact → takeoff clearance → readback
   → State: DEPARTURE_CLEARED
2. PTT → "Hotel Bravo Lima Uniform Kilo, leaving your frequency, good day"
   → Expect: "{callsign}, good day."
   → State: EN_ROUTE
3. Verify: no further ATC responses while in EN_ROUTE

### Departure flow (pattern — regression test):

4. Complete M1 flow through takeoff → readback
   → State: DEPARTURE_CLEARED
5. Wait 5 seconds (auto-correction) OR PTT → "downwind runway 28"
   → State: PATTERN_ENTRY (existing behavior preserved)

### En-route + airport change:

6. After reaching EN_ROUTE, fly away from airport A
7. When nearest airport changes → verify log: "Airport changed: {A} -> {B}"
8. State should reset to IDLE
9. PTT during EN_ROUTE (before airport change) → expect silent response (no ATC)

### Arrival at airport B:

10. After state resets to IDLE at airport B
11. PTT → "Bern Tower, Hotel Bravo Lima Uniform Kilo, ten miles south, inbound for landing"
    → Expect: "enter {pattern_direction} downwind runway {rwy}, report midfield downwind"
    → State: PATTERN_ENTRY
12. Continue with M1 pattern flow: downwind → base → final → landing → vacated

### Full cross-country:

13. Complete full flow: LSZG → LSZB (or similar short route)
    - Ground contact, taxi, takeoff at LSZG
    - Frequency change → EN_ROUTE
    - Airport change detected
    - Inbound call, pattern, landing at LSZB
    → Verify all state transitions through the entire flight

### Uncontrolled arrival:

14. Arrive at uncontrolled airport, state IDLE
15. PTT → "Traffic, {airport}, inbound for landing"
    → Expect: self-announce acknowledgement
    → State: IDLE (no state change at uncontrolled)

### Edge cases:

16. PTT during EN_ROUTE → expect silent/no response
17. "Good day" while in PATTERN_ENTRY → should NOT trigger LEAVING_FREQUENCY (wrong state, no template)
18. Frequency change within 5 seconds of takeoff → should beat auto-correction timer
19. REQUEST_FREQUENCY from IDLE → should NOT transition to EN_ROUTE (no template in IDLE for this)
20. Build verification: `make build` compiles without errors

---

## Commit

```bash
git add -A
git commit -m "feat(M3): cross-country flight with frequency changes and airport transitions"
```
