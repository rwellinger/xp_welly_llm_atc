# MILESTONE M5 — ATC State Machine + GPT Fallback

Read CLAUDE.md completely before starting.
M4 must be complete before this milestone begins.

## Goal

At the end of this milestone:
- ATC state machine processes `PilotIntent` and generates response text
- GPT-4o-mini fallback handles unknown/low-confidence intents
- Full text conversation is visible in ImGui transcript (no audio yet)
- All changes committed to git

---

## Task 1 — atc_state_machine

Implement the state machine as defined in CLAUDE.md.

```cpp
namespace atc_state_machine {
    void       init();
    void       stop();
    void       reset();                        // back to IDLE
    ATCState   get_state();
    ATCResponse process(const PilotMessage& msg, const XPlaneContext& ctx);
}
```

### Response Templates

Use the pilot's callsign (from `PilotMessage::callsign` if extracted, otherwise from `settings::pilot_callsign`) in all responses.

**IDLE → GROUND_CONTACT** (trigger: `INITIAL_CALL` targeting "ground" or "delivery"):
```
"[Callsign], [Airport] Ground, information [X], runway [active], QNH [qnh], taxi to holding point [runway] via [taxiway]."
```
Fill `[active]` from `XPlaneContext::nearest_airport_id` + context. Use `sim/weather/barometer_sealevel_inhg` DataRef for QNH. Taxiway = "Alpha" as default placeholder.

**GROUND_CONTACT → TAXI_CLEARED** (trigger: `REQUEST_TAXI`):
```
"[Callsign], taxi to holding point runway [runway] via [taxiway], QNH [qnh]."
```

**TAXI_CLEARED → TOWER_CONTACT** (trigger: `INITIAL_CALL` targeting "tower"):
```
"[Callsign], [Airport] Tower, runway [runway], hold short, number [n]."
```
Number = always "one" for simplicity.

**TOWER_CONTACT → DEPARTURE_CLEARED** (trigger: `READY_FOR_DEPARTURE`):
```
"[Callsign], runway [runway], cleared for takeoff, wind [dir] degrees [spd] knots."
```
Wind from `sim/weather/wind_direction_degt[0]` and `sim/weather/wind_speed_kt[0]`.

**DEPARTURE_CLEARED → IDLE** (trigger: `READBACK`):
```
"[Callsign], frequency change approved, good day."
```

**TOWER_CONTACT → PATTERN_ENTRY** (trigger: `REQUEST_LANDING` or `REPORT_POSITION` when airborne):
```
"[Callsign], number one, runway [runway], report final."
```

**PATTERN_ENTRY → LANDING_CLEARED** (trigger: `REPORT_POSITION` with "final"):
```
"[Callsign], runway [runway], cleared to land, wind [dir] degrees [spd] knots."
```

**LANDING_CLEARED → IDLE** (trigger: `RUNWAY_VACATED`):
```
"[Callsign], contact ground on [freq], good day."
```
Use 121.9 as default ground freq placeholder.

**UNICOM_ACTIVE** (trigger: `SELF_ANNOUNCE`, non-towered airport):
No clearances. Respond with traffic awareness only:
```
"Traffic in the area, [Callsign] is [position]."
```

**Invalid transition** (intent not valid for current state):
Return empty `ATCResponse` text — this triggers GPT fallback.

---

## Task 2 — gpt_client

Implement `gpt_client::ask_async()`:

```cpp
namespace gpt_client {
    void init();
    void stop();
    void ask_async(
        const std::string& pilot_text,
        const XPlaneContext& ctx,
        std::function<void(std::string response, bool success)> callback
    );
}
```

**System prompt:**
```
You are an ATC controller at [nearest_airport_id] airport.
The pilot is flying VFR. Respond using standard ICAO phraseology only.
Plain text, no markdown. Maximum 2 sentences.
The pilot's callsign is [callsign].
Current conditions: on ground=[on_ground], runway=[nearest runway].
```

Use `gpt-4o-mini` model. Same async/callback pattern as `whisper_client` — results delivered via main-thread queue.

Only called when:
- `atc_state_machine::process()` returns empty response (invalid transition)
- `PilotMessage::confidence < 0.6`
- `PilotIntent == UNKNOWN`

If `settings::gpt_fallback_enabled == false`, return a generic response instead:
```
"Say again, [Callsign]."
```

---

## Task 3 — atc_session: Full Text Pipeline

Update session flow after intent parsing:

```
PilotMessage
    ↓
atc_state_machine::process()
    ├─ valid response → use it
    └─ empty / low confidence → gpt_client::ask_async() → wait for callback
         ↓
ATCResponse text
    ↓
Append to TranscriptEntry (is_pilot=false)
    ↓
[TTS comes in M6 — for now just log and display]
Transition to IDLE
```

---

## Task 4 — atc_ui: Full Transcript

Update Transcript tab:
- Pilot lines: white, prefixed with "You:"
- ATC lines: cyan, prefixed with "[Airport] ATC:"
- Show current `ATCState` in status bar (e.g. "State: TAXI_CLEARED")
- Add "Reset ATC State" button → calls `atc_state_machine::reset()`

---

## Task 5 — Verify

Full text-only conversation flow at a towered airport:

1. On ground at LSZH (or similar)
2. PTT → "Zurich Ground, HB-ABC, request taxi runway 28" → expect taxi clearance text
3. PTT → "Zurich Tower, HB-ABC, holding short runway 28, ready for departure" → expect takeoff clearance text
4. PTT → "Roger, cleared for takeoff, HB-ABC" → expect frequency change text, state resets
5. Test GPT fallback: PTT → "uh what do I do now" → expect GPT-generated response (not a crash)

---

## Commit

```bash
git add -A
git commit -m "feat(M5): ATC state machine with VFR phraseology templates, GPT-4o-mini fallback, full text transcript"
```
