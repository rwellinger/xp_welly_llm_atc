# MILESTONE M2 — Touch and Go

Read CLAUDE.md completely before starting.
M1 (Template Engine + Traffic Pattern) must be complete before this milestone begins.

## Goal

At the end of this milestone:
- Touch-and-go clearances work in the traffic pattern
- Go-around procedure works from any approach phase
- Multiple pattern circuits supported (repeat without resetting to IDLE)
- Templates extended in `data/atc_templates.json`
- All changes committed to git

---

## Background

M1 established the full traffic pattern: taxi → takeoff → pattern → landing → vacated.
M2 adds two procedures that keep the pilot in the pattern:

1. **Touch and Go**: Pilot requests touch-and-go instead of full-stop landing. ATC clears for touch-and-go. After touchdown, pilot takes off again and re-enters the pattern.

2. **Go-Around**: At any point during approach (PATTERN_ENTRY or LANDING_CLEARED), pilot announces going around. ATC acknowledges and instructs to re-enter the pattern.

---

## New Intents

Add to `PilotIntent` enum and local parser rules:

| Intent | Keywords | Confidence |
|---|---|---|
| `REQUEST_TOUCH_AND_GO` | "touch and go", "request touch and go" | 0.90 |
| `GO_AROUND` | "going around", "go around", "missed approach" | 0.95 |

---

## New State

Add `TOUCH_AND_GO_CLEARED` to `ATCState` enum.

State flow:
```
PATTERN_ENTRY → REQUEST_TOUCH_AND_GO → TOUCH_AND_GO_CLEARED
TOUCH_AND_GO_CLEARED → REPORT_POSITION (after T&G) → PATTERN_ENTRY (re-enter pattern)
TOUCH_AND_GO_CLEARED → GO_AROUND → PATTERN_ENTRY

PATTERN_ENTRY → GO_AROUND → PATTERN_ENTRY
LANDING_CLEARED → GO_AROUND → PATTERN_ENTRY
```

---

## Template Additions (`data/atc_templates.json`)

Add to the `"towered"` section:

```json
"TOWER_CONTACT": {
    "REQUEST_TOUCH_AND_GO": {
        "response": "{callsign}, runway {runway}, cleared touch and go, wind {wind}.",
        "next_state": "TOUCH_AND_GO_CLEARED",
        "requires_readback": true
    }
},
"PATTERN_ENTRY": {
    "REQUEST_TOUCH_AND_GO": {
        "response": "{callsign}, runway {runway}, cleared touch and go, wind {wind}.",
        "next_state": "TOUCH_AND_GO_CLEARED",
        "requires_readback": true
    },
    "GO_AROUND": {
        "response": "{callsign}, roger, fly runway heading, climb and maintain pattern altitude, re-enter left downwind runway {runway}.",
        "next_state": "PATTERN_ENTRY",
        "requires_readback": true
    }
},
"TOUCH_AND_GO_CLEARED": {
    "REPORT_POSITION_DOWNWIND": {
        "response": "{callsign}, number one, runway {runway}, continue approach, report final.",
        "next_state": "PATTERN_ENTRY",
        "requires_readback": false
    },
    "REPORT_POSITION_BASE": {
        "response": "{callsign}, number one, runway {runway}, continue approach.",
        "next_state": "PATTERN_ENTRY",
        "requires_readback": false
    },
    "REPORT_POSITION_FINAL": {
        "response": "{callsign}, runway {runway}, cleared to land, wind {wind}.",
        "next_state": "LANDING_CLEARED",
        "requires_readback": true
    },
    "REQUEST_TOUCH_AND_GO": {
        "response": "{callsign}, runway {runway}, cleared touch and go, wind {wind}.",
        "next_state": "TOUCH_AND_GO_CLEARED",
        "requires_readback": true
    },
    "GO_AROUND": {
        "response": "{callsign}, roger, fly runway heading, re-enter left downwind runway {runway}.",
        "next_state": "PATTERN_ENTRY",
        "requires_readback": true
    },
    "_INVALID": {
        "response": "{callsign}, report your position.",
        "next_state": "TOUCH_AND_GO_CLEARED",
        "requires_readback": false
    }
},
"LANDING_CLEARED": {
    "GO_AROUND": {
        "response": "{callsign}, roger, fly runway heading, climb and maintain pattern altitude, re-enter left downwind runway {runway}.",
        "next_state": "PATTERN_ENTRY",
        "requires_readback": true
    }
}
```

---

## Files to modify

| File | Action |
|---|---|
| `data/atc_templates.json` | **MODIFY** — add touch-and-go + go-around templates |
| `src/intent_parser.hpp` | **MODIFY** — add REQUEST_TOUCH_AND_GO, GO_AROUND to enum |
| `src/intent_parser.cpp` | **MODIFY** — add keyword rules for new intents |
| `src/atc_state_machine.hpp` | **MODIFY** — add TOUCH_AND_GO_CLEARED to ATCState enum |
| `src/atc_state_machine.cpp` | **MODIFY** — handle new state + transitions (template-based from M1) |
| `CLAUDE.md` | **MODIFY** — update state machine diagram |

---

## Verify

### Touch and Go flow:

1. Complete M1 flow up to TOWER_CONTACT
2. PTT → "Hotel Bravo Lima Uniform Kilo, request touch and go runway two six"
   → Expect: "cleared touch and go, wind ..."
   → State: TOUCH_AND_GO_CLEARED
3. Perform touch and go
4. PTT → "Hotel Bravo Lima Uniform Kilo, left downwind runway two six, touch and go"
   → Expect: "continue approach, report final"
   → State: PATTERN_ENTRY
5. PTT → "Hotel Bravo Lima Uniform Kilo, final runway two six"
   → Expect: "cleared to land" (full stop this time)
   → State: LANDING_CLEARED

### Go-Around flow:

6. From PATTERN_ENTRY or LANDING_CLEARED:
   PTT → "Hotel Bravo Lima Uniform Kilo, going around"
   → Expect: "roger, fly runway heading, re-enter left downwind"
   → State: PATTERN_ENTRY
7. Continue pattern normally

### Multiple circuits:

8. Complete touch-and-go → pattern → touch-and-go → pattern → full stop landing
   → Verify state transitions are correct through all circuits

---

## Commit

```bash
git add -A
git commit -m "feat(M2): touch-and-go + go-around procedures with pattern re-entry"
```
