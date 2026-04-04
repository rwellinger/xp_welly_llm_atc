 Patterns | Confidence |
|---|---|---|
| `SELF_ANNOUNCE` | "traffic" AND (`is_towered_airport == false`) | 0.9 |
| `INITIAL_CALL` | "ground" OR "tower" OR "delivery" at start of transcript | 0.85 |
| `REQUEST_TAXI` | "taxi" OR "request taxi" OR "taxiing" | 0.9 |
| `READY_FOR_DEPARTURE` | "ready" AND ("departure" OR "takeoff" OR "holding short") | 0.9 |
| `REPORT_POSITION` | "downwind" OR "base" OR "final" OR "crosswind" OR "upwind" | 0.9 |
| `REQUEST_LANDING` | "inbound" OR "landing" OR "full stop" OR "touch and go" | 0.85 |
| `RUNWAY_VACATED` | "clear" AND "runway" OR "vacated" | 0.9 |
| `READBACK` | ends with a runway identifier (e.g. "two six", "one eight left") OR "wilco" OR "roger" | 0.75 |
| `REQUEST_FREQUENCY` | "frequency change" OR "switching" OR "w# MILESTONE M4 — Intent Parser

Read CLAUDE.md completely before starting.
M3 must be complete before this milestone begins.

## Goal

At the end of this milestone:
- Transcribed text is parsed into a `PilotIntent` with confidence score
- Extracted callsign and runway are displayed in ImGui
- No ATC response yet — parsing only
- All changes committed to git

---

## Task 1 — intent_parser

Implement rule-based intent detection in `intent_parser::parse()`:

```cpp
namespace intent_parser {
    PilotMessage parse(const std::string& transcript, const XPlaneContext& ctx);
}
```

**Approach:** lowercase the transcript, apply keyword/phrase rules in priority order. Return the first match with its confidence.

### Rules (implement all):

| Intent | Keywords /ith you" | 0.8 |
| `UNABLE` | "unable" | 0.95 |
| `UNKNOWN` | no rule matched | 0.0 |

**Callsign extraction:**
Look for patterns like "november", "hotel bravo", "alpha bravo" or alphanumeric sequences. Compare against `settings::pilot_callsign`. Store in `PilotMessage::callsign` if found.

**Runway extraction:**
Look for cardinal numbers ("runway two six", "runway one eight left/right/center"). Convert spoken numbers to runway identifier string. Store in `PilotMessage::runway`.

**Context weighting:**
- If `ctx.on_ground == false` and intent is `REQUEST_TAXI`: reduce confidence to 0.3
- If `ctx.is_towered_airport == false` and intent is `INITIAL_CALL` with "tower": reduce confidence to 0.4

---

## Task 2 — atc_session: Integrate Parser

After Whisper callback returns transcript:
- Call `intent_parser::parse(transcript, xplane_context::get())`
- Store `PilotMessage` in session
- Log intent and confidence via `XPLMDebugString`
- Transition to `IDLE` (state machine comes in M5)

---

## Task 3 — atc_ui: Show Parsed Intent

Update ImGui Status tab:
- Add section "Last Parsed Intent":
  - Intent enum name (e.g. "REQUEST_TAXI")
  - Confidence (e.g. "0.90")
  - Callsign if extracted
  - Runway if extracted
  - Raw transcript

---

## Task 4 — Verify

Test these spoken inputs and confirm correct intent detection:

| Spoken | Expected Intent |
|---|---|
| "Zurich Ground, HB-ABC, request taxi runway 28" | REQUEST_TAXI |
| "Tower, HB-ABC, holding short runway 28, ready for departure" | READY_FOR_DEPARTURE |
| "HB-ABC, on final runway 28" | REPORT_POSITION |
| "HB-ABC, clear of runway" | RUNWAY_VACATED |
| "Traffic, HB-ABC, downwind runway 28" (non-towered) | SELF_ANNOUNCE |
| "Roger, runway 28, cleared for takeoff, HB-ABC" | READBACK |

---

## Commit

```bash
git add -A
git commit -m "feat(M4): rule-based intent parser, callsign/runway extraction, context weighting"
```
