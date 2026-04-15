# Milestone: Multi-Leg Cross-Country Flights with ATC Handoffs

## Context

Heute unterstützt xp_wellys_atc den kompletten Flow an einem einzelnen Flughafen (Ground → Tower → Pattern → Landing) und kennt einen `EN_ROUTE` State als "stumme" Phase zwischen Abflug-Tower-Release und Ankunfts-Tower-Kontakt. Auf einem längeren VFR-Cross-Country (z.B. LSZB → LSZH) passiert dazwischen in Realität jedoch mandatorische ATC-Kommunikation: Flight Information Service (FIS), Approach-Freigaben für TMA/CTR-Eintritt, und am Zielort ein Handoff von Approach → Tower.

Der Pilot soll:
1. Nach Departure eine FIS-/Info-Frequenz rufen können (en-route advisories).
2. Beim Annäherungsflug an kontrollierten Luftraum (z.B. Zürich TMA) vom abgebenden Service aktiv auf Approach verwiesen werden.
3. Von Approach nach Pattern-Einflug an Tower übergeben werden.
4. Optional: Bei Durchflug einer CTR/TMA eines Drittflughafens kurz Kontakt aufnehmen.

Motivation: Realistischeres VFR-Erlebnis in Airspace-dichten Regionen (DACH, UK, Benelux), wo reine "Tower → EN_ROUTE → Tower"-Logik unrealistisch ist.

---

## Scope (M2 — Multi-Leg Handoffs)

**In Scope:**
- Neue Facility-Typen: `APPROACH` (bereits in enum), `FIS` / `INFORMATION`, `CENTER` (ACC).
- Neue ATC-States: `FIS_CONTACT`, `APPROACH_CONTACT` (Ankunft), optional `DEPARTURE_CONTACT` (Abflug Radar).
- Aktive Handoff-Instruktionen ("Contact Zurich Approach on 118.100").
- Nutzung von X-Plane 12 `atc.dat` Airspace-Polygonen für geometrische Luftraum-Erkennung.
- Frequenz-getriebene State-Transitions: Pilot tunt Frequenz → Plugin erkennt Facility anhand `atc.dat` + Position.
- Readback-Handling für Frequenzwechsel ("Contact Approach 118.1" → Pilot: "118.1, Good Day").

**Out of Scope (späterer Milestone):**
- IFR-Flüge, Clearance-Delivery-Routen, SID/STAR.
- Squawk-Code-Zuteilung und Transponder-Management.
- VFR-Reporting-Points ("Whiskey", "Echo") — heute schon schwach im UNICOM-Flow.
- Cross-Border-Handoffs (LSAS → LFMM).
- Nicht-VFR-Airspace-Klassen (A/B) mit Sonder-Clearances.

---

## Data Sources

1. **`apt.dat`** (bereits geladen, `xplane_context.cpp`): Flughafen-Frequenzen Codes 50–55/1050–1055. Enthält ATIS, UNICOM, Delivery, Ground, Tower, Approach. **Kein FIS/Center.**

2. **`~/X-Plane 12/Custom Data/1200 atc data/Earth nav data/atc.dat`** (NEU, noch nicht geladen): Enthält `CONTROLLER`-Blöcke mit `NAME`, `FACILITY_ID`, `ROLE` (`twr`/`tracon`/`ctr`), `FREQ` (mehrere pro Controller), `CLASS`, `TRANSITION_ALT` und `AIRSPACE_POLYGON_BEGIN ... POINT lat lon ... END` samt Höhen-Floor/Ceiling.

   Beispiel LSAS (Swiss ACC): Role `ctr`, liefert de facto die FIS-Funktion für VFR über Approach-Höhe. LSZH und LSGG haben je einen `tracon`- und einen `twr`-Block mit eigenen Polygonen.

   **Fallback bei Fehlen:** Wenn die Datei nicht existiert (z.B. bei X-Plane 11 Nutzern oder fehlenden Custom Data), wird das Feature grazil deaktiviert — EN_ROUTE bleibt wie heute stumm. Status im UI sichtbar.

3. **Kein Navigraph erforderlich** — FIS/Center-Polygone und Frequenzen sind in `atc.dat` enthalten (durch Tests für LSAS/LSZH/LSGG bestätigt).

### atc.dat Memory Budget & Loading-Strategie

Raw-Datei: **10.3 MB Text**, 7'974 Controller weltweit, 328k Polygon-Punkte.

Vollständig parsed (naive struct + `std::vector<std::pair<double,double>>`) wären das geschätzt **8–10 MB RAM** — akzeptabel, aber unnötig, da der Pilot pro Session typisch in einer Region fliegt.

**Gewählter Ansatz: Two-Pass Load mit Region-Filter + BBox-Index**

1. **Pass 1 (eager, bei Plugin-Start, ~1–2 MB RAM):** Nur `CONTROLLER` Header parsen — `NAME`, `FACILITY_ID`, `ROLE`, `FREQ`-Liste, `CLASS`, `TRANSITION_ALT`, und **Bounding Box** der Polygone (min/max lat/lon + floor/ceiling). Polygon-Punkte werden dabei gescannt um die BBox zu berechnen, aber **nicht gespeichert** — Dateizeiger/Offset zum Polygon-Block wird gemerkt (`uint64_t` file offset).

2. **Pass 2 (lazy, on-demand):** Sobald eine Position (aircraft oder airport lookup) in einer BBox+Höhenband liegt, werden die detaillierten Polygon-Punkte dieses Controllers von disk nachgeladen und in einem LRU-Cache (z.B. 50 Controller) gehalten. Cache-Eviction setzt gebundene RAM-Obergrenze.

**RAM-Profil:**
- Idle (nur Header-Index): **~1.5 MB** (7974 × ~200 Byte avg)
- Aktiv in einer Region (50 Controller Polygone geladen): **+1–2 MB**
- Worst case (Flug quer über Europa, Cache voll): **~3–4 MB total**

**Alternative falls Komplexität zu hoch:** Full eager load mit 10 MB RAM als M2.1 Fallback — einfacher Code, auf heutigen Maschinen vernachlässigbar. Entscheidung fällt während M2.1 basierend auf Parser-Performance. Default im Plan: **Two-Pass**.

---

## Architecture Changes

### New Module: `airspace_db`

`src/airspace_db.hpp` / `.cpp`

- Lädt `atc.dat` beim Plugin-Start einmalig (wie `xplane_context` heute `apt.dat` lädt).
- Parst `CONTROLLER`-Blöcke in Struktur:
  ```cpp
  struct Controller {
      std::string name;              // "ZURICH", "SWITZERLAND"
      std::string facility_id;       // "LSZH", "LSAS"
      ControllerRole role;           // TWR, TRACON, CTR
      std::vector<uint32_t> freqs_khz;
      std::vector<std::vector<std::pair<double,double>>> polygons;
      int floor_ft, ceiling_ft;      // AIRSPACE_POLYGON_BEGIN floor ceiling
      std::string airspace_class;    // A/B/C/D/E/G
      int transition_alt_ft;
  };
  ```
- API:
  - `find_enclosing(lat, lon, alt_ft) → std::vector<const Controller*>` — alle Controller, deren Polygon & Höhenband den Punkt enthalten.
  - `lookup_by_freq(freq_khz, lat, lon) → const Controller*` — Controller mit passender Frequenz, geometrisch relevant (Point-in-Polygon oder Distanz-Fallback).
  - `find_by_role_near(role, lat, lon, alt_ft) → const Controller*` — z.B. nächste CTR, nächste TRACON.
- Point-in-Polygon via Standard Ray-Casting; keine externe Lib.

### Changes to `xplane_context`

- Neues Feld `std::vector<const Controller*> enclosing_airspaces;` — gefüllt jeden Frame aus `airspace_db::find_enclosing()`.
- `FrequencyType` enum erweitern um `FIS`, `CENTER`, `DEPARTURE` (DEPARTURE reserviert, M2 nutzt TRACON-ROLE für sowohl Dep als auch App).
- `frequency_type` Ableitung erweitert: falls COM-Frequenz nicht in apt.dat-Airport-DB gefunden wird, in `airspace_db::lookup_by_freq()` suchen.

### Changes to `atc_state_machine`

- Neue States:
  - `DEPARTURE_CONTACT` — nach `LEAVING_FREQUENCY` vom abfliegenden Tower, wenn TRACON im Abflug-Polygon gefunden wird.
  - `FIS_CONTACT` — en-route Info-Kontakt (CENTER/LSAS-Polygon).
  - `APPROACH_CONTACT` — Ankunft, nach FIS/EN_ROUTE beim Eintritt ins Destination-TRACON-Polygon.
- Transitions:
  ```
  DEPARTURE_CLEARED (cross-country) → LEAVING_FREQUENCY
      → [if departure TRACON exists] → DEPARTURE_CONTACT
      → [after handoff or exit polygon] → FIS_CONTACT / EN_ROUTE
  FIS_CONTACT / EN_ROUTE
      → [entering destination TRACON polygon] → APPROACH_CONTACT
      → [Approach clears for pattern/landing] → PATTERN_ENTRY (existing flow)
  ```
- Neue Intents (in `intent_parser`):
  - `INITIAL_CALL_APPROACH` — "Zurich Approach, …"
  - `INITIAL_CALL_INFORMATION` — "Swiss Information, …", "Swiss Radar, …"
  - `REQUEST_FLIGHT_FOLLOWING` (optional) — VFR advisories.
  - `READBACK_FREQUENCY` — spezialisierter Readback für Handoff ("118.1, good day").
- `ATCResponse` erweitert um optionales `suggested_freq_khz` — wenn gesetzt, schreibt `atc_session` die Frequenz in den Standby-Slot (`xplane_context::set_standby_freq()` existiert bereits).

### Changes to `atc_templates.json`

Neue Template-Sektionen pro State + Intent. Beispiele:

- `towered.DEPARTURE_CLEARED.LEAVING_FREQUENCY` (cross-country Variante) → Controller schlägt aktiv FIS/Approach-Frequenz vor:
  > "{callsign}, contact {next_facility_name} on {next_freq}, good day."
- `towered.APPROACH_CONTACT.INITIAL_CALL_INBOUND` → Approach-Clearance für Pattern-Eintritt und Handoff an Tower:
  > "{callsign}, cleared into {airport} control zone, maintain VFR, report {reporting_point}, contact Tower on {tower_freq}."
- `towered.FIS_CONTACT.REPORT_POSITION` → FIS-Acknowledgement + Hinweis auf nächste Facility wenn im Eintrittsradius.

Template-Variablen neu: `{next_facility_name}`, `{next_freq}`, `{reporting_point}` (für M2 stubbed, echte VFR-Reporting-Points sind späterer Milestone).

### Changes to `flight_phase` / `flight_rules.json`

- Preconditions für neue Intents: `INITIAL_CALL_APPROACH` nur in `CRUISE` oder `CLIMB`, nicht in `PARKED`/`TAXI`.
- Auto-corrections: Wenn Pilot in `APPROACH_CONTACT` State ist aber `FlightPhase == LANDING_ROLL`, auto-reset auf `IDLE` nach Delay (analog zu bestehendem Pattern).

### Changes to `atc_session`

- Nach jedem State-Transition Handoff-Check: Wenn neuer Zielstate `DEPARTURE_CONTACT`/`FIS_CONTACT`/`APPROACH_CONTACT` und `ATCResponse.suggested_freq_khz` gesetzt → `xplane_context::set_standby_freq()` aufrufen. UI zeigt "Suggested NAV: 118.100 STBY" im Status-Panel.
- GPT-Intent-Classifier Prompt erweitern: Neue Intents in `valid_intents()`-Liste.

### UI Changes (`atc_ui`)

- Status-Bar neu: Aktuelle Facility (z.B. "Zurich Approach — 118.100"), State, und "Next handoff: Tower 118.100" wenn Controller das vorgeschlagen hat.
- Transcript markiert Frequenzwechsel visuell (neue Zeile, "— 118.100 —").
- Settings-Toggle: "Enable multi-leg handoffs" (default true, aus falls `atc.dat` fehlt).

---

## Critical Files to Modify

| File | Change |
|---|---|
| `src/airspace_db.hpp/.cpp` | **NEU** — atc.dat Parser + Polygon-Lookup |
| `src/xplane_context.hpp/.cpp` | `enclosing_airspaces` Feld, `FrequencyType` erweitert, atc.dat Lookup für Frequenzen |
| `src/atc_state_machine.hpp/.cpp` | Neue States, Handoff-Logik, `suggested_freq_khz` |
| `src/intent_parser.hpp/.cpp` | Neue Intents (`INITIAL_CALL_APPROACH`, `INITIAL_CALL_INFORMATION`, `READBACK_FREQUENCY`) |
| `src/atc_session.cpp` | Standby-Frequenz setzen nach Handoff |
| `src/atc_ui.cpp` | Status-Anzeige erweitert |
| `src/gpt_client.cpp` | Classifier-Prompt um neue Intents ergänzt |
| `src/main.cpp` | `airspace_db::init()`/`stop()` Aufrufe |
| `src/flight_phase.cpp` | Preconditions für neue Intents |
| `data/atc_templates.json` | Neue Template-Sektionen für FIS/Approach/Departure-Flows |
| `data/flight_rules.json` | Precondition-Einträge für neue Intents |
| `CMakeLists.txt` | `airspace_db.cpp` in `add_library()` aufnehmen |

---

## Implementation Phases

Zur Risiko-Reduktion in drei Sub-Milestones:

**M2.1 — Data Layer:** `airspace_db` modul, atc.dat parsen, Point-in-Polygon, Unit-Test mit LSZH/LSAS Koordinaten. `xplane_context` liest `enclosing_airspaces`. UI zeigt enclosing-Liste debug-halber.

**M2.2 — Approach Flow (Ankunft):** `APPROACH_CONTACT` State + Templates + Handoff an Tower. Flow LSZB → LSZH: Tower release → EN_ROUTE → (Pilot ruft Zurich Approach) → APPROACH_CONTACT → Handoff Tower. Kein FIS, kein Departure-Radar.

**M2.3 — FIS & Departure Radar:** `FIS_CONTACT` / `DEPARTURE_CONTACT` States, aktive Handoff-Vorschläge, Standby-Frequenz-Schreiben.

---

## Reused Existing Infrastructure

- `AirportFrequencies::lookup()` Muster (`xplane_context.cpp`) — als Vorlage für `airspace_db::lookup_by_freq()`.
- `XPlaneContext::set_standby_freq()` (bereits vorhanden) — für Handoff-Frequenz-Schreiben.
- `atc_templates::lookup()` + `fill()` — Template-Engine unverändert, nur neue JSON-Einträge.
- `intent_parser::intent_template_key()` Mapping-Pattern — für neue Intents analog erweitern.
- `flight_phase::check_precondition()` — neue Einträge in `flight_rules.json`, Code-Logik unverändert.
- `gpt_client::classify_intent_async()` — nur Prompt-Update nötig, keine Struktur-Änderung.
- ATC-Session State-Machine Struktur `IDLE → RECORDING → PROCESSING → PLAYING` unverändert.

---

## Verification

Nach Implementierung jedes Sub-Milestones:

1. **Build:** `make lint && make build` muss clean sein.
2. **Unit-Ebene:** Temporäres Debug-Hook in `atc_ui.cpp` zeigt `enclosing_airspaces` Liste. Bei LSZB am Boden erwartet: LSAS (CTR, floor 0 oder ab FL…). Bei 5000ft über Bern erwartet: LSAS. Bei 3000ft über Kloten erwartet: LSZH TRACON + LSAS.
3. **End-to-End LSZB → LSZH Testflug** mit Logitech Headset (aus Memory bekannt):
   - PTT "Bern Tower, HB-XYZ, ready for departure VFR to Zurich"
   - Clearance akzeptieren, Takeoff, Climb
   - Frequenz auf Swiss Info stellen → Pilot: "Swiss Information, HB-XYZ, 5500ft requesting flight following"
   - Erwartete Response: FIS-Acknowledgement.
   - Bei ~20 NM Zürich: Frequenz auf Zurich Approach → Approach-Call → Pattern-Clearance + Tower-Handoff.
   - Tower-Call → Landing-Clearance → Roll-Out → RUNWAY_VACATED → IDLE.
4. **Regression:** Single-Airport Pattern-Flow an LSZB (Start + Landung gleicher Platz) darf nicht brechen.
5. **Robustness:** Test mit gelöschter `atc.dat` → Plugin lädt, neue States bleiben inaktiv, keine Crashes, UI zeigt Hinweis.
6. **Logs:** X-Plane Log.txt darf keine API-Keys enthalten, keine std::exception über Plugin-Boundary.

---

## Open Questions für später (nicht M2)

- VFR-Reporting-Points ("report Whiskey for entry") — benötigt Datenquelle (Navigraph / Custom JSON).
- Cross-Border Handoffs (LSAS → LFMM) mit Sprach-/Akzentwechsel.
- ATC kann Piloten aktiv rufen (nicht nur Pilot-initiiert) — wenn Pilot in CTR einfliegt ohne Call.
