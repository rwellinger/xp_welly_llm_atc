# M2 — Deutsche Templates für die Platzrunde (towered)

**Plan-Referenz:** `~/.claude/plans/kannst-du-mir-einen-jolly-flamingo.md`
**Voraussetzung:** M1 (DE-Region geladen)
**Folge-Milestones:** M3 (Normalizer), M5 (OpenAI), M9 (VRPs erweitern)

## Ziel
Eine vollständige Platzrunde (Start → Touch-and-Go → Landung)
funktioniert deutschsprachig mit OpenAI-Backend (Local kommt in M6).

## Test-Airport
**EDNY (Friedrichshafen)** — Class D, gute VFR-AIP-Doku, typischer
GA-Platz mit Tower. Alternativ EDLN/EDTL.

## Kritische Dateien
- `data/regions/de/atc_templates.json` — alle Templates für die
  Platzrunde-relevanten States übersetzen:
  - `towered.IDLE.RADIO_CHECK`
  - `towered.IDLE.INITIAL_CALL_GROUND` →
    `"{callsign}, {airport} Rollkontrolle, guten Tag, am Vorfeld, Information {atis_letter}, bitte Rollen zur Piste {runway}."`
  - `towered.GROUND_CONTACT.REQUEST_TAXI` →
    `"{callsign}, rollen Sie zum Rollhalt Piste {runway} ueber {taxi_route}, QNH {qnh}."`
  - `TAXI_CLEARED.READY_FOR_DEPARTURE_VFR`
  - `TOWER_CONTACT.READY_FOR_DEPARTURE_VFR` →
    `"{callsign}, Wind {wind}, Piste {runway}, Startfreigabe."`
  - `DEPARTURE_CLEARED.REPORT_POSITION_DOWNWIND` /
    `_BASE` / `_FINAL`
  - `PATTERN_ENTRY.REQUEST_LANDING` /
    `REQUEST_TOUCH_AND_GO` →
    `"{callsign}, Wind {wind}, Piste {runway}, Landefreigabe."`
  - `LANDING_CLEARED.RUNWAY_VACATED` →
    `"{callsign}, verlassen Sie die Piste bei {exit}, melden Sie sich bei der Rollkontrolle."`
  - `_INVALID` Fallbacks für jeden State.
- `data/regions/de/phraseology_hints.json` — Pilot-Hint-Strings ins
  Deutsche, gleicher Schema-Aufbau wie EU.

## Konventionen
**Maßeinheiten:** Metrisch (m, km, hPa, °C) — bereits der EU-Default in
`atis_generator.cpp`; DE übernimmt automatisch.

**Wichtig:** Templates bleiben **reine String-Substitution**, keine
neuen Variablen erfinden. Falls eine deutsche Phrase eine neue Variable
braucht (z. B. `{taxi_route}`), zuerst prüfen ob `atc_state_machine`
sie schon liefert — sonst Variable in `build_vars()` ergänzen, nicht
hardcoden.

**Templates müssen Ziffern enthalten, nicht "zwo"/"null sieben"** —
die Aussprache-Normalisierung übernimmt M3.

## DoD
Mit OpenAI-Backend + DE-Region läuft eine komplette Platzrunde mit
deutschen Templates. Die TTS-Stimme klingt englisch-akzentuiert (das
ist OK, wird in M5/M6 verbessert).
