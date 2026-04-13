# xp_wellys_atc

[![Build](https://github.com/rwellinger/xp_welly_atc/actions/workflows/build.yml/badge.svg)](https://github.com/rwellinger/xp_welly_atc/actions/workflows/build.yml)

AI-powered ATC voice communication plugin for X-Plane 12 VFR flights.

Talk to ATC using your microphone via push-to-talk. The plugin transcribes your speech (OpenAI Whisper), interprets your intent through a rule-based ATC state machine, and plays back realistic ATC responses (OpenAI TTS).

## Features

- **Push-to-Talk** — via X-Plane command binding (keyboard or joystick)
- **Speech-to-Text** — OpenAI Whisper transcription
- **ATC State Machine** — VFR phraseology for towered and non-towered airports
- **Flight Phase Detection** — context-aware guards prevent unrealistic ATC interactions based on aircraft state (parked, taxi, airborne, etc.)
- **ATIS Generation** — automatic ATIS broadcasts from live sim weather data
- **GPT Fallback** — GPT-4o-mini handles ambiguous or unrecognized intents
- **Text-to-Speech** — natural ATC voice responses via OpenAI TTS (separate voices for Tower, Ground, and ATIS)
- **ImGui UI** — in-sim transcript panel, status bar, and settings

## Getting Started — Video Tutorial

[![Watch the tutorial](https://img.youtube.com/vi/jh2lbBgA8Fw/maxresdefault.jpg)](https://youtu.be/jh2lbBgA8Fw)

This walkthrough covers initial plugin configuration inside X-Plane — setting your callsign, entering your OpenAI API key, and binding push-to-talk — followed by a first live ATC interaction: tuning the ATIS frequency at Grenchen (LSZG) and receiving the automated weather broadcast.

## Preparing for a Traffic Pattern — Video Tutorial

[![Watch the tutorial](https://img.youtube.com/vi/N333BPBY4Rs/maxresdefault.jpg)](https://youtu.be/N333BPBY4Rs)

Preparing for a left-hand traffic pattern at Grenchen (LSZG): tuning the ATIS frequency for current weather and runway information, then contacting Ground to request taxi to the active runway.

## Platform

**macOS only.** Windows and Linux are not supported. The plugin relies on macOS-specific frameworks (Core Audio, AudioToolbox, Security/Keychain, AVFoundation).

## Requirements

- macOS 12.0+ (ARM64 / x86_64 universal binary)
- X-Plane 12
- CMake 3.21+
- Homebrew LLVM (`brew install llvm`)
- OpenAI API key

## Quick Start

```bash
# 1. Clone and setup dependencies
git clone <repo-url>
cd xp_wellys_atc
make setup      # Downloads X-Plane SDK, Dear ImGui, nlohmann/json

# 2. Build
make build      # CMake Release build → build/xp_wellys_atc.xpl

# 3. Install into X-Plane
make install    # Code-sign + deploy to X-Plane plugins directory
```

## OpenAI API Key

This plugin requires an OpenAI API key for speech recognition (Whisper), intent classification (GPT-4o-mini), and voice synthesis (TTS). You can create an API key at [platform.openai.com](https://platform.openai.com).

**Cost estimate:** A single traffic pattern circuit (taxi, takeoff, pattern, landing) uses roughly 8–10 API calls and costs approximately **$0.02–0.05 USD**. A touch-and-go session with multiple circuits costs proportionally more. These are rough estimates — actual costs depend on transmission length and how often the GPT fallback is triggered.

**Security:** The API key is stored exclusively in the macOS Keychain. It is never written to disk, never logged, and never visible within the plugin. It is only used for HTTPS requests to the OpenAI API.

## Configuration

On first launch, open the plugin menu in X-Plane and enter your OpenAI API key in the settings tab.

Settings are stored in `data/settings.json`:

| Setting | Default | Description |
|---|---|---|
| `tts_voice_tower` | `onyx` | OpenAI TTS voice for Tower |
| `tts_voice_ground` | `echo` | OpenAI TTS voice for Ground |
| `tts_voice_atis` | `nova` | OpenAI TTS voice for ATIS |
| `tts_model` | `tts-1` | OpenAI TTS model |
| `whisper_model` | `whisper-1` | OpenAI Whisper model |
| `gpt_model` | `gpt-4o-mini` | OpenAI GPT model for fallback |
| `pilot_callsign` | *(empty)* | Your callsign (set in plugin settings) |
| `active_com` | `1` | Active COM radio (1 or 2) |
| `volume` | `1.0` | Playback volume (0.0–1.0) |
| `gpt_fallback_enabled` | `true` | Use GPT when intent confidence is low |
| `pattern_direction` | `left` | Default traffic pattern direction (left/right) — overridden per airport/runway by `airport_vrps.json` |
| `debug_logging` | `false` | Enable verbose debug output |

ATC response templates are defined in `data/atc_templates.json` (towered and uncontrolled airports). Flight phase detection thresholds, ATC precondition guards, and auto-correction rules are configured in `data/flight_rules.json`. All data files can be edited without rebuilding the plugin.

### Airport Database (`data/airport_vrps.json`)

Per-airport configuration for Visual Reporting Points (VRPs) and traffic pattern directions. The plugin ships with pre-configured data for common Swiss and German VFR airports.

**VRPs** are used to recognize VRP names in pilot transmissions and issue realistic arrival instructions (e.g., *"cleared to enter control zone via November"*).

**Pattern direction** can be configured globally in `settings.json` (`pattern_direction`) or per airport/runway in `airport_vrps.json`. The airport-specific value takes precedence over the global setting. This supports airports where the pattern direction differs by runway (e.g., Grenchen LSZG: right traffic for runway 07, left traffic for runway 25).

```json
{
  "LSZG": {
    "name": "Grenchen",
    "pattern_direction": {
      "07": "right",
      "25": "left"
    },
    "vrps": [
      { "name": "Tango", "lat": 47.175, "lon": 7.410, "alt_ft": 3000 }
    ],
    "arrival_routes": {
      "07": ["Golf", "Tango"],
      "25": ["Mike", "Tango"]
    }
  }
}
```

`pattern_direction` can be a simple string (`"left"`) to apply to all runways, or an object with per-runway entries. Lookup order: exact runway match → base runway (strip L/R/C suffix) → airport default → global `settings.json` fallback.

### ATC Response Templates (`data/atc_templates.json`)

Defines the ATC response text for every combination of airport type, ATC state, and pilot intent. The file is split into two top-level sections:

- **`towered`** — full ATC flow (Ground → Tower → Pattern → Landing)
- **`uncontrolled`** — CTAF/UNICOM self-announce (no clearances, acknowledgement only)

Each entry contains:

| Field | Description |
|---|---|
| `response` | Template text with `{variable}` placeholders (e.g., `{callsign}`, `{runway}`, `{wind}`) |
| `next_state` | ATC state machine transition after this response |
| `requires_readback` | Whether the pilot must read back the clearance |

The special key `_INVALID` is the fallback response when no matching intent exists for the current state (e.g., *"say again your request"*). Variables are substituted at runtime from `XPlaneContext` data. The file can be edited without rebuilding the plugin.

### Flight Rules (`data/flight_rules.json`)

Controls flight phase detection, ATC state guards, and automatic state corrections. Contains five sections:

| Section | Purpose |
|---|---|
| `phase_thresholds` | Sensor values for `FlightPhase` transitions (e.g., taxi above 5 kt GS, pattern below 3000 ft AGL) |
| `hysteresis` | Time delays (seconds) to prevent phase jitter during transitions |
| `intent_preconditions` | Guards that block invalid intents based on current flight phase — e.g., a taxi request while airborne returns a rejection message |
| `auto_corrections` | Fixes state/phase mismatches after a configurable delay — e.g., pilot is airborne but state is still `DEPARTURE_CLEARED` → auto-transition to `PATTERN_ENTRY` after 5 seconds |
| `intent_frequency` | Restricts which intents are valid on which frequency type (`GROUND`, `TOWER`, `UNICOM`) |
| `pilot_phraseology` | Example phrases per intent, used for UI display |

### GPT Prompt Templates (`data/atc_prompt_templates.json`)

Contains the prompts sent to OpenAI APIs:

| Key | Purpose |
|---|---|
| `whisper_prompt` | Static context hint sent to the Whisper API to bias transcription toward aviation vocabulary and NATO phonetic alphabet |
| `gpt_classify_prompt` | System prompt for GPT-4o-mini intent classification, used when the local keyword parser returns low confidence (< 0.7) or `UNKNOWN`. Variables: `{state}`, `{valid_intents}`, `{transcript}`, `{frequency_type}`, `{on_ground}`, `{altitude_ft}`, `{groundspeed_kts}`, `{airport}` |
| `gpt_fallback_prompt` | Emergency fallback system prompt that generates a full ATC response via GPT when both the local parser and intent classification fail. Variables: `{airport}`, `{callsign}`, `{on_ground}` |

All prompt templates can be customized without rebuilding the plugin.

**Push-to-Talk** is configured via X-Plane's keyboard or joystick settings. The plugin registers the command `xp_wellys_atc/ptt` which can be bound to any key or joystick button in X-Plane.

## Usage

1. Tune COM1/COM2 to the appropriate frequency in X-Plane
2. Hold the PTT key and speak your radio call (e.g., *"Zurich Tower, November 123AB, request taxi"*)
3. Release PTT — the plugin transcribes, processes, and plays back the ATC response
4. Check the ImGui overlay for transcript history and current ATC state

## Other Make Targets

```bash
make format     # Run clang-format on all source files
make lint       # Run clang-tidy
make clean      # Remove build artifacts
```

## Known Limitations

| Limitation | Impact | Effort |
|---|---|---|
| **"via Alpha" hardcoded** — taxiway name is always "Alpha" regardless of airport | Unrealistic at airports with different taxiway layouts | High — would need taxiway data from apt.dat or WED |
| **Readback after taxi clearance gets a response** — "readback correct, contact tower when ready" | Real ATC: silence after correct readback. Useful as pilot guidance in the sim | Intentional design choice |
| **No traffic** — always "number one", no sequencing | Unrealistic at busy airports | Very high — would require traffic awareness |
| **No callsign validation** — ATC accepts any callsign without checking against configured one | In real ATC, unknown callsigns get "station calling, say again" | Low — but low priority for single-player |

## License

This project is licensed under the [GNU General Public License v3.0](https://www.gnu.org/licenses/gpl-3.0.html).
