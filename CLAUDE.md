# CLAUDE.md

This file provides permanent guidance to Claude Code when working in this repository.
Read this file completely before doing anything else.

---

## Project Overview

**xp_wellys_atc** is a C++17 X-Plane 12 plugin for macOS (ARM64 + x86_64 universal binary) that provides AI-powered ATC voice communication for VFR flight simulation.

The plugin captures microphone input via push-to-talk, transcribes speech via OpenAI Whisper, processes pilot intent through a rule-based ATC state machine (with optional GPT-4o-mini fallback), and plays back ATC responses via OpenAI TTS.

Reference project for toolchain patterns and conventions: https://github.com/rwellinger/xp_pilot

---

## Build System

```bash
make setup    # Download X-Plane SDK, Dear ImGui, nlohmann/json
make build    # CMake Release build → build/xp_wellys_atc.xpl
make install  # Code-sign + install to X-Plane plugins directory
```

- **CMake 3.21+**, C++17, macOS 12.0+ universal binary (`arm64` + `x86_64`)
- Toolchain: Homebrew LLVM (`/opt/homebrew/opt/llvm`)
- Output: `build/xp_wellys_atc.xpl`
- Compiler flags: `-Wall -Wextra -fvisibility=hidden`, OpenGL deprecation suppressed
- System frameworks linked via CMake: `AudioToolbox`, `AudioUnit`, `CoreAudio`, `AVFoundation`, `Security`
- HTTP: system libcurl via `find_package(CURL)`

## Vendor Dependencies

Populated by `make setup`, never committed to git:

| Path | Content |
|---|---|
| `sdk/` | X-Plane SDK headers (XPLM/, XPWidgets/) |
| `vendor/imgui/` | Dear ImGui v1.91.9 |
| `vendor/json.hpp` | nlohmann/json v3.11.3 |

---

## Directory Structure

```
xp_wellys_atc/
├── CLAUDE.md
├── START.md
├── CMakeLists.txt
├── Makefile
├── .clang-format
├── .clang-tidy
├── .gitignore
├── src/
│   ├── main.cpp                # Plugin entry points, menu, flight loop
│   ├── atc_session.hpp/.cpp    # Central coordinator
│   ├── ptt_input.hpp/.cpp      # Push-to-talk (keyboard + joystick)
│   ├── audio_recorder.hpp/.cpp # Core Audio mic capture → WAV buffer
│   ├── whisper_client.hpp/.cpp # OpenAI Whisper API
│   ├── intent_parser.hpp/.cpp  # Rule-based transcript → PilotIntent (with sub-variants)
│   ├── atc_state_machine.hpp/.cpp  # VFR ATC logic + template-based response generation
│   ├── atc_templates.hpp/.cpp  # JSON template engine for ATC responses
│   ├── gpt_client.hpp/.cpp     # GPT-4o-mini fallback + intent classification
│   ├── tts_client.hpp/.cpp     # OpenAI TTS API
│   ├── audio_player.hpp/.cpp   # Core Audio MP3 playback
│   ├── atis_generator.hpp/.cpp # ATIS broadcast generation + letter management
│   ├── xplane_context.hpp/.cpp # X-Plane DataRef reader
│   ├── settings.hpp/.cpp       # JSON config + Keychain API key
│   └── atc_ui.hpp/.cpp         # Dear ImGui window
├── data/
│   ├── settings.json           # Runtime config, never committed
│   └── atc_templates.json      # ATC response templates (towered + uncontrolled)
├── sdk/                        # make setup, not committed
└── vendor/                     # make setup, not committed
```

---

## Architecture

### Module Responsibilities

Each module uses a C++ namespace with `init()` and `stop()` lifecycle functions called from `main.cpp`.

**`main.cpp`** — `XPluginStart`, `XPluginStop`, `XPluginEnable`, `XPluginDisable`. Registers flight loop callback and key sniffer. Calls `init()`/`stop()` on all modules in dependency order.

**`xplane_context`** — Reads DataRefs every flight loop iteration into `XPlaneContext` struct. Derives `nearest_airport_id` and `is_towered_airport` via `XPLMGetNavAidInfo`. Parses `apt.dat` at init to build runway cache (`RunwayInfo` per airport) and **full airport frequency database** (`AirportFrequencies` per airport, covering all apt.dat codes 50-55/1050-1055: ATIS, UNICOM, Delivery, Ground, Tower, Approach). Determines `active_runway` from wind direction/speed every ~1 second. Also reads weather DataRefs: visibility, cloud base/type, temperature, dewpoint. Runway selection: calm wind (< 3 kt) uses longest runway; otherwise picks runway end with largest headwind component. Frequency type (`FrequencyType`) is derived by matching active COM frequency against the airport's frequency database (not by frequency band heuristics). Supports `tower_only` flag for airports that have Tower but no Ground frequency (Tower handles taxi). Provides `set_standby_freq()` to write a frequency to the active COM's standby slot.

**`atis_generator`** — Generates realistic ATIS broadcasts from XPlaneContext weather data. Manages ATIS information letter (Alpha–Zulu), incrementing on significant changes: active runway change, wind direction >30°, QNH >1 hPa, visibility category change. Provides `is_tuned_to_atis()` to detect when pilot's COM matches the airport's ATIS frequency (parsed from apt.dat) within VHF range (~60 NM). ATIS auto-plays via TTS when tuned, with 30s cooldown.

**`settings`** — Loads/saves `data/settings.json`. API key stored exclusively in macOS Keychain via `Security.framework` (`SecKeychainItemAdd` / `SecKeychainFindGenericPassword`). `settings.json` only stores `"api_key_saved": true` as a flag.

**`ptt_input`** — Detects PTT activation via `XPLMRegisterKeySniffer` (keyboard) or joystick button DataRef polling. Notifies `atc_session` on press/release.

**`audio_recorder`** — Core Audio `AudioUnit` (`kAudioUnitSubType_HALOutput`) captures mic at 16kHz mono 16-bit PCM into `std::vector<int16_t>`. On PTT release, encodes to WAV in memory (`std::vector<uint8_t>`) — no temp files.

**`whisper_client`** — POSTs WAV buffer to OpenAI `/v1/audio/transcriptions` via libcurl multipart. Returns `std::string` transcript. Runs on `std::thread`.

**`intent_parser`** — Rule-based keyword/pattern matching on transcript text + `XPlaneContext`. Returns `PilotMessage` with `PilotIntent` enum and confidence score. Supports sub-variant intents: `INITIAL_CALL_GROUND/TOWER/INBOUND`, `REPORT_POSITION_DOWNWIND/BASE/FINAL`, `RADIO_CHECK`. Provides `intent_template_key()` mapping enum to JSON template keys.

**`atc_templates`** — JSON template engine. Loads `data/atc_templates.json` at init. Provides `lookup(is_towered, state, intent_key)` for template resolution with `_INVALID` fallback, `fill(template, vars)` for variable substitution, and `valid_intents()` for GPT classification prompts. Supports hot-reload via `reload()`.

**`atc_state_machine`** — Owns current `ATCState`. On valid `PilotIntent`, transitions state and returns `ATCResponse` text via template lookup. Provides `build_vars()` for constructing template variable maps, `state_from_name()` for string-to-enum conversion, and `set_state()` for external state transitions (GPT path).

**`gpt_client`** — Two functions: `ask_async()` for full ATC response generation (emergency fallback), and `classify_intent_async()` for lightweight intent classification (max_tokens=20, temperature=0.0, gpt-4o-mini). Both POST to OpenAI `/v1/chat/completions`. Run on `std::thread`.

**`tts_client`** — POSTs ATC response text to OpenAI `/v1/audio/speech`. Returns MP3 as `std::vector<uint8_t>`. Runs on `std::thread`.

**`audio_player`** — Decodes MP3 via `AudioToolbox ExtAudioFile`. Plays PCM via Core Audio default output. Respects `settings.volume`.

**`atc_session`** — Owns the PTT state machine (`IDLE → RECORDING → PROCESSING → PLAYING`). Coordinates the full pipeline with two-stage intent resolution: high-confidence intents (≥0.7) go directly through the state machine; low-confidence or UNKNOWN intents route to GPT intent classification. Blocks new PTT input while `PROCESSING` or `PLAYING`.

**`atc_ui`** — Dear ImGui window with Transcript panel, Status bar, and Settings tab.

---

## Key Data Structures

```cpp
struct AirportFrequency {
    uint32_t freq_khz;                  // e.g. 121900 for 121.900 MHz
    FrequencyType type;                 // ATIS, GROUND, TOWER, etc.
};

struct AirportFrequencies {
    std::vector<AirportFrequency> all;  // all frequencies parsed from apt.dat
    bool has(FrequencyType) const;      // at least one freq of this type?
    float first_mhz(FrequencyType) const; // first freq as MHz (0.0 if none)
    FrequencyType lookup(float) const;  // match COM freq → type (UNKNOWN if no match)
    bool has_ground() const;            // convenience: has(GROUND)
};

struct XPlaneContext {
    double      latitude, longitude;
    float       altitude_ft_msl;
    float       groundspeed_kts;
    float       indicated_airspeed_kts;
    float       vertical_speed_fpm;
    float       heading_true;
    bool        on_ground;
    bool        engines_running;
    float       com1_freq_mhz, com2_freq_mhz;
    int         active_com;             // 1 or 2
    std::string aircraft_icao;
    std::string nearest_airport_id;
    bool        is_towered_airport;
    FrequencyType frequency_type;       // derived from COM freq vs airport freqs
    float       visibility_m;           // sim/weather/visibility_reported_m
    float       cloud_base_ft_msl;      // cloud_base_msl_m[0] * 3.28084
    int         cloud_type;             // 0=clear,1=few,2=scattered,3=broken,4=overcast
    float       temperature_c;          // sim/weather/temperature_sealevel_c
    float       dewpoint_c;             // sim/weather/dewpoint_sealevel_c
    float       atis_freq_mhz;          // from apt.dat ATIS/AWOS freq
    AirportFrequencies airport_freqs;   // all frequencies for nearest airport
    bool        tower_only;             // towered but no separate ground freq
    double      airport_lat, airport_lon; // airport position (for range checks)
    std::vector<RunwayInfo> runways;    // all runways at nearest airport
    std::string active_runway;          // wind-determined (e.g. "28", "09L")
};

enum class PilotIntent {
    UNKNOWN,
    RADIO_CHECK,
    INITIAL_CALL,             // generic fallback
    INITIAL_CALL_GROUND,      // "ground" / "delivery"
    INITIAL_CALL_TOWER,       // "tower" (without inbound)
    INITIAL_CALL_INBOUND,     // "tower" + "inbound"/"landing"
    REQUEST_TAXI,
    READY_FOR_DEPARTURE,
    REPORT_POSITION,          // generic (crosswind/upwind)
    REPORT_POSITION_DOWNWIND,
    REPORT_POSITION_BASE,
    REPORT_POSITION_FINAL,
    REQUEST_LANDING,
    RUNWAY_VACATED,
    READBACK,
    REQUEST_FREQUENCY,
    UNABLE,
    SELF_ANNOUNCE,            // CTAF only
};

struct PilotMessage {
    std::string  raw_transcript;
    PilotIntent  intent;
    float        confidence;    // 0.0–1.0
    std::string  callsign;      // extracted if present
    std::string  runway;        // extracted if present
};

struct ATCResponse {
    std::string  text;
    ATCState     next_state;
    bool         requires_readback;
};

struct TranscriptEntry {
    double      sim_time;
    bool        is_pilot;
    std::string text;
    std::string frequency;
};
```

---

## ATC State Machine States

```
IDLE
GROUND_CONTACT → TAXI_CLEARED → TOWER_CONTACT
TOWER_CONTACT  → DEPARTURE_CLEARED / PATTERN_ENTRY
DEPARTURE_CLEARED → FREQUENCY_CHANGE → IDLE
PATTERN_ENTRY  → LANDING_CLEARED / GO_AROUND
LANDING_CLEARED → RUNWAY_VACATED → IDLE
UNICOM_ACTIVE  → IDLE
```

Towered airports use the GROUND/TOWER flow. Non-towered airports use UNICOM_ACTIVE (self-announce acknowledgement only, no clearances).

---

## OpenAI API Endpoints

All calls use `Authorization: Bearer <key>` from Keychain. All run on `std::thread`, never on the X-Plane main thread.

**Whisper:** `POST /v1/audio/transcriptions` — multipart/form-data, `file=<wav>`, `model=whisper-1`, `language=en`

**GPT fallback:** `POST /v1/chat/completions` — system prompt: ATC controller, ICAO phraseology only, plain text, max 2 sentences.

**TTS:** `POST /v1/audio/speech` — `model=tts-1`, `voice=<configurable>`, `response_format=mp3`

---

## Settings (data/settings.json)

```json
{
  "api_key_saved": false,
  "ptt_key_vk": -1,
  "ptt_joystick_button": -1,
  "tts_voice": "onyx",
  "tts_model": "tts-1",
  "whisper_model": "whisper-1",
  "gpt_model": "gpt-4o-mini",
  "gpt_fallback_enabled": true,
  "pilot_callsign": "November One Two Three Alpha Bravo",
  "active_com": 1,
  "volume": 1.0,
  "debug_logging": false
}
```

API key is never written to this file. It lives exclusively in the macOS Keychain under service name `xp_wellys_atc`.

---

## Coding Conventions

- C++17, no exceptions crossing the plugin boundary — catch all in `main.cpp`
- All X-Plane API calls on main thread only
- All network/audio calls on `std::thread` — use `std::atomic` flags for status
- `XPLMDebugString` for all logging (output → X-Plane Log.txt)
- `nlohmann::json` for all JSON parsing
- API key never appears in any log output, debug string, or error message
- clang-format + clang-tidy enforced
- No exceptions in destructors
- Each module header is self-contained — no circular includes
