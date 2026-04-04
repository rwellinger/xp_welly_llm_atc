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
│   ├── intent_parser.hpp/.cpp  # Rule-based transcript → PilotIntent
│   ├── atc_state_machine.hpp/.cpp  # VFR ATC logic + response generation
│   ├── gpt_client.hpp/.cpp     # GPT-4o-mini fallback
│   ├── tts_client.hpp/.cpp     # OpenAI TTS API
│   ├── audio_player.hpp/.cpp   # Core Audio MP3 playback
│   ├── xplane_context.hpp/.cpp # X-Plane DataRef reader
│   ├── settings.hpp/.cpp       # JSON config + Keychain API key
│   └── atc_ui.hpp/.cpp         # Dear ImGui window
├── data/
│   └── settings.json           # Runtime config, never committed
├── sdk/                        # make setup, not committed
└── vendor/                     # make setup, not committed
```

---

## Architecture

### Module Responsibilities

Each module uses a C++ namespace with `init()` and `stop()` lifecycle functions called from `main.cpp`.

**`main.cpp`** — `XPluginStart`, `XPluginStop`, `XPluginEnable`, `XPluginDisable`. Registers flight loop callback and key sniffer. Calls `init()`/`stop()` on all modules in dependency order.

**`xplane_context`** — Reads DataRefs every flight loop iteration into `XPlaneContext` struct. Derives `nearest_airport_id` and `is_towered_airport` via `XPLMGetNavAidInfo`.

**`settings`** — Loads/saves `data/settings.json`. API key stored exclusively in macOS Keychain via `Security.framework` (`SecKeychainItemAdd` / `SecKeychainFindGenericPassword`). `settings.json` only stores `"api_key_saved": true` as a flag.

**`ptt_input`** — Detects PTT activation via `XPLMRegisterKeySniffer` (keyboard) or joystick button DataRef polling. Notifies `atc_session` on press/release.

**`audio_recorder`** — Core Audio `AudioUnit` (`kAudioUnitSubType_HALOutput`) captures mic at 16kHz mono 16-bit PCM into `std::vector<int16_t>`. On PTT release, encodes to WAV in memory (`std::vector<uint8_t>`) — no temp files.

**`whisper_client`** — POSTs WAV buffer to OpenAI `/v1/audio/transcriptions` via libcurl multipart. Returns `std::string` transcript. Runs on `std::thread`.

**`intent_parser`** — Rule-based keyword/pattern matching on transcript text + `XPlaneContext`. Returns `PilotMessage` with `PilotIntent` enum and confidence score.

**`atc_state_machine`** — Owns current `ATCState`. On valid `PilotIntent`, transitions state and returns `ATCResponse` text using phraseology templates.

**`gpt_client`** — Called by `atc_state_machine` when intent confidence is low or intent is `UNKNOWN`. POSTs to OpenAI `/v1/chat/completions` with ATC system prompt. Runs on `std::thread`.

**`tts_client`** — POSTs ATC response text to OpenAI `/v1/audio/speech`. Returns MP3 as `std::vector<uint8_t>`. Runs on `std::thread`.

**`audio_player`** — Decodes MP3 via `AudioToolbox ExtAudioFile`. Plays PCM via Core Audio default output. Respects `settings.volume`.

**`atc_session`** — Owns the PTT state machine (`IDLE → RECORDING → PROCESSING → PLAYING`). Coordinates the full pipeline. Blocks new PTT input while `PROCESSING` or `PLAYING`.

**`atc_ui`** — Dear ImGui window with Transcript panel, Status bar, and Settings tab.

---

## Key Data Structures

```cpp
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
};

enum class PilotIntent {
    UNKNOWN,
    INITIAL_CALL,
    REQUEST_TAXI,
    READY_FOR_DEPARTURE,
    TRAFFIC_REPORT,
    REQUEST_LANDING,
    REPORT_POSITION,        // downwind / base / final
    RUNWAY_VACATED,
    REQUEST_FREQUENCY,
    READBACK,
    UNABLE,
    SELF_ANNOUNCE,          // CTAF only
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
