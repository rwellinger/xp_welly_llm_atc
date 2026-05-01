# CLAUDE.md

This file provides permanent guidance to Claude Code when working in this repository.
Read this file completely before doing anything else.

---

## Project Overview

**xp_wellys_atc** (repo `xp_welly_llm_atc`) is a C++17 X-Plane 12 plugin for
**Apple Silicon macOS (ARM64 only)** that provides AI-powered ATC voice
communication for VFR flight simulation.

This is the **local-inference fork** of the upstream `rwellinger/xp_welly_atc`
project. The cloud-based STT / LLM / TTS stack is replaced with **fully local
inference**:

- **STT:** whisper.cpp (`small.en-q5_1`, Metal-accelerated)
- **LLM:** llama.cpp (Llama 3.2 3B Instruct Q4_K_M, Metal) — used only for
  low-confidence intent classification, not full response generation
- **TTS:** Piper (`en_US-lessac-medium`, CPU + onnxruntime) plus bundled
  `espeak-ng-data` for phonemization

Models (~2.0 GB combined) are **not bundled**. The plugin downloads them via
an in-sim ImGui dialog from HuggingFace on first launch (HTTPS, resumable,
SHA256-verified) into `<plugin>/Resources/models/`.

The ATC state machine, intent parser, ATIS generator, and UI structure are
unchanged from the upstream cloud project — only the inference backend differs.

License: **GPL-3.0-or-later** (inherited from upstream + required by espeak-ng).

---

## Build System

```bash
make setup    # X-Plane SDK, Dear ImGui, nlohmann/json, Catch2, spike submodules
make build    # CMake Release build → build/xp_wellys_atc.xpl
make install  # Code-sign + install to X-Plane plugins directory
make all      # clean + format + build + lint + test (full local CI)
make repl     # headless atc_repl tool (no X-Plane / no audio / no models)
make test     # Catch2 unit tests + scenario tests
make sanitize # ASan + UBSan build of engine OBJECT lib + atc_repl + tests
```

`make sanitize` instruments only the SDK-free engine code path. The
`.xpl` plugin module is NOT sanitized — ASan inside the X-Plane process
is fragile on macOS ARM64. Use Instruments.app (Leaks / Allocations
templates) attached to the X-Plane process for runtime leak hunting in
the live plugin.

- **CMake 3.26+**, C++17, **macOS 13.3+** (onnxruntime 1.22.0 requires this),
  **ARM64 only** — `CMAKE_OSX_ARCHITECTURES = "arm64"` is hard-set
- Toolchain: Homebrew LLVM (`/opt/homebrew/opt/llvm`), `ccache` auto-detected
- Output: `build/xp_wellys_atc.xpl` + staged `libpiper.dylib` +
  `libonnxruntime.{1.22.0,}.dylib` next to the `.xpl`, resolved at runtime
  via `@loader_path` rpath
- Compiler flags: `-Wall -Wextra -fvisibility=hidden`, OpenGL deprecation
  suppressed in our TUs only
- System frameworks linked: `AudioToolbox`, `AudioUnit`, `CoreAudio`,
  `AVFoundation`, `CoreFoundation`, `OpenGL`
- Network: system libcurl via `find_package(CURL)` (used **only** by the
  in-plugin model downloader — no other HTTP traffic)
- Inference libs: `whisper`, `llama`, `common` (static) + `piper` (shared
  dylib, links `libonnxruntime.1.22.0.dylib`)

## Vendor Dependencies

Populated by `make setup`, never committed:

| Path | Content |
|---|---|
| `sdk/` | X-Plane SDK headers (XPLM/, XPWidgets/) |
| `vendor/imgui/` | Dear ImGui v1.91.x |
| `vendor/json.hpp` | nlohmann/json v3.11.x |
| `spikes/spike_whisper/third_party/whisper.cpp/` | whisper.cpp submodule |
| `spikes/spike_llama/third_party/llama.cpp/` | llama.cpp submodule (provides `ggml`) |
| `spikes/spike_piper/third_party/piper1-gpl/` | Piper submodule (espeak-ng + onnxruntime) |

The CMake build pulls llama.cpp **first** so its pinned `ggml` target wins;
whisper.cpp then short-circuits on the existing target. This is documented
inline in `CMakeLists.txt`.

---

## Directory Structure

```
xp_welly_llm_atc/
├── CLAUDE.md
├── README.md, THIRD_PARTY.md, LICENSE
├── CMakeLists.txt
├── Makefile
├── VERSION.txt
├── src/
│   ├── main.cpp                # XPlugin* entry points, menu, flight loop
│   ├── atc/
│   │   ├── atc_session.hpp/.cpp        # PTT coordinator (plugin-only)
│   │   ├── engine.hpp/.cpp             # SDK-free transcript → response orchestrator
│   │   ├── intent_parser.hpp/.cpp      # Rule-based transcript → PilotIntent
│   │   ├── atc_state_machine.hpp/.cpp  # VFR ATC logic + template-based responses
│   │   ├── atc_templates.hpp/.cpp      # JSON template engine
│   │   ├── atis_generator.hpp/.cpp     # ATIS broadcast + letter management
│   │   └── flight_phase.hpp/.cpp       # Flight phase + precondition guards
│   ├── audio/
│   │   ├── ptt_input.hpp/.cpp          # Push-to-talk command binding
│   │   ├── audio_recorder.hpp/.cpp     # Core Audio mic capture → WAV buffer
│   │   ├── audio_player.hpp/.cpp       # Core Audio PCM playback on radio bus
│   │   └── mic_permission.hpp/.mm      # macOS microphone permission prompt
│   ├── backends/
│   │   ├── i_speech_to_text.hpp        # Strategy interface — STT
│   │   ├── i_language_model.hpp        # Strategy interface — LLM
│   │   ├── i_text_to_speech.hpp        # Strategy interface — TTS
│   │   ├── whisper_stt.hpp/.cpp        # whisper.cpp wrapper (plugin-only)
│   │   ├── llama_lm.hpp/.cpp           # llama.cpp wrapper (plugin-only)
│   │   ├── piper_tts.hpp/.cpp          # Piper wrapper (plugin-only)
│   │   ├── manager.hpp/.cpp            # std::thread async dispatch (SDK-free)
│   │   ├── loader.hpp/.cpp             # Verify + load concrete backends (plugin-only)
│   │   └── downloader.hpp/.cpp         # libcurl + Range resume + SHA256 (plugin-only)
│   ├── core/
│   │   ├── logging.hpp/.cpp            # XPLMDebugString + level-based logging
│   │   ├── xplane_context.hpp/.cpp     # SDK-free XPlaneContext struct + helpers
│   │   └── xplane_context_runtime.cpp  # SDK-coupled DataRef reader (plugin-only)
│   ├── data/
│   │   ├── airport_vrps.hpp/.cpp       # JSON-loaded VFR reporting points
│   │   └── airspace_db.hpp/.cpp        # apt.dat-derived airspace/controller index
│   ├── persistence/
│   │   ├── settings.hpp/.cpp           # JSON config (plugin-only — depends on plugin paths)
│   │   ├── model_paths.hpp/.cpp        # Resolve <plugin>/Resources/models/ via XPLMGetPluginInfo
│   │   └── model_manifest.hpp/.cpp     # Manifest entries + SHA256 (CommonCrypto, SDK-free)
│   └── ui/
│       └── atc_ui.hpp/.cpp             # Dear ImGui ATC panel + Models tab
├── data/
│   ├── settings.json                   # Runtime defaults (no secrets — committed)
│   ├── atc_prompt_templates.json       # whisper_prompt + gpt_classify_prompt
│   └── regions/
│       ├── eu/{atc_templates,flight_rules,airport_vrps}.json
│       └── us/{atc_templates,flight_rules}.json
├── tools/atc_repl/                     # Headless dev tool (engine OBJECT lib only)
├── tests/                              # Catch2 unit + scenario tests
├── spikes/                             # Spike submodules + experiments
├── sdk/                                # make setup, not committed
└── vendor/                             # make setup, not committed
```

Each `src/` subdirectory owns one concern. Includes use the subdir-prefixed
form (e.g. `#include "backends/whisper_stt.hpp"`) so dependencies are
visible at the call site.

The `xp_atc_engine` CMake **OBJECT** library compiles all SDK-free TUs
(engine, intent_parser, state machine, templates, flight phase, ATIS,
manager, data loaders, logging, xplane_context struct, model_manifest).
Both the plugin module and the headless `atc_repl` tool reuse it. The
plugin module adds the SDK-coupled units (main, atc_session, audio/*,
xplane_context_runtime, concrete backends, loader, downloader,
model_paths, settings, ui).

---

## Architecture

### Module Responsibilities

Each module uses a C++ namespace with `init()` and `stop()` lifecycle
functions called from `main.cpp` in dependency order.

**`main.cpp`** — `XPluginStart`, `XPluginStop`, `XPluginEnable`,
`XPluginDisable`. Registers flight loop callback. Calls `init()` / `stop()`
on all modules.

**`xplane_context`** — Reads DataRefs every flight loop iteration into the
`XPlaneContext` struct. Derives `nearest_airport_id` and `is_towered_airport`
via `XPLMGetNavAidInfo`. Parses `apt.dat` at init to build runway cache
(`RunwayInfo` per airport) and **full airport frequency database**
(`AirportFrequencies`, codes 50-55/1050-1055: ATIS, UNICOM, Delivery,
Ground, Tower, Approach). Determines `active_runway` from wind every
~1 second (calm wind < 3 kt → longest runway; otherwise largest headwind).
Frequency type derived by matching active COM frequency against the
airport's frequency database. Supports `tower_only` flag (Tower handles
taxi). Provides `set_standby_freq()` for ImGui frequency clicks.

**`atis_generator`** — Generates realistic ATIS broadcasts from
XPlaneContext weather data. Manages ATIS information letter (Alpha–Zulu),
incrementing on significant changes (active runway, wind dir >30°,
QNH >1 hPa, visibility category change). Auto-plays via TTS when pilot's
COM matches the airport's ATIS frequency within ~60 NM, with cooldown.

**`settings`** — Loads/saves `data/settings.json`. **No API keys** in this
fork — the local-inference build has no cloud credentials to manage.

**`ptt_input`** — Detects PTT activation via the X-Plane command
`xp_wellys_atc/ptt`. Notifies `atc_session` on press/release.

**`audio_recorder`** — Core Audio `AudioUnit` (`kAudioUnitSubType_HALOutput`)
captures mic at 16 kHz mono 16-bit PCM into `std::vector<int16_t>`. On PTT
release, hands the PCM buffer directly to `whisper_stt` — no WAV file roundtrip.

**`backends/i_speech_to_text`, `i_language_model`, `i_text_to_speech`** —
Pure-virtual strategy interfaces. The engine code only ever talks to these.

**`backends/whisper_stt`** — Loads `ggml-small.en-q5_1.bin`, runs Metal-
accelerated transcription. Reads `whisper_prompt` from
`atc_prompt_templates.json` to bias toward aviation vocabulary + NATO
phonetics. Returns `std::string` transcript.

**`backends/llama_lm`** — Loads `Llama-3.2-3B-Instruct-Q4_K_M.gguf`.
Runs Metal-accelerated inference for **low-confidence intent
classification only** (max_tokens ≈ 20, temperature 0.0). System prompt
from `gpt_classify_prompt` in `atc_prompt_templates.json`. The historical
`gpt_*` key name is preserved from the upstream cloud version.

**`backends/piper_tts`** — Loads `en_US-lessac-medium.onnx`, synthesizes
PCM via Piper + onnxruntime + bundled espeak-ng-data. ATIS speaks slower
via `length_scale=1.18`.

**`backends/manager`** — SDK-free `std::thread` dispatch + status atomics.
Lives in the engine OBJECT lib so the headless `atc_repl` can reuse it
(without any concrete backend registered).

**`backends/loader`** — Plugin-side. On startup, verifies model SHA256
hashes (`model_manifest`) and constructs the concrete backends on a worker
thread. Surfaces a "Models not ready" banner when files are missing or
invalid; PTT is disabled until all three backends are loaded.

**`backends/downloader`** — Plugin-side. libcurl HTTPS GET with `Range`
resume, streamed straight to the install volume (no temp roundtrip via
system disk — important for users on external SSDs). SHA256-verified
before renaming `<file>.part` → final filename.

**`intent_parser`** — Rule-based keyword/pattern matching on transcript +
`XPlaneContext`. Returns `PilotMessage` with `PilotIntent` enum and
confidence score. Supports sub-variant intents:
`INITIAL_CALL_GROUND/TOWER/INBOUND`,
`REPORT_POSITION_DOWNWIND/BASE/FINAL`, `RADIO_CHECK`,
`READY_FOR_DEPARTURE_VFR` (cross-country) etc.

**`atc_templates`** — JSON template engine. Loads `atc_templates.json`
for the active region at init. Provides `lookup(is_towered, state,
intent_key)` with `_INVALID` fallback, `fill(template, vars)` for variable
substitution. Hot-reload via `reload()`.

**`flight_phase`** — Detects current flight phase from `XPlaneContext`
each frame: `PARKED`, `TAXI`, `TAKEOFF_ROLL`, `CLIMB`, `PATTERN`,
`FINAL_APPROACH`, `LANDING_ROLL`, `CRUISE`. Phase detection is purely
geometric (groundspeed + AGL + heading); engine state is deliberately
ignored. Configurable thresholds from `flight_rules.json` with temporal
hysteresis. Provides `check_precondition`, `check_frequency_precondition`,
`get_auto_corrections`. Hot-reload via `reload()`.

**`atc_state_machine`** — Owns current `ATCState`. On valid `PilotIntent`,
transitions state and returns `ATCResponse` text via template lookup.
Applies two precondition guards from `flight_rules.json` before the
template lookup: flight-phase guard, then frequency guard. Tower-only
airports exempt Ground-class intents on the TOWER frequency. Provides
`check_auto_correction(phase, dt)` for state/phase mismatches,
`build_vars()` for template variable maps, `state_from_name()`,
`set_state()`.

**`atc_session`** — Owns the PTT state machine
(`IDLE → RECORDING → PROCESSING → PLAYING`). Coordinates the full
pipeline with two-stage intent resolution: high-confidence intents (≥0.7)
go directly through the state machine; low-confidence or UNKNOWN intents
route to `llama_lm` for classification. Blocks new PTT input while
`PROCESSING` or `PLAYING`.

**`audio_player`** — Plays PCM directly on the X-Plane radio bus,
respecting `settings.volume`.

**`atc_ui`** — Dear ImGui window. Status panel, Frequencies panel,
Phraseology Hints, Transcript history, Settings tab, **Models tab**
(download / re-verify / progress).

---

## Key Data Structures

```cpp
struct AirportFrequency {
    uint32_t freq_khz;                  // e.g. 121900 for 121.900 MHz
    FrequencyType type;                 // ATIS, GROUND, TOWER, etc.
};

struct AirportFrequencies {
    std::vector<AirportFrequency> all;
    bool has(FrequencyType) const;
    float first_mhz(FrequencyType) const; // 0.0 if none
    FrequencyType lookup(float) const;    // UNKNOWN if no match
    bool has_ground() const;
};

struct XPlaneContext {
    double      latitude, longitude;
    float       altitude_ft_msl;
    float       groundspeed_kts;
    float       indicated_airspeed_kts;
    float       vertical_speed_fpm;
    float       heading_true;
    float       height_agl_ft;
    bool        on_ground;
    float       com1_freq_mhz, com2_freq_mhz;
    int         active_com;             // 1 or 2
    std::string aircraft_icao;
    std::string nearest_airport_id;
    bool        is_towered_airport;
    FrequencyType frequency_type;
    float       visibility_m;
    float       cloud_base_ft_msl;
    int         cloud_type;             // 0=clear,1=few,2=scattered,3=broken,4=overcast
    float       temperature_c, dewpoint_c;
    float       atis_freq_mhz;
    AirportFrequencies airport_freqs;
    bool        tower_only;
    double      airport_lat, airport_lon;
    std::vector<RunwayInfo> runways;
    std::string active_runway;
};

enum class FlightPhase {
    PARKED, TAXI, TAKEOFF_ROLL, CLIMB,
    PATTERN, FINAL_APPROACH, LANDING_ROLL, CRUISE,
};

enum class PilotIntent {
    UNKNOWN,
    RADIO_CHECK,
    INITIAL_CALL,
    INITIAL_CALL_GROUND, INITIAL_CALL_TOWER, INITIAL_CALL_INBOUND,
    REQUEST_TAXI,
    READY_FOR_DEPARTURE, READY_FOR_DEPARTURE_VFR,
    REPORT_POSITION,
    REPORT_POSITION_DOWNWIND, REPORT_POSITION_BASE, REPORT_POSITION_FINAL,
    REQUEST_LANDING, REQUEST_TOUCH_AND_GO, GO_AROUND, RUNWAY_VACATED,
    READBACK, REQUEST_FREQUENCY, LEAVING_FREQUENCY, UNABLE,
    SELF_ANNOUNCE,
};

struct PilotMessage {
    std::string  raw_transcript;
    PilotIntent  intent;
    float        confidence;            // 0.0–1.0
    std::string  callsign, runway;
};

struct ATCResponse {
    std::string  text;
    ATCState     next_state;
    bool         requires_readback;
};
```

---

## ATC State Machine States

```
IDLE
GROUND_CONTACT → TAXI_CLEARED → TOWER_CONTACT
TOWER_CONTACT  → DEPARTURE_CLEARED / PATTERN_ENTRY / TOUCH_AND_GO_CLEARED
DEPARTURE_CLEARED (pattern) → PATTERN_ENTRY (auto-correction after takeoff)
DEPARTURE_CLEARED (cross-country) → REQUEST_FREQUENCY / LEAVING_FREQUENCY → EN_ROUTE → (airport change) → IDLE
PATTERN_ENTRY  → LANDING_CLEARED / TOUCH_AND_GO_CLEARED / GO_AROUND → PATTERN_ENTRY
TOUCH_AND_GO_CLEARED → PATTERN_ENTRY / LANDING_CLEARED / GO_AROUND → PATTERN_ENTRY
LANDING_CLEARED → RUNWAY_VACATED → IDLE / GO_AROUND → PATTERN_ENTRY
EN_ROUTE       → (silent, no ATC contact) → IDLE on nearest-airport change
UNICOM_ACTIVE  → IDLE
```

Towered airports use the GROUND/TOWER flow. Non-towered airports use
`UNICOM_ACTIVE` (self-announce acknowledgement only, no clearances).

---

## Local Inference Pipeline

All inference runs **locally** on Apple Silicon. No cloud, no API keys,
no network traffic at runtime (except the one-time HuggingFace model
download). Measured warm-pipeline latency on M4: STT 321 ms · LM 634 ms ·
TTS 200 ms · **total ≈ 1.16 s per request** — well under the 3 s
acceptance target.

| Stage | Backend | Model | Size |
|---|---|---|---|
| STT | whisper.cpp (Metal) | `ggml-small.en-q5_1.bin` | 181 MB |
| LM (intent classify) | llama.cpp (Metal) | `Llama-3.2-3B-Instruct-Q4_K_M.gguf` | 1.88 GB |
| TTS | Piper (onnxruntime CPU) | `en_US-lessac-medium.onnx` + `.json` | 60 MB |

The LM is **only** invoked when the rule-based `intent_parser` reports
confidence < 0.7. High-confidence intents skip the LM entirely.

Models live under `<plugin>/Resources/models/`. The downloader uses HTTPS
`Range`-resumable GETs from HuggingFace and SHA256-verifies before flipping
`<file>.part` → final filename. SHA256 hashes are listed in `README.md` for
manual fallback (corporate proxies, captive portals).

All inference work runs on `std::thread` — never on the X-Plane main
thread. Status is reported back via `std::atomic` flags.

---

## Settings (data/settings.json)

The local-inference build ships a slim schema — **no API keys, no model
selectors**:

```json
{
  "ptt_key_vk": 49,
  "ptt_joystick_button": -1,
  "pilot_callsign": "November One Two Three Alpha Bravo",
  "active_com": 1,
  "volume": 1.0,
  "pattern_direction": "left",
  "disable_default_atc": false,
  "skip_radio_power_check": false,
  "show_phraseology_hints": true,
  "auto_correction_factor": 1.0,
  "flow_region": "EU",
  "debug_logging": false
}
```

`settings.json` **is committed** in this repo with sensible defaults. It
contains no secrets in any historical revision. Push-to-Talk is bound via
the X-Plane command `xp_wellys_atc/ptt` (keyboard or joystick).

---

## Coding Conventions

- C++17, no exceptions crossing the plugin boundary — catch all in `main.cpp`
- All X-Plane API calls on the main thread only
- All inference / network / heavy work on `std::thread` — use `std::atomic`
  flags for status; never block the X-Plane main thread
- `XPLMDebugString` for all logging (output → X-Plane `Log.txt`).
  **Plain ASCII only (0x20–0x7E)** — both `XPLMDebugString` and the in-sim
  ImGui font render UTF-8 special chars as `?`
- `nlohmann::json` for all JSON parsing
- clang-format + clang-tidy enforced (`make format`, `make lint`)
- No exceptions in destructors
- Each module header is self-contained — no circular includes
- Use `make` for build, lint, release
- Use clean-code best practice — keep it simple to read
- Avoid deep `if`/`switch` nesting — extract helpers when it gets long
- Engine OBJECT library must stay SDK-free — any TU pulling in
  `<XPLM*.h>` belongs in the plugin module instead
