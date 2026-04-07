# Project Status — xp_wellys_atc

Letzte Aktualisierung: 2026-04-05 (M8)

## Milestone-Ubersicht

| # | Milestone | Status |
|---|-----------|--------|
| M1 | Settings, ImGui UI, Keychain API Key | Done |
| M2 | Core Audio Recording, PTT, WAV Encoding | Done |
| M3 | Whisper STT — async transcription, transcript display | Done |
| M4 | Intent Parser — rule-based transcript → PilotIntent | Done |
| M5 | ATC State Machine + GPT Fallback + Full Text Pipeline | Done |
| M6 | TTS + Audio Playback — OpenAI TTS + Core Audio MP3 | Done |
| M7 | Joystick PTT + Settings Persistence | Done |
| M8 | Polish — frequency awareness, CTAF, error handling, debug logging, UI | Done |

## M5 — Was wurde gemacht

- `atc_state_machine.hpp/.cpp`: ATCState Enum (8 States), ATCResponse Struct, `process()` mit allen VFR Transitions und Phraseologie-Templates
  - IDLE → GROUND_CONTACT → TAXI_CLEARED → TOWER_CONTACT → DEPARTURE_CLEARED → IDLE
  - TOWER_CONTACT → PATTERN_ENTRY → LANDING_CLEARED → IDLE
  - UNICOM_ACTIVE fuer nicht-kontrollierte Flugplaetze
- `gpt_client.hpp/.cpp`: Async POST an `/v1/chat/completions` (gpt-4o-mini), gleicher callback-queue Pattern wie whisper_client, ATC System-Prompt mit ICAO Phraseologie
- `atc_session.cpp`: Nach Intent-Parse → State Machine → bei leerem Response/low confidence/UNKNOWN → GPT Fallback (oder "Say again" wenn disabled) → ATC Response ins Transcript
- `atc_ui.cpp`: ATCState-Anzeige + Reset-Button im Status-Tab, Airport-Prefix bei ATC-Lines im Transcript
- `main.cpp`: init/stop fuer atc_state_machine + gpt_client, drain gpt callback queue im Flight Loop
- `xplane_context`: QNH (barometer_sealevel_inhg), Wind Direction/Speed DataRefs hinzugefuegt

## Bugfixes und Erweiterungen (zwischen M1-M5)

### ImGui UI komplett umgebaut
- **Problem:** ImGui Fenster war leer, dann ausserhalb des Dialogs, nicht klickbar
- **Loesung:** Kompletter Umbau nach xp_pilot Pattern:
  - Fullscreen unsichtbares XPLM-Fenster (`DecorationNone`) fuer Mouse/Keyboard Capture
  - Separater `XPLMRegisterDrawCallback` (`xplm_Phase_Window`) fuer ImGui Rendering
  - Eigene GL State Verwaltung (Viewport, Projection Matrix, Save/Restore)
  - Mouse-Forwarding (X-Plane Screen-Coords → ImGui Coords)
  - Keyboard-Forwarding (Printable chars + Backspace/Delete/Enter/Escape)
  - `XPLMTakeKeyboardFocus` fuer Input-Capture
  - `XPLMGetMouseLocationGlobal` im Draw-Callback fuer Hover-Support
  - ImGui Fenster zentriert, draggable, resizable

### HFS-Pfad-Fix (macOS)
- **Problem:** `XPLMGetSystemPath` und `XPLMGetPluginInfo` geben auf macOS HFS-Pfade zurueck (Doppelpunkte statt Slashes: `Macintosh HD:Users:...`)
- **Betroffen:** settings.json konnte nicht geschrieben/gelesen werden, apt.dat nicht geoeffnet
- **Loesung:** HFS → POSIX Konvertierung in `xplane_context.cpp` und `settings.cpp`

### Towered Airport Detection via apt.dat
- **Problem:** ILS-basierte Heuristik war unzuverlaessig (ILS-IDs ≠ Airport-ICAO, LSZB als "Uncontrolled")
- **Loesung:** apt.dat Parse beim Plugin-Start (Background-Thread), sucht TWR-Frequenzen (Code 54 + 1054 fuer X-Plane 12 Format), cached in `unordered_set` fuer O(1) Lookup
- Default `towered=true` bis Cache geladen (~1-2s)

### PTT Click-Sound
- 880Hz Sinus-Pip, 80ms, Fade-in/out Envelope
- Wird bei PTT-Press abgespielt ueber Core Audio AudioQueue
- Lautstaerke folgt Volume-Setting

### Audio Output Device Selection
- Core Audio Device-Enumeration (alle Output-Devices)
- Dropdown im Settings-Tab mit Refresh-Button
- AudioQueue wird auf gewaehltes Device geroutet (`kAudioQueueProperty_CurrentDevice`)
- Gespeichert in settings.json als Device-UID
- Zweck: ATC-Audio aufs Headset, X-Plane Engine-Sound auf Hauptlautsprecher

### Avionics-Check fuer PTT
- PTT blockiert wenn `sim/cockpit/electrical/avionics_on == 0` (Cold & Dark)
- Log-Meldung: "PTT blocked — avionics off"

### API Key Paste-Button
- "Paste" Button neben API Key Input (Cmd+V geht nicht in ImGui/X-Plane)
- Liest aus macOS Clipboard via `pbpaste`

### PTT via X-Plane Command
- PTT-Zuweisung ueber X-Plane Key/Joystick Binding statt settings.json

### Sonstige Fixes
- `compile_commands.json` Symlink fuer LSP/Editor Support
- clang-tidy clean (alle Warnings behoben)

## M4 — Was wurde gemacht

- `intent_parser.hpp`: PilotIntent-Enum (12 Intents), PilotMessage-Struct mit raw_transcript/intent/confidence/callsign/runway
- `intent_parser.cpp`: Rule-based Keyword-Matching in Prioritaetsreihenfolge:
  - UNABLE (0.95), SELF_ANNOUNCE (0.90), READY_FOR_DEPARTURE (0.90), RUNWAY_VACATED (0.90), REQUEST_TAXI (0.90), REPORT_POSITION (0.90), REQUEST_LANDING (0.85), REQUEST_FREQUENCY (0.80), INITIAL_CALL (0.85), READBACK (0.75)
- Callsign-Extraktion: Phonetic-Alphabet-Sequenzen, N-Number ("november ..."), Vergleich mit settings::pilot_callsign
- Runway-Extraktion: Spoken Numbers ("two six" → "26"), Suffixe (left/right/center → L/R/C), direkte Ziffern
- Context Weighting: REQUEST_TAXI bei !on_ground → 0.3, INITIAL_CALL mit "tower" bei !is_towered → 0.4, SELF_ANNOUNCE bei towered → 0.3
- `atc_session.cpp`: Nach Whisper-Callback wird intent_parser::parse() aufgerufen, PilotMessage gespeichert, Intent+Confidence geloggt
- `atc_ui.cpp`: Status-Tab zeigt "Last Parsed Intent" mit Intent-Name, Confidence, Callsign, Runway, Raw Transcript

## M3 — Was wurde gemacht

- `whisper_client.cpp`: Async POST an OpenAI `/v1/audio/transcriptions` via libcurl multipart, detached thread, thread-safe callback queue (drained im flight loop)
- `atc_session.cpp`: `on_ptt_released()` ruft jetzt Whisper auf, Minimum-Duration-Gate (< 0.5s wird verworfen), State → PROCESSING waehrend API-Call
- `atc_ui.cpp`: Neuer "Transcript"-Tab mit scrollbarer Liste (Pilot = weiss, ATC = cyan vorbereitet), Auto-Scroll, Clear-Button
- `main.cpp`: `whisper_client::init/stop` + `drain_callback_queue()` im Flight Loop

## M6 — Was wurde gemacht

- `tts_client.hpp/.cpp`: Async POST an OpenAI `/v1/audio/speech` (TTS-1), gleicher callback-queue Pattern wie whisper_client/gpt_client, MP3-Binary-Response via `CURLOPT_WRITEFUNCTION`, JSON-Body via nlohmann/json (sichere Escaping)
- `audio_player.hpp/.cpp`: MP3 Decode via AudioToolbox (`AudioFileOpenWithCallbacks` + `ExtAudioFileRead` → float32 PCM), Playback via AudioQueue mit Triple-Buffering, Volume-Scaling im Render-Callback, `is_playing()` Atomic-Flag, `kAudioQueueProperty_IsRunning` Listener fuer Cleanup
- `atc_session.cpp`: Neue `speak_response()` Funktion — nach jeder ATC-Antwort (State Machine, GPT Fallback, "Say again") wird TTS aufgerufen statt direkt IDLE. State-Flow: PROCESSING → PLAYING → IDLE. Neue `update()` Funktion pollt `audio_player::is_playing()` im Flight Loop
- `main.cpp`: `tts_client::init/stop`, `audio_player::init/stop`, `tts_client::drain_callback_queue()` + `atc_session::update()` im Flight Loop
- Status-Labels angepasst: IDLE="Ready", PROCESSING="⟳ Processing...", PLAYING="▶ ATC speaking..."
- Device-Routing: TTS Audio wird auf gewaehltes Output-Device geroutet (gleich wie PTT Click)

## M7 — Was wurde gemacht

- `ptt_input.hpp/.cpp`: Komplett neu — 3 PTT-Quellen parallel:
  - XPLMCommand (`xp_wellys_atc/ptt`) fuer X-Plane Key/Joystick Binding (bestehendes System)
  - XPLMRegisterKeySniffer fuer direkte Keyboard VK Bindung (`settings::ptt_key_vk`)
  - DataRef-Polling `sim/joystick/joy_buttons` (3200 Buttons) fuer direkte Joystick Bindung (`settings::ptt_joystick_button`)
  - OR-Logik: Jede der 3 Quellen kann PTT ausloesen
  - Capture-Mode API: `start_capture(KEYBOARD/JOYSTICK)`, `poll_capture_result()`, `cancel_capture()`
  - 10s Timeout fuer Capture-Mode
  - `update()` wird jeden Flight-Loop-Frame aufgerufen
- `atc_ui.cpp`: PTT Binding Panel im Settings-Tab:
  - Keyboard: Aktuelle Bindung anzeigen, "Bind key" Button → Capture Mode → gelber "Press any key..." Text, "Clear" Button
  - Joystick: Aktuelle Bindung anzeigen, "Bind button" Button → Capture Mode → gelber "Press any button..." Text, "Clear" Button
  - VK-Code zu lesbarem Namen (A-Z, 0-9, F1-F12, Space, Enter, etc.)
  - Hinweis auf X-Plane Command als Alternative
- `settings.hpp/.cpp`: `set_ptt_key_vk()` und `set_ptt_joystick_button()` Setter hinzugefuegt
- `main.cpp`: `ptt_input::update()` im Flight Loop

## M8 — Was wurde gemacht

- **Frequency Awareness:**
  - `FrequencyType` Enum (8 Types: UNKNOWN, DELIVERY, GROUND, TOWER, APPROACH, UNICOM, CTAF, ATIS)
  - Heuristische Ableitung aus aktiver COM-Frequenz in `xplane_context::update()`
  - Frequenz-Typ Anzeige im Status-Tab ("COM1: 119.100 MHz (Tower)")
  - State Machine validiert Frequenz-Kontext (Ground-Freq → nur Ground-States, Tower-Freq → nur Tower-States)

- **CTAF/Unicom Flow:**
  - Non-towered Airports + UNICOM/CTAF-Frequenz → immer Unicom-Flow
  - Traffic-Awareness Response mit Position-Extraktion (downwind/base/final/etc.)
  - Sofort zurueck zu IDLE nach Acknowledgement (kein State-Progression)

- **Error Handling:**
  - API Key Check bei PTT Press (blockiert ohne Key, Warning im Status-Tab)
  - Whisper: 15s Timeout, HTTP-Fehler → `[Error: transcription failed]` im Transcript
  - GPT: 15s Timeout, Fehler → "Say again" Fallback
  - TTS: 15s Timeout, Fehler → Text bleibt im Transcript, kein Audio
  - Alle Fehler geloggt mit `[xp_wellys_atc][ERROR]` Prefix

- **Debug Logging:**
  - Alle Debug-Ausgaben hinter `settings::debug_logging()` Guard
  - Strukturiertes PTT-Cycle Logging: PTT pressed → Recording stopped → WAV encoded → Whisper response → Intent → ATC response → TTS response → Playback started/finished

- **UI Polish:**
  - Version String im Window-Titel: "Welly's ATC v1.0.0"
  - Version String im Plugin-Name (X-Plane Plugin Manager)
  - About-Section im Settings-Tab (Version, Beschreibung, GitHub-Link)
  - Frequenz neben jedem Transcript-Eintrag
  - Session-Stats im Status-Tab (Transcriptions, API Calls)
  - Warning-Anzeige wenn API Key fehlt

- **Version + Release:**
  - `VERSION.txt` (1.0.0)
  - CMakeLists.txt liest VERSION.txt → `XP_WELLYS_ATC_VERSION` Compile-Time Define

## Status

Plugin ist komplett und bereit fuer den persoenlichen Einsatz.
Future Work: GitHub Actions CI, Windows Build, Public Release.
