# xp_wellys_atc

AI-powered ATC voice communication plugin for X-Plane 12 VFR flights.

Talk to ATC using your microphone via push-to-talk. The plugin transcribes your speech (OpenAI Whisper), interprets your intent through a rule-based ATC state machine, and plays back realistic ATC responses (OpenAI TTS).

## Features

- **Push-to-Talk** — keyboard or joystick button
- **Speech-to-Text** — OpenAI Whisper transcription
- **ATC State Machine** — VFR phraseology for towered and non-towered airports
- **GPT Fallback** — GPT-4o-mini handles ambiguous or unrecognized intents
- **Text-to-Speech** — natural ATC voice responses via OpenAI TTS
- **ImGui UI** — in-sim transcript panel, status bar, and settings

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
| `ptt_key_vk` | `49` (Space) | Virtual key code for push-to-talk |
| `ptt_joystick_button` | `-1` (off) | Joystick button index for PTT |
| `tts_voice` | `onyx` | OpenAI TTS voice |
| `pilot_callsign` | `November One Two Three Alpha Bravo` | Your callsign |
| `volume` | `1.0` | Playback volume (0.0–1.0) |
| `gpt_fallback_enabled` | `true` | Use GPT when intent confidence is low |

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
| **Pattern direction configurable** — left/right downwind selectable in settings (default: left) | — | — |
| **Readback after taxi clearance gets a response** — "readback correct, contact tower when ready" | Real ATC: silence after correct readback. Useful as pilot guidance in the sim | Intentional design choice |
| **No traffic** — always "number one", no sequencing | Unrealistic at busy airports | Very high — would require traffic awareness |
| **No callsign validation** — ATC accepts any callsign without checking against configured one | In real ATC, unknown callsigns get "station calling, say again" | Low — but low priority for single-player |
| **No callsign abbreviation** — ATC always uses full callsign, never shortens to last 3 letters after initial contact | Real ATC abbreviates after first contact (e.g. "Lima Uniform Kilo" instead of "Hotel Bravo Lima Uniform Kilo") | Low-Medium — needs `{callsign_short}` template variable + update templates |

## License

Private project — not licensed for redistribution.
