# MILESTONE M1 — Template Engine + Platzrunde (Traffic Pattern)

Read CLAUDE.md completely before starting.

## Goal

At the end of this milestone:
- JSON-based template engine replaces hardcoded response strings
- AI-assisted intent parsing (GPT) replaces pure keyword matching
- Full VFR traffic pattern flow works reliably at towered AND uncontrolled airports
- Templates are editable in `data/atc_templates.json` without recompilation
- All changes committed to git

---

## Background — Current Problems

The existing system has two weaknesses:

1. **Intent Parser** (`intent_parser.cpp`) uses keyword matching only:
   - "radio check" contains "tower" → misclassified as `INITIAL_CALL`
   - Readbacks often hit `UNKNOWN` because keywords overlap
   - No awareness of current ATC state when parsing intent

2. **Response Generator** (`atc_state_machine.cpp`) uses hardcoded C++ strings:
   - Adding/editing responses requires recompilation
   - No variation in responses (always identical wording)
   - Missing intents: `RADIO_CHECK`, `REQUEST_TOUCH_AND_GO`, `GO_AROUND`

## Architecture Overview

```
Pilot speaks → Whisper STT → Transcript
                                  ↓
                     ┌────────────────────────┐
                     │   Intent Resolution     │
                     │                        │
                     │  1. Local parser first  │
                     │     (fast, free)        │
                     │  2. If confidence < 0.7 │
                     │     → GPT intent parse  │
                     │     (state-aware)       │
                     └────────────────────────┘
                                  ↓
                          PilotIntent
                                  ↓
                     ┌────────────────────────┐
                     │   Template Engine       │
                     │                        │
                     │  state + intent →      │
                     │  JSON template lookup  │
                     │  → variable fill       │
                     │  → ATCResponse         │
                     └────────────────────────┘
                                  ↓
                     TTS → Audio → Pilot
```

---

## Task 1 — Template Engine (`data/atc_templates.json`)

Create the template file. Structure: **airport_type → state → intent → response template**.

Variables available in templates (filled at runtime from `XPlaneContext` + `PilotMessage`):
- `{callsign}` — pilot callsign (from message or settings fallback)
- `{airport}` — nearest airport ICAO
- `{runway}` — active runway (from message or default)
- `{wind}` — formatted wind string ("240 degrees 8 knots")
- `{qnh}` — QNH in hPa
- `{frequency}` — ground/tower frequency
- `{position}` — reported position (downwind/base/final/etc.)

```json
{
  "towered": {
    "IDLE": {
      "INITIAL_CALL_GROUND": {
        "response": "{callsign}, {airport} Ground, information Alpha, runway {runway}, QNH {qnh}.",
        "next_state": "GROUND_CONTACT",
        "requires_readback": false
      },
      "INITIAL_CALL_TOWER": {
        "response": "{callsign}, {airport} Tower, go ahead.",
        "next_state": "TOWER_CONTACT",
        "requires_readback": false
      },
      "INITIAL_CALL_INBOUND": {
        "response": "{callsign}, {airport} Tower, enter left downwind runway {runway}, report midfield downwind.",
        "next_state": "PATTERN_ENTRY",
        "requires_readback": true
      },
      "RADIO_CHECK": {
        "response": "{callsign}, {airport} Tower, reading you five by five.",
        "next_state": "IDLE",
        "requires_readback": false
      },
      "_INVALID": {
        "response": "{callsign}, say again your request.",
        "next_state": "IDLE",
        "requires_readback": false
      }
    },
    "GROUND_CONTACT": {
      "REQUEST_TAXI": {
        "response": "{callsign}, taxi to holding point runway {runway} via Alpha, QNH {qnh}.",
        "next_state": "TAXI_CLEARED",
        "requires_readback": true
      },
      "READBACK": {
        "response": "{callsign}, readback correct.",
        "next_state": "GROUND_CONTACT",
        "requires_readback": false
      },
      "_INVALID": {
        "response": "{callsign}, request taxi when ready.",
        "next_state": "GROUND_CONTACT",
        "requires_readback": false
      }
    },
    "TAXI_CLEARED": {
      "INITIAL_CALL_TOWER": {
        "response": "{callsign}, {airport} Tower, runway {runway}, hold short, number one.",
        "next_state": "TOWER_CONTACT",
        "requires_readback": false
      },
      "READBACK": {
        "response": "{callsign}, readback correct, contact tower when ready.",
        "next_state": "TAXI_CLEARED",
        "requires_readback": false
      },
      "_INVALID": {
        "response": "{callsign}, continue taxi, contact tower when ready.",
        "next_state": "TAXI_CLEARED",
        "requires_readback": false
      }
    },
    "TOWER_CONTACT": {
      "READY_FOR_DEPARTURE": {
        "response": "{callsign}, runway {runway}, cleared for takeoff, wind {wind}.",
        "next_state": "DEPARTURE_CLEARED",
        "requires_readback": true
      },
      "REQUEST_LANDING": {
        "response": "{callsign}, number one, runway {runway}, report final.",
        "next_state": "PATTERN_ENTRY",
        "requires_readback": false
      },
      "REPORT_POSITION": {
        "response": "{callsign}, number one, runway {runway}, report final.",
        "next_state": "PATTERN_ENTRY",
        "requires_readback": false
      },
      "READBACK": {
        "response": "{callsign}, readback correct.",
        "next_state": "TOWER_CONTACT",
        "requires_readback": false
      },
      "_INVALID": {
        "response": "{callsign}, say again your request.",
        "next_state": "TOWER_CONTACT",
        "requires_readback": false
      }
    },
    "DEPARTURE_CLEARED": {
      "READBACK": {
        "response": "{callsign}, readback correct, frequency change approved, good day.",
        "next_state": "IDLE",
        "requires_readback": false
      },
      "_INVALID": {
        "response": "{callsign}, read back takeoff clearance.",
        "next_state": "DEPARTURE_CLEARED",
        "requires_readback": false
      }
    },
    "PATTERN_ENTRY": {
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
      "_INVALID": {
        "response": "{callsign}, report your position.",
        "next_state": "PATTERN_ENTRY",
        "requires_readback": false
      }
    },
    "LANDING_CLEARED": {
      "RUNWAY_VACATED": {
        "response": "{callsign}, contact ground on {frequency}, good day.",
        "next_state": "IDLE",
        "requires_readback": false
      },
      "READBACK": {
        "response": "{callsign}, readback correct.",
        "next_state": "LANDING_CLEARED",
        "requires_readback": false
      },
      "_INVALID": {
        "response": "{callsign}, cleared to land runway {runway}, acknowledge.",
        "next_state": "LANDING_CLEARED",
        "requires_readback": false
      }
    }
  },
  "uncontrolled": {
    "IDLE": {
      "SELF_ANNOUNCE": {
        "response": "Traffic, {airport}, {callsign} {position}.",
        "next_state": "IDLE",
        "requires_readback": false
      },
      "RADIO_CHECK": {
        "response": "No response on CTAF. Frequency appears active.",
        "next_state": "IDLE",
        "requires_readback": false
      },
      "_INVALID": {
        "response": "Traffic, {airport}, {callsign} on frequency.",
        "next_state": "IDLE",
        "requires_readback": false
      }
    }
  }
}
```

---

## Task 2 — Template Loader (`src/atc_templates.hpp/.cpp`)

New module that loads and queries `data/atc_templates.json`.

```cpp
namespace atc_templates {

void init();   // Load JSON from data/atc_templates.json
void stop();
void reload(); // Hot-reload from file (for testing without restart)

struct TemplateEntry {
    std::string response_template; // e.g. "{callsign}, {airport} Tower..."
    std::string next_state;        // e.g. "TOWER_CONTACT"
    bool requires_readback;
};

// Lookup: returns matching template or _INVALID fallback for that state
// intent_key examples: "INITIAL_CALL_GROUND", "REQUEST_TAXI", "READBACK"
TemplateEntry lookup(bool is_towered,
                     const std::string& state,
                     const std::string& intent_key);

// Fill variables in template string
std::string fill(const std::string& tmpl,
                 const std::map<std::string, std::string>& vars);

} // namespace atc_templates
```

Implementation notes:
- Use `nlohmann::json` to parse
- File path: `plugin_path + "/data/atc_templates.json"`
  (derive plugin path from `XPLMGetPluginInfo` like `settings.cpp` does)
- On parse error: log warning, fall back to hardcoded "Say again, {callsign}."
- `fill()` replaces all `{key}` occurrences in the template string with values from the map

---

## Task 3 — AI-assisted Intent Parser

Modify `intent_parser.cpp` to use GPT for ambiguous cases. Two-stage approach:

### Stage 1: Local parser (existing, refined)

Keep the existing keyword rules but add:
- New intent: `RADIO_CHECK` (keywords: "radio check", "how do you read")
- Split `INITIAL_CALL` into sub-variants detected via transcript keywords:
  - "ground"/"delivery" in transcript → `INITIAL_CALL_GROUND`
  - "tower" + "inbound"/"landing" → `INITIAL_CALL_INBOUND`
  - "tower" (without inbound) → `INITIAL_CALL_TOWER`
- Split `REPORT_POSITION` into sub-variants:
  - "downwind" → `REPORT_POSITION_DOWNWIND`
  - "base" → `REPORT_POSITION_BASE`
  - "final" → `REPORT_POSITION_FINAL`

Update `PilotIntent` enum accordingly.

### Stage 2: GPT intent classification (new, for confidence < 0.7 or UNKNOWN)

New function in `intent_parser.cpp`:

```cpp
// Called when local parser returns UNKNOWN or low confidence.
// GPT chooses from valid intents for the current state.
void classify_with_gpt(
    const std::string& transcript,
    const std::string& current_state,
    const std::vector<std::string>& valid_intents,
    std::function<void(std::string intent_key, bool success)> callback
);
```

GPT system prompt:
```
You are an ATC intent classifier. The pilot is in state {current_state}.
Valid intents: {comma-separated list of valid_intents}.
The pilot said: "{transcript}"
Respond with ONLY the intent name, nothing else.
If none match, respond with "_INVALID".
```

This uses `gpt_client::ask_async()` internally but with a classification-specific prompt.
Temperature = 0.0 for deterministic results. `max_tokens` = 20.

### Integration in `atc_session.cpp`

Change the flow after Whisper returns:

```
transcript
    → intent_parser::parse(transcript, ctx)
    → if confidence >= 0.7 AND intent != UNKNOWN:
        → use local result directly
    → else:
        → get valid intents for current state from atc_templates
        → classify_with_gpt(transcript, state, valid_intents, callback)
        → use GPT result
    → atc_templates::lookup(is_towered, state, intent_key)
    → atc_templates::fill(template, variables)
    → speak_response()
```

---

## Task 4 — Refactor `atc_state_machine.cpp`

Replace all hardcoded response strings with template lookups:

```cpp
ATCResponse process(const PilotMessage& msg, const XPlaneContext& ctx) {
    // 1. Build variable map
    std::map<std::string, std::string> vars = {
        {"callsign", get_callsign(msg)},
        {"airport", airport_name(ctx)},
        {"runway", get_runway(msg)},
        {"wind", format_wind(ctx)},
        {"qnh", format_qnh(ctx)},
        {"frequency", "121.9"}, // TODO: real ground freq
        {"position", extract_position(msg)},
    };

    // 2. Determine intent key (already refined by Task 3)
    std::string intent_key = intent_to_template_key(msg.intent);

    // 3. Lookup template
    bool is_towered = ctx.is_towered_airport;
    std::string state_str = state_name(state_);
    auto tmpl = atc_templates::lookup(is_towered, state_str, intent_key);

    // 4. Fill and return
    ATCResponse resp;
    resp.text = atc_templates::fill(tmpl.response_template, vars);
    resp.next_state = state_from_name(tmpl.next_state);
    resp.requires_readback = tmpl.requires_readback;

    // 5. Apply transition
    state_ = resp.next_state;
    return resp;
}
```

The state machine retains ownership of **state transitions** and **validation**.
Templates only define the **response text** and **target state**.

---

## Task 5 — Update `gpt_client.cpp`

Add a second function for intent classification (lighter than full response generation):

```cpp
namespace gpt_client {
    // Existing: full ATC response generation (kept as emergency fallback)
    void ask_async(...);

    // New: intent classification only
    void classify_intent_async(
        const std::string& transcript,
        const std::string& system_prompt,
        std::function<void(std::string intent, bool success)> callback
    );
}
```

- Uses same curl/thread pattern
- `max_tokens: 20`, `temperature: 0.0`
- Model: `gpt-4o-mini` (cheap, fast, sufficient for classification)

---

## Task 6 — Files to modify

| File | Action |
|---|---|
| `data/atc_templates.json` | **NEW** — template definitions |
| `src/atc_templates.hpp` | **NEW** — template loader header |
| `src/atc_templates.cpp` | **NEW** — template loader + fill logic |
| `src/intent_parser.hpp` | **MODIFY** — add new PilotIntent values |
| `src/intent_parser.cpp` | **MODIFY** — add RADIO_CHECK, split INITIAL_CALL/REPORT_POSITION, add GPT classification |
| `src/atc_state_machine.cpp` | **MODIFY** — replace hardcoded strings with template lookups |
| `src/atc_session.cpp` | **MODIFY** — integrate two-stage intent resolution |
| `src/gpt_client.hpp` | **MODIFY** — add classify_intent_async |
| `src/gpt_client.cpp` | **MODIFY** — implement classify_intent_async |
| `CMakeLists.txt` | **MODIFY** — add atc_templates.cpp |
| `CLAUDE.md` | **MODIFY** — document new template engine + intents |

---

## Task 7 — Verify

### Test at towered airport (LSZG or similar):

1. Start on ground, avionics on, COM1 on ground frequency
2. PTT → "Grenchen Ground, Hotel Bravo Lima Uniform Kilo, request taxi"
   → Expect: template-based taxi clearance with QNH
3. PTT → "Taxi to holding point runway two six via Alpha, Hotel Bravo Lima Uniform Kilo"
   → Expect: readback correct
4. PTT → "Grenchen Tower, Hotel Bravo Lima Uniform Kilo, holding short runway two six, ready for departure"
   → Expect: takeoff clearance with wind
5. PTT → "Cleared for takeoff runway two six, Hotel Bravo Lima Uniform Kilo"
   → Expect: readback correct, frequency change
6. Fly pattern, PTT → "Hotel Bravo Lima Uniform Kilo, midfield left downwind runway two six"
   → Expect: continue approach, report final
7. PTT → "Hotel Bravo Lima Uniform Kilo, final runway two six"
   → Expect: cleared to land with wind
8. Land, exit runway, PTT → "Grenchen Ground, Hotel Bravo Lima Uniform Kilo, clear of runway two six"
   → Expect: taxi to parking

### Test edge cases:

9. PTT → "Radio check" → Expect: "reading you five by five" (NOT initial call)
10. PTT → "Ich moechte eine Pizza" → Expect: "say again your request" (from _INVALID)
11. PTT → "Ready for departure" while in IDLE → Expect: _INVALID response (wrong state)

### Test at uncontrolled airport:

12. Tune CTAF, PTT → "Oakdale traffic, Hotel Bravo Lima Uniform Kilo, downwind runway 28, Oakdale"
    → Expect: traffic awareness response

---

## Commit

```bash
git add -A
git commit -m "feat(M1): JSON template engine + AI-assisted intent parsing for traffic pattern"
```
