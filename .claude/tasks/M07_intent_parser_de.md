# M7 — Intent-Parser DE-Keywords (inkl. Reverse-Normalisierung)

**Plan-Referenz:** `~/.claude/plans/kannst-du-mir-einen-jolly-flamingo.md`
**Voraussetzung:** M3 (`de_phraseology`-Modul existiert, hostet auch
den Reverse-Normalizer)
**Folge-Milestones:** keine direkt; alle nachfolgenden profitieren

## Ziel
Rule-based Intent-Erkennung versteht deutsche Funksprüche mit ≥ 70 %
High-Confidence-Rate (damit der LM-Pfad selten getriggert wird).

## Kritische Dateien

### Keyword-Mapping
- `data/regions/de/intent_rules.json` — deutsche Keywords:
  - `"bereit"` / `"abflugbereit"` → `READY_FOR_DEPARTURE_VFR`
  - `"rollen"` / `"rollfreigabe"` → `REQUEST_TAXI`
  - `"haltepunkt"` / `"rollhalt"` → Position-Marker
  - `"piste frei"` / `"piste verlassen"` → `RUNWAY_VACATED`
  - `"gegenanflug"` / `"queranflug"` / `"endanflug"` →
    `REPORT_POSITION_{DOWNWIND,BASE,FINAL}`
  - `"in sicht"` / `"negativ"` → Traffic-Replies (`traffic_dialog`)
  - `"durchstarten"` → `GO_AROUND`
  - `"verstanden"` / `"wilco"` → `READBACK`

### Runway- und Zahlwort-Parser
- `src/atc/intent_parser.cpp:79-99` — Runway-Number-Parser: englische
  Zahlwörter (`"zero"..."niner"`) ersetzen/ergänzen durch deutsche
  (`"null", "eins", "zwo", "drei", "vier", "fuenf", "sechs", "sieben",
  "acht", "neun"`). **"zwo" mandatory** als Aussprachevariante von 2
  (BZF-Standard, siehe M3). **Wichtig:** keine Englisch-Mappings
  entfernen, additiv arbeiten — Auswahl per Region steuern.
  - Suffixe: `"links"` / `"rechts"` / `"mitte"` zusätzlich zu
    `"left"` / `"right"` / `"center"`.

### Reverse-Normalizer (Mirror zu M3)
Pilot spricht BZF-Aussprache, Whisper transkribiert sie wortwörtlich
(z. B. `"QNH eins null eins drei"`). Engine muss daraus strukturierte
Werte ableiten:
- `"eins eins acht Komma drei null null"` → `118.300` (Frequenz)
- `"Steuerkurs null fuenf null"` → `050` (Heading)
- `"QNH eins null eins drei"` → `1013`
- `"Piste zwo fuenf"` → Runway `25`

Implementation: `de_phraseology::parse_spoken_number(text)` als Pendant
zur Forward-Funktion. Lebt im selben Modul wie M3.

### Callsigns
Callsign-Parsing: deutsche Phonetik ("Delta-Echo-Mike-Yankee-Foxtrot")
ist NATO-identisch — kein Code-Change nötig, ggf. lokale
Aussprache-Varianten ("Echo" vs. "Edisson") in einer Aliases-Liste.

## DoD
20 typische BZF-Funksprüche aus AIP Germany / Praxisbeispielen werden
korrekt klassifiziert (Test-Set in `tests/` als `scenario_de_basic.cpp`
oder Equivalent).
