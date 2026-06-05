# M1 — Region-Scaffolding `DE` (kein Sprachwechsel)

**Plan-Referenz:** `~/.claude/plans/kannst-du-mir-einen-jolly-flamingo.md`
**Voraussetzung:** keine
**Folge-Milestones:** M2 (Templates), M4 (UI)

## Ziel
`DE` ist als dritte Region wählbar; Plugin lädt DE-Templates, verhält
sich aber funktional identisch zu EU (Kopie der EU-Files).

## Kritische Dateien
- `src/persistence/settings.cpp:168-169, 207-216, 329-334` —
  Whitelist `"eu"|"us"` auf `"eu"|"us"|"de"` erweitern,
  `set_flow_region()` analog.
- `src/ui/atc_ui.cpp:86-87, 1189` — Combo-Array von `{"EU", "US"}` auf
  `{"EU", "US", "DE"}` und Item-Count auf `IM_ARRAYSIZE(arr)` umstellen
  (nicht hardcoded `2`).
- `src/atc/atis_generator.cpp:66, 232` — Unit-Branching aktuell binär
  `== "US"`. Wenn nicht `"US"`, fällt automatisch auf EU-Style (metrisch,
  QNH) — passt für DE, kein Code-Change nötig, aber dokumentieren.
- **Neu:** `data/regions/de/` mit Kopien von
  `atc_templates.json`, `flight_rules.json`, `airport_vrps.json` (leer
  oder leeres `{}`), `intent_rules.json`, `phraseology_hints.json`
  aus dem EU-Ordner.

## DoD
Region-Wechsel zu DE im UI lädt die DE-Files, Plugin verhält sich
identisch zur EU-Variante (englisch). Smoke-Test: `atc_repl` mit
DE-Region läuft ohne Fehler.
