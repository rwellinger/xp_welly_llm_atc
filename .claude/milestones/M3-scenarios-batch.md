# M3 — Szenario-Loader + Batch-Runner

**Parent-Plan:** `/Users/robertw/.claude/plans/was-aktuell-f-r-mich-swirling-plum.md`
**Vorgänger:** M2 (abgeschlossen — `atc_repl` existiert als Executable, Engine linkt ohne X-Plane SDK, hardcoded Context läuft)

## Ziel dieser Session

Den CLI vom "hardcoded Beispiel" zu einem echten Regression-Tool machen. JSON-Szenarien laden, Steps durchspielen, optional `expect`-Assertions prüfen. Am Ende der Session kannst du mit `make test` in wenigen Sekunden eine ganze Batterie von ATC-Szenarien fahren und siehst sofort, ob eine Änderung in der Engine ein bekanntes Verhalten bricht.

## Deliverables

1. JSON-Loader in `tools/atc_repl/scenario.{hpp,cpp}` — parst `testscripts/*.json` in einen `XPlaneContext` + Liste von `say`-Steps
2. Batch-Modus: `atc_repl run <file...>` mit `expect`-Substring-Assertion, Exit-Code ≠ 0 bei Mismatch
3. 5 Start-Szenarien unter `testscripts/`:
   - `lszh_ground_taxi.json` — towered Ground-Contact → Taxi-Clearance → Readback
   - `lszr_pattern.json` — towered Tower-Contact → Pattern-Entry → Downwind → Base → Final → Landing-Cleared
   - `lszh_cross_country.json` — Ready for departure VFR on course → Departure-Clearance → Leaving-Frequency
   - `uncontrolled_ctaf.json` — non-towered Self-Announce auf CTAF
   - `radio_check.json` — Radio-Check auf beliebiger Frequenz
4. Makefile-Target `make test`

**Explizit NICHT in M3:** Interaktiver REPL-Modus (kommt in M4), mehr als Substring-`expect` (keine Regex, keine State-Assertions), Audio-Pfad (bleibt Zukunft).

## Scope — welche Dateien anfassen

### Neu anlegen
- `tools/atc_repl/scenario.hpp`
- `tools/atc_repl/scenario.cpp`
- `testscripts/lszh_ground_taxi.json`
- `testscripts/lszr_pattern.json`
- `testscripts/lszh_cross_country.json`
- `testscripts/uncontrolled_ctaf.json`
- `testscripts/radio_check.json`

### Refactorieren
- `tools/atc_repl/main.cpp` — Command-Dispatcher: `run <files>` vs. (noch) interaktiv (hardcoded aus M2 bleibt als Fallback wenn ohne Args aufgerufen, wird in M4 durch echten REPL ersetzt)
- `CMakeLists.txt` — `scenario.cpp` zur `atc_repl`-Sources hinzufügen
- `Makefile` — `test`-Target

## Konkreter Entwurf

### Szenario-JSON-Schema

```json
{
  "name": "LSZH Ground — taxi clearance",
  "context": {
    "airport": "LSZH",
    "airport_name": "Zurich",
    "towered": true,
    "on_ground": true,
    "engines_on": true,
    "com": 121.800,
    "freq_type": "GROUND",
    "runway": "28",
    "callsign": "November One Two Three Alpha Bravo"
  },
  "say": [
    "Zurich Ground November One Two Three Alpha Bravo at general aviation requesting taxi",
    { "text": "Taxi to holding point runway two eight November One Two Three Alpha Bravo", "expect": "taxi" }
  ]
}
```

Einzelner Step ist entweder String (keine Assertion, nur Ausführung) oder Objekt `{text, expect?}`. `expect` ist ein case-insensitiver Substring-Match gegen `response_text`. Fehlt `expect`, wird nichts geprüft — nur ausgeführt und gedruckt.

Alle `context`-Felder sind optional. Defaults:

| Feld | Default |
|---|---|
| `airport` | `""` |
| `airport_name` | `""` |
| `towered` | `true` |
| `on_ground` | `true` |
| `engines_on` | `true` |
| `com` | `121.800` |
| `freq_type` | `"GROUND"` |
| `runway` | `"28"` |
| `callsign` | `"November One Two Three Alpha Bravo"` |
| `altitude_ft` | `1400` |
| `heading` | `0` |
| `groundspeed_kt` | `0` |
| `agl_ft` | `0` |

### `tools/atc_repl/scenario.hpp`

```cpp
#ifndef ATC_REPL_SCENARIO_HPP
#define ATC_REPL_SCENARIO_HPP

#include "xplane_context.hpp"

#include <optional>
#include <string>
#include <vector>

namespace scenario {

struct Step {
  std::string text;
  std::optional<std::string> expect; // leer = nur ausführen, keine Assertion
};

struct Scenario {
  std::string name;                           // aus JSON "name" oder Dateiname
  xplane_context::XPlaneContext ctx;
  std::string pilot_callsign;
  std::vector<Step> steps;
};

// Wirft std::runtime_error bei Parse-Fehler (fehlendes Pflichtfeld, falscher
// Typ, unbekannter freq_type-String).
Scenario load(const std::string &path);

// Führt alle Steps aus. Returns: Anzahl fehlgeschlagener Assertions.
// Druckt auf stdout: "PILOT: ...", "ATC: ...", "EXPECT: <ok|MISMATCH>".
int run(const Scenario &scn);

} // namespace scenario
#endif
```

### `tools/atc_repl/scenario.cpp` (Kern)

- `load()`: öffnet File, parst mit `nlohmann::json`, mappt Felder in `XPlaneContext`. Für `freq_type`: String → Enum (`GROUND` → `FrequencyType::GROUND`). Bei unbekanntem String: throw.
- `run()`: iteriert Steps, baut `engine::Input`, ruft `engine::process_transcript` synchron (GPT disabled → Callback läuft sync). Vergleicht `response_text` case-insensitiv gegen `expect`, zählt Mismatches. Am Ende: `N steps, M mismatches` auf stderr.

**Wichtig — Response-Text-Normalisierung:** Für `expect`-Match beide Seiten auf Lowercase. Keine Regex, kein Whitespace-Stripping — bewusst einfach. Wenn das in der Praxis brennt, in M4 erweitern.

### Main-Dispatcher

```cpp
int main(int argc, char **argv) {
  atc_templates::init(/* data dir */);
  flight_phase::init(/* data dir */);

  if (argc >= 3 && std::string(argv[1]) == "run") {
    int total_fails = 0;
    for (int i = 2; i < argc; ++i) {
      try {
        auto scn = scenario::load(argv[i]);
        std::printf("=== %s (%s) ===\n", scn.name.c_str(), argv[i]);
        int fails = scenario::run(scn);
        total_fails += fails;
        std::printf("-- %d step(s), %d mismatch(es)\n\n",
                    (int)scn.steps.size(), fails);
      } catch (const std::exception &e) {
        std::fprintf(stderr, "ERROR loading %s: %s\n", argv[i], e.what());
        ++total_fails;
      }
    }
    return total_fails == 0 ? 0 : 1;
  }

  // Kein 'run' → M2-Hardcoded-Interaktiv bleibt (wird in M4 ersetzt)
  return interactive_fallback();
}
```

### Start-Szenarien

Die fünf JSON-Files decken die wichtigsten Flows ab:

**`testscripts/lszh_ground_taxi.json`**
```json
{
  "name": "LSZH Ground — initial contact + taxi readback",
  "context": {
    "airport": "LSZH", "airport_name": "Zurich", "towered": true,
    "on_ground": true, "engines_on": true,
    "com": 121.800, "freq_type": "GROUND", "runway": "28",
    "callsign": "November One Two Three Alpha Bravo"
  },
  "say": [
    "Zurich Ground November One Two Three Alpha Bravo at general aviation requesting taxi",
    { "text": "Taxi to holding point runway two eight November One Two Three Alpha Bravo", "expect": "taxi" }
  ]
}
```

**`testscripts/lszr_pattern.json`** — St. Gallen-Altenrhein, towered Tower + Pattern
```json
{
  "name": "LSZR Tower — pattern entry, downwind, base, final, landing",
  "context": {
    "airport": "LSZR", "airport_name": "St Gallen-Altenrhein", "towered": true,
    "on_ground": false, "engines_on": true,
    "com": 120.100, "freq_type": "TOWER", "runway": "10",
    "altitude_ft": 2500, "agl_ft": 1400, "groundspeed_kt": 85,
    "callsign": "November One Two Three Alpha Bravo"
  },
  "say": [
    "Altenrhein Tower November One Two Three Alpha Bravo inbound for landing",
    { "text": "November One Two Three Alpha Bravo downwind runway one zero", "expect": "runway" },
    "November One Two Three Alpha Bravo base runway one zero",
    { "text": "November One Two Three Alpha Bravo final runway one zero", "expect": "cleared to land" }
  ]
}
```

**`testscripts/lszh_cross_country.json`** — Cross-Country-Departure + Leaving-Frequency
```json
{
  "name": "LSZH Tower — VFR cross-country departure",
  "context": {
    "airport": "LSZH", "towered": true,
    "on_ground": true, "engines_on": true,
    "com": 118.100, "freq_type": "TOWER", "runway": "28",
    "callsign": "November One Two Three Alpha Bravo"
  },
  "say": [
    "Zurich Tower November One Two Three Alpha Bravo ready for departure VFR northbound on course",
    { "text": "November One Two Three Alpha Bravo leaving frequency good day", "expect": "good day" }
  ]
}
```
**Achtung:** Dieses Szenario triggert den GPT-Disambiguation-Pfad (READY_FOR_DEPARTURE_VFR). Da der CLI GPT deaktiviert, fällt es auf den rule-based Parse zurück. Wenn das Keyword "on course" / "northbound" im Rule-Parser als VFR erkannt wird, geht's ohne GPT durch. Prüfen beim Schreiben.

**`testscripts/uncontrolled_ctaf.json`** — Non-Towered
```json
{
  "name": "Uncontrolled CTAF — self-announce",
  "context": {
    "airport": "LSZO", "airport_name": "Samedan", "towered": false,
    "on_ground": true, "engines_on": true,
    "com": 123.500, "freq_type": "CTAF", "runway": "21",
    "callsign": "November One Two Three Alpha Bravo"
  },
  "say": [
    { "text": "Samedan traffic November One Two Three Alpha Bravo taxiing to runway two one", "expect": "" }
  ]
}
```
Bei non-towered: Plugin gibt typischerweise ein leeres oder sehr kurzes Acknowledgement. `expect: ""` matcht alles (auch leer). Hier geht's nur darum zu prüfen dass kein Crash passiert und der State korrekt zu `UNICOM_ACTIVE` wandert.

**`testscripts/radio_check.json`**
```json
{
  "name": "Radio check",
  "context": {
    "airport": "LSZH", "towered": true,
    "com": 121.800, "freq_type": "GROUND",
    "callsign": "November One Two Three Alpha Bravo"
  },
  "say": [
    { "text": "Zurich Ground November One Two Three Alpha Bravo radio check", "expect": "read" }
  ]
}
```
(„loud and clear" / „read you" — Substring `read` matcht beides.)

### `Makefile`

```make
.PHONY: test
test: repl
	@echo "Running scenario tests..."
	./build/atc_repl run testscripts/*.json
```

## Reihenfolge in der Session

1. **Schema final machen:** Entscheiden ob `FrequencyType`-Mapping case-insensitive sein soll, ob Enum-Strings dokumentiert werden. Liste der zulässigen Werte als `constexpr` Map.
2. **`scenario.hpp/cpp`** schreiben — nur Parser + Runner, noch keine Szenarios.
3. **Unit-Smoke:** Ein einzelnes Inline-JSON im Code parsen und einen Step ausführen — sicherstellen dass XPlaneContext korrekt befüllt und engine::process_transcript die erwartete Response liefert.
4. **`main.cpp`** erweitern um `run`-Dispatcher.
5. **Erstes echtes Szenario:** `lszh_ground_taxi.json` schreiben, `./build/atc_repl run testscripts/lszh_ground_taxi.json` — iterieren bis die Assertion grün ist.
6. **Restliche 4 Szenarios:** Eins nach dem anderen schreiben und grün kriegen. Bei Überraschungen (Rule-Parser matcht nicht wie erwartet, State-Transition anders): Entscheiden ob Szenario oder Engine-Verhalten falsch ist. Oft ist das Szenario-Schreiben selbst schon der Bug-Finder.
7. **`make test`-Target** hinzufügen, `make test` läuft alle 5 grün durch.
8. **Doku:** `README.md` oder `docs/testing.md` bekommt einen Kurzhinweis: wie schreibt man ein Szenario.
9. **Regression-Check:** `make build && make install` — Plugin-Binary weiter unverändert.

## Verifikations-Checkliste

- [ ] `make test` exit-code 0, alle 5 Szenarios grün
- [ ] `make test` läuft in < 3 Sekunden
- [ ] Absichtlicher Break: `expect` eines Szenarios auf Unsinn ändern → `make test` exit-code 1, zeigt welches Step-Mismatch
- [ ] Korrupte JSON-Datei → `make test` exit-code 1 mit Fehlertext (nicht Crash)
- [ ] `./build/atc_repl run testscripts/lszh_ground_taxi.json` zeigt für jeden Step PILOT/ATC/INTENT
- [ ] `grep -c XPLM build/atc_repl` = 0
- [ ] Plugin-Regression: ein Ground→Taxi-Cycle im Simulator, Verhalten identisch

## Commit-Strategie

- Commit 1: `scenario`-Modul + Dispatcher (ohne Szenario-Files)
- Commit 2: 5 Start-Szenarien + `make test`

## Risiken / Stolperfallen

- **`FrequencyType`-Enum-Mapping:** Ein Tippfehler (`"TOWR"` statt `"TOWER"`) darf nicht stumm zu `UNKNOWN` fallen — `load()` muss explizit throwen bei unbekanntem String.
- **Data-Dir-Auflösung im CLI:** Szenarien setzen keinen Data-Dir, sondern `atc_templates::init()` muss vor `scenario::run()` aufgerufen worden sein. Falls `main.cpp` das noch nicht macht (aus M2 übrig), hier sicherstellen — sonst liefert die State-Machine leere Responses weil Templates nicht geladen sind.
- **State-Persistenz zwischen Szenarios:** Der State-Machine-Zustand ist typischerweise global (static in atc_state_machine.cpp). Zwischen Szenarios `atc_state_machine::reset()` o.ä. aufrufen, sonst färbt Szenario 1 in Szenario 2. Prüfen ob so eine Reset-Funktion existiert — falls nicht, eine anlegen (kleine Engine-API-Ergänzung, legitim in M3).
- **`engine::reset()` zwischen Szenarios:** Profanity-Counter ist engine-internal — zwischen Szenarios via `engine::reset()` zurücksetzen.
- **GPT-Disambiguation im Cross-Country-Test:** Fällt auf Rule-Parser zurück. Falls der Rule-Parser bei "VFR northbound on course" als `READY_FOR_DEPARTURE` (Pattern) statt `READY_FOR_DEPARTURE_VFR` klassifiziert, bricht der Test. Entweder (a) Szenario so wählen dass Rule-Parser eindeutig ist, (b) GPT im Test aktivieren mit Env-Var, (c) neue Rule einbauen. Wahrscheinlich (a) — Transcript so formulieren dass Rule-Parser eindeutig VFR sagt.
- **Plattform-Pfade:** `testscripts/*.json` wird bei `make test` vom Repo-Root aufgelöst. Wenn du `./build/atc_repl` direkt aufrufst, muss der Pfad stimmen. Absolute vs. relative Pfade konsistent halten.
