# CLAUDE.md

This file provides permanent guidance to Claude Code when working in this repository.
Read this file completely before doing anything else.

---

## Project Overview

**xp_wellys_atc** (repo `xp_welly_llm_atc`) is a C++17 X-Plane 12 plugin for
**macOS 13.3+** that provides AI-powered ATC voice communication for VFR
flight simulation. Ships as a **universal binary** (`arm64 + x86_64`).

**Triple-backend inference** — the user picks the mode at runtime in Settings:

- **Local** (Apple Silicon `arm64` only): whisper.cpp `small.en-q5_1`
  (Metal STT) + llama.cpp Llama 3.2 3B Instruct Q4_K_M (Metal LM, used
  only for low-confidence intent classification) + Piper
  `en_US-lessac-medium` (CPU + onnxruntime TTS) with bundled
  `espeak-ng-data`. Models (~2.0 GB) are NOT bundled — downloaded in-sim
  from HuggingFace on first launch (HTTPS, resumable, SHA256-verified)
  into `<plugin>/Resources/models/`. Voice catalog is multi-language
  (en + de Piper voices, optional rows).
- **OpenAI Cloud** (any Mac, BYO API key): Whisper API (STT) + Chat
  Completions `gpt-4o-mini` JSON-mode (LM) + TTS API (TTS, six voices).
  API key stored in the macOS Keychain via `Security.framework` under
  service `com.xp_wellys_atc.openai`, never in `settings.json`.
- **Mistral Cloud** (any Mac, BYO API key): Voxtral STT
  (`voxtral-mini-2507`) + Mistral Chat Completions
  (`mistral-small-latest`, JSON-mode) + Voxtral TTS
  (`voxtral-mini-tts-2603`, 30 preset voices across GB/US/FR speakers).
  Separate Keychain entry `com.xp_wellys_atc.mistral` so the OpenAI key
  (if any) stays untouched. Multilingual TTS — only mode that speaks
  German without a US accent in cloud.

The `x86_64` slice has **no** local backends compiled in; **OpenAI** or
**Mistral** is the only option on Intel Macs (the loader silently
rewrites `local` → `openai` on startup for that slice).

All three backend families share the same three abstract `i_*.hpp`
strategy interfaces. Engine code never touches a concrete backend. See
the **Backend Adapter Rule** section — it is a hard invariant enforced
by `tests/test_audit_logging.cpp`. For user-facing details (mode
switching, audit log tags, model URLs / SHA256) see `README.md`.

License: **GPL-3.0-or-later** (required by espeak-ng, statically linked
into the bundled `libpiper.dylib` used by the `arm64` slice).

---

## Build System

```bash
make setup     # X-Plane SDK, Dear ImGui, nlohmann/json, Catch2, spike submodules
make build     # Universal Release build → build/xp_wellys_atc.xpl (arm64+x86_64 lipo'd)
make install   # Code-sign + install to X-Plane plugins directory
make all       # clean + format + build + lint + test (full local CI)
make repl      # headless atc_repl tool (no X-Plane / no audio / no models)
make test      # Catch2 unit tests + scenario tests (incl. audit invariant)
make sanitize  # ASan + UBSan build of engine OBJECT lib + atc_repl + tests
```

`make build` always produces the universal binary: CMake runs twice —
arm64 with `XPWELLYS_USE_LOCAL_INFERENCE=ON` (build-arm64/), x86_64 with
the same flag `OFF` (build-x86_64/) — and `lipo`-merges the two `.xpl`s
into `build/xp_wellys_atc.xpl`. The arm64 slice's `libpiper.dylib` and
`libonnxruntime.{1.22.0,}.dylib` are staged next to the lipo'd binary
so `make install` finds them. `make release-build` is the same build
with `-DRELEASE=ON` passed through (used by the GitHub Actions tag
workflow).

`make sanitize` instruments only the SDK-free engine code path. The
`.xpl` plugin module is NOT sanitized — ASan inside the X-Plane process
is fragile on macOS ARM64. Use Instruments.app (Leaks / Allocations
templates) attached to the X-Plane process for runtime leak hunting in
the live plugin.

- **CMake 3.26+**, C++17, **macOS 13.3+** (onnxruntime 1.22.0 requires this)
- **CMake option `XPWELLYS_USE_LOCAL_INFERENCE`** (default `ON`) — gates
  whether the three local backends (`whisper_stt`, `llama_lm`, `piper_tts`)
  and their submodule dependencies (whisper.cpp, llama.cpp, Piper) are
  compiled and linked. Turn `OFF` for the x86_64 slice; the resulting
  binary has zero local-inference code.
- Toolchain: Homebrew LLVM (`/opt/homebrew/opt/llvm`), `ccache` auto-detected
- Output: `build/xp_wellys_atc.xpl`; on the arm64 slice also staged
  `libpiper.dylib` + `libonnxruntime.{1.22.0,}.dylib` next to the `.xpl`,
  resolved at runtime via `@loader_path` rpath. The x86_64 slice has
  none of these dylibs — it is a much smaller artifact.
- Compiler flags: `-Wall -Wextra -fvisibility=hidden`, OpenGL deprecation
  suppressed in our TUs only
- System frameworks linked: `AudioToolbox`, `AudioUnit`, `CoreAudio`,
  `AVFoundation`, `CoreFoundation`, `OpenGL`, `Security` (Keychain)
- Network: system libcurl via `find_package(CURL)` — used by the model
  downloader (arm64 / Local mode) AND by every cloud HTTPS call (both
  OpenAI and Mistral clients). The three local backends MUST NOT use
  libcurl; see Backend Adapter Rule.
- Inference libs (arm64 only): `whisper`, `llama`, `common` (static) +
  `piper` (shared dylib, links `libonnxruntime.1.22.0.dylib`). The
  x86_64 slice links only libcurl + the system frameworks above.

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
│   │   ├── atc_session.hpp/.cpp        # PTT coordinator + Debug-Text injection (plugin-only)
│   │   ├── engine.hpp/.cpp             # SDK-free transcript → response orchestrator
│   │   ├── intent_parser.hpp/.cpp      # Rule-based transcript → PilotIntent (lexing + scoring)
│   │   ├── intent_rules.hpp/.cpp       # Per-profile rule tables consumed by intent_parser
│   │   ├── atc_state_machine.hpp/.cpp  # VFR ATC logic + template-based responses + state-revert guard
│   │   ├── atc_templates.hpp/.cpp      # JSON template engine
│   │   ├── atis_generator.hpp/.cpp     # ATIS broadcast + letter management
│   │   ├── flight_phase.hpp/.cpp       # Flight phase + precondition guards
│   │   ├── phraseology_hints.hpp/.cpp  # Context-aware ICAO phraseology cheat sheet (SDK-free)
│   │   ├── traffic_advisor.hpp/.cpp    # En-route traffic advisory generator (SDK-free)
│   │   ├── traffic_dialog.hpp/.cpp     # Pilot reply parser ("in sight" / "negative") (SDK-free)
│   │   ├── landing_sequence.hpp/.cpp   # Phase-4 sequencing + runway-occupancy primitives (SDK-free)
│   │   ├── bzf_compliance.hpp/.cpp     # DE-only: NfL §25 b) Nr. 1 readback check (SDK-free)
│   │   ├── de_phraseology.hpp/.cpp     # DE-only: callsign / number expansion helpers (SDK-free)
│   │   └── flows/                      # Per-context flow modules (SDK-free)
│   │       ├── ground_operations.hpp/.cpp   # Taxi / clearance-delivery / engine-start
│   │       ├── pattern_flow.hpp/.cpp        # Pattern + landing-sequence overlay
│   │       ├── crosscountry_flow.hpp/.cpp   # En-route freq change / handoff
│   │       ├── flow_coordinator.hpp/.cpp    # Dispatch by ATCState
│   │       └── state_storage.hpp            # AtcStateSnapshot + generation counter
│   ├── audio/
│   │   ├── ptt_input.hpp/.cpp          # Push-to-talk command binding
│   │   ├── audio_recorder.hpp/.cpp     # Core Audio mic capture → 16 kHz PCM
│   │   ├── audio_player.hpp/.cpp       # Core Audio PCM playback on radio bus + squelch-burst
│   │   └── mic_permission.hpp/.mm      # macOS microphone permission prompt
│   ├── backends/
│   │   ├── i_speech_to_text.hpp        # Strategy interface — STT
│   │   ├── i_language_model.hpp        # Strategy interface — LLM
│   │   ├── i_text_to_speech.hpp        # Strategy interface — TTS
│   │   ├── whisper_stt.hpp/.cpp        # whisper.cpp wrapper — Local backend (gated by XPWELLYS_USE_LOCAL_INFERENCE)
│   │   ├── llama_lm.hpp/.cpp           # llama.cpp wrapper — Local backend (gated)
│   │   ├── piper_tts.hpp/.cpp          # Piper wrapper — Local backend (gated)
│   │   ├── openai_common.hpp/.cpp      # Shared libcurl + JSON helpers for OpenAI clients
│   │   ├── openai_stt.hpp/.cpp         # OpenAI Whisper API client (both slices)
│   │   ├── openai_lm.hpp/.cpp          # OpenAI Chat Completions client (both slices)
│   │   ├── openai_tts.hpp/.cpp         # OpenAI TTS API client (both slices)
│   │   ├── mistral_stt.hpp/.cpp        # Mistral Voxtral STT client (both slices)
│   │   ├── mistral_lm.hpp/.cpp         # Mistral Chat Completions client (both slices)
│   │   ├── mistral_tts.hpp/.cpp        # Mistral Voxtral TTS client — JSON envelope w/ base64 WAV (both slices)
│   │   ├── manager.hpp/.cpp            # std::thread async dispatch (SDK-free)
│   │   ├── loader.hpp/.cpp             # Mode-aware backend bring-up (plugin-only)
│   │   └── downloader.hpp/.cpp         # libcurl + Range resume + SHA256 (plugin-only)
│   ├── core/
│   │   ├── logging.hpp/.cpp            # XPLMDebugString + level-based logging
│   │   ├── xplane_context.hpp/.cpp     # SDK-free XPlaneContext struct + helpers
│   │   └── xplane_context_runtime.cpp  # SDK-coupled DataRef reader (plugin-only)
│   ├── data/
│   │   ├── airport_vrps.hpp/.cpp       # JSON-loaded VFR reporting points (bundled + user override)
│   │   ├── airspace_db.hpp/.cpp        # apt.dat-derived airspace/controller index
│   │   ├── traffic_context.hpp/.cpp    # SDK-free TrafficContext struct + helpers
│   │   ├── traffic_context_runtime.cpp # 2 Hz TCAS DataRef snapshot (plugin-only)
│   │   ├── traffic_geometry.hpp/.cpp   # Relative-bearing / clock-position math (SDK-free)
│   │   └── traffic_phase_classifier.hpp/.cpp # Promotes airborne targets to Pattern/Final (SDK-free)
│   ├── persistence/
│   │   ├── settings.hpp/.cpp           # JSON config (plugin-only — depends on plugin paths)
│   │   ├── keychain.hpp/.cpp           # macOS Security.framework wrapper for cloud API keys (OpenAI + Mistral)
│   │   ├── model_paths.hpp/.cpp        # Resolve <plugin>/Resources/models/ via XPLMGetPluginInfo
│   │   ├── model_manifest.hpp/.cpp     # Manifest entries + SHA256 (CommonCrypto, SDK-free)
│   │   └── models_catalog.hpp/.cpp     # Loads data/models_catalog.json — single source of truth for all selectable model slugs and voices
│   └── ui/
│       ├── atc_ui.hpp/.cpp             # Dear ImGui ATC panel + Settings/Models/Traffic tabs + Debug-Texteingabe row
│       ├── ui_strings.hpp/.cpp         # Per-profile i18n string lookup (eu/us/de)
│       └── clipboard.hpp/.mm           # System pasteboard read helper (NSPasteboard) — backs [Paste] buttons
├── data/
│   ├── settings.json                   # Runtime defaults (no secrets — committed)
│   ├── models_catalog.json             # Selectable model slugs + voices for all three backends (editable, no recompile)
│   ├── atc_prompt_templates.json       # whisper_prompt + gpt_classify_prompt (key name kept for compatibility)
│   ├── vrps/airport_vrps.json          # Profile-independent VRP / pattern-direction DB (overridable by user file)
│   └── atc_profiles/
│       ├── eu/{atc_templates,flight_rules,ui_strings}.json
│       ├── us/{atc_templates,flight_rules,ui_strings}.json
│       └── de/{atc_templates,flight_rules,ui_strings}.json
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
(engine, intent_parser, intent_rules, state machine, templates, flight
phase, ATIS, phraseology_hints, traffic_advisor, traffic_dialog,
landing_sequence, flows/*, bzf_compliance, de_phraseology, manager,
data loaders, traffic_context struct, traffic_geometry,
traffic_phase_classifier, logging, xplane_context struct,
model_manifest, models_catalog, ui_strings).
Both the plugin module and the headless `atc_repl` tool reuse it. The
plugin module adds the SDK-coupled units (main, atc_session, audio/*,
xplane_context_runtime, traffic_context_runtime, loader, downloader,
model_paths, settings, keychain, ui/atc_ui, ui/clipboard) and the two
cloud backend families: OpenAI (`openai_common`, `openai_stt`,
`openai_lm`, `openai_tts`) and Mistral (`mistral_stt`, `mistral_lm`,
`mistral_tts`). The three local backends (`whisper_stt`, `llama_lm`,
`piper_tts`) are added only when `XPWELLYS_USE_LOCAL_INFERENCE=ON`.

---

## Architecture

### Module Responsibilities

Each module uses a C++ namespace with `init()` and `stop()` lifecycle
functions called from `main.cpp` in dependency order.

**`main.cpp`** — `XPluginStart`, `XPluginStop`, `XPluginEnable`,
`XPluginDisable`. Registers flight loop callback. Calls `init()` / `stop()`
on all modules.

**`xplane_context`** — Reads DataRefs each flight loop into the
`XPlaneContext` struct. Derives `nearest_airport_id` / `is_towered_airport`
via `XPLMGetNavAidInfo`. Parses `apt.dat` at init (background thread,
`build_towered_cache()` in `xplane_context_runtime.cpp`) and builds:

- **Frequency DB** (`AirportFrequencies`) — row codes 50-55 (legacy) and
  1050-1055 (XP12): ATIS, UNICOM, Delivery, Ground, Tower, Approach.
  Both old (MHz×100) and new (kHz direct) numeric formats handled.
- **Runway cache** (`RunwayInfo` per airport) — row code **100** (land
  runways): both ends (number, lat/lon, heading), width, surface code,
  length. Used for active-runway selection and traffic geometry.
- **Airport name + elevation** — from the row-1/16/17 header tokens.
- **Airport reference position** — midpoint of first runway pair.

Row codes **not yet parsed** (reserved for future taxi-routing work):
- **1201** — taxiway nodes (lat/lon + name: A1, B3, hold-short points)
- **1202** — taxiway edges (from/to node, taxiway name, one-way flag)
- **1204** — active zones (edges crossing runways — hot spots)

Picks `active_runway` from wind ~1 Hz (calm < 3 kt → longest runway,
else largest headwind; CIFP SID runway preferred when IFR squawk is
assigned). `frequency_type` derived by matching the active COM against
the airport DB. Supports `tower_only` (Tower handles taxi).
Provides `set_standby_freq()` for ImGui frequency clicks.

**`atis_generator`** — Generates realistic ATIS broadcasts from
XPlaneContext weather data. Manages ATIS information letter (Alpha–Zulu),
incrementing on significant changes (active runway, wind dir >30°,
QNH >1 hPa, visibility category change). Auto-plays via TTS when pilot's
COM matches the airport's ATIS frequency within ~60 NM, with cooldown.

**`settings`** — Loads/saves `data/settings.json`. Holds the
`backend_mode` toggle (`local` | `openai` | `mistral`), the OpenAI and
Mistral model/voice IDs, the `atc_profile` (`EU` | `US` | `DE` — the
legacy key `flow_region` is written in parallel for rollback safety
and will be dropped later), `bzf_strict_mode`, `start_mode`,
`traffic_features_enabled`, `debug_text_input`, etc. **No API key ever
lives in `settings.json`** — only the flags `api_key_saved` and
`mistral_api_key_saved` are persisted; the actual secrets sit in the
macOS Keychain under two separate services
(`com.xp_wellys_atc.openai` and `com.xp_wellys_atc.mistral`, both with
account `default`) via `persistence/keychain`.

**`keychain`** — Plugin-only. Wraps macOS `Security.framework`
(`SecItemAdd`, `SecItemCopyMatching`, `SecItemDelete`) as a generic
`(service, account) → secret` interface. Used twice: once for the
OpenAI key (`com.xp_wellys_atc.openai`) and once for the Mistral key
(`com.xp_wellys_atc.mistral`), so the two coexist and switching modes
never requires re-pasting. Logs only the last 4 characters of the key
(`sk-...ABCD` for OpenAI, `...ABCD` for Mistral — no `sk-` prefix
there); a full key value must never appear in `Log.txt`.

**`models_catalog`** — SDK-free loader for `data/models_catalog.json`.
Exposes per-backend STT / LM / TTS / voice options
(`models_catalog::openai_*_options()`, `mistral_*_options()`,
`local_piper_voices()`, `local_whisper_files()`,
`local_llama_files()`). The Settings dropdowns and the Models-tab
download list are driven by it. Read once at startup; restart X-Plane
after editing the JSON. New OpenAI/Mistral model slugs and new Piper
voices land here, not in C++.

**`ptt_input`** — Detects PTT activation via the X-Plane command
`xp_wellys_atc/ptt`. Notifies `atc_session` on press/release.

**`audio_recorder`** — Core Audio `AudioUnit` (`kAudioUnitSubType_HALOutput`)
captures mic at 16 kHz mono 16-bit PCM into `std::vector<int16_t>`. On PTT
release, hands the PCM buffer to the active STT strategy via
`backends::stt()` — concrete backend choice (whisper.cpp / OpenAI /
Mistral) is invisible here. No WAV file roundtrip.

**`backends/i_speech_to_text`, `i_language_model`, `i_text_to_speech`** —
Pure-virtual strategy interfaces. The engine code only ever talks to
these — see **Backend Adapter Rule** below.

**Local backends** (`whisper_stt`, `llama_lm`, `piper_tts`) — all gated
by `XPWELLYS_USE_LOCAL_INFERENCE`, emit `[STT|LM|TTS]-LOCAL` audit
tags. Model files (path + SHA256) come from `models_catalog` via
`model_manifest`; the active Whisper file is `en` or `de` depending on
`atc_profile`, the Llama file is multilingual + shared. `whisper_stt`
loads with Metal accel and the `whisper_prompt` aviation bias.
`llama_lm` runs at max_tokens ≈ 20, temp 0.0 and uses the
`gpt_classify_prompt` system prompt (key name kept for backwards
compatibility). `piper_tts` loads the per-role voice ONNX via Piper +
onnxruntime + bundled espeak-ng-data; ATIS uses `length_scale=1.18`.

**OpenAI backends** (`openai_stt`, `openai_lm`, `openai_tts`) — built
into both slices, emit `[STT|LM|TTS]-OPENAI` audit tags. Endpoints:
`v1/audio/transcriptions` (multipart WAV), `v1/chat/completions` (JSON
mode), `v1/audio/speech` (MP3/Opus → PCM). Model IDs and per-role
voices come from `settings::openai_*` (the dropdown options behind
those are populated from `models_catalog`). `openai_common` provides
the shared libcurl + JSON helpers and truncates the API key to last 4
chars (`sk-...ABCD`) for any log line.

**Mistral backends** (`mistral_stt`, `mistral_lm`, `mistral_tts`) —
built into both slices, emit `[STT|LM|TTS]-MISTRAL` audit tags.
Endpoints: `v1/audio/transcriptions` (Voxtral STT, with `context_bias[]`
airport biasing), `v1/chat/completions` (JSON mode), `v1/audio/speech`
(Voxtral TTS — note: returns a **JSON envelope** `{"audio_data":
"<base64 WAV>"}` regardless of the `response_format` requested; inner
WAV is 24 kHz mono 16-bit, decoded in-process). Model IDs and per-role
voices come from `settings::mistral_*`, again sourced from
`models_catalog`. Reuses `openai_common`'s libcurl/JSON helpers where
shape is identical; key is truncated to `...ABCD` (no `sk-` prefix —
Mistral keys are not OpenAI-formatted).

**`backends/manager`** — SDK-free `std::thread` dispatch + status
atomics. Lives in the engine OBJECT lib so the headless `atc_repl` can
reuse it without a concrete backend registered.

**`backends/loader`** — Plugin-side, mode-aware bring-up on a worker
thread driven by `settings::backend_mode()`. `"openai"` → reads the
OpenAI Keychain entry (or surfaces a "no key" error) and registers
`OpenAi{Stt,Lm,Tts}`. `"mistral"` → reads the Mistral Keychain entry
and registers `Mistral{Stt,Lm,Tts}`. `"local"` (requires
`XPWELLYS_USE_LOCAL_INFERENCE`) → verifies SHA256 hashes via
`model_manifest`, then registers `{WhisperStt,LlamaLm,PiperShim}`. The
x86_64 slice silently rewrites `local` → `openai` and persists
(`mistral` is honored on both slices). Logs a one-line
`BACKEND MODE: ...` banner — the audit anchor for which side served a
session.

**`backends/downloader`** — Plugin-side. libcurl HTTPS GET with `Range`
resume, streamed straight to the install volume (no temp roundtrip via
system disk). SHA256-verified before renaming `<file>.part` → final
filename. Used only by Local mode for HuggingFace model fetches; never
touches the cloud APIs.

**`intent_parser` / `intent_rules`** — Rule-based keyword/pattern
matching on transcript + `XPlaneContext`. `intent_parser` is the
lexer/scorer; `intent_rules` carries the per-profile rule tables it
consumes. Returns `PilotMessage` (intent + confidence). Sub-variants
like `INITIAL_CALL_{GROUND,TOWER,INBOUND}`, `REPORT_POSITION_{DOWNWIND,
BASE,FINAL}`, `READY_FOR_DEPARTURE_VFR` are first-class.

**`atc_templates`** — JSON template engine. Loads `atc_templates.json`
per region at init. `lookup(is_towered, state, intent_key)` with
`_INVALID` fallback; `fill(template, vars)` for substitution. Hot-reload
via `reload()`.

**`flight_phase`** — Geometric phase detection (`PARKED`, `TAXI`,
`TAKEOFF_ROLL`, `CLIMB`, `PATTERN`, `FINAL_APPROACH`, `LANDING_ROLL`,
`CRUISE`) from groundspeed + AGL + heading; engine state ignored.
Thresholds + hysteresis from `flight_rules.json`. Provides
`check_precondition`, `check_frequency_precondition`,
`get_auto_corrections`. Hot-reload via `reload()`.

**`atc_state_machine`** — Owns the current `ATCState`. On a valid
`PilotIntent` it applies two precondition guards (flight-phase, then
frequency) from `flight_rules.json`, then looks up the template and
returns `ATCResponse`. Tower-only airports exempt Ground-class intents
on the TOWER frequency. Also: `check_auto_correction(phase, dt)`,
`build_vars()`, `state_from_name()`, `set_state()`.

**`atc_session`** — Owns the PTT state machine
(`IDLE → RECORDING → PROCESSING → PLAYING`). Coordinates the full
pipeline with two-stage intent resolution: high-confidence intents (≥0.7)
go directly through the state machine; low-confidence or UNKNOWN intents
route through the LM strategy interface (`backends::lm()`) for
classification — whether that resolves to local Llama, `gpt-4o-mini`,
or `mistral-small` is invisible to this module. Blocks new PTT input
while `PROCESSING` or `PLAYING`. Also exposes `submit_text(text)` —
the Debug-Texteingabe entry point (gated by `settings::debug_text_input`)
that bypasses STT and feeds a typed transcript straight into
`engine::process_transcript`; LM + state machine + TTS run identically
to the voice path, so the Tower reply is still spoken through the
active backend's TTS. The state-revert / TTS-failure guard
(snapshot before each pilot turn, restore-or-`REQUEST_REPEAT` on TTS
failure with a squelch burst) lives here too — see README for the
behavioural contract.

**`audio_player`** — Plays PCM directly on the X-Plane radio bus,
respecting `settings.volume`.

**Traffic subsystem** (v2.1 advisories + v2.2 sequencing) —
provider-agnostic 2 Hz `TrafficContext` snapshot from
`sim/cockpit2/tcas/targets/...` (works with stock, LiveTraffic, xPilot,
etc.). `traffic_geometry` computes relative bearing / clock-position /
slant range plus the Phase-4 runway-centerline projection
(`is_on_runway_centerline`). `traffic_advisor` builds EU-phraseology
advisories ("Traffic, two o'clock, 3 miles, same altitude, opposite
direction") and dispatches them through the TTS strategy interface on a
**side channel** that does not block the main ATC flow; cooldown + dedup
built in. `traffic_dialog` parses the pilot reply (`"in sight" /
"negative contact" / "looking"`) into a `TrafficReply` enum.
`traffic_phase_classifier` promotes airborne targets to
`Pattern` / `Final` when the live `AirportRunwayHints` (active-runway
threshold + heading) match — feeds Phase-4 sequencing.
`landing_sequence::compute_landing_sequence()` is a pure function that
sorts Final-phase targets by distance-to-threshold, derives the user's
1-based sequence position, and scans for ground-phase occupants on the
runway centerline. `pattern_flow::apply_landing_sequence()` is the
Pattern-side overlay that rewrites `Pattern/LANDING_CLEARED` responses
into `number_to_land_follow` ("number N, follow the X on Y") or
`continue_approach_traffic_runway` when sequencing applies.
`engine::poll_go_around()` is the frame-driven unsolicited Tower call
that fires `go_around_traffic_runway` when the user is within 1 NM of
an occupied runway (render-only, no state change, 60 s cooldown).
**Master switch** `settings::traffic_features_enabled` (default `true`)
gates the entire subsystem at one point: `traffic_context::update()`
returns early with an empty snapshot when off, so every downstream
consumer (advisor / sequencing overlay / poll_go_around) becomes a
no-op against the empty `TrafficContext`.

**`atc_ui`** — Dear ImGui window. Status panel, Frequencies panel,
Phraseology Hints, Transcript history with an optional
**Debug-Texteingabe** InputText row (toggle:
`settings::debug_text_input`, calls `atc_session::submit_text`),
**Settings tab** (Backend Mode switcher with separate `[Paste]` /
`Save Key` / `Delete Key` buttons for the OpenAI and Mistral keys —
Cmd+V is intercepted by X-Plane command bindings; per-backend STT / LM
/ TTS / voice combos driven by `models_catalog`), **Models tab**
(download / re-verify / progress; relevant in Local mode but visible
in all modes), and an optional **Traffic tab** (debug, gated by
`settings.debug_traffic`, lists the 10 nearest aircraft). All strings
are looked up via `ui_strings` against the active profile's
`ui_strings.json`. The `[Paste]` buttons read the system pasteboard
via `ui/clipboard`.

---

## Backend Adapter Rule (HARD INVARIANT)

Engine code talks exclusively to the three strategy interfaces
`backends/i_{speech_to_text,language_model,text_to_speech}.hpp`. The
mode decision lives in **one place only**: `backends/loader.cpp::run_worker()`.
Engine code MUST NOT inspect `settings::backend_mode()` or branch on it
— if you want to, the interface is missing an abstraction.

**Source-level invariants enforced by `tests/test_audit_logging.cpp`**
(literal `grep` against the .cpp files at `make test` time):

- `whisper_stt.cpp` / `llama_lm.cpp` / `piper_tts.cpp` — must carry
  their `*-LOCAL` audit tag; must NOT contain `OPENAI`, `MISTRAL`,
  `api.openai.com`, `api.mistral.ai`, or `curl_easy_perform`.
- `openai_stt.cpp` / `openai_lm.cpp` / `openai_tts.cpp` — must carry
  their `*-OPENAI` audit tag; must NOT `#include` `whisper.h`, `llama.h`,
  or `piper.h`; must NOT contain a `-LOCAL]` or `-MISTRAL]` tag.
- `mistral_stt.cpp` / `mistral_lm.cpp` / `mistral_tts.cpp` — must carry
  their `*-MISTRAL` audit tag; must NOT `#include` `whisper.h`,
  `llama.h`, or `piper.h`; must NOT contain a `-LOCAL]` or `-OPENAI]`
  tag, and must NOT reference `api.openai.com`.

A stray include or copy-pasted log tag fails CI.

**Build-time enforcement:** the CMake option `XPWELLYS_USE_LOCAL_INFERENCE`
gates whether the three local `.cpp` files are added to the build at all.
The x86_64 slice is built with this `OFF` and has zero whisper/llama/piper
symbols. The local backends' headers MUST NOT be `#include`d outside an
`#ifdef XPWELLYS_USE_LOCAL_INFERENCE` block — the only legitimate
consumer is `backends/loader.cpp`. The two cloud backend families
(OpenAI + Mistral) are unconditionally compiled into both slices.

**Adding a backend-touching feature:** extend the `i_*.hpp` interface
first, then implement symmetrically in all three families (Local AND
OpenAI AND Mistral). Every inference call emits an
`[STT|LM|TTS]-[LOCAL|OPENAI|MISTRAL]` log line; the startup
`BACKEND MODE: ...` banner is the durable audit anchor.

---

## Key Data Structures

Authoritative declarations live in `src/core/xplane_context.hpp` and
`src/atc/{intent_parser,atc_state_machine}.hpp`. Sketch for quick
orientation:

```cpp
struct XPlaneContext {                  // src/core/xplane_context.hpp
    // position + dynamics
    double latitude, longitude;
    float  altitude_ft_msl, height_agl_ft;
    float  groundspeed_kts, indicated_airspeed_kts, vertical_speed_fpm;
    float  heading_true;
    bool   on_ground;
    // radios
    float  com1_freq_mhz, com2_freq_mhz;
    int    active_com;                              // 1 or 2
    // nearest airport (derived each frame)
    std::string aircraft_icao, nearest_airport_id;
    bool        is_towered_airport, tower_only;
    FrequencyType frequency_type;                   // matched against airport_freqs
    AirportFrequencies airport_freqs;               // 50-55 / 1050-1055
    double airport_lat, airport_lon;
    std::vector<RunwayInfo> runways;
    std::string active_runway;
    // weather (for ATIS)
    float visibility_m, cloud_base_ft_msl;
    int   cloud_type;                               // 0=clear..4=overcast
    float temperature_c, dewpoint_c, atis_freq_mhz;
};

enum class FlightPhase { PARKED, TAXI, TAKEOFF_ROLL, CLIMB,
                         PATTERN, FINAL_APPROACH, LANDING_ROLL, CRUISE };

enum class PilotIntent { UNKNOWN, RADIO_CHECK,
    INITIAL_CALL, INITIAL_CALL_GROUND, INITIAL_CALL_TOWER, INITIAL_CALL_INBOUND,
    REQUEST_TAXI, READY_FOR_DEPARTURE, READY_FOR_DEPARTURE_VFR,
    REPORT_POSITION, REPORT_POSITION_DOWNWIND, REPORT_POSITION_BASE, REPORT_POSITION_FINAL,
    REQUEST_LANDING, REQUEST_TOUCH_AND_GO, GO_AROUND, RUNWAY_VACATED,
    READBACK, REQUEST_FREQUENCY, LEAVING_FREQUENCY, UNABLE, SELF_ANNOUNCE };

struct PilotMessage { std::string raw_transcript, callsign, runway;
                     PilotIntent intent; float confidence; };
struct ATCResponse  { std::string text; ATCState next_state; bool requires_readback; };
```

`AirportFrequencies` exposes `has(FrequencyType)`, `first_mhz(...)`,
`lookup(float mhz)` (returns `UNKNOWN` on miss), `has_ground()`.

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

## Inference Pipelines

Three backends, same `i_*.hpp` interface. The LM stage is invoked **only**
when `intent_parser` returns confidence < 0.7 — high-confidence intents
skip it entirely. All inference runs on `std::thread`s with `std::atomic`
status; the X-Plane main thread is never blocked.

- **Local** (arm64, `XPWELLYS_USE_LOCAL_INFERENCE=ON`): whisper.cpp →
  llama.cpp (max_tokens ≈ 20, temp 0.0) → Piper. Warm pipeline ≈ 1.16 s
  on M4 (321 + 634 + 200 ms). Models in `<plugin>/Resources/models/`,
  fetched on first launch via HuggingFace HTTPS+`Range`+SHA256. See
  `README.md` for model URLs and hashes.
- **OpenAI Cloud** (both slices): `whisper-1` → `gpt-4o-mini` (JSON
  mode) → `tts-1`. Latency typically 2–3 s warm. Voices selectable
  per role (`atis` / `tower` / `ground`); `onyx` is closest to real
  ATC. API key in Keychain under `com.xp_wellys_atc.openai`, last 4
  chars logged for audit.
- **Mistral Cloud** (both slices): `voxtral-mini-2507` →
  `mistral-small-latest` (JSON mode) → `voxtral-mini-tts-2603`.
  Latency typically 2–3 s warm. 30 preset voices across GB/US/FR
  speakers; defaults are `gb_oliver_neutral` (ATIS), `en_paul_confident`
  (Tower), `en_paul_neutral` (Ground). Multilingual TTS — only mode
  that speaks German without a US accent in cloud. API key in
  Keychain under `com.xp_wellys_atc.mistral`, last 4 chars logged for
  audit. Note: Voxtral TTS returns a JSON envelope
  `{"audio_data":"<base64 WAV>"}` regardless of `response_format`.

---

## Settings (data/settings.json)

```jsonc
{
  "ptt_key_vk": 49,
  "ptt_joystick_button": -1,
  "pilot_callsign": "November One Two Three Alpha Bravo",
  "active_com": 1,
  "volume": 1.0,

  // ATC training profile + legacy mirror (kept in sync; flow_region drops in a later release)
  "atc_profile": "EU",                      // "EU" | "US" | "DE"
  "flow_region": "EU",
  "start_mode": "engines_running",          // "engines_running" | "cold_and_dark"

  "pattern_direction": "left",
  "disable_default_atc": false,
  "skip_radio_power_check": false,
  "show_phraseology_hints": true,
  "auto_correction_factor": 1.0,

  "debug_logging": false,
  "debug_traffic": false,
  "debug_text_input": false,                // Status-tab InputText -> atc_session::submit_text

  "traffic_features_enabled": true,         // master switch — advisor / sequencing / go-around
  "bzf_strict_mode": false,                 // DE-only: NfL §25 b) Nr. 1 readback check

  "backend_mode": "local",                  // "local" | "openai" | "mistral"

  // OpenAI key in Keychain @ com.xp_wellys_atc.openai / default
  "api_key_saved": false,
  "openai_stt_model": "whisper-1",
  "openai_lm_model": "gpt-4o-mini",
  "openai_tts_model": "tts-1",
  "openai_tts_voice_atis": "onyx",
  "openai_tts_voice_tower": "echo",
  "openai_tts_voice_ground": "alloy",

  // Mistral key in Keychain @ com.xp_wellys_atc.mistral / default
  "mistral_api_key_saved": false,
  "mistral_stt_model": "voxtral-mini-2507",
  "mistral_lm_model": "mistral-small-latest",
  "mistral_tts_model": "voxtral-mini-tts-2603",
  "mistral_tts_voice_atis": "gb_oliver_neutral",
  "mistral_tts_voice_tower": "en_paul_confident",
  "mistral_tts_voice_ground": "en_paul_neutral"
}
```

`settings.json` **is committed** in this repo with sensible defaults and
contains no secrets in any historical revision. **No API key is ever
persisted here** — both the OpenAI and Mistral keys live in the macOS
Keychain under separate services (`com.xp_wellys_atc.openai` and
`com.xp_wellys_atc.mistral`, account `default`) and are managed via the
Settings tab's `[Paste]` / `Save Key` / `Delete Key` buttons. Push-to-Talk
is bound via the X-Plane command `xp_wellys_atc/ptt` (keyboard or
joystick).

The model and voice dropdowns in the Settings tab are populated from
`data/models_catalog.json` — edit that file (not `settings.json`) to
add a new OpenAI / Mistral slug or a new Piper voice. Restart X-Plane
after editing; the catalog is read once at startup.

The x86_64 slice automatically rewrites `backend_mode: "local"` to
`"openai"` on startup (the cloud-only slice has no local backends to
load); `mistral` is honored on both slices. See
`backends/loader::run_worker()`.

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
- When touching backend code, go exclusively through
  `i_speech_to_text` / `i_language_model` / `i_text_to_speech`. Never
  reach into a concrete backend from engine code, and never branch on
  `backend_mode` outside `backends/loader.cpp`. New backend-touching
  features must be implemented symmetrically in all three families
  (Local, OpenAI, Mistral). See **Backend Adapter Rule** —
  `tests/test_audit_logging.cpp` will fail CI on a cross-include or a
  stray log tag.
