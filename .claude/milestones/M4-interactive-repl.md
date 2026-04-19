# M4 — Interaktiver REPL

**Parent-Plan:** `/Users/robertw/.claude/plans/was-aktuell-f-r-mich-swirling-plum.md`
**Vorgänger:** M3 (abgeschlossen — Szenarien laden, `make test` läuft grün, Batch-Assertions funktionieren)

## Ziel dieser Session

Den CLI um einen echten interaktiven Modus erweitern, damit du **explorativ** Szenarien durchspielen kannst, ohne jedes Mal eine JSON-Datei zu schreiben. Context laden, dann frei `say`/`set`/`state`/`reset` eintippen. Fehlende Dinge sofort entdecken (z.B. "warum sagt ATC das?"), Context live verändern, noch mal probieren. Das ist der Modus für tägliche Arbeit an der Engine.

## Deliverables

1. Interaktiver Modus: `atc_repl repl [testscripts/...json]` — optional ein Szenario als Startzustand laden
2. Commands:
   - `say <text>` — Transcript durchspielen, ATC-Response drucken
   - `set <field> <value>` — Context-Felder live ändern
   - `state` — aktueller ATC-State, Flight-Phase, Context-Summary
   - `reset` — Engine zurück auf IDLE, Profanity-Counter 0
   - `load <scenario.json>` — anderes Szenario im laufenden Prompt laden
   - `help` — Liste aller Commands
   - `quit` / Ctrl+D — Ende
3. Command-Parser in `tools/atc_repl/repl.{hpp,cpp}`
4. Der hardcoded M2-Fallback aus `main.cpp` wird entfernt — `atc_repl` ohne Args startet im REPL-Modus mit Default-Context

**Explizit NICHT in M4:** Readline-Integration mit History/Autocomplete (nur plain `std::getline`), Audio, Multi-Session-Logs, GPT-Integration, Szenario-Speichern aus dem REPL.

## Scope — welche Dateien anfassen

### Neu anlegen
- `tools/atc_repl/repl.hpp`
- `tools/atc_repl/repl.cpp`

### Refactorieren
- `tools/atc_repl/main.cpp` — Dispatcher: `run` → Batch (aus M3), `repl` oder ohne Args → interaktiv
- `CMakeLists.txt` — `repl.cpp` zu atc_repl-Sources

## Konkreter Entwurf

### `tools/atc_repl/repl.hpp`

```cpp
#ifndef ATC_REPL_REPL_HPP
#define ATC_REPL_REPL_HPP

#include "scenario.hpp"
#include "xplane_context.hpp"

namespace repl {

// Startet die interaktive Schleife. Optional ein bereits geladenes Szenario
// als initialen Context. Returns wenn User quit oder Ctrl+D drückt.
int run(xplane_context::XPlaneContext ctx, std::string pilot_callsign);

} // namespace repl
#endif
```

### `tools/atc_repl/repl.cpp` (Struktur)

```cpp
int repl::run(XPlaneContext ctx, std::string callsign) {
  print_banner();
  print_state(ctx);
  std::string line;
  while (true) {
    std::fprintf(stderr, "> ");
    if (!std::getline(std::cin, line)) return 0; // Ctrl+D
    line = trim(line);
    if (line.empty()) continue;

    auto [cmd, rest] = split_first_word(line);
    if      (cmd == "say")    cmd_say(ctx, callsign, rest);
    else if (cmd == "set")    cmd_set(ctx, rest);
    else if (cmd == "state")  cmd_state(ctx);
    else if (cmd == "reset")  cmd_reset();
    else if (cmd == "load")   cmd_load(ctx, callsign, rest);
    else if (cmd == "help")   cmd_help();
    else if (cmd == "quit")   return 0;
    else std::fprintf(stderr, "Unknown command: %s (try 'help')\n", cmd.c_str());
  }
}
```

### Commands im Detail

**`say <text>`**
Identisch zu einem Szenario-Step ohne `expect`. Baut `engine::Input`, ruft `process_transcript`, druckt PILOT/ATC/INTENT. Der State bleibt über mehrere `say`-Calls erhalten — das ist der Punkt. Beispiel-Session:
```
> say Zurich Ground November One Two Three Alpha Bravo requesting taxi
PILOT: Zurich Ground ... requesting taxi
ATC:   November One Two Three Alpha Bravo, Zurich Ground, taxi to holding point runway two eight, QNH ...
INTENT: REQUEST_TAXI (confidence=0.91)
STATE: TAXI_CLEARED
> say Taxi to holding point two eight November One Two Three Alpha Bravo
PILOT: Taxi to holding point ...
ATC:   November One Two Three Alpha Bravo, readback correct.
INTENT: READBACK (confidence=0.78)
STATE: TAXI_CLEARED
```

**`set <field> <value>`**
Ändert ein Feld im Context. Unterstützte Felder (dieselben wie im Szenario-Schema aus M3 + phase):
```
set airport LSZR
set towered true
set on_ground false
set com 118.100
set freq_type TOWER
set runway 10
set altitude_ft 2500
set agl_ft 1400
set groundspeed_kt 85
set heading 100
set callsign Hotel Bravo Charlie Delta Echo
```
Bei unbekanntem Feld oder falschem Typ: Fehlermeldung, Context unverändert.

**`state`**
Druckt kompakt:
```
Airport: LSZH (Zurich), towered
Runway:  28   COM: 121.800 (GROUND)
On ground: yes   Engines: on   Alt: 1400ft   AGL: 0ft
Flight phase: GROUND_READY
ATC state:    TAXI_CLEARED
Callsign:     November One Two Three Alpha Bravo
Profanity warnings: 0
```
Flight-Phase und ATC-State liest der REPL via neue kleine Abfragefunktionen:
- `flight_phase::current(const XPlaneContext&)` — existiert vermutlich schon
- `atc_state_machine::current_state()` — existiert vermutlich als `state()` oder `current()`; falls nicht, kleine Query-Funktion hinzufügen

**`reset`**
```cpp
engine::reset();           // Profanity-Counter, GPT-API-Zähler
atc_state_machine::reset(); // ATC-State → IDLE (falls in M3 angelegt)
```
Context bleibt unverändert. Nur der Engine-State wird zurückgesetzt.

**`load <path>`**
Ersetzt Context+Callsign mit geladenem Szenario. Gibt die `say`-Steps des Szenarios **nicht** automatisch aus — nur der Context wird übernommen. Der User tippt danach selbst `say`-Kommandos.
```
> load testscripts/lszr_pattern.json
Loaded: LSZR Tower — pattern entry, downwind, base, final, landing
Context: LSZR towered, TOWER 120.100, runway 10, airborne at 2500ft
> say Altenrhein Tower November One Two Three Alpha Bravo inbound for landing
...
```

**`help`**
```
Commands:
  say <text>            Process a pilot transcript through the engine
  set <field> <value>   Modify context (airport, com, runway, phase, ...)
  state                 Show current context, flight phase, ATC state
  reset                 Reset engine state (keeps context)
  load <file>           Load scenario as new context
  help                  This message
  quit                  Exit (or Ctrl+D)
```

### `main.cpp` Dispatcher final

```cpp
int main(int argc, char **argv) {
  atc_templates::init(data_dir());
  flight_phase::init(data_dir());

  if (argc >= 2 && std::string(argv[1]) == "run") {
    // Batch-Modus aus M3
    return run_batch(argc - 2, argv + 2);
  }

  xplane_context::XPlaneContext ctx;
  std::string callsign = "November One Two Three Alpha Bravo";

  if (argc >= 3 && std::string(argv[1]) == "repl") {
    auto scn = scenario::load(argv[2]);
    ctx = std::move(scn.ctx);
    callsign = scn.pilot_callsign;
    std::fprintf(stderr, "Loaded: %s\n", scn.name.c_str());
  } else {
    // Default-Context: LSZH Ground
    ctx.nearest_airport_id = "LSZH";
    ctx.is_towered_airport = true;
    ctx.on_ground = true;
    ctx.engines_running = true;
    ctx.com1_freq_mhz = 121.800f;
    ctx.active_com = 1;
    ctx.frequency_type = xplane_context::FrequencyType::GROUND;
    ctx.active_runway = "28";
  }

  return repl::run(std::move(ctx), std::move(callsign));
}
```

## Reihenfolge in der Session

1. **Engine-Query-APIs prüfen/ergänzen:** Gibt es `atc_state_machine::current_state()` und `flight_phase::current(ctx)` als reine Getter? Falls nicht, anlegen — minimale Hinzufügung, keine Verhaltensänderung.
2. **`repl.hpp/cpp`** schreiben mit allen Command-Handlern. Commands separat testen durch Code-Inspektion.
3. **`main.cpp`** umbauen: M2-hardcoded-Loop entfernen, Dispatcher final machen.
4. **Manueller Durchlauf:** `./build/atc_repl` (kein Arg) → REPL startet mit Default-Context. `say`, `set com 118.1`, `state`, `load testscripts/lszr_pattern.json`, weiter `say`, `reset`, `quit`. Alles muss sinnvoll reagieren.
5. **Fehler-Pfade:** Unbekanntes Command, kaputter `set`-Value, nicht existierendes Szenario — alle mit klarer Fehlermeldung, kein Crash.
6. **`help`-Output** ausformulieren.
7. **Kurze Doku:** Ergänzung in `docs/testing.md` (aus M3) — REPL-Session-Beispiel zeigen.
8. **`make test`** weiter grün (REPL bricht Batch-Modus nicht).
9. **Plugin-Regression:** Noch mal im Simulator prüfen dass sich nichts geändert hat.

## Verifikations-Checkliste

- [ ] `./build/atc_repl` ohne Args startet REPL mit LSZH-Default-Context
- [ ] `./build/atc_repl repl testscripts/lszr_pattern.json` startet REPL mit geladenem LSZR-Context
- [ ] `say <text>` produziert PILOT/ATC/INTENT/STATE-Ausgabe
- [ ] `set com 118.100` ändert Frequenz, nachfolgendes `say` nutzt neuen Wert
- [ ] `set towered false` + `set freq_type CTAF` → nächstes `say` → Uncontrolled-Flow greift
- [ ] `state` zeigt korrekten Airport/Freq/Phase/ATC-State
- [ ] `reset` setzt ATC-State auf IDLE zurück (per `state` verifizieren)
- [ ] `load testscripts/lszh_ground_taxi.json` übernimmt Context, Name wird bestätigt
- [ ] Unbekanntes Command → "Unknown command, try 'help'"
- [ ] Ctrl+D beendet sauber mit Exit 0
- [ ] `make test` weiter grün
- [ ] Plugin-Binary unverändert funktional

## Commit-Strategie

Ein einziger Commit am Ende: "REPL-Modus für atc_repl mit say/set/state/reset/load". Kleine Ergänzungen (Query-APIs in Engine-Modulen) entweder als vorgezogener Commit oder gebündelt — je nach Umfang.

## Risiken / Stolperfallen

- **Thread-Problem mit async-Callback:** `engine::process_transcript` ist async wegen GPT. In REPL mit `gpt_fallback_enabled=false` läuft der Callback sync, d.h. der REPL funktioniert. Falls jemand GPT aktiviert, würde der Callback ggf. auf einem anderen Thread feuern — dann bräuchten wir eine Callback-Drain-Schleife. Für M4 reicht: GPT bleibt deaktiviert, Annahme dokumentieren.
- **`set phase <X>`:** Flight-Phase ist aus Context **abgeleitet** (on_ground, agl_ft, groundspeed_kt, ...), nicht direkt setzbar. Wenn man `set phase PATTERN` anbieten will, muss das entweder die Ableitungsfelder setzen (on_ground=false, agl_ft=1400, ...) oder eine Phase-Override-Funktion in `flight_phase` einführen. Vorschlag: `set phase` **nicht** anbieten — der User setzt die zugrunde liegenden Felder selbst. Doku macht das klar.
- **`set airport LSZR` lädt keine Runway-Daten:** Im Plugin wird beim Airport-Wechsel die Runway-Cache aus apt.dat neu aufgebaut. Im CLI gibt es kein apt.dat. `set airport` ändert nur die String-Felder; `runways`-Vector bleibt leer. Für ATC-Logik typischerweise irrelevant, weil `active_runway` als String reicht. Wenn die Engine irgendwo `runways`-Details braucht, hier auffallen.
- **State-Konsistenz nach `load`:** Nach Szenario-`load` sollte auch `engine::reset()` laufen — sonst hängt der State vom vorigen Session-Chunk nach. Im `cmd_load` einbauen.
- **`std::getline` auf stdin in einem Pipe:** Wenn jemand `./build/atc_repl < commands.txt` nutzt, soll das funktionieren. Keine terminal-Control-Codes rendern, keine Readline nötig — plain `getline` ist perfekt. Bei EOF sauber beenden (bereits so gedacht).
