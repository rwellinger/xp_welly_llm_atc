# M2 — CMake OBJECT Library + CLI-Skeleton

**Parent-Plan:** `/Users/robertw/.claude/plans/was-aktuell-f-r-mich-swirling-plum.md`
**Vorgänger:** M1 (abgeschlossen — `engine::process_transcript` existiert, Logging-Sink installiert)

## Ziel dieser Session

Die in M1 geschaffene Engine physisch vom Plugin-Code trennen: als eigene CMake OBJECT library, die **ohne X-Plane SDK** kompilierbar ist. Darauf basierend ein minimales Kommandozeilen-Tool `atc_repl` bauen, das du aus der Shell starten kannst und das Transcripts direkt durch die Engine schickt. Noch kein JSON-Loader — der Context wird hardcoded im CLI-Quelltext gesetzt. Das reicht, um zu beweisen, dass die Engine headless läuft.

## Deliverables

1. `CMakeLists.txt` split: OBJECT library `xp_atc_engine` + Plugin MODULE `xp_wellys_atc` + Executable `atc_repl`
2. `src/xplane_context.cpp` → zerteilt in `src/xplane_context.cpp` (POD-Helper, Engine) + `src/xplane_context_runtime.cpp` (DataRefs, apt.dat, Plugin-only)
3. Neues Executable `tools/atc_repl/main.cpp` mit hardcoded Beispiel-Context
4. Makefile-Target `make repl`
5. `make build` (Plugin) baut unverändert, `make repl` baut `build/atc_repl` standalone

**Explizit NICHT in M2:** JSON-Loader, Szenario-Files, Batch-Assertions, interaktive REPL-Commands. Das kommt in M3/M4. In M2 geht es nur um Build-Infrastruktur und den Beweis, dass die Engine ohne X-Plane linkt.

## Scope — welche Dateien anfassen

### Neu anlegen
- `tools/atc_repl/main.cpp` — minimales CLI
- `src/xplane_context_runtime.cpp` — Plugin-only Teil nach dem Split

### Refactorieren
- `CMakeLists.txt` — OBJECT library + Executable Target
- `src/xplane_context.cpp` — alles mit `XPLM*` rauszieht nach `xplane_context_runtime.cpp`; zurück bleiben nur POD-Helper (`AirportFrequencies::has/lookup/first_mhz/has_ground`, `frequency_type_name`)
- `Makefile` — `repl`-Target

### Nicht anfassen
- `src/engine/*`, `src/logging.*` (in M1 stabil)
- Engine-Module (intent_parser, atc_state_machine, etc.) — sollten heute schon ohne SDK kompilierbar sein
- Plugin-spezifische Files: `main.cpp`, `atc_ui.cpp`, `ptt_input.cpp`, `audio_recorder.cpp`, `audio_player.cpp`, `atc_session.cpp`, `settings.cpp`, `mic_permission.mm`

## Konkreter Entwurf

### `CMakeLists.txt` Struktur

Aktuell ist alles in einem `add_library(xp_wellys_atc MODULE ...)`. Neue Struktur:

```cmake
# ── Engine OBJECT library (X-Plane SDK-frei) ─────────────────────────
add_library(xp_atc_engine OBJECT
    src/logging.cpp
    src/engine/engine.cpp
    src/intent_parser.cpp
    src/atc_state_machine.cpp
    src/atc_templates.cpp
    src/flight_phase.cpp
    src/atis_generator.cpp
    src/gpt_client.cpp
    src/whisper_client.cpp
    src/tts_client.cpp
    src/airport_vrps.cpp
    src/airspace_db.cpp
    src/xplane_context.cpp         # nur POD-Helper nach dem Split
)
target_include_directories(xp_atc_engine PRIVATE
    ${CMAKE_SOURCE_DIR}/vendor
)
target_compile_definitions(xp_atc_engine PRIVATE
    XP_WELLYS_ATC_VERSION="${VERSION_STRING}"
)
target_compile_options(xp_atc_engine PRIVATE
    -Wall -Wextra -Wno-unused-parameter -fvisibility=hidden
)
find_package(CURL REQUIRED)
target_link_libraries(xp_atc_engine PRIVATE CURL::libcurl)

# ── Plugin MODULE (Engine + X-Plane-spezifisch) ──────────────────────
add_library(xp_wellys_atc MODULE
    $<TARGET_OBJECTS:xp_atc_engine>
    src/main.cpp
    src/atc_session.cpp
    src/xplane_context_runtime.cpp   # NEU nach dem Split
    src/ptt_input.cpp
    src/audio_recorder.cpp
    src/audio_player.cpp
    src/settings.cpp
    src/atc_ui.cpp
    src/mic_permission.mm
    ${IMGUI_SOURCES}
)
# bestehende Plugin-target_link_libraries, Frameworks, XPLM, etc.

# ── CLI Executable (ohne X-Plane) ────────────────────────────────────
add_executable(atc_repl
    $<TARGET_OBJECTS:xp_atc_engine>
    tools/atc_repl/main.cpp
)
target_include_directories(atc_repl PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/vendor
)
target_link_libraries(atc_repl PRIVATE CURL::libcurl)
target_compile_options(atc_repl PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)
set_target_properties(atc_repl PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
```

**Hinweis:** Die `-DXPLM200=1` / `APL=1` Defines setzen wir **nur** aufs Plugin-Target, nicht auf `xp_atc_engine`. Damit scheitert der Engine-Build sofort, falls jemand in einem Engine-Modul versehentlich XPLM-Code einzieht — genau die gewünschte Guardrail.

### `xplane_context` Split

**Bleibt in `xplane_context.cpp` (Engine-tauglich):**
- `bool AirportFrequencies::has(FrequencyType) const`
- `float AirportFrequencies::first_mhz(FrequencyType) const`
- `FrequencyType AirportFrequencies::lookup(float) const`
- `bool AirportFrequencies::has_ground() const`
- `const char *frequency_type_name(FrequencyType)`

**Wandert nach `xplane_context_runtime.cpp` (Plugin-only):**
- Alle `XPLMDataRef dr_*` globals
- `void init()`, `void stop()`, `void update()`
- `const XPlaneContext &get()`
- `void set_standby_freq(uint32_t)`
- `void lock_airport(...)`, `unlock_airport()`, `locked_airport()`
- `std::vector<NearbyAirport> nearby_airports(...)` falls vorhanden
- apt.dat-Parser, Runway-Cache, Frequency-DB-Aufbau

**Header `src/xplane_context.hpp`** bleibt unverändert — beide .cpp-Files includen es.

### `tools/atc_repl/main.cpp` (minimal)

```cpp
#include "engine/engine.hpp"
#include "intent_parser.hpp"
#include "logging.hpp"
#include "xplane_context.hpp"

#include <cstdio>
#include <iostream>
#include <string>

int main() {
  // Logging-Sink: stderr (Default reicht; explizit setzen nicht nötig)

  // Hardcoded Beispiel-Context: LSZH, auf dem Boden, GROUND-Frequenz
  xplane_context::XPlaneContext ctx;
  ctx.nearest_airport_id = "LSZH";
  ctx.nearest_airport_name = "Zurich";
  ctx.is_towered_airport = true;
  ctx.on_ground = true;
  ctx.engines_running = true;
  ctx.com1_freq_mhz = 121.800f;
  ctx.active_com = 1;
  ctx.frequency_type = xplane_context::FrequencyType::GROUND;
  ctx.active_runway = "28";

  std::fprintf(stderr,
               "atc_repl — type a transcript, Ctrl+D to quit.\n"
               "Context: %s %s, freq %.3f, runway %s\n> ",
               ctx.nearest_airport_id.c_str(),
               ctx.is_towered_airport ? "towered" : "uncontrolled",
               ctx.com1_freq_mhz, ctx.active_runway.c_str());

  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) {
      std::fprintf(stderr, "> ");
      continue;
    }
    engine::Input in{
        line,
        1.0f,
        &ctx,
        "November One Two Three Alpha Bravo",
        false,  // GPT disabled im CLI — rein lokal, keine OpenAI-Calls
    };
    engine::process_transcript(std::move(in), [](engine::Output out) {
      std::printf("PILOT: %s\n",
                  out.parsed.raw_transcript.c_str());
      std::printf("ATC:   %s\n",
                  out.response_text.empty() ? "(silent)"
                                             : out.response_text.c_str());
      std::printf("INTENT: %s (confidence=%.2f)\n",
                  intent_parser::intent_name(out.parsed.intent),
                  out.parsed.confidence);
    });
    std::fprintf(stderr, "> ");
  }
  return 0;
}
```

**Wichtig:** `gpt_fallback_enabled=false` — der REPL nutzt **keinen** OpenAI-Key. Das ist M2-Entscheidung: CLI läuft offline/kostenlos; der GPT-Disambiguation-Pfad wird erst in M3 (Szenario-Modus mit optionalem Key) oder nie im CLI aktiviert. Wenn du später doch GPT testen willst, Umgebungsvariable `OPENAI_API_KEY` lesen und `gpt_fallback_enabled=true` setzen.

**Settings-Abhängigkeit:** `atc_templates::init()` und `flight_phase::init()` werden aktuell vom Plugin in `XPluginStart` aufgerufen und laden JSON-Files aus dem Plugin-Datenverzeichnis. Für den CLI muss Main diese Init-Aufrufe **selbst** machen mit einem Pfad relativ zum Binary (z.B. `./data/atc_templates.json`). Falls das init() signaturen mit XPLM-Pfad-Auflösung hat, hier möglicherweise eine zweite Overload anbieten, die einen expliziten Pfad nimmt.

→ **Aktionspunkt im ersten Drittel der Session:** Prüfen, wie `atc_templates::init()` und `flight_phase::init()` ihre JSON-Dateien laden. Falls sie auf `XPLMGetSystemPath` o.ä. basieren, dort eine Overload `init(std::string data_dir)` hinzufügen und den CLI den Pfad setzen lassen.

### `Makefile` Ergänzungen

```make
.PHONY: repl
repl: build-dir
	cmake --build build --target atc_repl
	@echo "Built: build/atc_repl"

.PHONY: run-repl
run-repl: repl
	./build/atc_repl
```

## Reihenfolge in der Session

1. **Inventur:** `grep -n "XPLM\|XPlaneContext\s*&\s*get\|set_standby_freq\|lock_airport\|init\|stop\|update" src/xplane_context.cpp` — welche Funktionen nutzen SDK, welche nicht?
2. **`xplane_context_runtime.cpp` anlegen:** alle SDK-Funktionen rüberziehen. `xplane_context.cpp` zurückschrumpfen auf POD-Helper.
3. **Lokaler Sanity-Check:** `make build` muss weiter grün sein (derselbe Plugin-Code, nur in zwei Files gesplittet).
4. **Data-Loader-Pfade prüfen:** Siehe Aktionspunkt oben. Falls nötig, Init-Overloads hinzufügen.
5. **CMake Split:** OBJECT library einführen, Plugin umstellen, Executable anlegen.
6. **`make build`** → Plugin-Binary identisch wie vorher.
7. **`tools/atc_repl/main.cpp`** schreiben, `make repl` → `build/atc_repl` existiert.
8. **Smoketest:** `./build/atc_repl`, "Zurich Ground November One Two Three Alpha Bravo requesting taxi" eintippen, sehen dass eine sinnvolle ATC-Antwort kommt.
9. **`make format && make lint`** über die neuen Files.
10. **Kurze Plugin-Regression im Simulator** — ein Ground→Takeoff→Pattern-Zyklus, damit der CMake-Umbau keine Link-Fehler eingeführt hat.

## Verifikations-Checkliste

- [ ] `make build` grün, Plugin-Binary startet in X-Plane
- [ ] `make repl` grün, `build/atc_repl` existiert und ist ausführbar
- [ ] `./build/atc_repl` startet, nimmt Input, druckt ATC-Response
- [ ] "Zurich Ground ... requesting taxi" → ATC gibt sinnvolle Taxi-Clearance-Antwort
- [ ] "Taxi to holding point two eight" (Readback) → State-Maschine akzeptiert
- [ ] Plugin-Regression im Simulator: Verhalten identisch zu vor M2
- [ ] `nm build/atc_repl 2>/dev/null | grep -c XPLM` → **0** (nicht ein einziges XPLM-Symbol im CLI-Binary)
- [ ] `grep -rn "XPLM" src/engine/ src/intent_parser.cpp src/atc_state_machine.cpp src/atc_templates.cpp src/flight_phase.cpp src/atis_generator.cpp src/gpt_client.cpp src/whisper_client.cpp src/tts_client.cpp src/airport_vrps.cpp src/airspace_db.cpp src/logging.cpp src/xplane_context.cpp` → leer
- [ ] Executable-Binary < 5 MB (Indiz dass nur Engine + curl drin ist)

## Commit-Strategie

Zwei saubere Commits am Ende:
1. `xplane_context` Split (Plugin bleibt funktional, keine Verhaltensänderung)
2. CMake OBJECT library + `atc_repl` Executable + Makefile-Target

User gibt den Commit-Befehl.

## Risiken / Stolperfallen

- **`settings.cpp` Abhängigkeit:** Engine-Module rufen heute `settings::debug_logging()`, `settings::pilot_callsign()`, etc. Falls das nicht sauber über `Input`-Struct läuft, würde der CLI beim Linken `settings`-Symbole vermissen. Zwei Optionen: (a) Stub-Settings im CLI-Main definieren; (b) Engine komplett von settings entkoppeln (bereits Plan laut M1-Struct — `pilot_callsign`/`gpt_fallback_enabled` kommen per Input). Ersten Build-Fehler abwarten und dann entscheiden.
- **`atc_templates::init()` / `flight_phase::init()` Pfad-Auflösung:** Aktuell vermutlich über X-Plane-Plugin-Pfad. CLI muss einen eigenen Weg finden — relativer Pfad zum Binary oder `ATC_DATA_DIR` Env-Var.
- **`audio_player`-Aufrufe in engine?** Falls `engine.cpp` zufällig `speak_response` o.ä. ruft, hätte der CLI-Link ein Problem. Sollte laut M1-Design nicht der Fall sein (TTS-Playback bleibt im Plugin), aber beim ersten Link-Fehler schnell prüfen.
- **Universal Binary vs. CLI:** Das Plugin braucht arm64+x86_64. Der CLI braucht nur die Host-Arch (arm64 auf MacStudio). Überlegen ob wir `atc_repl` aus dem `CMAKE_OSX_ARCHITECTURES`-Setting ausnehmen (sonst langsamer Build ohne Nutzen) — `set_target_properties(atc_repl PROPERTIES OSX_ARCHITECTURES "arm64")`.
- **`mic_permission.mm`:** Ist ObjC; darf nur ins Plugin, nicht in die Engine. Stellt sicher, dass das nicht versehentlich in der OBJECT library landet.
