### AI-powered ATC voice communication plugin for X-Plane 12 VFR flights

macOS only. Built for macOS users who lack access to commercial ATC voice solutions on their platform.


### What's New in v1.5.0

  - **Voice-only workflow** — pilot action buttons removed entirely. All ATC communication is now exclusively via push-to-talk voice input. The former button panel is replaced by read-only **Phraseology Hints** — a context-aware cheat sheet showing the correct radio call for your current situation. Hover any hint for the full ICAO phraseology with phonetic callsign.
  - **"Disregard" recovery button** — when stuck in an ATC loop (e.g. repeated "say again"), a Disregard button appears next to the Phraseology Hints header. One click resets the ATC conversation to IDLE so you can start fresh without restarting X-Plane.
  - **Frequency buttons set standby** — clicking a frequency in the ATC panel now writes it to the standby COM slot instead of the active frequency. Flip-flop your COM radio to activate. The active frequency remains highlighted in green.
  - **Radio power awareness** — the ATC panel disables frequency buttons and PTT when the COM radio has no electrical power (avionics bus dead or radio switch off). A warning is shown with a hint to check the `skip_radio_power_check` setting for exotic aircraft.
  - **Smarter phraseology hints** — hints now follow EU/ICAO VFR flow: at airports with a separate Ground frequency, "Tune to Ground frequency first" is shown when on Tower in IDLE. Departure intents are hidden until taxi clearance is received. Post-landing, "Ready for Departure" is correctly suppressed until the session resets.
  - **Intent parser fix** — "Tower, inbound for landing" is now correctly classified as `INITIAL_CALL_INBOUND` instead of `REQUEST_LANDING`, fixing cross-country arrivals that previously got stuck in a "say again" loop.
  - **Flight phase fix** — eliminated false `TAKEOFF_ROLL` detection after landing when groundspeed briefly fluctuates above 40 kt during runway exit.
  - **Landing timeout extended** — `LANDING_CLEARED` auto-correction increased from 30s to 90s, giving pilots enough time to decelerate, vacate the runway, and report "runway vacated" before the state resets.
  - **Configurable ATC recovery time** — new `auto_correction_factor` setting (0.5x–2.0x) controls how quickly the plugin corrects state/phase mismatches. Lower values suit experienced pilots; higher values give beginners more time.
  - **Cleaner logs** — audio playback, recording, and MP3 decode messages moved behind `debug_logging` flag, significantly reducing log noise during normal flights.
  - **Documentation** — new Section 7 (ATC Panel UI) in both English and German user manuals covering frequency buttons, phraseology hints, Disregard button, and nearby airports panel.


### Features

  - Push-to-Talk with speech recognition (OpenAI Whisper)
  - Rule-based ATC state machine for towered and non-towered airports
  - GPT-4o-mini fallback for ambiguous intents
  - Natural ATC voice responses (OpenAI TTS) with separate voices for Tower, Ground, and ATIS
  - Full VFR traffic pattern: taxi, takeoff, pattern, landing
  - Cross-country flights with active ATC handoffs: Tower → Departure/FIS → Approach → Tower, driven by X-Plane airspace polygons
  - "On course" departure detection, VRP-based inbound at destination
  - Touch-and-go and go-around procedures
  - ATIS generation from live sim weather
  - Automatic runway selection based on wind
  - Frequency-aware state management (Ground / Tower / Approach / FIS / ATIS / UNICOM)
  - Per-airport/runway pattern direction and Visual Reporting Points (VRPs)
  - Context-aware Phraseology Hints with ICAO phraseology tooltips
  - "Disregard" button for recovering from stuck ATC conversations
  - Radio power detection with configurable bypass
  - Configurable ATC recovery timing for beginners and experienced pilots
  - Optional "Disable Default X-Plane ATC" to avoid double audio
  - ImGui in-sim UI with frequency management, phraseology hints, transcript panel, and settings


### Installation

  Download xp_wellys_atc.zip, extract into your X-Plane Resources/plugins/ directory. See the README for setup instructions and configuration.


### Requirements

  - macOS 12.0+ (ARM64 / x86_64 universal binary)
  - X-Plane 12
  - Own OpenAI API key


### Known Limitations

  - Single pilot — no traffic sequencing, always "number one"
  - English communication only
  - EU / ICAO phraseology only — ATC flow and wording follow European / ICAO standards (Ground hands off to Tower at the holding point, pilot reports "ready for departure" to Ground). US-style procedures (e.g., Ground issuing "contact tower when ready" already in the taxi clearance) are not modelled.
  - Hardcoded taxiway — taxi instructions always use "via Alpha" regardless of airport layout
  - Multi-leg handoffs require X-Plane 12 atc.dat (ships by default); gracefully disabled if missing
  - macOS only — no Windows or Linux support


### Roadmap

  - Improved ATIS with real-world NOTAMs and airport-specific information
  - Additional airports with VRPs and pattern directions
  - VFR Reporting Points beyond the currently shipped Swiss / German set
  - IFR Support — IFR introduces significantly more complexity (clearances, holds, approach procedures, etc.) and will be tackled in a later phase. Stay tuned!

  Full Changelog: https://github.com/rwellinger/xp_welly_atc/compare/v1.4.0...v1.5.0
