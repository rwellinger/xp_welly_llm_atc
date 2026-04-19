# M5 — Bad-Case / Error-Handling Test-Abdeckung

**Parent-Plan:** `/Users/robertw/.claude/plans/was-aktuell-f-r-mich-swirling-plum.md`
**Vorgänger:** M4 (abgeschlossen — REPL + 8 Happy-Flow-Szenarien, `make test` grün mit JUnit-Summary, Airport-Change-Auto-Reset in die Engine verschoben)

## Ziel dieser Session

Die Test-Suite um **Bad-Case-Szenarien** ergänzen. Bisher deckt `testscripts/flow_*.json` nur den Happy-Path ab: der Pilot macht alles richtig, ATC antwortet korrekt. In der Realität macht der Pilot aber Fehler — und genau da muss das Plugin sinnvoll reagieren **und der Flow muss trotz Fehler weiterlaufen**. Ohne diese Tests können neue Engine-Änderungen still die Fehlerbehandlung brechen.

**Warum jetzt:** Die Engine hat bereits eine Menge Bad-Case-Logik (Profanity-Warnings, `_INVALID`-Templates, Frequenz-Umleitungen, Flight-Phase-Preconditions, NEGATIVE_CORRECTION, "say position"-Remark). Diese Logik ist aktuell **ungetestet** — eine Intent-Parser- oder Template-Änderung könnte sie stillschweigend ausschalten.

## Deliverables

1. Benennungs-Konvention etabliert:
   - `testscripts/flow_NN_<name>.json` = Happy-Flow (wie bisher)
   - `testscripts/bad_NN_<name>.json` = Bad-Case / Fehler-Szenario
   - Runner zeigt beide Kategorien nebeneinander in der JUnit-Summary (optional: getrennte Zähler "Happy: N/M  Bad: N/M")
2. Mindestens **8 Bad-Case-Szenarien**, jedes mit zwei Teilen:
   - **Teil A**: der Fehler (falsche Freq, falsche Phase, etc.) → Assertion auf die erwartete Controller-Reaktion
   - **Teil B**: Pilot korrigiert → Assertion dass der Flow wieder sauber weiterläuft
3. Kurze Dokumentation der Bad-Case-Taxonomie in `docs/testing.md` (oder Anhang in einem bestehenden doc) — wichtig damit bei neuen Intents klar ist welche Fehler-Fälle mitgedacht werden müssen
4. `make test` bleibt grün — alle Bad-Cases sollen PASS ergeben weil die Engine sie bereits richtig behandelt. Falls nicht → echter Bug, genauso behandeln wie in M4 beim Airport-Change-Reset (Fix in die Engine, nicht in den Test).

**Explizit NICHT in M5:** Neue Bad-Case-Logik in der Engine. GPT-Fallback-Tests (GPT bleibt auch hier deaktiviert wie in M3/M4). Audio-Qualitäts-Tests via echtes Whisper (nur via `quality`-Feld simuliert).

## Scope — welche Dateien anfassen

### Neu anlegen
- `testscripts/bad_01_*.json` bis `bad_08_*.json` (mindestens 8)
- Evtl. `docs/testing.md` oder ein Kapitel in `docs/xpwellysatc_manual_en.md`

### Eventuell anpassen
- `tools/atc_repl/scenario.cpp/hpp` — falls `quality`-Feld auf Step-Ebene nötig ist (für "Pilot redet Quatsch / Whisper-Low-Confidence"-Szenario)
- `tools/atc_repl/main.cpp` — optional: Happy/Bad-Counter in Summary trennen
- `Makefile` — falls Subdir-Struktur gewünscht (NICHT empfohlen, flat mit Prefix ist einfacher)

### Nicht anfassen
- Engine (`src/`), Templates, Flight-Rules — wenn ein Bad-Case fehlschlägt, fixe die Engine in einem separaten Commit vor M5.

## Bad-Case-Taxonomie — was getestet werden soll

Kategorien, sortiert nach Bug-Risiko beim Refactoring:

### 1. Falsche Frequenz für Intent
Die Engine hat Sonder-Routing für `REQUEST_TAXI` auf TOWER und `REQUEST_FLIGHT_FOLLOWING` auf Non-APPROACH. Ein Template-Refactor könnte das brechen.
- **`bad_01_taxi_on_tower_eu.json`** — LSZG, freq_type=TOWER, REQUEST_TAXI → erwartet "contact ground for taxi". Danach: Pilot switcht auf GROUND, REQUEST_TAXI funktioniert wieder.
- **`bad_02_ready_for_departure_on_ground_us.json`** — KPAO, freq_type=GROUND, bereits am Holding Point, READY_FOR_DEPARTURE → erwartet "monitor Tower on ..." (korrekt, ist schon eine Art "Fehler-Handling": Pilot soll auf Tower switchen). Teil B: switch + neu rufen.
- **`bad_03_flight_following_on_tower_us.json`** — KPAO freq=TOWER (nicht APPROACH), REQUEST_FLIGHT_FOLLOWING → erwartet Rejection/Redirect. Teil B: switch auf Approach und Anfrage erneut.

### 2. Falsche Flight-Phase für Intent
`flight_rules.json` hat Preconditions pro Intent. Airborne + REQUEST_TAXI darf nicht clearen.
- **`bad_04_inbound_while_parked.json`** — LSZR, on_ground=true, engines_on=true, GS=0, INITIAL_CALL_INBOUND → erwartet phase-rejection (aus `flight_rules.json`). Teil B: simuliere Takeoff + fly-out, dann airborne + INITIAL_CALL_INBOUND → OK.
- **`bad_05_runway_vacated_while_airborne.json`** — LSZG, airborne, nach "cleared to land", Pilot sagt vorschnell "clear of runway" → phase-rejection. Teil B: simuliere Touchdown + on_ground=true, gleiche Aussage → OK.

### 3. Radio-Disziplin
Profanity-Counter eskaliert: 1. = Hinweis, 2. = letzte Warnung, 3. = Meldung an Behörde. State bleibt unverändert.
- **`bad_06_profanity_escalation.json`** — 3 aufeinanderfolgende Profanity-Aussagen, jede mit `expect` auf passende Warnstufe. Teil B: normaler Intent danach → Flow läuft weiter, State ist noch korrekt von vor den Warnungen.

### 4. Unverständliche Sprache (Low Whisper Quality)
`engine::Input.quality < 0.3f` triggert "say again". Braucht `quality`-Feld im Step.
- **`bad_07_low_quality_transcript.json`** — Step mit `quality: 0.1` + irgendeinem Text → erwartet "say again". Teil B: normale quality=1.0 + korrekter Intent → OK.
- **Vorbedingung**: Scenario-Schema um optionales `quality`-Feld pro Step erweitern (gehört in den Scope von M5 falls nötig).

### 5. Selbst-Korrektur
Intent-Parser kennt "correction" — alles davor wird verworfen.
- **`bad_08_self_correction.json`** — "Grenchen Ground ... request taxi correction request takeoff runway zero six ready for departure" → erwartet READY_FOR_DEPARTURE-Behandlung, nicht REQUEST_TAXI. Teil B: darauffolgender korrekter Flow.

### Optional (wenn Zeit reicht)
- **`bad_09_out_of_sequence.json`** — "cleared to land" Readback ohne vorheriges "report final" → `_INVALID`-Fallback der entsprechenden State. Teil B: korrekte Reihenfolge.
- **`bad_10_unable_clearance.json`** — Tower clearance erhalten, Pilot sagt "unable" → UNABLE-Intent. Teil B: Pilot rerequests + akzeptiert.
- **`bad_11_missing_position_on_first_contact.json`** — REQUEST_TAXI ohne "at parking"/"on the apron" → Controller fügt "say position" remark hinzu (bereits implementiert via `position_remark`-Variable). Teil B: Pilot gibt Position an + re-request.

## Format — wie ein Bad-Case-Szenario aussieht

```json
{
  "_comment": "Bad Case 01 — REQUEST_TAXI on TOWER frequency (EU). Engine must redirect ('contact ground for taxi') without state change, then accept the same request on GROUND afterwards.",
  "name": "Bad case — taxi on Tower freq (EU LSZG)",
  "region": "EU",
  "kind": "bad_case",
  "context": { ... LSZG on TOWER freq ... },
  "say": [
    { "note": "Part A — wrong frequency, controller redirects",
      "text": "Grenchen Tower HB-LUK request taxi",
      "expect": "contact ground" },
    { "note": "Part B — pilot switches frequency, flow continues",
      "set": {"com": 121.800, "freq_type": "GROUND"} },
    { "text": "Grenchen Ground HB-LUK request taxi",
      "expect": "holding point" }
  ]
}
```

**Optional `kind`-Feld** im JSON (`"happy"` default, `"bad_case"` explizit). Runner kann damit Summary gruppieren. Wenn die Namens-Konvention `bad_NN_*` schon reicht, kann das Feld entfallen — keine harte Anforderung.

## Reihenfolge in der Session

1. **Engine-Verhalten manuell im REPL probieren** — für jede geplante Kategorie (Taxonomie oben) prüfen, *was* die Engine aktuell sagt. Screenshot/copy in Kommentare. Das definiert die erwarteten `expect`-Strings.
2. **Szenario-Schema ggf. um `quality` erweitern** — nur wenn `bad_07_low_quality_transcript.json` wirklich gewünscht ist. Sonst überspringen.
3. **Bad-Case-Szenarien nacheinander schreiben und testen** — jedes einzeln via `./build/atc_repl run testscripts/bad_NN_*.json` bis grün. Dann nächstes.
4. **Falls eine Engine-Reaktion unerwartet / inkorrekt** — STOP und dokumentieren. Entweder in separatem Commit fixen (Engine) oder Milestone dokumentieren als bekannter Bug für später. **Niemals den Test anpassen nur um grün zu werden.**
5. **`make test` voll durchziehen** — alle Happy + alle Bad müssen PASS sein.
6. **JUnit-Summary erweitern (optional)** — getrennte Zähler "Happy Flows: 8/8, Bad Cases: 8/8".
7. **Doku**: Kurzes Kapitel in `docs/testing.md` (neu) oder `docs/xpwellysatc_manual_en.md` Appendix — "Error Handling Matrix: was die Engine bei welchem Fehler tut".

## Verifikations-Checkliste

- [ ] Mindestens 8 `bad_NN_*.json` Szenarien existieren
- [ ] Jedes Szenario hat Teil A (Fehler + Controller-Reaktion) und Teil B (Recovery)
- [ ] `make test` grün (Happy + Bad)
- [ ] JUnit-Summary zeigt alle Bad-Cases als PASS
- [ ] Keine `expect`-Assertion wurde künstlich aufgeweicht um grün zu werden
- [ ] Keine Engine-Umgehung im Test-Harness (kein `reset:true`, keine magischen Kontext-Flags)
- [ ] Dokumentation der Bad-Case-Kategorien existiert
- [ ] Regel-Memory aktualisiert falls neue Patterns entdeckt wurden

## Commit-Strategie

Ein Commit pro Bad-Case-Kategorie ist sauber, aber für M5 reicht auch ein gebündelter Commit pro M5-Session. Die Meldung sollte klarmachen dass dies Tests sind, keine Verhaltensänderung: `Bad-Case-Test-Szenarien: wrong freq, wrong phase, profanity, low quality, self-correction`.

Falls während M5 ein echter Engine-Bug auffällt → separater Commit VOR den Test-Commits, damit die Git-Historie den Fix sauber zeigt: "Fix: REQUEST_TAXI on TOWER redirect-message was empty after template refactor" + dann "Tests: bad_01_taxi_on_tower_eu".

## Risiken / Stolperfallen

- **Die Versuchung den Test weicher zu machen**: sehr real. Wenn der Test fehlschlägt und die Engine-Reaktion "nicht ganz falsch, aber auch nicht das was im Test steht" ist — STOP. Frage: Was sagt der Manual? Was würde ein Fluglehrer sagen? Dann entweder Engine fixen oder den Manual-Eintrag präzisieren. **Niemals** die `expect`-Needle so lange kürzen bis sie matcht.
- **Low-quality-Test braucht Schema-Erweiterung**: `engine::Input.quality` ist aktuell hardcoded 1.0 in `scenario::run`. Falls `quality` pro Step setzbar sein soll, Scenario-Schema + Runner anpassen. 10 Zeilen Code, aber neu.
- **Profanity-Counter ist global** pro Engine-Lauf. Die M3/M4-Runner rufen `engine::reset()` vor jedem Szenario — wichtig dass das weiterhin funktioniert sonst leaked der Counter zwischen Szenarien.
- **Flight-Phase-Hysterese**: Für "inbound while parked" muss on_ground=true + GS=0 stabil sein (30-Tick-Prime läuft bereits). Für "runway vacated while airborne" muss on_ground=false stabil sein. Ggf. höhere AGL/GS-Werte damit Phase klar airborne/PATTERN ist.
- **`_INVALID`-Templates sind oft stumm oder generisch** ("say again"). Assertions auf generische Strings (`"say"`) sind schwach — besser auf State prüfen: der State darf sich nicht ändern wenn das Bad-Case rejected wurde. Braucht eventuell eine Erweiterung des Szenario-Schemas um `expect_state: "IDLE"` oder ähnlich.

## Offene Design-Fragen (vor Implementierung klären)

1. **Tagging-Strategie**: reicht Dateiname-Prefix `bad_NN_*`, oder auch ein JSON-Feld `kind`? Meine Empfehlung: Prefix reicht, JSON-Feld optional als Zukunfts-Option.
2. **`expect_state`-Feld**: für Bad-Cases oft wichtiger als `expect_text`, weil die korrekte Reaktion ein "nichts passiert" ist (kein State-Change). Soll ich das Schema erweitern?
3. **Low-quality-Szenario**: dafür kommt `quality` pro Step ins Schema — soll das in M5 rein oder separat?

Diese drei Fragen am Anfang der Implementierungs-Session beantworten, dann erst Tests schreiben.
