# M1 — Logging-Abstraktion + Engine-Extraktion

**Parent-Plan:** `/Users/robertw/.claude/plans/was-aktuell-f-r-mich-swirling-plum.md`

## Ziel dieser Session

Die ATC-Kernlogik hinter einen **Single Entry Point** legen, sodass sie später von einem standalone Test-Client aufgerufen werden kann — ohne dass sich Audio- und Text-Flow je wieder auseinanderdriften können. In dieser Session wird nur refactoriert; das Plugin muss danach **identisches Verhalten** zeigen.

## Deliverables

1. Neues Modul `src/logging.{hpp,cpp}` — Sink-basierter Logging-Wrapper
2. Neues Modul `src/engine/engine.{hpp,cpp}` — Single Entry Point `process_transcript`
3. Refactor: `src/atc_session.cpp` Codeblock Zeile ~213–335 ruft nur noch `engine::process_transcript(...)`
4. Refactor: Alle `XPLMDebugString(...)`-Calls in Engine-Modulen → `logging::debug/error(...)`
5. Plugin baut mit `make build` und läuft im Simulator unverändert durch

**Explizit NICHT in M1:** CMake OBJECT library split, CLI, xplane_context split, Szenario-JSON. Das kommt in M2/M3.

## Scope — welche Dateien anfassen

### Neu anlegen
- `src/logging.hpp`
- `src/logging.cpp`
- `src/engine/engine.hpp`
- `src/engine/engine.cpp`

### Refactorieren (nur Logging-Calls)
Engine-Module, wo `XPLMDebugString` durch `logging::*` ersetzt wird (alle verwenden es heute nur fürs Logging, nicht für echte X-Plane-API):
- `src/intent_parser.cpp`
- `src/atc_state_machine.cpp`
- `src/atc_templates.cpp`
- `src/flight_phase.cpp`
- `src/atis_generator.cpp`
- `src/gpt_client.cpp`
- `src/whisper_client.cpp`
- `src/tts_client.cpp`
- `src/airport_vrps.cpp`
- `src/airspace_db.cpp`

**Nicht** anfassen in M1:
- `src/xplane_context.cpp` (bleibt Plugin-Code, Split kommt in M2)
- `src/main.cpp`, `src/atc_ui.cpp`, `src/ptt_input.cpp`, `src/audio_recorder.cpp`, `src/audio_player.cpp`, `src/settings.cpp` — das sind Plugin-spezifische Files, die dürfen X-Plane SDK direkt nutzen

### Refactorieren (engine-Extraktion)
- `src/atc_session.cpp` — Codeblock im Whisper-Callback (ca. Zeile 213–335) wird durch `engine::process_transcript(...)` ersetzt
- `src/main.cpp` — `XPluginStart` setzt den Logging-Sink auf einen XPLMDebugString-Wrapper; `engine::init()` Aufruf in der Init-Sequenz

### CMakeLists.txt
- `src/logging.cpp` und `src/engine/engine.cpp` zur SOURCES-Liste hinzufügen
- **Keine** OBJECT library / zweites Target — das kommt in M2

## Konkreter Entwurf

### `src/logging.hpp`
```cpp
#ifndef LOGGING_HPP
#define LOGGING_HPP

namespace logging {
  using Sink = void (*)(const char *);
  // Default-Sink schreibt nach stderr. Plugin setzt in XPluginStart einen
  // Wrapper auf XPLMDebugString.
  void set_sink(Sink s);
  void debug(const char *fmt, ...);
  void info(const char *fmt, ...);
  void error(const char *fmt, ...);
}
#endif
```

### `src/logging.cpp`
- Default-Sink = fprintf auf stderr mit Newline
- `debug/info/error` formatieren in einen `char buf[1024]`, prependen `[xp_wellys_atc][DEBUG|INFO|ERROR] `, rufen Sink
- `debug()` checkt `settings::debug_logging()` (falls Settings zu dem Zeitpunkt verfügbar sind — sonst einfach immer loggen, die Filterung passiert aktuell auch auf Call-Site)

**Wichtig:** Die bestehenden Call-Sites ("[xp_wellys_atc][DEBUG] ..." Präfixe) werden aktuell per Hand gesetzt. Beim sed-Replace die Prefix-Duplikation vermeiden — entweder:
(a) Präfix weglassen in Call-Sites, Logging-Wrapper fügt ihn zu; oder
(b) Wrapper prependet nichts, Call-Sites behalten ihren Präfix.

**Entscheidung: (a)** — sauberer. Beim Replace die `[xp_wellys_atc][DEBUG] ` und `[xp_wellys_atc][ERROR] ` aus den Format-Strings entfernen.

### `src/engine/engine.hpp`
```cpp
#ifndef ENGINE_HPP
#define ENGINE_HPP

#include "atc_state_machine.hpp"
#include "intent_parser.hpp"
#include "xplane_context.hpp"

#include <functional>
#include <string>

namespace engine {

struct Input {
  std::string transcript;
  float quality = 1.0f; // Whisper quality, 1.0 für Text-Tests
  const xplane_context::XPlaneContext *ctx = nullptr;
  std::string pilot_callsign; // aus settings im Plugin, aus Szenario im CLI
  bool gpt_fallback_enabled = true;
};

struct Output {
  std::string response_text; // leer = silent
  intent_parser::PilotMessage parsed;
  bool is_warning = false; // Language-Warning, ändert State nicht
};

// Async wegen optionaler GPT-Disambiguation. Bei gpt_fallback_enabled=false
// wird das Callback synchron aufgerufen.
void process_transcript(Input in, std::function<void(Output)> done);

} // namespace engine
#endif
```

### `src/engine/engine.cpp`
Enthält 1:1 die Logik aus `atc_session.cpp` Zeile 213–335:
1. Quality-Check (< 0.3f → "say again"-Response)
2. `intent_parser::parse`
3. INAPPROPRIATE_LANGUAGE-Interception (mit statischem Warning-Counter intern in engine.cpp — muss auch im Plugin weiter funktionieren, also Counter bleibt in engine)
4. GPT-Disambiguation für READY_FOR_DEPARTURE-Varianten (via `gpt_client::classify_intent_async`)
5. `atc_state_machine::process`
6. `done(Output)` aufrufen

**Wichtig:** Der `profanity_warnings_` Counter muss rüberwandern — aktuell in atc_session.cpp als static int. Gehört logisch in die Engine.

### `src/atc_session.cpp` Refactor
Der Codeblock im Whisper-Callback wird ersetzt durch:
```cpp
engine::Input in{
    transcript,
    wr.quality,
    &xplane_context::get(),
    settings::pilot_callsign(),
    settings::gpt_fallback_enabled(),
};
engine::process_transcript(std::move(in), [freq_str_copy, voice_copy](engine::Output out) {
  // Transcript-Liste + TTS + PTT-State-Transition
  last_pilot_message_ = out.parsed;
  if (!out.response_text.empty()) {
    transcript_.push_back(TranscriptEntry{
        static_cast<double>(XPLMGetElapsedTime()),
        false, out.response_text, freq_str_copy,
    });
    speak_response(out.response_text, 1.0f, voice_copy);
  } else {
    state_ = PTTState::IDLE;
  }
});
```

Plus: Das Hinzufügen der Pilot-Transcript-Zeile + die `++total_transcriptions_`-Zählung bleiben **vor** dem `engine::process_transcript`-Call — das ist UI-State, nicht Engine-Logik.

## Reihenfolge in der Session

1. **Lesen:** `src/atc_session.cpp` Zeile 150–550 komplett verstehen (Whisper-Callback + GPT-Disambiguation-Callback)
2. **logging.hpp/cpp** schreiben, lokal testen (ein .cpp anpassen, `make build`, `make install`, X-Plane Log.txt prüfen)
3. **sed-Replace** für Engine-Module — ein Modul nach dem anderen, jedes einzeln builden
4. **engine.hpp/cpp** schreiben mit der extrahierten Logik (ohne GPT-Disambiguation erst mal — sync-Path)
5. **GPT-Disambiguation** in engine einbauen (async Callback)
6. **atc_session.cpp** auf `engine::process_transcript` umstellen
7. **main.cpp:** `logging::set_sink(...)` in `XPluginStart`
8. **CMakeLists.txt:** neue Sources hinzufügen
9. **`make format && make lint && make build && make install`** (laut Memory der user-Workflow)
10. **Regression im Simulator:** Ground → Taxi → Takeoff → Pattern → Landing mit bekannten Phrasen

## Verifikations-Checkliste

- [ ] `make build` grün ohne Warnings (`-Wall -Wextra`)
- [ ] `make lint` grün (clang-tidy)
- [ ] `make install` erfolgreich auf MacStudioNAS
- [ ] X-Plane Log.txt zeigt bei Plugin-Start die gleichen Messages wie vorher (nur evtl. minimale Präfix-Unterschiede)
- [ ] Testflug: PTT → "Zurich Ground requesting taxi" → ATC antwortet normal
- [ ] Testflug: Taxi-Clearance-Readback → nächster State
- [ ] Testflug: Pattern-Entry, Downwind-Report, Cleared-to-land → normal
- [ ] Testflug: Profanity-Test (einmal fluchen → Warnung; nicht State-wechselnd)
- [ ] Testflug: Departure-Disambiguation "ready for departure on course northbound" → GPT-Fallback greift, korrekter Intent
- [ ] `grep -r "XPLMDebugString" src/intent_parser.cpp src/atc_state_machine.cpp src/atc_templates.cpp src/flight_phase.cpp src/atis_generator.cpp src/gpt_client.cpp src/whisper_client.cpp src/tts_client.cpp src/airport_vrps.cpp src/airspace_db.cpp` → leer
- [ ] `grep -rn "intent_parser::parse\|atc_state_machine::process" src --include="*.cpp" | grep -v "engine/engine.cpp"` → leer (nur engine.cpp ruft diese Funktionen)

## Commit-Strategie

Laut CLAUDE.md-Konventionen und User-Feedback: nicht auto-committen. Am Ende der Session, wenn die Regression durch ist, gibt der User den Commit-Befehl.

Ein sinnvoller Commit-Cut wäre:
- Commit 1: logging-Abstraktion
- Commit 2: engine::process_transcript extrahiert + atc_session umgestellt

## Risiken / Stolperfallen

- **Async-Callback im GPT-Disambiguation-Pfad:** Aktuell hängen Captured-References in den Lambdas (`msg_copy`, `run_state_machine`). Beim Umzug in engine.cpp darauf achten, dass diese sauber kopiert werden und kein Lifetime-Problem entsteht, wenn die Engine parallele Anfragen bekommt (aktuell nicht, aber Test-Client könnte es).
- **Profanity-Warning-Counter:** Ist aktuell ein statisches Feld in atc_session.cpp. Bei Verschiebung in engine.cpp: muss genauso persistent über mehrere `process_transcript`-Calls hinweg funktionieren. Plus: Reset-Mechanismus (z.B. bei `engine::reset()`).
- **settings-Zugriffe in der Engine:** `settings::debug_logging()`, `settings::pilot_callsign()`, `settings::gpt_fallback_enabled()` werden aktuell direkt gerufen. In der Engine müssen diese via `Input`-Struct reinkommen, damit die Engine später ohne `settings` kompilierbar ist. In M1 reicht es aber, wenn Plugin+Engine weiter dagegen linken — der echte Cut passiert in M2.
