# M10 — Cross-Country: FIS-Handover + unkontrollierte Plätze

**Plan-Referenz:** `~/.claude/plans/kannst-du-mir-einen-jolly-flamingo.md`
**Voraussetzung:** M2 (Towered-Templates), M9 (VRPs für
Anflugrouten-Beschreibung)
**Folge-Milestones:** keine (Cross-Country = Letzte MVP-Erweiterung)

## Ziel
Cross-Country-VFR funktioniert: kontrollierter Start → FIS-Handover
en route → unkontrollierte Landung (oder umgekehrt).

## Neue Phraseology-Patterns

### FIS-Handover (von Tower)
`"{callsign}, wechseln Sie auf Langen Information, 119.450, schoenen Flug."`

### FIS-Initial-Call
`"Langen Information, D-EXYZ, guten Tag, PA-28, von EDNY nach EDTL,
 position {VRP}, Hoehe 3500 Fuss, VFR, Information {atis_letter}."`

### Unkontrollierter Platz ("Info"-Frequenz, kein Tower)
- Templates in `data/regions/de/atc_templates.json` unter `uncontrolled.*` —
  Self-Announce-Style:
  `"{airport} Info, D-EXYZ, im Gegenanflug Piste {runway}."`
- Antwort kommt vom Flugleiter (informativ, **keine Freigabe**):
  `"D-EXYZ, {airport} Info, Wind {wind}, QNH {qnh}, andere Verkehre nicht bekannt."`

## Kritische Dateien
- `src/atc/atc_state_machine.cpp` — `EN_ROUTE` State existiert bereits.
  FIS-Frequenz-Handling über vorhandene `intent_frequency` Guards in
  `flight_rules.json`.
- `data/regions/de/flight_rules.json` — Frequenz-Type-Checks ergänzen
  falls deutsche FIS-Frequenzen anders behandelt werden müssen
  (vermutlich nicht — die Logik ist generisch).
- `data/regions/de/atc_templates.json` — komplettes `uncontrolled.*`
  Subtree übersetzen.

## DoD
Flug EDNY → EDTL: Start aus EDNY mit Tower-Freigabe, en-route Handover
an Langen Information, Anflug auf EDTL via "Info"-Frequenz mit
Self-Announce. Alles deutsch, beide Backends.
