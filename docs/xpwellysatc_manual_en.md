# xp_wellys_atc — User Manual

## 1. Introduction

**xp_wellys_atc** is an AI-powered ATC voice communication plugin for X-Plane 12 on macOS. It enables realistic VFR radio communication with virtual Air Traffic Control using your own voice.

### How It Works

```
Push-to-Talk → Microphone Capture → whisper.cpp (Metal STT, local)
    → Rule-based Intent Parser → Llama 3.2 3B Classifier (Metal, local,
      always-on with rule-based short-circuit; also repairs Whisper artefacts)
    → ATC State Machine → Template Response → Piper TTS (local CPU+onnxruntime)
    → Audio Playback (X-Plane radio bus)
```

1. **Press PTT** — the plugin records your microphone input
2. **Speech Recognition** — `whisper.cpp` (`small.en-q5_1`, Metal-accelerated) transcribes your radio call locally
3. **Intent Classification** — a rule-based parser identifies your intent (e.g. "request taxi", "ready for departure"). For high-confidence keyword matches (≥ 0.92, e.g. `RADIO_CHECK`, `READY_FOR_DEPARTURE_VFR`, `NEGATIVE_CORRECTION`) the rule parser is authoritative. For everything else, a local **Llama 3.2 3B Instruct (Q4_K_M)** classifier verifies the intent against the valid intents for the current state and repairs common Whisper transcription artefacts (e.g. "take of" → "take off", "Doxy" → "taxi", "TOC" → "taxi") without correcting genuine pilot phraseology errors
4. **ATC State Machine** — validates the intent against the current conversation state and flight phase, then generates an appropriate ATC response from templates
5. **Voice Playback** — Piper synthesizes the response using a per-role voice (Tower / Ground / ATIS / Center) and plays it back on the X-Plane radio bus

All inference is fully **local** on Apple Silicon. No cloud, no API keys, no runtime network traffic. Measured warm-pipeline latency on M4: STT ~320 ms · LM ~600 ms · TTS ~200 ms · **total ≈ 1.2 s per request** — well below the 3 s acceptance target.

The plugin supports **towered airports** (full Ground/Tower flow) and **uncontrolled airports** (CTAF self-announce). It also generates **ATIS broadcasts** from live weather data when you tune the ATIS frequency.

---

## 2. Configuration

### 2.1 Models

The plugin runs three local models, downloaded once from HuggingFace on first launch via the in-sim **Models** tab. Total disk footprint ≈ 2 GB.

| Stage | Backend | Model File | Size |
|---|---|---|---|
| STT | whisper.cpp (Metal) | `ggml-small.en-q5_1.bin` | 181 MB |
| LM (intent classify + Whisper repair) | llama.cpp (Metal) | `Llama-3.2-3B-Instruct-Q4_K_M.gguf` | 1.88 GB |
| TTS | Piper (CPU + onnxruntime) | `en_US-lessac-medium.onnx` (default) + optional voices | 60 MB each |

Models live under `<plugin>/Resources/models/`. The downloader uses HTTPS with `Range`-resumable GETs and SHA256-verifies before flipping `<file>.part` → final filename. Optional alternative TTS voices (`en_GB-alan-medium`, `en_US-amy-medium`, `en_US-ryan-high`) can be added per role; the plugin falls back to the default if a configured voice is missing.

There is **no API key**, no Keychain entry, no cloud account to set up.

### 2.2 Settings File (`data/settings.json`)

Settings are loaded from `<plugin>/data/settings.json`. Missing keys are merged from defaults at startup. Legacy OpenAI-era keys (`api_key_saved`, `tts_voice`, `tts_model`, `whisper_model`, `gpt_model`, `gpt_fallback_enabled`) are recognised but ignored — they are remnants of the cloud-based predecessor and have no effect.

| Setting | Type | Default | Description |
|---|---|---|---|
| `ptt_key_vk` | int | `49` | Virtual key code for push-to-talk (keyboard fallback if X-Plane command binding is not used) |
| `ptt_joystick_button` | int | `-1` | Joystick button index for PTT (`-1` = use X-Plane command binding) |
| `pilot_callsign_raw` | string | `""` | Your aircraft registration in raw form (e.g. `HBAKA`, `N123AB`) |
| `pilot_callsign` | string | `""` | ICAO phonetic callsign auto-derived from `pilot_callsign_raw` (e.g. `Hotel Bravo Alpha Kilo Alpha`). Override only if you need a non-standard rendering. |
| `active_com` | int | `1` | Which COM radio to monitor (`1` or `2`) |
| `volume` | float | `1.0` | ATC response playback volume (`0.0`–`1.0`) |
| `pattern_direction` | string | `"left"` | Default traffic pattern direction (overridden per airport/runway by `airport_vrps.json`) |
| `flow_region` | string | `"EU"` | ATC phraseology region: `"EU"` (ICAO, QNH hPa, holding point, VRP arrivals) or `"US"` (FAA/NAV CANADA, altimeter inHg, hold short, CTAF self-announce). Selects `data/regions/<region>/` config files. |
| `voice_tower` | string | `en_US-lessac-medium` | Piper voice ID for Tower responses |
| `voice_ground` | string | `en_US-lessac-medium` | Piper voice ID for Ground responses |
| `voice_atis` | string | `en_US-lessac-medium` | Piper voice ID for ATIS broadcasts (played at `length_scale 1.18` — slightly slower) |
| `voice_center` | string | `en_US-lessac-medium` | Piper voice ID for Approach / Center responses |
| `disable_default_atc` | bool | `false` | Suppress default X-Plane ATC messages |
| `skip_radio_power_check` | bool | `false` | Bypass radio power check (workaround for exotic aircraft) |
| `show_phraseology_hints` | bool | `true` | Show phraseology cheat sheet in the ATC panel |
| `auto_correction_factor` | float | `1.0` | Multiplier for ATC recovery timeout (`0.5`–`2.0`). Lower = faster correction, higher = more time to make the call |
| `debug_logging` | bool | `false` | Enable verbose debug output to X-Plane Log.txt |
| `debug_traffic` | bool | `false` | Enable the optional **Traffic** debug tab in the ATC Commands panel |

### 2.2.1 Region Selection (EU vs US/Canada)

The plugin ships with two ATC phraseology sets. Switch between them in the Settings tab (`Region: EU | US`) or by editing `flow_region` in `settings.json`.

- **EU** uses ICAO phraseology: `"QNH 1013"`, `"taxi to holding point runway X via Alpha"`, `"squawk 7000"`, VRP-based arrival clearances, and CTAF self-announce with the airport name as prefix only.
- **US** uses FAA / NAV CANADA phraseology (covers both United States and Canada): `"Altimeter 29.92"`, `"taxi via Alpha, hold short runway X"`, `"squawk 1200"`, `"request flight following"` (VFR advisory service on Approach/Center), and CTAF self-announce with the airport name as both prefix and suffix (e.g. *"Palo Alto traffic, N123AB, midfield downwind runway 31, Palo Alto."*).

Region-specific files live under `data/regions/eu/` and `data/regions/us/`:

| File | Region |
|---|---|
| `atc_templates.json` | EU + US |
| `flight_rules.json` | EU + US |
| `airport_vrps.json` | EU only (US has no VRPs) |

Changing the region in the UI triggers a hot-reload of all three files without restarting X-Plane.

### 2.3 Push-to-Talk Binding

PTT can be bound via the X-Plane command system:

- **X-Plane Command:** `xp_wellys_atc/ptt`
- Bind this command to any key or joystick button in X-Plane's keyboard/joystick settings
- The `ptt_key_vk` and `ptt_joystick_button` settings in `settings.json` provide an alternative direct binding

### 2.4 COM Radio Selection

The plugin monitors the COM radio specified by `active_com` (1 or 2). It matches the active COM frequency against the nearest airport's frequency database (parsed from X-Plane's `apt.dat`) to determine whether you are on Ground, Tower, ATIS, or UNICOM.

**Part-time towers:** Some airports (common in the US, e.g. KVRB Vero Beach) list the same frequency in `apt.dat` twice — once as Tower, once as UNICOM — because when the tower is closed, that same frequency becomes the CTAF/UNICOM. The plugin resolves such collisions by priority: **Tower wins over UNICOM**. Even at night, when the real-world tower would be closed, the plugin responds as Tower. It does not automatically fall back to UNICOM mode outside tower hours.

---

## 3. Data Files Reference

All data files are located in the `data/` directory within the plugin folder.

### 3.1 `atc_templates.json` — ATC Response Templates

Defines all ATC response texts organized hierarchically:

```
airport type → ATC state → pilot intent → response
```

**Structure:**
- **`towered`** — responses for controlled airports (Ground + Tower)
- **`uncontrolled`** — responses for CTAF/UNICOM airports

Each response entry contains:

| Field | Description |
|---|---|
| `response` | Template text with `{variable}` placeholders |
| `next_state` | State machine transition after this response |
| `requires_readback` | Whether the pilot must read back the clearance |

**Template Variables** (filled at runtime from X-Plane data):

| Variable | Source |
|---|---|
| `{callsign}` | Pilot callsign from settings |
| `{airport}` | Nearest airport name |
| `{runway}` | Wind-selected active runway |
| `{wind}` | Current wind direction and speed |
| `{qnh}` | Barometric pressure in hPa (used by EU templates) |
| `{altimeter}` | Altimeter setting in inHg with two decimals (used by US templates) |
| `{atis_letter}` | Current ATIS information letter (Alpha–Zulu) |
| `{pattern_direction}` | Traffic pattern side (left/right) |
| `{entry_vrp}` | Detected Visual Reporting Point name |
| `{frequency}` | Ground/handoff frequency |
| `{position_remark}` | Position description |

The `_INVALID` key in each state is the fallback response when no matching intent is found (typically a "say again" style response).

### 3.2 `flight_rules.json` — Flight Phase Rules and Guards

Controls how the plugin detects your flight phase and prevents invalid radio calls.

**Phase Thresholds:**

| Parameter | Value | Purpose |
|---|---|---|
| `taxi_min_gs_kt` | 5.0 | Minimum groundspeed for TAXI phase |
| `roll_min_gs_kt` | 40.0 | Minimum groundspeed for TAKEOFF_ROLL |
| `climb_min_vs_fpm` | 300.0 | Minimum vertical speed for CLIMB |
| `pattern_max_agl_ft` | 3000.0 | Maximum AGL altitude for PATTERN |
| `near_airport_nm` | 5.0 | Maximum distance to be considered "near airport" |
| `runway_aligned_deg` | 30.0 | Heading tolerance for runway alignment |
| `final_descent_rate_fpm` | -200.0 | Minimum descent rate for FINAL_APPROACH |

**Hysteresis** (anti-jitter delays):

| Parameter | Value | Purpose |
|---|---|---|
| `ground_to_airborne_sec` | 0.5 | Delay before transitioning to airborne |
| `airborne_to_landing_sec` | 0.3 | Delay before transitioning to landed |
| `auto_correction_delay_sec` | 3.0 | Default delay for automatic state corrections |

**Intent Preconditions:**
The plugin blocks invalid radio calls based on your current flight phase. For example:
- You cannot request taxi while airborne
- You cannot report "runway vacated" while still in the air
- You cannot call inbound while on the ground

If you make an invalid call, ATC responds with a rejection message (e.g. *"Unable, you appear to be airborne."*).

**Auto-Corrections:**
The plugin automatically corrects state/phase mismatches after a configurable delay. For example:
- If you are airborne but the state is still `DEPARTURE_CLEARED`, it auto-transitions to `PATTERN_ENTRY` after 5 seconds
- If you are on the ground but the state is `PATTERN_ENTRY`, it auto-resets to `IDLE` after 3 seconds

**Frequency Restrictions:**
Certain intents are only valid on specific frequencies:
- `REQUEST_TAXI` — only on Ground frequency
- `READY_FOR_DEPARTURE` — on Tower or Ground frequency (at the holding point, the pilot reports "ready for departure" on Ground, which triggers a Tower handoff)
- `SELF_ANNOUNCE` — only on UNICOM/CTAF

### 3.3 `airport_vrps.json` — Visual Reporting Points

Defines Visual Reporting Points (VRPs) and traffic pattern directions for specific airports.

**Structure per airport:**

| Field | Description |
|---|---|
| `name` | Airport name |
| `pattern_direction` | Left/right per runway (or global for all runways) |
| `vrps` | List of VRPs with name, lat, lon, altitude |
| `arrival_routes` | Recommended VRP sequences per runway |

**Supported Airports:**

| ICAO | Name | Country |
|---|---|---|
| LSGS | Sion | Switzerland |
| LSPN | Triengen | Switzerland |
| LSPV | Wangen-Lachen | Switzerland |
| LSZB | Bern-Belp | Switzerland |
| LSZC | Buochs | Switzerland |
| LSZF | Birrfeld | Switzerland |
| LSZG | Grenchen | Switzerland |
| LSZI | Fricktal-Schupfart | Switzerland |
| LSZK | Speck-Fehraltorf | Switzerland |
| LSZO | Luzern-Beromünster | Switzerland |
| LSZR | St. Gallen-Altenrhein | Switzerland |
| EDFE | Egelsbach | Germany |
| EDKB | Bonn-Hangelar | Germany |
| EDMA | Augsburg | Germany |
| EDTF | Freiburg | Germany |
| EDNY | Friedrichshafen | Germany |

The Swiss airport data is aligned with the companion plugin **xp_swiss_vfr** (sourced from AIP Switzerland, Skyguide VAC and Navigraph).

**Usage:** When you announce your position over a VRP (e.g. *"Bern Tower, over Whiskey, inbound"*), the plugin recognizes the VRP name and ATC responds with entry instructions via that point.

Airports not listed in this file use the global `pattern_direction` from settings and have no VRP recognition.

### 3.4 `atc_prompt_templates.json` — Local LLM Prompts

Contains the prompts fed to the local Whisper and Llama backends. The `gpt_*` key names are kept for backwards compatibility with template files written for the cloud-based predecessor; the prompts now drive Llama 3.2 3B.

| Prompt | Purpose |
|---|---|
| `whisper_prompt` | Static context sent to whisper.cpp's `initial_prompt` to bias transcription toward aviation vocabulary (NATO phonetic alphabet, common ATC phrases). Per-utterance, the plugin appends the nearest airport name + ICAO so local proper nouns (e.g. *Grenchen*) are not mistranscribed as everyday words. |
| `gpt_classify_prompt` | System prompt for Llama 3.2 3B intent classification on every transcript that doesn't short-circuit via a high-confidence rule match. The model is constrained to pick from the valid intents for the current ATC state and asked to mark whether it repaired a Whisper artefact. Variables: `{state}`, `{valid_intents}`, `{transcript}`, `{frequency_type}`, `{on_ground}`, `{altitude_ft}`, `{groundspeed_kts}`, `{airport}`, `{hint_intent}` (rule-parser's guess). |
| `gpt_fallback_prompt`, `gpt_fallback_prompt_us` | Vestigial — kept for upgrade compatibility but currently unused (the engine resolves `_INVALID` cases via region-specific `_INVALID` templates and the three-tier "say again" escalation, not via free-form LLM generation). |

These prompts are pre-configured and generally do not need modification.

---

## 4. ATC Communication Reference

### 4.1 State Machine Overview

```
IDLE ──────────────────────────────────────────────────────┐
 ├── Contact Ground ──→ GROUND_CONTACT ──→ TAXI_CLEARED ──┤
 ├── Contact Tower ──→ TOWER_CONTACT ──────────────────────┤
 └── Inbound Call ──→ PATTERN_ENTRY ───────────────────────┤
                                                           │
TOWER_CONTACT ─┬── Ready for Departure ──→ DEPARTURE_CLEARED
               ├── Request Landing ──→ PATTERN_ENTRY
               └── Request Touch & Go ──→ TOUCH_AND_GO_CLEARED
                                                           │
DEPARTURE_CLEARED ─┬── Report Downwind ──→ PATTERN_ENTRY   │
                   └── Leave Frequency ──→ EN_ROUTE ──→ IDLE
                                                           │
PATTERN_ENTRY ─┬── Report Final ──→ LANDING_CLEARED        │
               ├── Request Touch & Go ──→ TOUCH_AND_GO_CLEARED
               └── Go Around ──→ PATTERN_ENTRY             │
                                                           │
LANDING_CLEARED ─┬── Runway Vacated ──→ IDLE               │
                 └── Go Around ──→ PATTERN_ENTRY           │
                                                           │
TOUCH_AND_GO_CLEARED ─┬── Report Downwind ──→ PATTERN_ENTRY
                      ├── Runway Vacated ──→ IDLE
                      └── Go Around ──→ PATTERN_ENTRY
```

### 4.2 States and Valid Intents

#### State: `IDLE`

Initial state — no active ATC conversation.

| Pilot Intent | Example Pilot Call | ATC Response |
|---|---|---|
| `INITIAL_CALL_GROUND` | *"Springfield Ground, N123AB, request taxi."* | *"N123AB, Springfield Ground, information Bravo current, runway 26, QNH 1013."* |
| `INITIAL_CALL_TOWER` | *"Springfield Tower, N123AB."* | *"N123AB, Springfield Tower, go ahead."* |
| `INITIAL_CALL_INBOUND` | *"Springfield Tower, N123AB, ten miles south, inbound for landing."* | *"N123AB, Springfield Tower, enter left downwind runway 26, report midfield downwind."* |
| `INITIAL_CALL_INBOUND_VRP` | *"Bern Tower, N123AB, over Whiskey, inbound."* | *"N123AB, Bern Tower, cleared to enter control zone via Whiskey, runway 14, report left downwind."* |
| `REQUEST_TAXI` | *"Springfield Ground, N123AB, request taxi."* | *"N123AB, Springfield Ground, information Bravo current, taxi to holding point runway 26 via Alpha, QNH 1013."* |
| `READY_FOR_DEPARTURE` | *"Springfield Tower, N123AB, holding short runway 26, ready for departure."* | *"N123AB, Springfield Tower, runway 26, cleared for takeoff, wind 240 at 8, report left downwind."* |
| `READY_FOR_DEPARTURE_VFR` | *"Springfield Tower, N123AB, ready for departure, on course."* | *"N123AB, Springfield Tower, runway 26, cleared for takeoff, wind 240 at 8, on course approved, frequency change approved when airborne."* |
| `RADIO_CHECK` | *"Springfield Tower, N123AB, radio check."* | *"N123AB, Springfield Tower, reading you five by five."* |

**Tip — report your position on first contact:** On `INITIAL_CALL_GROUND` and `REQUEST_TAXI` from `IDLE`, the plugin checks whether you mentioned your location (e.g. *"on parking"*, *"on the apron"*, *"at stand 5"*, *"on taxiway Alpha"*). If you didn't, the controller adds a short "say position" remark to the clearance. Phrase your call the real-world way — *"who you call, who you are, where you are, what you want"* — and you'll get a clean response.

#### State: `GROUND_CONTACT`

After initial contact with Ground.

| Pilot Intent | Example Pilot Call | ATC Response |
|---|---|---|
| `REQUEST_TAXI` | *"N123AB, request taxi."* | *"N123AB, taxi to holding point runway 26 via Alpha, QNH 1013."* |
| `READBACK` | *"Taxi runway 26 via Alpha, N123AB."* | *(silent — ICAO standard: no acknowledgement of a correct readback)* |

#### State: `TAXI_CLEARED`

Taxiing to the holding point. Ground keeps control on the manoeuvring area;
the tower handoff happens when the pilot reports "ready for departure" at the
holding point — not as part of the taxi readback.

| Pilot Intent | Example Pilot Call | ATC Response |
|---|---|---|
| `READY_FOR_DEPARTURE` | *"Springfield Ground, N123AB, holding short runway 26, ready for departure."* | *"N123AB, roger, contact Tower on 120.100."* (→ `TOWER_CONTACT`) |
| `READY_FOR_DEPARTURE_VFR` | *"Ground, N123AB, holding short runway 26, ready for departure, VFR northbound."* | *"N123AB, roger, contact Tower on 120.100."* (→ `TOWER_CONTACT`) |
| `INITIAL_CALL_TOWER` | *"Springfield Tower, N123AB."* | *"N123AB, Tower, runway 26, hold short, number one."* |
| `READBACK` | *"Taxi runway 26 via Alpha, N123AB."* | *(silent)* |

#### State: `TOWER_CONTACT`

Tower has acknowledged but no clearance issued yet.

| Pilot Intent | Example Pilot Call | ATC Response |
|---|---|---|
| `READY_FOR_DEPARTURE` | *"N123AB, ready for departure runway 26."* | *"N123AB, runway 26, cleared for takeoff, wind 240 at 8, report left downwind."* |
| `READY_FOR_DEPARTURE_VFR` | *"N123AB, ready for departure, on course."* | *"N123AB, runway 26, cleared for takeoff, wind 240 at 8, on course approved, frequency change approved when airborne."* |
| `REQUEST_LANDING` | *"N123AB, request landing runway 26."* | *"N123AB, number one, runway 26, report final."* |
| `REQUEST_TOUCH_AND_GO` | *"N123AB, request touch and go."* | *"N123AB, runway 26, cleared touch and go, wind 240 at 8."* |
| `REPORT_POSITION` | *"N123AB, five miles south."* | *"N123AB, number one, runway 26, report final."* |
| `READBACK` | *"Cleared for takeoff 26, N123AB."* | *(silent)* |

#### State: `DEPARTURE_CLEARED`

Airborne after takeoff clearance.

| Pilot Intent | Example Pilot Call | ATC Response |
|---|---|---|
| `REPORT_POSITION_DOWNWIND` | *"N123AB, midfield left downwind runway 26."* | *"N123AB, number one, runway 26, continue approach, report final."* |
| `REPORT_POSITION_BASE` | *"N123AB, turning left base runway 26."* | *"N123AB, number one, runway 26, continue approach."* |
| `REPORT_POSITION_FINAL` | *"N123AB, final runway 26."* | *"N123AB, runway 26, cleared to land, wind 240 at 8."* |
| `READBACK` | *"Cleared for takeoff runway 26, on course, N123AB."* | *(silent)* |
| `REQUEST_FREQUENCY` | *"Tower, N123AB, request frequency change."* | *"N123AB, frequency change approved, good day."* |
| `LEAVING_FREQUENCY` | *"N123AB, leaving frequency, good day."* | *"N123AB, good day."* |

#### State: `PATTERN_ENTRY`

In the traffic pattern (after inbound clearance or downwind report).

| Pilot Intent | Example Pilot Call | ATC Response |
|---|---|---|
| `REPORT_POSITION_DOWNWIND` | *"N123AB, midfield left downwind runway 26."* | *"N123AB, number one, runway 26, continue approach, report final."* |
| `REPORT_POSITION_BASE` | *"N123AB, turning left base runway 26."* | *"N123AB, number one, runway 26, continue approach."* |
| `REPORT_POSITION_FINAL` | *"N123AB, final runway 26."* | *"N123AB, runway 26, cleared to land, wind 240 at 8."* |
| `REQUEST_LANDING` | *"N123AB, request landing runway 26."* | *"N123AB, runway 26, cleared to land, wind 240 at 8."* |
| `REQUEST_TOUCH_AND_GO` | *"N123AB, request touch and go."* | *"N123AB, runway 26, cleared touch and go, wind 240 at 8."* |
| `GO_AROUND` | *"N123AB, going around."* | *"N123AB, roger, fly runway heading, climb and maintain pattern altitude, re-enter left downwind runway 26."* |
| `READBACK` | *"Cleared via Whiskey, runway 14, wilco report left downwind, N123AB."* | *(silent)* |

#### State: `TOUCH_AND_GO_CLEARED`

After touch-and-go clearance.

| Pilot Intent | Example Pilot Call | ATC Response |
|---|---|---|
| `REPORT_POSITION_DOWNWIND` | *"N123AB, midfield left downwind runway 26."* | *"N123AB, number one, runway 26, continue approach, report final."* |
| `REPORT_POSITION_BASE` | *"N123AB, turning left base runway 26."* | *"N123AB, number one, runway 26, continue approach."* |
| `REPORT_POSITION_FINAL` | *"N123AB, final runway 26."* | *"N123AB, runway 26, cleared to land, wind 240 at 8."* |
| `REQUEST_LANDING` | *"N123AB, request full stop."* | *"N123AB, runway 26, cleared to land, wind 240 at 8."* |
| `REQUEST_TOUCH_AND_GO` | *"N123AB, request another touch and go."* | *"N123AB, runway 26, cleared touch and go, wind 240 at 8."* |
| `GO_AROUND` | *"N123AB, going around."* | *"N123AB, roger, fly runway heading, re-enter left downwind runway 26."* |
| `RUNWAY_VACATED` | *"N123AB, clear of runway 26."* | *"N123AB, contact ground on 121.9, good day."* |
| `READBACK` | *"Cleared touch and go runway 26, N123AB."* | *(silent)* |

#### State: `LANDING_CLEARED`

Cleared to land — waiting for touchdown and runway exit.

| Pilot Intent | Example Pilot Call | ATC Response |
|---|---|---|
| `RUNWAY_VACATED` | *"N123AB, clear of runway 26."* | *"N123AB, contact ground on 121.9, good day."* |
| `REQUEST_TAXI_PARKING` | *"Ground, N123AB, request taxi to parking."* | *"N123AB, Ground, taxi to general aviation parking via Alpha."* |
| `GO_AROUND` | *"N123AB, going around."* | *"N123AB, roger, fly runway heading, climb and maintain pattern altitude, re-enter left downwind runway 26."* |
| `READBACK` | *"Cleared to land runway 26, N123AB."* | *(silent)* |

**Note — `REQUEST_TAXI_PARKING` is only valid after landing** (flight phases `TAXI` or `LANDING_ROLL`). Trying to request taxi to parking while still at the parking position (flight phase `PARKED`) is rejected — you can't taxi somewhere you already are.

#### State: `EN_ROUTE`

Cross-country cruise — no ATC contact. The state automatically resets to `IDLE` when the nearest airport changes (so an inbound call to the next field starts cleanly).

#### Universal Intents (valid in every state)

A few pilot intents are accepted at any point in the conversation, regardless of the current state:

| Pilot Intent | Example Pilot Call | Effect |
|---|---|---|
| `NEGATIVE_CORRECTION` | *"Negative, N123AB, request VFR departure on course."* / *"Disregard, N123AB, ..."* / *"No correction, ..."* | State machine reverts one step (e.g. `DEPARTURE_CLEARED → TOWER_CONTACT`, `TAXI_CLEARED → GROUND_CONTACT`) so the pilot can re-issue the request. ATC replies *"N123AB, roger, correction noted, say intentions."* If the controller misinterprets your departure type (pattern vs. cross-country), this is the realistic way to correct it. |
| `UNABLE` | *"Unable, N123AB."* | Acknowledged with *"N123AB, roger."* No state change. |
| `INAPPROPRIATE_LANGUAGE` | (any rude wording) | Triggers a polite warning on first offence, escalates on repeats. The pilot's actual request is still processed normally on the first warning. State unchanged. |
| `TRAFFIC_IN_SIGHT` / `TRAFFIC_NEGATIVE_CONTACT` / `TRAFFIC_LOOKING` | *"Traffic in sight, N123AB."* | Only valid while the controller has just issued a traffic advisory; routed to the parallel traffic dialog. |

### 4.3 Radio Discipline

ATC monitors the frequency for inappropriate language. Real-world controllers react to unprofessional R/T, and so does the virtual one:

1. **First offense** — a polite reminder about radio discipline; the pilot's actual request is still processed as normal
2. **Repeated offense** — a firm *"last warning"* from the controller; further transmissions on the frequency remain possible but the controller's patience is clearly running out

The feature is intended to encourage realistic, professional radio communication — not to punish slips of the tongue. Stay calm on the radio, and the controller stays calm too.

---

## 5. Example: Traffic Pattern

Airport: **LSZG Grenchen**, Runway **06**, Left Traffic
Callsign: **Hotel Bravo Lima Uniform Kilo** (HB-LUK)

### Step 1 — Contact Ground

> **Pilot:** Grenchen Ground, Hotel Bravo Lima Uniform Kilo, request taxi.
>
> **ATC:** Hotel Bravo Lima Uniform Kilo, Grenchen Ground, information Alpha, taxi to holding point runway zero six via Alpha, QNH 1013.
>
> **Pilot (readback):** Taxi to holding point runway zero six via Alpha, QNH 1013, Hotel Bravo Lima Uniform Kilo.

### Step 2 — Ready for Departure (Ground handoff)

> **Pilot:** Grenchen Ground, Hotel Bravo Lima Uniform Kilo, holding short runway zero six, ready for departure.
>
> **ATC (Ground):** Hotel Bravo Lima Uniform Kilo, roger, contact Tower on 120.100.
>
> **Pilot:** Contact Tower on 120.100, Hotel Bravo Lima Uniform Kilo.

*(Pilot switches to Tower frequency.)*

### Step 3 — Takeoff Clearance

> **Pilot:** Grenchen Tower, Hotel Bravo Lima Uniform Kilo, holding short runway zero six, ready for departure.
>
> **ATC (Tower):** Hotel Bravo Lima Uniform Kilo, runway zero six, cleared for takeoff, wind calm, report left downwind.
>
> **Pilot (readback):** Cleared for takeoff runway zero six, wilco report downwind, Hotel Bravo Lima Uniform Kilo.

### Step 4 — Report Downwind

> **Pilot:** Grenchen Tower, Hotel Bravo Lima Uniform Kilo, midfield left downwind runway zero six.
>
> **ATC:** Hotel Bravo Lima Uniform Kilo, number one, runway zero six, continue approach, report final.
>
> **Pilot:** Wilco, will report final, Hotel Bravo Lima Uniform Kilo.

### Step 5 — Report Final

> **Pilot:** Hotel Bravo Lima Uniform Kilo, final runway zero six.
>
> **ATC:** Hotel Bravo Lima Uniform Kilo, runway zero six, cleared to land, wind calm.
>
> **Pilot (readback):** Cleared to land runway zero six, Hotel Bravo Lima Uniform Kilo.

### Step 6 — Runway Vacated

> **Pilot:** Grenchen Tower, Hotel Bravo Lima Uniform Kilo, clear of runway zero six.
>
> **ATC:** Hotel Bravo Lima Uniform Kilo, contact ground on 121.800, good day.
>
> **Pilot:** Ground on 121.800, Hotel Bravo Lima Uniform Kilo, good day.

### Step 7 — Taxi to Parking

> **Pilot:** Grenchen Ground, Hotel Bravo Lima Uniform Kilo, request taxi to general aviation parking.
>
> **ATC:** Hotel Bravo Lima Uniform Kilo, taxi to general aviation parking via Alpha, good day.
>
> **Pilot:** Taxi to parking via Alpha, Hotel Bravo Lima Uniform Kilo, good day.

---

## 6. Example: Cross-Country Flight

Route: **LSZG Grenchen → LSZB Bern-Belp**
Callsign: **Hotel Bravo Lima Uniform Kilo** (HB-LUK)

### Phase 1 — Departure (LSZG)

#### Step 1 — Contact Ground

> **Pilot:** Grenchen Ground, Hotel Bravo Lima Uniform Kilo, request taxi.
>
> **ATC:** Hotel Bravo Lima Uniform Kilo, Grenchen Ground, information Alpha, taxi to holding point runway zero six via Alpha, QNH 1013.
>
> **Pilot (readback):** Taxi to holding point runway zero six via Alpha, QNH 1013, Hotel Bravo Lima Uniform Kilo.

#### Step 2 — Ready for Departure (Ground handoff)

The key phrase **"on course"** tells ATC this is a cross-country departure, not a pattern flight.

> **Pilot:** Grenchen Ground, Hotel Bravo Lima Uniform Kilo, holding short runway zero six, ready for departure, on course.
>
> **ATC (Ground):** Hotel Bravo Lima Uniform Kilo, roger, contact Tower on 120.100.
>
> **Pilot:** Contact Tower on 120.100, Hotel Bravo Lima Uniform Kilo.

*(Pilot switches to Tower frequency.)*

#### Step 3 — Takeoff Clearance (On Course)

> **Pilot:** Grenchen Tower, Hotel Bravo Lima Uniform Kilo, holding short runway zero six, ready for departure, on course.
>
> **ATC (Tower):** Hotel Bravo Lima Uniform Kilo, Grenchen Tower, runway zero six, cleared for takeoff, wind calm, on course approved, frequency change approved when airborne.
>
> **Pilot (readback):** Cleared for takeoff runway zero six, on course, Hotel Bravo Lima Uniform Kilo.

#### Step 4 — Leaving Frequency

> **Pilot:** Hotel Bravo Lima Uniform Kilo, leaving your frequency, good day.
>
> **ATC:** Hotel Bravo Lima Uniform Kilo, good day.

### Phase 2 — En Route

No ATC contact. Cruise toward destination. The plugin state is `EN_ROUTE`.

### Phase 3 — Arrival (LSZB)

#### Step 5 — Inbound Call via VRP

Bern-Belp has Visual Reporting Points: **November**, **Sierra**, **Whiskey**, **Echo**. Announce your position over the VRP you are crossing.

> **Pilot:** Bern Tower, Hotel Bravo Lima Uniform Kilo, over Whiskey, 3500 feet, inbound for landing, information Bravo.
>
> **ATC:** Hotel Bravo Lima Uniform Kilo, Bern-Belp Tower, cleared to enter control zone via Whiskey, runway one four, report left downwind.
>
> **Pilot (readback):** Cleared via Whiskey, runway one four, wilco report left downwind, Hotel Bravo Lima Uniform Kilo.

*Without a VRP:*

> **Pilot:** Bern Tower, Hotel Bravo Lima Uniform Kilo, ten miles northwest, inbound for landing, information Bravo.
>
> **ATC:** Hotel Bravo Lima Uniform Kilo, Bern Tower, enter left downwind runway one four, report midfield downwind.

#### Step 6 — Report Downwind

> **Pilot:** Hotel Bravo Lima Uniform Kilo, midfield left downwind runway one four.
>
> **ATC:** Hotel Bravo Lima Uniform Kilo, number one, runway one four, continue approach, report final.

#### Step 7 — Report Final and Landing

> **Pilot:** Hotel Bravo Lima Uniform Kilo, final runway one four.
>
> **ATC:** Hotel Bravo Lima Uniform Kilo, runway one four, cleared to land, wind calm.
>
> **Pilot (readback):** Cleared to land runway one four, Hotel Bravo Lima Uniform Kilo.

#### Step 8 — Runway Vacated

LSZB has no separate Ground frequency — Tower handles taxi.

> **Pilot:** Bern Tower, Hotel Bravo Lima Uniform Kilo, clear of runway one four, request taxi to general aviation parking.
>
> **ATC:** Hotel Bravo Lima Uniform Kilo, taxi to general aviation parking via Alpha, good day.
>
> **Pilot:** Taxi to parking via Alpha, Hotel Bravo Lima Uniform Kilo, good day.

---

## 7. ATC Panel UI

The in-sim ATC Commands panel provides frequency management, phraseology hints, and transcript history.

### 7.1 Frequency Buttons

The panel displays all frequencies for the nearest airport (ATIS, Ground, Tower, Approach). The currently active COM frequency is highlighted in green. Clicking a frequency button sets it as the **standby frequency** -- flip-flop your COM radio to activate it.

When the COM radio has no electrical power (engines off, avionics bus dead), the frequency buttons are disabled and a warning is shown. You can bypass this check via the `skip_radio_power_check` setting for aircraft with non-standard electrical systems.

### 7.2 Phraseology Hints

When `show_phraseology_hints` is enabled (default), the panel shows context-aware radio call suggestions below the frequency list. The hints update dynamically based on your current ATC state, flight phase, and tuned frequency.

- **Green text** -- the suggested radio call using your short callsign (e.g. HB-AKA)
- **Hover tooltip** -- the full ICAO phraseology with phonetic callsign (e.g. Hotel Bravo Alpha Kilo Alpha)
- Hints are grouped into categories: Ground Operations, Tower Operations, Pattern/Approach, General

The hints are read-only -- all communication is done via voice (push-to-talk). The hints serve as a cheat sheet showing you what to say.

**EU/ICAO VFR flow at towered airports with Ground frequency:**
At airports with a separate Ground frequency, the hints guide you through the correct flow: contact Ground first, get taxi clearance, report "ready for departure" on Ground, then contact Tower for takeoff clearance. If you are tuned to Tower but should be on Ground, the panel shows "Tune to Ground frequency first".

### 7.3 Disregard Button

When the ATC state is not IDLE (i.e. you are in an active conversation), a **Disregard** button appears next to the "Phraseology Hints" header. Clicking it resets the ATC conversation to IDLE, allowing you to start fresh.

Use this when you are stuck in a loop (e.g. ATC keeps saying "say again") or when you want to abandon the current conversation and start over. It does not affect your flight -- only the ATC dialog state is reset.

### 7.4 Nearby Airports

The collapsible "Nearby Airports" section lists airports within 40 NM, sorted by distance. Click an airport to lock it as the active airport and tune its most useful frequency (ATIS > Tower > UNICOM) to standby. Click "Unlock" to return to automatic nearest-airport detection.
