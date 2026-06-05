# M9 — Deutsche VRPs + Pflichtmeldepunkte

**Plan-Referenz:** `~/.claude/plans/kannst-du-mir-einen-jolly-flamingo.md`
**Voraussetzung:** M2 (Templates-Grundstock existiert)
**Folge-Milestones:** M10 (Cross-Country nutzt VRPs für Anflugrouten)

## Ziel
VRP-basierte Anflug-Templates funktionieren für die wichtigsten
deutschen VFR-Plätze.

## Kritische Dateien
- `data/regions/de/airport_vrps.json` — initial 10–15 Plätze nach AIP
  Germany:
  - EDNY (Friedrichshafen) — Whiskey, Sierra, November
  - EDDM München-VRPs (Whiskey, Echo, Sierra, November)
  - EDDF Frankfurt VRPs (für Anflug Bahn 36)
  - EDDH Hamburg
  - EDLN (Mönchengladbach), EDTL (Lahr), EDDS (Stuttgart),
    EDLW (Dortmund), EDDV (Hannover), EDDB (Berlin)
  - Pattern-Direction je Runway aus AIP.

- `data/regions/de/atc_templates.json` — VRP-Templates:
  - `INITIAL_CALL_INBOUND`:
    `"{callsign}, {airport} Turm, guten Tag, {position}, {altitude} Fuss, Information {atis_letter}, zur Landung."`
  - VRP-Entry-Clearance:
    `"{callsign}, freigegeben zum Einflug in die Kontrollzone via {entry_vrp}, Piste {runway}, melden Sie {pattern_direction} Gegenanflug."`

## Datenquelle
AIP Germany (DFS), öffentlich auf `https://aip.dfs.de` — Koordinaten
und Pattern-Direction sind dort authoritative.

**Memory-Notiz:** Daten vor Kodierung verifizieren (siehe
`feedback_verify_apis.md`). Hand-Übertragung aus PDFs ist
fehleranfällig — vor Commit stichprobenartig gegen aktuelles AIP-Chart
abgleichen.

## DoD
Anflug nach EDNY via VRP "Sierra" wird korrekt mit deutscher
Phraseologie + VRP-Namen abgewickelt.
