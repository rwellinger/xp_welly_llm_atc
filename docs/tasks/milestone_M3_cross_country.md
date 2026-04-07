# MILESTONE M3 — Cross-Country Flight (A → B)

Read CLAUDE.md completely before starting.
M2 (Touch and Go) must be complete before this milestone begins.

## Goal

At the end of this milestone:
- Complete VFR cross-country flow: departure at airport A → en-route → arrival at airport B
- Frequency changes between ground/tower/departure/approach handled
- Inbound calls at destination airport work
- Airport context switches automatically based on nearest airport
- Templates extended in `data/atc_templates.json`
- All changes committed to git

---

## Background

M1+M2 cover all operations within a single airport's airspace. M3 extends the flow to cover a flight between two airports:

```
Airport A: Startup → Taxi → Takeoff → Departure
    → Frequency Change → En-Route (no ATC contact)
    → Frequency Change → Arrival
Airport B: Inbound Call → Pattern Entry → Landing → Taxi to Parking
```

Key challenges:
- The pilot changes frequencies multiple times
- The nearest airport changes mid-flight
- ATC identity changes (A Tower → B Tower)
- State must reset appropriately when changing airports

---

## New Intents

Add to `PilotIntent` enum and parser rules:

| Intent | Keywords | Confidence |
|---|---|---|
| `REQUEST_FREQUENCY_CHANGE` | "request frequency change", "request to leave frequency" | 0.85 |
| `LEAVING_FREQUENCY` | "switching frequency", "leaving your frequency", "good day" | 0.80 |
| `INBOUND_CALL` | "inbound", "inbound for landing", "request landing" + NOT already in pattern | 0.85 |

Note: `INBOUND_CALL` vs `REQUEST_LANDING` distinction:
- `INBOUND_CALL`: pilot is far from airport, first contact with tower → enters pattern
- `REQUEST_LANDING`: pilot is already in pattern, requesting final clearance

Detection heuristic: if state is IDLE and transcript contains "inbound" → `INBOUND_CALL`.
If state is PATTERN_ENTRY/TOWER_CONTACT → `REQUEST_LANDING`.

---

## New States

Add to `ATCState` enum:

| State | Description |
|---|---|
| `DEPARTURE_FREQUENCY` | After takeoff, still on tower frequency, requesting to leave |
| `EN_ROUTE` | Between airports, no active ATC contact |

State flow for departure:
```
DEPARTURE_CLEARED → READBACK → DEPARTURE_FREQUENCY
DEPARTURE_FREQUENCY → LEAVING_FREQUENCY → EN_ROUTE
EN_ROUTE → (pilot tunes new frequency) → IDLE (at new airport)
```

State flow for arrival:
```
EN_ROUTE / IDLE → INBOUND_CALL → PATTERN_ENTRY → (M1 landing flow)
```

### Automatic state reset

When the nearest airport changes (detected via `XPlaneContext::nearest_airport_id`):
- If state is `EN_ROUTE` → reset to `IDLE`
- Log: "Airport changed from {old} to {new}, resetting ATC state"

This is checked in `atc_session::update()` each flight loop.

---

## Template Additions (`data/atc_templates.json`)

Add to the `"towered"` section:

```json
"DEPARTURE_CLEARED": {
    "READBACK": {
        "response": "{callsign}, readback correct. When ready, report leaving the frequency.",
        "next_state": "DEPARTURE_FREQUENCY",
        "requires_readback": false
    }
},
"DEPARTURE_FREQUENCY": {
    "REQUEST_FREQUENCY_CHANGE": {
        "response": "{callsign}, frequency change approved, good day.",
        "next_state": "EN_ROUTE",
        "requires_readback": false
    },
    "LEAVING_FREQUENCY": {
        "response": "{callsign}, good day.",
        "next_state": "EN_ROUTE",
        "requires_readback": false
    },
    "_INVALID": {
        "response": "{callsign}, frequency change approved when ready, good day.",
        "next_state": "EN_ROUTE",
        "requires_readback": false
    }
},
"IDLE": {
    "INBOUND_CALL": {
        "response": "{callsign}, {airport} Tower, enter left downwind runway {runway}, report midfield downwind.",
        "next_state": "PATTERN_ENTRY",
        "requires_readback": true
    }
}
```

Also add inbound handling for uncontrolled airports:

```json
"uncontrolled": {
    "IDLE": {
        "INBOUND_CALL": {
            "response": "Traffic, {airport}, {callsign} inbound, will report pattern entry.",
            "next_state": "IDLE",
            "requires_readback": false
        }
    }
}
```

---

## Modifications to existing logic

### `atc_session.cpp` — Airport change detection

```cpp
static std::string last_airport_id_;

void update() {
    // ... existing playback check ...

    // Airport change detection
    const auto& ctx = xplane_context::get();
    if (!last_airport_id_.empty() &&
        ctx.nearest_airport_id != last_airport_id_ &&
        atc_state_machine::get_state() != ATCState::IDLE) {

        char log[256];
        std::snprintf(log, sizeof(log),
            "[xp_wellys_atc] Airport changed: %s -> %s, resetting ATC state\n",
            last_airport_id_.c_str(), ctx.nearest_airport_id.c_str());
        XPLMDebugString(log);
        atc_state_machine::reset();
    }
    last_airport_id_ = ctx.nearest_airport_id;
}
```

### `intent_parser.cpp` — State-aware intent detection

The parser needs the current ATC state to distinguish:
- `INBOUND_CALL` (state=IDLE, contains "inbound") vs `REQUEST_LANDING` (state=PATTERN_ENTRY)
- `LEAVING_FREQUENCY` (state=DEPARTURE_FREQUENCY) vs generic "good day"

Update the parse function signature:

```cpp
PilotMessage parse(const std::string& transcript,
                   const XPlaneContext& ctx,
                   ATCState current_state); // NEW parameter
```

---

## Files to modify

| File | Action |
|---|---|
| `data/atc_templates.json` | **MODIFY** — add departure/en-route/inbound templates |
| `src/intent_parser.hpp` | **MODIFY** — add new intents, update parse() signature |
| `src/intent_parser.cpp` | **MODIFY** — add keyword rules, state-aware detection |
| `src/atc_state_machine.hpp` | **MODIFY** — add DEPARTURE_FREQUENCY, EN_ROUTE states |
| `src/atc_state_machine.cpp` | **MODIFY** — handle new states + transitions |
| `src/atc_session.cpp` | **MODIFY** — airport change detection, pass state to parser |
| `CLAUDE.md` | **MODIFY** — update state diagram + intent list |

---

## Verify

### Departure flow:

1. Complete M1 flow: taxi → takeoff clearance → readback
   → State should advance to DEPARTURE_FREQUENCY (NOT directly to IDLE)
2. PTT → "Hotel Bravo Lima Uniform Kilo, leaving your frequency, good day"
   → Expect: "good day"
   → State: EN_ROUTE

### En-route:

3. Fly away from airport A
4. Nearest airport changes → verify log message "Airport changed"
5. State should reset to IDLE

### Arrival at airport B:

6. Tune airport B tower frequency
7. PTT → "Bern Tower, Hotel Bravo Lima Uniform Kilo, ten miles south, inbound for landing with information Bravo"
   → Expect: "enter left downwind runway {rwy}, report midfield downwind"
   → State: PATTERN_ENTRY
8. Continue with M1 pattern flow: downwind → base → final → landing → vacated

### Full cross-country:

9. Complete full flow: LSZG → LSZB (or similar short route)
   - Ground contact, taxi, takeoff at LSZG
   - Frequency change, en-route
   - Inbound call, pattern, landing at LSZB
   → Verify all state transitions through the entire flight

### Edge cases:

10. PTT during EN_ROUTE → Expect: _INVALID or "say again" (no ATC contact en-route)
11. "Inbound" while already in PATTERN_ENTRY → should be REQUEST_LANDING, not INBOUND_CALL
12. Frequency change without readback → should still work (DEPARTURE_FREQUENCY → _INVALID → freq change)

---

## Commit

```bash
git add -A
git commit -m "feat(M3): cross-country flight with frequency changes and airport transitions"
```
