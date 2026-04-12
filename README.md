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
| `pattern_direction` | `left` | Traffic pattern direction (left/right) |
| `debug_logging` | `false` | Enable verbose debug output |

ATC response templates are defined in `data/atc_templates.json` (towered and uncontrolled airports). Flight phase detection thresholds, ATC precondition guards, and auto-correction rules are configured in `data/flight_rules.json`. Visual Reporting Points (VRPs) for inbound routing are defined per airport in `data/airport_vrps.json` — the plugin uses these to recognize VRP names in pilot transmissions and issue realistic arrival instructions (e.g., *"cleared to enter control zone via November"*). All three files can be edited without rebuilding the plugin.

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
